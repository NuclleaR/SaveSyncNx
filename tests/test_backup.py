"""One-action backup: every changed account and device save becomes a new version in Koofr."""

import io
import os
import zipfile

import pytest

from conftest import APP_ROOT

DATE1 = "Thu, 12 Nov 2026 18:45:01 GMT"
STAMP1 = "20261112-184501"
DATE2 = "Fri, 13 Nov 2026 09:00:00 GMT"
STAMP2 = "20261113-090000"
G1 = ("0100000000001000", "Game One")
G2 = ("0100000000002000", "Game Two")
F1 = f"0x{G1[0]} {G1[1]}"
F2 = f"0x{G2[0]} {G2[1]}"


def summary(u, n, f):
    return f"Backup done: {u} uploaded, {n} unchanged, {f} failed."


@pytest.fixture
def backup(run_script, dav, seed_vault, sd):
    seed_vault()
    profiles = sd / APP_ROOT / "config" / "savesync-profiles.txt"
    profiles.write_text("Serhii\nSon\n")

    def run(profile="Serhii", date=DATE1, faults=None):
        dav.fixed_date = date
        r = run_script([{"kind": "PickOne", "item": profile}, {"kind": "Message"}], koofr_url=dav.url, faults=faults)
        assert r.exit == 0, r.output
        assert r.save_writes == []
        return r

    return run


def zips(dav, kind, folder):
    d = dav.root / "SaveSyncNx" / kind / folder
    return sorted(p.name for p in d.iterdir()) if d.exists() else []


def zip_files(path):
    with zipfile.ZipFile(path) as z:
        return {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}


def test_first_run_uploads_account_and_device_skips_bcat_system(backup, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one"})
    catalog.add(G2[0], G2[1], "device", {"/dev.bin": b"two"})
    catalog.add("0100000000003000", "Bcat Thing", "bcat", {"/b.bin": b"3"})
    catalog.add("8000000000000010", "0x8000000000000010", "system", {"/s.bin": b"4"})
    r = backup()
    assert r.ui[-1]["prompt"] == summary(2, 0, 0)
    assert zips(dav, "saves", F1) == [f"{STAMP1} Serhii.zip"]
    assert zips(dav, "device", F2) == [f"{STAMP1}.zip"]
    assert sorted(p.name for p in (dav.root / "SaveSyncNx" / "saves").iterdir()) == [F1]
    assert sorted(p.name for p in (dav.root / "SaveSyncNx" / "device").iterdir()) == [F2]
    assert summary(2, 0, 0) in r.output


def test_second_run_unchanged(backup, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one"})
    catalog.add(G2[0], G2[1], "device", {"/dev.bin": b"two"})
    backup()
    seen = len(dav.log)
    r = backup(date=DATE2)
    assert r.ui[-1]["prompt"] == summary(0, 2, 0)
    assert [e for e in dav.log[seen:] if e[0] in ("PUT", "MOVE")] == []


def test_changed_save_adds_version(backup, catalog, dav):
    d1 = catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one"})
    catalog.add(G2[0], G2[1], "device", {"/dev.bin": b"two"})
    backup()
    (d1 / "save.bin").write_bytes(b"ONE")
    r = backup(date=DATE2)
    assert r.ui[-1]["prompt"] == summary(1, 1, 0)
    assert zips(dav, "saves", F1) == [f"{STAMP1} Serhii.zip", f"{STAMP2} Serhii.zip"]
    assert zips(dav, "device", F2) == [f"{STAMP1}.zip"]
    assert zip_files(dav.root / "SaveSyncNx" / "saves" / F1 / f"{STAMP2} Serhii.zip") == {"save.bin": b"ONE"}


def test_zip_contents_match_save(backup, catalog, dav):
    files = {"/top.bin": os.urandom(5000), "/a/b/deep.bin": b"deep", "/a/side.txt": b"side", "/c/empty.bin": b""}
    catalog.add(G1[0], G1[1], "account", files)
    backup()
    got = zip_files(dav.root / "SaveSyncNx" / "saves" / F1 / f"{STAMP1} Serhii.zip")
    assert got == {k.lstrip("/"): v for k, v in files.items()}


def test_two_profiles_device_once(backup, catalog, dav, tmp_path):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"serhii's"})
    catalog.add(G1[0], G1[1], "device", {"/dev.bin": b"shared"})
    backup(profile="Serhii")
    son = tmp_path / "son-save"
    son.mkdir()
    (son / "save.bin").write_bytes(b"son's")
    catalog.titles[0]["dir"] = str(son)
    r = backup(profile="Son", date=DATE2)
    assert r.ui[-1]["prompt"] == summary(1, 1, 0)
    assert zips(dav, "saves", F1) == [f"{STAMP1} Serhii.zip", f"{STAMP2} Son.zip"]
    assert zips(dav, "device", F1) == [f"{STAMP1}.zip"]
    assert zip_files(dav.root / "SaveSyncNx" / "saves" / F1 / f"{STAMP2} Son.zip") == {"save.bin": b"son's"}


