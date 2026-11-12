"""WebDAV calls in savesync.c against fakedav."""

import re
import socket

from conftest import APP_ROOT

SETUP = (
    "void setup(char* pw)\n{\n    init_paths();\n"
    '    strcpy(g_user, "me@example.com");\n    strcpy(g_pw, pw);\n    build_auth();\n}\n'
    "void at(char* dst, char* segment)\n{\n    strcpy(dst, g_base);\n    url_add(dst, segment, 1536);\n}\n"
)


def run_dav(run_unit, dav, body, pw="app-pass"):
    return run_unit(f'setup("{pw}");\n    char u[1536];\n    char v[1536];\n' + body, prelude=SETUP, koofr_url=dav.url)


def values(output, key):
    return re.findall(rf"^{key}=(.*)$", output, re.M)


def test_mkcol_twice_ok(run_unit, dav):
    r = run_dav(run_unit, dav, 'at(u, "a");\n    printf("mk=%d\\n", dav_mkcol(u));\n    printf("mk=%d\\n", dav_mkcol(u));')
    assert r.exit == 0, r.output
    assert values(r.output, "mk") == ["1", "1"]
    assert (dav.root / "a").is_dir()
    assert [entry[2] for entry in dav.log if entry[0] == "MKCOL"] == [201, 405]


def test_put_get_roundtrip_bytes(run_unit, dav, sd):
    payload = bytes(range(256)) * 1000
    (sd / APP_ROOT / "payload.bin").write_bytes(payload)
    r = run_dav(run_unit, dav,
                'at(u, "p.bin");\n'
                '    printf("put=%d\\n", dav_put_file("sdmc:/switch/Checkpoint/payload.bin", u));\n'
                '    printf("get=%d\\n", dav_get_file(u, "sdmc:/switch/Checkpoint/back.bin"));')
    assert r.exit == 0, r.output
    assert values(r.output, "put") == ["201"]
    assert values(r.output, "get") == ["1"]
    assert (dav.root / "p.bin").read_bytes() == payload
    assert (sd / APP_ROOT / "back.bin").read_bytes() == payload


def test_get_missing_returns_0(run_unit, dav, sd):
    r = run_dav(run_unit, dav, 'at(u, "none.bin");\n    printf("get=%d\\n", dav_get_file(u, "sdmc:/switch/Checkpoint/back.bin"));')
    assert r.exit == 0, r.output
    assert values(r.output, "get") == ["0"]
    assert not (sd / APP_ROOT / "back.bin").exists()


def test_list_returns_decoded_leaf_names(run_unit, dav):
    d = dav.root / "d"
    (d / "sub dir").mkdir(parents=True)
    (d / "x é.zip").write_bytes(b"1")
    (d / "plain").write_bytes(b"2")
    r = run_dav(run_unit, dav,
                'struct names n;\n    n.count = 0;\n    n.cap = 0;\n    n.items = NULL;\n'
                '    at(u, "d");\n    printf("ok=%d\\n", dav_list(u, &n));\n'
                '    int i = 0;\n    while (i < n.count) {\n        printf("name=%s\\n", n.items[i]);\n        i = i + 1;\n    }\n'
                '    names_free(&n);')
    assert r.exit == 0, r.output
    assert values(r.output, "ok") == ["1"]
    assert sorted(values(r.output, "name")) == ["plain", "sub dir", "x é.zip"]


def test_list_missing_returns_0(run_unit, dav):
    r = run_dav(run_unit, dav,
                'struct names n;\n    n.count = 0;\n    n.cap = 0;\n    n.items = NULL;\n'
                '    at(u, "none");\n    printf("ok=%d count=%d\\n", dav_list(u, &n), n.count);\n    names_free(&n);')
    assert r.exit == 0, r.output
    assert "ok=0 count=0" in r.output


