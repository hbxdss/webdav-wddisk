# testdav.py - 最小 WebDAV 测试服务器（Python 标准库）
# 支持本测试所需的方法：OPTIONS PROPFIND GET PUT MKCOL DELETE MOVE
import http.server
import os
import sys
import urllib.parse
import shutil

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["TEMP"], "wddisk-test-dav")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

os.makedirs(ROOT, exist_ok=True)


class DavHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[dav]", self.command, self.path, *args, flush=True)

    def log_error(self, fmt, *args):
        print("[dav-ERROR]", self.requestline, fmt % args, flush=True)

    def _path(self):
        p = urllib.parse.unquote(self.path.split("?")[0])
        p = p.lstrip("/")
        return os.path.normpath(os.path.join(ROOT, p))

    def _send(self, code, body=b"", ctype="text/plain"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _read_body(self):
        self.headers and print("[hdr] Authorization:", self.headers.get("Authorization", "(none)"), flush=True)
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n > 0:
            self.rfile.read(n)

    def do_OPTIONS(self):
        self._send(200, b"", "text/plain")

    def do_PROPFIND(self):
        self._read_body()
        p = self._path()
        depth = self.headers.get("Depth", "0")
        if not os.path.exists(p):
            self._send(404, b"not found")
            return

        def entry_xml(path, href):
            isdir = os.path.isdir(path)
            name = os.path.basename(path) or "root"
            size = 0 if isdir else os.path.getsize(path)
            mtime = http.server.email.utils.formatdate(os.path.getmtime(path), usegmt=True)
            coll = "<d:collection/>" if isdir else ""
            return (
                f"<d:response><d:href>{urllib.parse.quote(href)}</d:href>"
                f"<d:propstat><d:prop>"
                f"<d:displayname>{name}</d:displayname>"
                f"<d:resourcetype>{coll}</d:resourcetype>"
                + ("" if isdir else f"<d:getcontentlength>{size}</d:getcontentlength>")
                + f"<d:getlastmodified>{mtime}</d:getlastmodified>"
                f"</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            )

        body = '<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">'
        body += entry_xml(p, self.path if self.path.endswith("/") else self.path)
        if depth == "1" and os.path.isdir(p):
            for name in os.listdir(p):
                child = os.path.join(p, name)
                href = (self.path.rstrip("/") if self.path != "/" else "") + "/" + name
                if os.path.isdir(child):
                    href += "/"
                body += entry_xml(child, href)
        body += "</d:multistatus>"
        self._send(207, body.encode("utf-8"), "application/xml; charset=utf-8")

    def do_GET(self):
        p = self._path()
        if not os.path.isfile(p):
            self._send(404, b"not found")
            return
        with open(p, "rb") as f:
            data = f.read()
        rng = self.headers.get("Range", "")
        if rng.startswith("bytes="):
            try:
                a, b = rng[6:].split("-", 1)
                start = int(a)
                end = int(b) if b else len(data) - 1
                chunk = data[start:end + 1]
                self.send_response(206)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                self.send_header("Content-Length", str(len(chunk)))
                self.end_headers()
                if chunk:
                    self.wfile.write(chunk)
                return
            except Exception:
                pass
        self._send(200, data, "application/octet-stream")

    def do_PUT(self):
        p = self._path()
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        self._send(201, b"created")

    def do_MKCOL(self):
        p = self._path()
        try:
            os.makedirs(p, exist_ok=True)
            self._send(201, b"created")
        except Exception as e:
            self._send(500, str(e).encode())

    def do_DELETE(self):
        p = self._path()
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
        elif os.path.isfile(p):
            os.remove(p)
        else:
            self._send(404, b"not found")
            return
        self._send(204, b"")

    @staticmethod
    def _dest_path(dest_url):
        dest = urllib.parse.unquote(dest_url.split("?")[0])
        # Destination 可能是完整 URL，剥离 scheme://host
        for prefix in ("http://", "https://"):
            i = dest.find(prefix)
            if i >= 0:
                rest = dest[i + len(prefix):]
                slash = rest.find("/")
                dest = rest[slash:] if slash >= 0 else "/"
                break
        return os.path.normpath(os.path.join(ROOT, dest.lstrip("/").rstrip("/")))

    def do_MOVE(self):
        src = self._path()
        dest = self._dest_path(self.headers.get("Destination", ""))
        if not os.path.exists(src):
            self._send(404, b"not found")
            return
        try:
            if os.path.isdir(src):
                shutil.move(src, dest)
            else:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.move(src, dest)
            self._send(201, b"moved")
        except Exception as e:
            import traceback
            traceback.print_exc()
            self._send(500, str(e).encode())

    def do_COPY(self):
        src = self._path()
        dest = self._dest_path(self.headers.get("Destination", ""))
        if not os.path.exists(src):
            self._send(404, b"not found")
            return
        try:
            if os.path.isdir(src):
                shutil.copytree(src, dest, dirs_exist_ok=True)
            else:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copy2(src, dest)
            self._send(201, b"copied")
        except Exception as e:
            self._send(500, str(e).encode())


if __name__ == "__main__":
    print(f"WebDAV test server on http://127.0.0.1:{PORT}/ root={ROOT}", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), DavHandler).serve_forever()
