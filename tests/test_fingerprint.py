"""Change detection: the fingerprint of a zip_dir zip, read from its central directory."""

import os
import re
import zipfile
import zlib

from conftest import APP_ROOT

SD_ROOT = "sdmc:/switch/Checkpoint"


def make_tree(sd, name, files, dirs=()):
    base = sd / APP_ROOT / name
    base.mkdir(parents=True, exist_ok=True)
    for d in dirs:
        (base / d).mkdir(parents=True, exist_ok=True)
    for rel, data in files.items():
        target = base / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return base


def fingerprints(run_unit, *trees):
    body = "char fp[128];\n"
    for t in trees:
        body += (
            f'    printf("zip=%d\\n", zip_dir("{SD_ROOT}/{t}", "{SD_ROOT}/{t}.zip"));\n'
            f'    printf("ok=%d\\n", zip_fingerprint("{SD_ROOT}/{t}.zip", fp, 128));\n'
            '    printf("fp=%s\\n", fp);\n'
        )
    r = run_unit(body)
    assert r.exit == 0, r.output
    assert re.findall(r"^zip=(.*)$", r.output, re.M) == ["0"] * len(trees), r.output
    assert re.findall(r"^ok=(.*)$", r.output, re.M) == ["1"] * len(trees), r.output
    return re.findall(r"^fp=(.*)$", r.output, re.M)


def python_fingerprint(zip_path):
    with zipfile.ZipFile(zip_path) as z:
        infos = z.infolist()
    lines = sorted(f"{i.CRC:08x} {i.file_size} {i.filename}\n" for i in infos)
    joined = "".join(lines).encode()
    return f"{len(infos)}:{sum(i.file_size for i in infos)}:{zlib.crc32(joined):08x}"


def test_crc32_vector(run_unit):
    r = run_unit('printf("crc=%08x\\n", crc32_buf("123456789", 9, 0));\n    printf("crc=%08x\\n", crc32_buf("6789", 4, crc32_buf("12345", 5, 0)));')
    assert r.exit == 0, r.output
    assert re.findall(r"^crc=(.*)$", r.output, re.M) == ["cbf43926", "cbf43926"]


def test_same_tree_same_fingerprint(run_unit, sd):
    files = {"a.bin": b"hello", "sub/b.bin": bytes(range(256))}
    make_tree(sd, "t1", files)
    make_tree(sd, "t2", files)
    fp1, fp2 = fingerprints(run_unit, "t1", "t2")
    assert fp1 == fp2
    assert fp1 == python_fingerprint(sd / APP_ROOT / "t1.zip")


def test_same_size_content_change_differs(run_unit, sd):
    make_tree(sd, "t1", {"x": b"aaaa"})
    make_tree(sd, "t2", {"x": b"aaab"})
    fp1, fp2 = fingerprints(run_unit, "t1", "t2")
    assert fp1 != fp2


def test_rename_differs(run_unit, sd):
    make_tree(sd, "t1", {"x": b"data"})
    make_tree(sd, "t2", {"y": b"data"})
    fp1, fp2 = fingerprints(run_unit, "t1", "t2")
    assert fp1 != fp2


def test_added_file_in_subfolder_differs(run_unit, sd):
    make_tree(sd, "t1", {"x": b"data", "sub/y": b"1"})
    make_tree(sd, "t2", {"x": b"data", "sub/y": b"1", "sub/z": b""})
    fp1, fp2 = fingerprints(run_unit, "t1", "t2")
    assert fp1 != fp2


def test_mtime_only_change_same(run_unit, sd):
    base = make_tree(sd, "t1", {"x": b"data", "sub/y": b"1"})
    (fp1,) = fingerprints(run_unit, "t1")
    for path in [base / "x", base / "sub" / "y"]:
        os.utime(path, (1_000_000_000, 1_000_000_000))
    (fp2,) = fingerprints(run_unit, "t1")
    assert fp1 == fp2


def test_creation_order_irrelevant(run_unit, sd):
    a = sd / APP_ROOT / "t1"
    b = sd / APP_ROOT / "t2"
    a.mkdir(parents=True)
    b.mkdir(parents=True)
    for name in ["a", "b", "c", "d"]:
        (a / name).write_bytes(name.encode())
    for name in ["d", "c", "b", "a"]:
        (b / name).write_bytes(name.encode())
    fp1, fp2 = fingerprints(run_unit, "t1", "t2")
    assert fp1 == fp2


def test_fingerprint_stable_with_subfolders(run_unit, sd):
    files = {"top.bin": b"t" * 1000, "a/b/c/deep.bin": b"d", "a/side.bin": b"s"}
    make_tree(sd, "t1", files, dirs=["empty", "a/b/also-empty"])
    runs = [fingerprints(run_unit, "t1")[0] for _ in range(3)]
    assert runs[0] == runs[1] == runs[2]
    assert runs[0] == python_fingerprint(sd / APP_ROOT / "t1.zip")


def test_not_a_zip_is_an_error(run_unit, sd):
    (sd / APP_ROOT / "junk.zip").write_bytes(b"not a zip")
    r = run_unit(f'char fp[128];\n    printf("ok=%d\\n", zip_fingerprint("{SD_ROOT}/junk.zip", fp, 128));\n'
                 f'    printf("ok=%d\\n", zip_fingerprint("{SD_ROOT}/missing.zip", fp, 128));')
    assert r.exit == 0, r.output
    assert re.findall(r"^ok=(.*)$", r.output, re.M) == ["0", "0"]
