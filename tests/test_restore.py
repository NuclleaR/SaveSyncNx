"""Restore: any version in Koofr becomes a backup folder in Checkpoint's list."""

import io
import os
import random
import zipfile

import pytest

from conftest import APP_ROOT

G1 = ("0100000000001000", "Game One")
G2 = ("0100000000002000", "Game Two")
G3 = ("0100000000003000", "Gone Game")
F1 = f"0x{G1[0]} {G1[1]}"
V1 = "20261112-184501 Serhii"
NOTHING = "There are no backups in Koofr for the games on this console."
DONE = ("Downloaded. Pick it in Checkpoint's backup\nlist for this game and press Restore.\n"
        "For an account save, select the right\nprofile first.")
FAILED = "The download failed and was removed.\nNothing was changed."
NET = "No connection to Koofr. Check Wi-Fi."


def confirm_copy(version, name):
    return (f"Download\n{version}\ninto Checkpoint's backup folder for\n{name}?\n\n"
            "Nothing is written to the game until\nyou restore it in Checkpoint.")


def pick(item):
    return {"kind": "PickOne", "item": item}


BACK = {"kind": "PickOne", "index": -1}
YES = {"kind": "Confirm", "yes": True}
NO = {"kind": "Confirm", "yes": False}
MESSAGE = {"kind": "Message"}