def test_no_save_writes(backup, catalog):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one", "/d/x": b"2"})
    catalog.add(G2[0], G2[1], "device", {"/dev.bin": b"two"})
    for date in (DATE1, DATE2):
        assert backup(date=date).save_writes == []


def test_nothing_left_on_sd(backup, catalog, sd):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one", "/d/x": b"2"})
    catalog.add(G2[0], G2[1], "device", {"/dev.bin": b"two"})
    backup()
    root = sd / APP_ROOT
    assert list((root / "savesync").iterdir()) == []
    assert sorted(p.name for p in (root / "config").iterdir()) == ["savesync-profiles.txt", "savesync-state.txt", "savesync.vault"]
    files = [p for p in root.rglob("*") if p.is_file() and p.parent.name != "config"]
    assert files == []


def test_lost_state_reuploads_once(backup, catalog, dav, sd):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one"})
    catalog.add(G2[0], G2[1], "device", {"/dev.bin": b"two"})
    backup()
    (sd / APP_ROOT / "config" / "savesync-state.txt").unlink()
    assert backup(date=DATE2).ui[-1]["prompt"] == summary(2, 0, 0)
    assert backup(date="Sat, 14 Nov 2026 10:00:00 GMT").ui[-1]["prompt"] == summary(0, 2, 0)
    assert zips(dav, "saves", F1) == [f"{STAMP1} Serhii.zip", f"{STAMP2} Serhii.zip"]


def test_nonascii_title_folder_keeps_its_name(backup, catalog, dav, sd):
    # Checkpoint names its own folder "0x<ID> 0x<ID>" for such a title; the
    # cloud folder keeps the real name, made safe, so people can find it in Koofr
    tid = "0100000000004000"
    catalog.add(tid, "ポケモン: Let's Go/Eevee.", "account", {"/save.bin": b"p"})
    r = backup()
    assert r.ui[-1]["prompt"] == summary(1, 0, 0)
    assert (sd / APP_ROOT / "saves" / f"0x{tid} 0x{tid}").is_dir()
    assert zips(dav, "saves", f"0x{tid} ポケモン_ Let's Go_Eevee_") == [f"{STAMP1} Serhii.zip"]


def test_ascii_title_folder_is_checkpoints(backup, catalog, dav):
    tid = "0100000000005000"
    catalog.add(tid, "Luigi's Mansion 3 & #1 50%. Go!", "account", {"/save.bin": b"l"})
    backup()
    assert zips(dav, "saves", f"0x{tid} Luigi's Mansion 3 & #1 50%  Go") == [f"{STAMP1} Serhii.zip"]


def test_unflushed_dump_counts_failed(backup, catalog, dav):
    # the SD card fails the final flush of a copied save file: the copy may
    # be short, so it must not become a version
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one"})
    r = backup(faults=["--fail-close", "savesync/dump/"])
    assert r.ui[-1]["prompt"] == summary(0, 0, 1)
    assert zips(dav, "saves", F1) == []


def test_empty_save_skipped(backup, catalog, dav):
    catalog.add(G1[0], G1[1], "account", {})
    r = backup()
    assert r.ui[-1]["prompt"] == summary(0, 1, 0)
    assert "empty save, skipped" in r.output
    assert zips(dav, "saves", F1) == []


def test_large_file_roundtrip(backup, catalog, dav):
    big = os.urandom(20 * 1024 * 1024)
    catalog.add(G1[0], G1[1], "account", {"/big.bin": big})
    r = backup()
    assert r.ui[-1]["prompt"] == summary(1, 0, 0)
    assert zip_files(dav.root / "SaveSyncNx" / "saves" / F1 / f"{STAMP1} Serhii.zip") == {"big.bin": big}
