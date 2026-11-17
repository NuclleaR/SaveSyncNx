"""Errors: login, network and quota stop the run; anything else fails only that save."""

import pytest

from conftest import APP_ROOT

NET = "No connection to Koofr. Check Wi-Fi."
LOGIN = "Koofr rejected the login. Check email and app password in Settings."
FULL = "Koofr storage is full."
G1 = ("0100000000001000", "Game One")
G2 = ("0100000000002000", "Game Two")
F1 = f"0x{G1[0]} {G1[1]}"
F2 = f"0x{G2[0]} {G2[1]}"
STAMP = "20261112-184501"


def summary(u, n, f):
    return f"Backup done: {u} uploaded, {n} unchanged, {f} failed."


@pytest.fixture
def backup(run_script, dav, seed_vault, sd):
    profiles = sd / APP_ROOT / "config" / "savesync-profiles.txt"

    def run(pw="app-pass", date="Thu, 12 Nov 2026 18:45:01 GMT"):
        seed_vault(pw=pw)
        profiles.write_text("Serhii\n")
        dav.fixed_date = date
        r = run_script([{"kind": "PickOne", "item": "Serhii"}, {"kind": "Message"}], koofr_url=dav.url)
        assert r.exit == 0, r.output
        assert r.save_writes == []
        return r

    return run


def zips(dav, kind, folder):
    d = dav.root / "SaveSyncNx" / kind / folder
    return sorted(p.name for p in d.iterdir()) if d.exists() else []


def incoming(dav):
    d = dav.root / "SaveSyncNx" / ".incoming"
    return sorted(p.name for p in d.iterdir()) if d.exists() else []


def two_games(catalog):
    catalog.add(G1[0], G1[1], "account", {"/save.bin": b"one"})
    catalog.add(G2[0], G2[1], "account", {"/save.bin": b"two"})


def assert_stopped(r, copy):
    assert r.ui[-1]["kind"] == "Message", r.ui
    assert r.ui[-1]["prompt"] == copy
    assert copy in r.output
    assert "Backup done:" not in r.output


def test_wrong_login_stops_run(backup, catalog, dav):
    two_games(catalog)
    r = backup(pw="wrong")
    assert_stopped(r, LOGIN)
    assert [e for e in dav.log if e[0] == "PUT"] == []


def test_forbidden_stops_run(backup, catalog, dav):
    two_games(catalog)
    dav.fail_next("MKCOL", 403)
    r = backup()
    assert_stopped(r, LOGIN)
    assert [e for e in dav.log if e[0] == "PUT"] == []


def probes(dav):
    """The Depth 0 PROPFINDs of SaveSyncNx/ itself: the mid-run check."""
    return [m for m, p, h in dav.request_headers if m == "PROPFIND" and p == "/dav/Koofr/SaveSyncNx" and h.get("Depth") == "0"]


def test_quota_full_stops_run(backup, catalog, dav):
    two_games(catalog)
    dav.quota_full = True
    r = backup()
    assert_stopped(r, FULL)
    assert [e[2] for e in dav.log if e[0] == "PUT"] == [507]
    assert zips(dav, "saves", F1) == [] and zips(dav, "saves", F2) == []
    assert probes(dav) == []


def test_network_down_stops_run(backup, catalog, dav):
    two_games(catalog)
    dav.stop()
    r = backup()
    assert_stopped(r, NET)


def test_network_lost_mid_run_stops_run(backup, catalog, dav):
    two_games(catalog)
    dav.fail_from("PUT", None, path_contains=G1[0])
    r = backup()
    assert_stopped(r, NET)
    assert len(probes(dav)) == 1
    assert [e for e in dav.log if e[0] == "PUT" and G2[0] in e[1]] == []


def test_login_lost_mid_run_stops_run(backup, catalog, dav):
    two_games(catalog)
    dav.fail_from("PUT", 401, path_contains=G1[0])
    r = backup()
    assert_stopped(r, LOGIN)
    assert len(probes(dav)) == 1
    assert [e for e in dav.log if e[0] == "PUT" and G2[0] in e[1]] == []


def test_one_dropped_put_fails_only_that_game(backup, catalog, dav):
    # one transfer cut off (curl gives up below 300 B/s for 10 s) while Koofr
    # still answers: only that save fails, so the games after it get backed up
    two_games(catalog)
    dav.drop_next("PUT", path_contains=G1[0])
    r = backup()
    assert r.ui[-1]["prompt"] == summary(1, 0, 1)
    assert len(probes(dav)) == 1
    assert zips(dav, "saves", F1) == []
    assert zips(dav, "saves", F2) == [f"{STAMP} Serhii.zip"]


def test_one_forbidden_put_fails_only_that_game(backup, catalog, dav):
    two_games(catalog)
    dav.fail_next("PUT", 403, path_contains=G1[0])
    r = backup()
    assert r.ui[-1]["prompt"] == summary(1, 0, 1)
    assert zips(dav, "saves", F1) == []
    assert zips(dav, "saves", F2) == [f"{STAMP} Serhii.zip"]


def test_server_error_at_start_says_so(backup, catalog, dav):
    two_games(catalog)
    dav.fail_next("MKCOL", 500)
    r = backup()
    assert_stopped(r, "Koofr answered HTTP 500.")
    assert [e for e in dav.log if e[0] == "PUT"] == []


def test_one_failed_put_fails_only_that_game(backup, catalog, dav):
    two_games(catalog)
    dav.fail_next("PUT", 500, path_contains=G1[0])
    r = backup()
    assert r.ui[-1]["prompt"] == summary(1, 0, 1)
    assert zips(dav, "saves", F1) == []
    assert zips(dav, "saves", F2) == [f"{STAMP} Serhii.zip"]


def test_failed_game_retried_next_run(backup, catalog, dav):
    two_games(catalog)
    dav.fail_next("PUT", 500, path_contains=G1[0])
    backup()
    r = backup(date="Fri, 13 Nov 2026 09:00:00 GMT")
    assert r.ui[-1]["prompt"] == summary(1, 1, 0)
    assert zips(dav, "saves", F1) == ["20261113-090000 Serhii.zip"]


def test_dropped_move_retried_next_run(backup, catalog, dav):
    two_games(catalog)
    dav.drop_next("MOVE", path_contains=G1[0])
    assert backup().ui[-1]["prompt"] == summary(1, 0, 1)
    assert len(incoming(dav)) == 1
    assert zips(dav, "saves", F1) == []
    r = backup(date="Fri, 13 Nov 2026 09:00:00 GMT")
    assert r.ui[-1]["prompt"] == summary(1, 1, 0)
    assert incoming(dav) == []
    assert zips(dav, "saves", F1) == ["20261113-090000 Serhii.zip"]


def test_unopenable_save_counts_failed(backup, catalog, dav):
    catalog.add(G1[0], G1[1], "account", has_dir=False)
    catalog.add(G2[0], G2[1], "account", {"/save.bin": b"two"})
    r = backup()
    assert r.ui[-1]["prompt"] == summary(1, 0, 1)
    assert "cannot open the save" in r.output
    assert zips(dav, "saves", F2) == [f"{STAMP} Serhii.zip"]