def test_list_more_than_128(run_unit, dav):
    d = dav.root / "many"
    d.mkdir()
    for i in range(300):
        (d / f"f{i:03d}.zip").write_bytes(b"")
    r = run_dav(run_unit, dav,
                'struct names n;\n    n.count = 0;\n    n.cap = 0;\n    n.items = NULL;\n'
                '    at(u, "many");\n    printf("ok=%d count=%d\\n", dav_list(u, &n), n.count);\n'
                '    printf("last=%s\\n", n.items[n.count - 1]);\n    names_free(&n);')
    assert r.exit == 0, r.output
    assert "ok=1 count=300" in r.output


def test_move_then_412(run_unit, dav):
    (dav.root / "a.zip").write_bytes(b"A")
    (dav.root / "b.zip").write_bytes(b"B")
    r = run_dav(run_unit, dav,
                'at(u, "a.zip");\n    at(v, "c.zip");\n    printf("mv=%d\\n", dav_move(u, v));\n'
                '    at(u, "b.zip");\n    printf("mv=%d\\n", dav_move(u, v));')
    assert r.exit == 0, r.output
    assert values(r.output, "mv") == ["201", "412"]
    assert (dav.root / "c.zip").read_bytes() == b"A"
    assert (dav.root / "b.zip").read_bytes() == b"B"


def test_delete(run_unit, dav):
    (dav.root / "t").mkdir()
    (dav.root / "t" / "f").write_bytes(b"1")
    r = run_dav(run_unit, dav, 'at(u, "t");\n    printf("del=%d\\n", dav_delete(u));\n    printf("del=%d\\n", dav_delete(u));')
    assert r.exit == 0, r.output
    assert values(r.output, "del") == ["204", "404"]
    assert not (dav.root / "t").exists()


def test_401_sets_fatal_login(run_unit, dav):
    r = run_dav(run_unit, dav, 'at(u, "a");\n    dav_mkcol(u);\n    printf("login=%d\\n", g_fatal == FATAL_LOGIN);', pw="wrong")
    assert r.exit == 0, r.output
    assert values(r.output, "login") == ["1"]


def test_507_sets_fatal_full(run_unit, dav, sd):
    (sd / APP_ROOT / "p.bin").write_bytes(b"x")
    dav.quota_full = True
    r = run_dav(run_unit, dav,
                'at(u, "p.bin");\n    printf("put=%d\\n", dav_put_file("sdmc:/switch/Checkpoint/p.bin", u));\n'
                '    printf("full=%d\\n", g_fatal == FATAL_FULL);')
    assert r.exit == 0, r.output
    assert values(r.output, "put") == ["507"]
    assert values(r.output, "full") == ["1"]


def test_server_down_sets_fatal_net(run_unit):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    r = run_unit('setup("app-pass");\n    char u[1536];\n    at(u, "a");\n'
                 '    printf("st=%d\\n", dav_mkcol(u));\n    printf("net=%d\\n", g_fatal == FATAL_NET);',
                 prelude=SETUP, koofr_url=f"http://127.0.0.1:{port}/dav/Koofr")
    assert r.exit == 0, r.output
    assert values(r.output, "net") == ["1"]


def test_date_captured_any_case(run_unit, dav):
    dav.fixed_date = "Thu, 12 Nov 2026 18:45:01 GMT"
    r1 = run_dav(run_unit, dav, 'at(u, "a");\n    dav_mkcol(u);\n    printf("date=%s\\n", g_date);')
    dav.fixed_date = "Fri, 13 Nov 2026 01:02:03 GMT"
    dav.lowercase_date = True
    r2 = run_dav(run_unit, dav, 'at(u, "a");\n    dav_mkcol(u);\n    printf("date=%s\\n", g_date);')
    dav.no_date = True
    r3 = run_dav(run_unit, dav, 'at(u, "a");\n    dav_mkcol(u);\n    printf("date=[%s]\\n", g_date);')
    assert values(r1.output, "date") == ["Thu, 12 Nov 2026 18:45:01 GMT"], r1.output
    assert values(r2.output, "date") == ["Fri, 13 Nov 2026 01:02:03 GMT"], r2.output
    assert values(r3.output, "date") == ["[]"], r3.output
