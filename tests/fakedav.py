"""A Koofr stand-in: the WebDAV subset savesync.c uses, with Koofr's answers.

The behaviour comes from the 3DSaveSync probe against the real service
(tests/fixtures/koofr-propfind.xml is one of its answers, byte for byte):
Basic auth, MKCOL 201/405/409, PUT 201, GET 200/404, PROPFIND 207/404 with
D:-prefixed, percent-encoded /dav/Koofr/... hrefs (folders without a trailing
slash), MOVE with Overwrite: F 201/412/404, recursive DELETE 204/404.

GET of any path outside /dav/ serves `static` (the GitHub fakes).
"""

import base64
import email.utils
import shutil
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit

PREFIX = "/dav/Koofr"


class FakeDav:
    def __init__(self, root=None, user="me@example.com", password="app-pass"):
        self.root = Path(root) if root is not None else Path(tempfile.mkdtemp(prefix="fakedav-"))
        self.root.mkdir(parents=True, exist_ok=True)
        self.user = user
        self.password = password
        self.fixed_date = None
        self.lowercase_date = False
        self.no_date = False
        self.quota_full = False
        self.static = {}
        self.log = []
        self._faults = []
        self._lock = threading.Lock()
        self._server = ThreadingHTTPServer(("127.0.0.1", 0), _handler_for(self))
        self._server.daemon_threads = True
        port = self._server.server_address[1]
        self.origin = f"http://127.0.0.1:{port}"
        self.url = self.origin + PREFIX
        # A short poll keeps stop() (and so every test's teardown) fast.
        self._thread = threading.Thread(target=self._server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self._thread.start()

    def fail_next(self, method, status, path_contains=None):
        """Answer the next matching request with `status` and an empty body."""
        with self._lock:
            self._faults.append((method, path_contains, status))

    def drop_next(self, method, path_contains=None):
        """Read the next matching request, then close the socket unanswered."""
        with self._lock:
            self._faults.append((method, path_contains, None))

    def stop(self):
        self._server.shutdown()
        self._server.server_close()

    def _take_fault(self, method, path):
        with self._lock:
            for i, (m, contains, status) in enumerate(self._faults):
                if m == method and (contains is None or contains in path):
                    del self._faults[i]
                    return True, status
        return False, None

    def _record(self, method, path, status):
        with self._lock:
            self.log.append((method, path, status))


def _href(rel):
    return PREFIX + quote("/" + rel if rel else "", safe="/")


def _propfind_entry(rel, target):
    if target.is_dir():
        return (
            f"<D:response><D:href>{_href(rel)}</D:href><D:propstat><D:prop><D:resourcetype>"
            '<D:collection xmlns:D="DAV:"/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status>'
            "</D:propstat><D:propstat><D:prop><D:getcontentlength></D:getcontentlength></D:prop>"
            "<D:status>HTTP/1.1 404 Not Found</D:status></D:propstat></D:response>"
        )
    return (
        f"<D:response><D:href>{_href(rel)}</D:href><D:propstat><D:prop><D:resourcetype></D:resourcetype>"
        f"<D:getcontentlength>{target.stat().st_size}</D:getcontentlength></D:prop>"
        "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
    )


def _handler_for(dav):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        # ---- plumbing ----------------------------------------------------

        def _read_body(self):
            if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                body = b""
                while True:
                    size = int(self.rfile.readline().strip().split(b";")[0], 16)
                    if size == 0:
                        self.rfile.readline()
                        return body
                    body += self.rfile.read(size)
                    self.rfile.readline()
            length = int(self.headers.get("Content-Length", 0) or 0)
            return self.rfile.read(length) if length else b""

        def _answer(self, status, body=b"", headers=None):
            dav._record(self.command, self._path, status)
            self.send_response_only(status)
            if not dav.no_date:
                date = dav.fixed_date or email.utils.formatdate(usegmt=True)
                self.send_header("date" if dav.lowercase_date else "Date", date)
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body and self.command != "HEAD":
                self.wfile.write(body)

        def _authorized(self):
            expected = base64.b64encode(f"{dav.user}:{dav.password}".encode()).decode()
            return self.headers.get("Authorization", "") == "Basic " + expected

        def _local(self, url_path):
            """The storage path for a /dav/Koofr URL path, or None outside it."""
            if url_path != PREFIX and not url_path.startswith(PREFIX + "/"):
                return None
            rel = url_path[len(PREFIX):].strip("/")
            target = dav.root.joinpath(*rel.split("/")) if rel else dav.root
            return rel, target

        def _dispatch(self):
            self._path = unquote(urlsplit(self.path).path)
            body = self._read_body()
            matched, status = dav._take_fault(self.command, self._path)
            if matched:
                if status is None:
                    dav._record(self.command, self._path, 0)
                    self.close_connection = True
                    return
                self._answer(status)
                return
            local = self._local(self._path)
            if local is None:
                if self.command == "GET" and self._path in dav.static:
                    status, data = dav.static[self._path]
                    self._answer(status, data)
                else:
                    self._answer(404)
                return
            if not self._authorized():
                self._answer(401, headers={"WWW-Authenticate": 'Basic realm="Koofr"'})
                return
            rel, target = local
            getattr(self, "_dav_" + self.command.lower())(rel, target, body)

        do_GET = do_PUT = do_MKCOL = do_PROPFIND = do_MOVE = do_DELETE = _dispatch

        # ---- WebDAV ------------------------------------------------------

        def _dav_mkcol(self, rel, target, body):
            if target.exists():
                self._answer(405)
            elif not target.parent.is_dir():
                self._answer(409)
            else:
                target.mkdir()
                self._answer(201)

        def _dav_put(self, rel, target, body):
            if dav.quota_full:
                self._answer(507)
            elif not target.parent.is_dir() or target.is_dir():
                self._answer(409)
            else:
                target.write_bytes(body)
                self._answer(201)

        def _dav_get(self, rel, target, body):
            if target.is_file():
                self._answer(200, target.read_bytes())
            else:
                self._answer(404)

        def _dav_propfind(self, rel, target, body):
            if not target.exists():
                self._answer(404)
                return
            entries = [_propfind_entry(rel, target)]
            if self.headers.get("Depth", "1") != "0" and target.is_dir():
                for child in sorted(target.iterdir(), key=lambda p: p.name):
                    entries.append(_propfind_entry((rel + "/" if rel else "") + child.name, child))
            xml = '<?xml version="1.0" encoding="UTF-8"?><D:multistatus xmlns:D="DAV:">' + "".join(entries) + "</D:multistatus>"
            self._answer(207, xml.encode(), {"Content-Type": "text/xml; charset=utf-8"})

        def _dav_move(self, rel, target, body):
            dest = self._local(unquote(urlsplit(self.headers.get("Destination", "")).path))
            if not target.exists():
                self._answer(404)
            elif dest is None:
                self._answer(400)
            elif not dest[1].parent.is_dir():
                self._answer(409)
            elif dest[1].exists() and self.headers.get("Overwrite", "T").upper() == "F":
                self._answer(412)
            else:
                existed = dest[1].exists()
                if existed:
                    shutil.rmtree(dest[1]) if dest[1].is_dir() else dest[1].unlink()
                target.rename(dest[1])
                self._answer(204 if existed else 201)

        def _dav_delete(self, rel, target, body):
            if not target.exists():
                self._answer(404)
            else:
                shutil.rmtree(target) if target.is_dir() else target.unlink()
                self._answer(204)

    return Handler
