"""fakedav behaves like Koofr's WebDAV endpoint, as probed from the 3DS project."""

import base64
import http.client
from pathlib import Path
from urllib.parse import urlsplit

FIXTURE = Path(__file__).parent / "fixtures" / "koofr-propfind.xml"


def request(dav, method, path, body=None, headers=None, auth=True, password=None):
    parts = urlsplit(dav.url)
    conn = http.client.HTTPConnection(parts.hostname, parts.port, timeout=10)
    hdrs = dict(headers or {})
    if auth:
        token = base64.b64encode(f"me@example.com:{password or 'app-pass'}".encode()).decode()
        hdrs["Authorization"] = "Basic " + token
    conn.request(method, path, body=body, headers=hdrs)
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp, data


def test_wrong_login_401(dav):
    resp, _ = request(dav, "PROPFIND", "/dav/Koofr", headers={"Depth": "0"}, password="wrong")
    assert resp.status == 401
    resp, _ = request(dav, "PROPFIND", "/dav/Koofr", headers={"Depth": "0"}, auth=False)
    assert resp.status == 401


def test_mkcol_201_then_405(dav):
    assert request(dav, "MKCOL", "/dav/Koofr/a")[0].status == 201
    assert request(dav, "MKCOL", "/dav/Koofr/a")[0].status == 405
    assert (dav.root / "a").is_dir()


def test_mkcol_missing_parent_409(dav):
    assert request(dav, "MKCOL", "/dav/Koofr/missing/child")[0].status == 409


def test_put_then_get_same_bytes(dav):
    payload = bytes(range(256)) * 10
    assert request(dav, "PUT", "/dav/Koofr/f.bin", body=payload)[0].status == 201
    resp, data = request(dav, "GET", "/dav/Koofr/f.bin")
    assert resp.status == 200
    assert data == payload
    assert request(dav, "GET", "/dav/Koofr/nope.bin")[0].status == 404


def test_propfind_depth1_shape_matches_koofr_fixture(dav):
    folder = dav.root / "3DSaveSync-probe" / "src" / "sub dir"
    folder.mkdir(parents=True)
    (folder / "file é.txt").write_bytes(b"hello")
    resp, data = request(dav, "PROPFIND", "/dav/Koofr/3DSaveSync-probe/src/sub%20dir", headers={"Depth": "1"})
    assert resp.status == 207
    # Byte for byte the answer Koofr gave: D: prefix, /dav/Koofr/ hrefs,
    # percent-encoded é, and folder hrefs without a trailing slash.
    assert data == FIXTURE.read_bytes().rstrip(b"\n")


def test_propfind_depth0_lists_only_the_target(dav):
    (dav.root / "d").mkdir()
    (dav.root / "d" / "x").write_bytes(b"1")
    resp, data = request(dav, "PROPFIND", "/dav/Koofr/d", headers={"Depth": "0"})
    assert resp.status == 207
    assert data.count(b"<D:response>") == 1
    assert b"<D:href>/dav/Koofr/d</D:href>" in data


def test_propfind_missing_404(dav):
    assert request(dav, "PROPFIND", "/dav/Koofr/none", headers={"Depth": "1"})[0].status == 404


def test_move_201_then_412(dav):
    (dav.root / "src.zip").write_bytes(b"one")
    (dav.root / "other.zip").write_bytes(b"two")
    dest = dav.url + "/dst.zip"
    assert request(dav, "MOVE", "/dav/Koofr/src.zip", headers={"Destination": dest, "Overwrite": "F"})[0].status == 201
    assert (dav.root / "dst.zip").read_bytes() == b"one"
    assert not (dav.root / "src.zip").exists()
    resp, _ = request(dav, "MOVE", "/dav/Koofr/other.zip", headers={"Destination": dest, "Overwrite": "F"})
    assert resp.status == 412
    assert (dav.root / "dst.zip").read_bytes() == b"one"
    assert (dav.root / "other.zip").read_bytes() == b"two"
    resp, _ = request(dav, "MOVE", "/dav/Koofr/gone.zip", headers={"Destination": dav.url + "/x.zip", "Overwrite": "F"})
    assert resp.status == 404


def test_delete_recursive_204_then_404(dav):
    (dav.root / "t" / "u").mkdir(parents=True)
    (dav.root / "t" / "u" / "f").write_bytes(b"1")
    assert request(dav, "DELETE", "/dav/Koofr/t")[0].status == 204
    assert not (dav.root / "t").exists()
    assert request(dav, "DELETE", "/dav/Koofr/t")[0].status == 404


def test_date_header_variants(dav):
    dav.fixed_date = "Thu, 12 Nov 2026 18:45:01 GMT"
    resp, _ = request(dav, "PROPFIND", "/dav/Koofr", headers={"Depth": "0"})
    assert ("Date", "Thu, 12 Nov 2026 18:45:01 GMT") in resp.getheaders()

    dav.lowercase_date = True
    resp, _ = request(dav, "PROPFIND", "/dav/Koofr", headers={"Depth": "0"})
    assert ("date", "Thu, 12 Nov 2026 18:45:01 GMT") in resp.getheaders()

    dav.no_date = True
    resp, _ = request(dav, "PROPFIND", "/dav/Koofr", headers={"Depth": "0"})
    assert all(name.lower() != "date" for name, _ in resp.getheaders())


def test_fail_next_and_drop_next(dav):
    dav.fail_next("PUT", 500, path_contains="b.bin")
    assert request(dav, "PUT", "/dav/Koofr/a.bin", body=b"1")[0].status == 201
    assert request(dav, "PUT", "/dav/Koofr/b.bin", body=b"1")[0].status == 500
    assert request(dav, "PUT", "/dav/Koofr/b.bin", body=b"1")[0].status == 201

    dav.drop_next("PUT")
    try:
        request(dav, "PUT", "/dav/Koofr/c.bin", body=b"1")
        dropped = False
    except (http.client.RemoteDisconnected, ConnectionResetError):
        dropped = True
    assert dropped
    assert not (dav.root / "c.bin").exists()
    assert request(dav, "PUT", "/dav/Koofr/c.bin", body=b"1")[0].status == 201


def test_quota_full_507(dav):
    dav.quota_full = True
    assert request(dav, "PUT", "/dav/Koofr/q.bin", body=b"1")[0].status == 507
    assert not (dav.root / "q.bin").exists()


def test_log_and_static_routes(dav):
    dav.static["/repos/x/releases/latest"] = (200, b'{"tag_name": "v0.2.0"}')
    resp, data = request(dav, "GET", "/repos/x/releases/latest", auth=False)
    assert resp.status == 200
    assert data == b'{"tag_name": "v0.2.0"}'
    assert request(dav, "GET", "/repos/other", auth=False)[0].status == 404
    request(dav, "MKCOL", "/dav/Koofr/Some%20Game")
    assert ("GET", "/repos/x/releases/latest", 200) in dav.log
    assert ("MKCOL", "/dav/Koofr/Some Game", 201) in dav.log
