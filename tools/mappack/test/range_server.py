"""A tiny web server that answers HTTP range requests (like build.protomaps.com does), for testing."""
import http.server, os, re, sys

ROOT = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
LOG = []

class H(http.server.BaseHTTPRequestHandler):
    def _file(self):
        p = os.path.join(ROOT, os.path.basename(self.path.split("?")[0]))
        return p if os.path.isfile(p) else None
    def do_HEAD(self):
        p = self._file()
        self.send_response(200 if p else 404)
        if p:
            self.send_header("Content-Length", str(os.path.getsize(p)))
            self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
    def do_GET(self):
        p = self._file()
        m = re.match(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
        if not p or not m:
            self.send_response(404 if not p else 416)
            self.end_headers()
            return
        size = os.path.getsize(p)
        a, b = int(m.group(1)), min(int(m.group(2)), size - 1)
        with open(p, "rb") as f:
            f.seek(a)
            data = f.read(b - a + 1)
        self.send_response(206)
        self.send_header("Content-Range", f"bytes {a}-{b}/{size}")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        with open(os.path.join(ROOT, "requests.log"), "a") as lf:
            lf.write(f"{a}-{b}\n")
    def log_message(self, *a):
        pass

http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
