"""Uploads: PUT into .incoming, then MOVE with Overwrite: F under a name that is free."""

import re

from conftest import APP_ROOT

SETUP = (
    "void setup(void)\n{\n    init_paths();\n"
    '    strcpy(g_user, "me@example.com");\n    strcpy(g_pw, "app-pass");\n    build_auth();\n}\n'
)
GAME = "0x0100ABCD00000000 Some Game"
ZIP = "sdmc:/switch/Checkpoint/savesync/up.zip"
DATE = "Thu, 12 Nov 2026 18:45:01 GMT"
STAMP = "20261112-184501"


def run(run_unit, dav, body):
    return run_unit("setup();\n    char fin[512];\n    fin[0] = 0;\n" + body, prelude=SETUP, koofr_url=dav.url)


def upload_call(game=GAME, base=f"{STAMP} Serhii"):
    return (f'    printf("prep=%d\\n", cloud_prepare());\n'
            f'    FILE* z = fopen("{ZIP}", "wb");\n    fputs("zipbytes", z);\n    fclose(z);\n'
            f'    printf("up=%d\\n", upload_version("{ZIP}", "saves", "{game}", "{base}", "0100ABCD00000000-a-{STAMP}.zip", fin));\n'
            '    printf("final=%s\\n", fin);\n')


def values(output, key):
    return re.findall(rf"^{key}=(.*)$", output, re.M)


def incoming(dav):
    return sorted(p.name for p in (dav.root / "SaveSyncNx" / ".incoming").iterdir())


def test_stamp_from_date_parses_and_refuses(run_unit):
    r = run_unit('char s[20];\n'
                 f'    printf("a=%d %s\\n", stamp_from_date("{DATE}", s), s);\n'
                 '    printf("b=%d\\n", stamp_from_date("Mon, 01 Jan 2024 00:00:09 GMT", s));\n    printf("bs=%s\\n", s);\n'
                 '    printf("c=%d\\n", stamp_from_date("yesterday", s));\n'
                 '    printf("d=%d\\n", stamp_from_date("Thu, 12 Foo 2026 18:45:01 GMT", s));\n'
                 '    printf("e=%d\\n", stamp_from_date("", s));')
    assert r.exit == 0, r.output
    assert f"a=1 {STAMP}" in r.output
    assert values(r.output, "b") == ["1"]
    assert values(r.output, "bs") == ["20240101-000009"]
    assert values(r.output, "c") == ["0"]
    assert values(r.output, "d") == ["0"]
    assert values(r.output, "e") == ["0"]


def test_stamp_variants(run_unit, dav):
    dav.fixed_date = DATE
    r = run(run_unit, dav, 'printf("prep=%d stamp=%s\\n", cloud_prepare(), g_stamp);')
    assert f"prep=1 stamp={STAMP}" in r.output, r.output
    dav.lowercase_date = True
    dav.fixed_date = "Fri, 13 Nov 2026 01:02:03 GMT"
    r = run(run_unit, dav, 'printf("prep=%d stamp=%s\\n", cloud_prepare(), g_stamp);')
    assert "prep=1 stamp=20261113-010203" in r.output, r.output
    dav.no_date = True
    r = run(run_unit, dav, 'printf("prep=%d stamp=%s\\n", cloud_prepare(), g_stamp);')
    assert re.search(r"^prep=1 stamp=\d{8}-\d{6}$", r.output, re.M), r.output
    assert "console clock" in r.output
    for sub in ["", "/saves", "/device", "/.incoming"]:
        assert (dav.root / ("SaveSyncNx" + sub)).is_dir()


def test_upload_creates_version(run_unit, dav):
    dav.fixed_date = DATE
    r = run(run_unit, dav, upload_call())
    assert r.exit == 0, r.output
    assert values(r.output, "up") == ["1"]
    assert values(r.output, "final") == [f"{STAMP} Serhii.zip"]
    assert (dav.root / "SaveSyncNx" / "saves" / GAME / f"{STAMP} Serhii.zip").read_bytes() == b"zipbytes"
    assert incoming(dav) == []
    puts = [e for e in dav.log if e[0] == "PUT"]
    assert [e[1] for e in puts] == [f"/dav/Koofr/SaveSyncNx/.incoming/0100ABCD00000000-a-{STAMP}.zip"]
    moves = [e for e in dav.log if e[0] == "MOVE"]
    assert moves and all(e[2] == 201 for e in moves)