def make_zip(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


def put_version(dav, kind, folder, version, files=None, raw=None):
    d = dav.root / "SaveSyncNx" / kind / folder
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{version}.zip").write_bytes(raw if raw is not None else make_zip(files or {"save.bin": b"x"}))


def local_dir(sd, kind, folder):
    return sd / APP_ROOT / kind / folder


def tree(path):
    return {str(p.relative_to(path)): p.read_bytes() for p in path.rglob("*") if p.is_file()}


@pytest.fixture
def restore(run_script, dav, seed_vault, sd):
    seed_vault()
    (sd / APP_ROOT / "config" / "savesync-profiles.txt").write_text("Serhii\n")

    def run(answers):
        r = run_script([pick("Restore...")] + answers, koofr_url=dav.url)
        assert r.exit == 0, r.output
        assert r.save_writes == []
        return r

    return run


def test_no_backups_says_so(restore, catalog):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    r = restore([MESSAGE])
    assert r.ui[-1]["prompt"] == NOTHING


def test_lists_only_games_on_console(restore, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    catalog.add(G2[0], G2[1], "account", {"/s": b"2"})
    put_version(dav, "saves", F1, V1)
    put_version(dav, "saves", f"0x{G3[0]} {G3[1]}", V1)
    put_version(dav, "device", f"0x{G2[0]} {G2[1]}", "20261112-184501")
    r = restore([BACK])
    assert r.ui[1]["prompt"] == ("Pick a game\n(2 more in Koofr have no save on this console;\n"
                                 "start a game once to create its save)")
    assert r.ui[1]["items"] == ["Game One"]


def test_versions_newest_first_no_cap(restore, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    names = [f"202611{d:02d}-{s:06d} Serhii" for d in range(1, 11) for s in range(20)]
    random.Random(7).shuffle(names)
    for n in names:
        put_version(dav, "saves", F1, n)
    r = restore([pick("Game One"), BACK, BACK])
    versions = r.ui[2]
    assert versions["prompt"] == "Pick a version of Game One"
    assert len(versions["items"]) == 200
    assert versions["items"] == sorted(names, reverse=True)
    assert versions["items"][0] == "20261110-000019 Serhii"
    assert [u["prompt"] for u in r.ui][3] == "Pick a game"


def test_restore_bytes_identical(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    files = {"top.bin": os.urandom(5000), "a/b/deep.bin": b"deep", "a/side.txt": b"side", "c/empty.bin": b""}
    put_version(dav, "saves", F1, V1, files)
    r = restore([pick("Game One"), pick(V1), YES, MESSAGE])
    assert r.ui[3]["prompt"] == confirm_copy(V1, "Game One")
    assert r.ui[-1]["prompt"] == DONE
    assert tree(local_dir(sd, "saves", F1) / V1) == files
    assert not (sd / APP_ROOT / "savesync" / "down.zip").exists()


def test_existing_local_folder_gets_suffix(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    put_version(dav, "saves", F1, V1, {"new.bin": b"new"})
    old = local_dir(sd, "saves", F1) / V1
    old.mkdir(parents=True)
    (old / "old.bin").write_bytes(b"old")
    (local_dir(sd, "saves", F1) / f"{V1} (2)").mkdir()
    r = restore([pick("Game One"), pick(V1), YES, MESSAGE])
    assert r.ui[-1]["prompt"] == DONE
    assert tree(old) == {"old.bin": b"old"}
    assert tree(local_dir(sd, "saves", F1) / f"{V1} (2)") == {}
    assert tree(local_dir(sd, "saves", F1) / f"{V1} (3)") == {"new.bin": b"new"}


def test_corrupt_zip_rolls_back(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    good = make_zip({"save.bin": os.urandom(4000), "b.bin": b"b"})
    put_version(dav, "saves", F1, V1, raw=good[: len(good) // 2])
    r = restore([pick("Game One"), pick(V1), YES, MESSAGE])
    assert r.ui[-1]["prompt"] == FAILED
    assert list(local_dir(sd, "saves", F1).iterdir()) == []
    assert not (sd / APP_ROOT / "savesync" / "down.zip").exists()


def test_abort_during_unzip_leaves_no_folder(run_script, seed_vault, catalog, dav, sd):
    # hold-B while unzip runs: picoc stops at the next statement, so nothing
    # after unzip runs; Checkpoint's list must still hold no half-written folder
    seed_vault()
    (sd / APP_ROOT / "config" / "savesync-profiles.txt").write_text("Serhii\n")
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    put_version(dav, "saves", F1, V1, {f"f{i}.bin": os.urandom(70000) for i in range(4)})
    r = run_script([pick("Restore..."), pick("Game One"), pick(V1), YES], koofr_url=dav.url,
                   faults=["--abort-during", "unzip"])
    assert r.cancelled, r.output
    assert list(local_dir(sd, "saves", F1).iterdir()) == []


def test_unflushed_download_rolls_back(run_script, seed_vault, catalog, dav, sd):
    seed_vault()
    (sd / APP_ROOT / "config" / "savesync-profiles.txt").write_text("Serhii\n")
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    put_version(dav, "saves", F1, V1)
    r = run_script([pick("Restore..."), pick("Game One"), pick(V1), YES, MESSAGE], koofr_url=dav.url,
                   faults=["--fail-close", "savesync/down.zip"])
    assert r.exit == 0, r.output
    assert r.ui[-1]["prompt"] == FAILED
    assert list(local_dir(sd, "saves", F1).iterdir()) == []
    assert not (sd / APP_ROOT / "savesync" / "down.zip").exists()


def test_download_error_rolls_back(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    put_version(dav, "saves", F1, V1)
    dav.fail_next("GET", 500)
    r = restore([pick("Game One"), pick(V1), YES, MESSAGE])
    assert r.ui[-1]["prompt"] == FAILED
    assert list(local_dir(sd, "saves", F1).iterdir()) == []
    assert not (sd / APP_ROOT / "savesync" / "down.zip").exists()


def test_renamed_game_folders_merge(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    put_version(dav, "saves", f"0x{G1[0]} Old Title", "20261101-100000 Serhii", {"old.bin": b"from old folder"})
    put_version(dav, "saves", F1, "20261105-100000 Son", {"new.bin": b"from new folder"})
    r = restore([pick("Game One"), pick("20261101-100000 Serhii"), YES, MESSAGE])
    assert r.ui[2]["items"] == ["20261105-100000 Son", "20261101-100000 Serhii"]
    assert r.ui[-1]["prompt"] == DONE
    assert tree(local_dir(sd, "saves", F1) / "20261101-100000 Serhii") == {"old.bin": b"from old folder"}


def test_device_save_labelled_and_restored_to_device_folder(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    catalog.add(G1[0], G1[1], "device", {"/d": b"2"})
    put_version(dav, "saves", F1, V1, {"acc.bin": b"account"})
    put_version(dav, "device", F1, "20261112-184501", {"dev.bin": b"device"})
    r = restore([pick("Game One (device)"), pick("20261112-184501"), YES, MESSAGE])
    assert r.ui[1]["items"] == ["Game One", "Game One (device)"]
    assert r.ui[2]["items"] == ["20261112-184501"]
    assert r.ui[-1]["prompt"] == DONE
    assert tree(local_dir(sd, "device", F1) / "20261112-184501") == {"dev.bin": b"device"}
    assert list(local_dir(sd, "saves", F1).iterdir()) == []


def test_cancel_at_confirm_writes_nothing(restore, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    put_version(dav, "saves", F1, V1)
    r = restore([pick("Game One"), pick(V1), NO, BACK, BACK])
    assert [u["prompt"] for u in r.ui][4:] == ["Pick a version of Game One", "Pick a game"]
    assert list(local_dir(sd, "saves", F1).iterdir()) == []
    assert [e for e in dav.log if e[0] == "GET"] == []


@pytest.mark.parametrize("name, cloud, local", [
    ("Café™ & 'Friends' #1: 50%?", "Café™ & 'Friends' #1_ 50%_", "0x0100000000004000"),
    ("Luigi's Mansion 3 & #1 50%", "Luigi's Mansion 3 & #1 50%", "Luigi's Mansion 3 & #1 50%"),
])
def test_restore_special_names(run_script, restore, catalog, dav, sd, name, cloud, local):
    # backed up by the script, then restored: the URL encoding of ' & # % and
    # non-ASCII names must survive both ways
    tid = "0100000000004000"
    files = {"/save.bin": b"caf\xc3\xa9"}
    catalog.add(tid, name, "account", files)
    dav.fixed_date = "Thu, 12 Nov 2026 18:45:01 GMT"
    r = run_script([pick("Serhii"), MESSAGE], koofr_url=dav.url)
    assert "Backup done: 1 uploaded" in r.output, r.output
    assert (dav.root / "SaveSyncNx" / "saves" / f"0x{tid} {cloud}").is_dir()
    r = restore([pick(name), pick(V1), YES, MESSAGE])
    assert r.ui[-1]["prompt"] == DONE
    assert tree(local_dir(sd, "saves", f"0x{tid} {local}") / V1) == {"save.bin": b"caf\xc3\xa9"}


def test_game_folder_without_versions(restore, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    (dav.root / "SaveSyncNx" / "saves" / F1).mkdir(parents=True)
    r = restore([pick("Game One"), MESSAGE, BACK])
    assert [u["prompt"] for u in r.ui][1:] == ["Pick a game", "There are no versions of Game One in Koofr.", "Pick a game"]


def test_restore_network_down(restore, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    dav.stop()
    r = restore([MESSAGE])
    assert r.ui[-1]["prompt"] == NET


def test_restore_never_writes_saves(restore, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/s": b"1"})
    catalog.add(G1[0], G1[1], "device", {"/d": b"2"})
    put_version(dav, "saves", F1, V1)
    put_version(dav, "device", F1, "20261112-184501")
    r = restore([pick("Game One"), pick(V1), YES, MESSAGE])
    assert r.ui[-1]["prompt"] == DONE
    assert r.save_writes == []