def test_upload_never_overwrites(run_unit, dav):
    dav.fixed_date = DATE
    folder = dav.root / "SaveSyncNx" / "saves" / GAME
    folder.mkdir(parents=True)
    (folder / f"{STAMP} Serhii.zip").write_bytes(b"old")
    (folder / f"{STAMP} Serhii (2).zip").write_bytes(b"older")
    r = run(run_unit, dav, upload_call())
    assert r.exit == 0, r.output
    assert values(r.output, "up") == ["1"]
    assert values(r.output, "final") == [f"{STAMP} Serhii (3).zip"]
    assert (folder / f"{STAMP} Serhii.zip").read_bytes() == b"old"
    assert (folder / f"{STAMP} Serhii (2).zip").read_bytes() == b"older"
    assert (folder / f"{STAMP} Serhii (3).zip").read_bytes() == b"zipbytes"
    assert [e[2] for e in dav.log if e[0] == "MOVE"] == [412, 412, 201]


def test_dropped_put_leaves_only_incoming(run_unit, dav):
    dav.fixed_date = DATE
    dav.drop_next("PUT")
    r = run(run_unit, dav, upload_call())
    assert r.exit == 0, r.output
    assert values(r.output, "up") == ["0"]
    assert not (dav.root / "SaveSyncNx" / "saves" / GAME).exists()
    # what a PUT cut off half way leaves behind on a real server
    (dav.root / "SaveSyncNx" / ".incoming" / f"0100ABCD00000000-a-{STAMP}.zip").write_bytes(b"zipb")
    r = run(run_unit, dav, 'cloud_prepare();\n    cloud_cleanup_incoming();')
    assert r.exit == 0, r.output
    assert incoming(dav) == []


def test_dropped_move_leaves_only_incoming(run_unit, dav):
    dav.fixed_date = DATE
    dav.drop_next("MOVE")
    r = run(run_unit, dav, upload_call())
    assert r.exit == 0, r.output
    assert values(r.output, "up") == ["0"]
    assert incoming(dav) == [f"0100ABCD00000000-a-{STAMP}.zip"]
    assert list((dav.root / "SaveSyncNx" / "saves" / GAME).iterdir()) == []
    r = run(run_unit, dav, 'cloud_prepare();\n    cloud_cleanup_incoming();')
    assert r.exit == 0, r.output
    assert incoming(dav) == []


def test_nonascii_game_folder_roundtrip(run_unit, dav):
    dav.fixed_date = DATE
    game = "0x0100ABCD00000000 Pokémon™ Let's Go  #1 & 50%"
    r = run(run_unit, dav, upload_call(game=game) +
            '    struct names n;\n    names_init(&n);\n    printf("list=%d\\n", dav_list(g_url_saves, &n));\n'
            '    printf("name=%s\\n", n.items[0]);\n    names_free(&n);')
    assert r.exit == 0, r.output
    assert values(r.output, "up") == ["1"]
    assert (dav.root / "SaveSyncNx" / "saves" / game / f"{STAMP} Serhii.zip").read_bytes() == b"zipbytes"
    assert values(r.output, "name") == [game]


def test_only_incoming_is_deleted(run_unit, dav):
    root = dav.root / "SaveSyncNx"
    (root / ".incoming").mkdir(parents=True)
    (root / ".incoming" / "a.zip").write_bytes(b"1")
    (root / ".incoming" / "b c.zip").write_bytes(b"2")
    (root / "saves" / GAME).mkdir(parents=True)
    (root / "saves" / GAME / "keep.zip").write_bytes(b"3")
    r = run(run_unit, dav, 'cloud_prepare();\n    cloud_cleanup_incoming();')
    assert r.exit == 0, r.output
    assert incoming(dav) == []
    assert (root / "saves" / GAME / "keep.zip").read_bytes() == b"3"
    deletes = [e for e in dav.log if e[0] == "DELETE"]
    assert len(deletes) == 2
    assert all(e[1].startswith("/dav/Koofr/SaveSyncNx/.incoming/") for e in deletes)
