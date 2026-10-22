"""LAN command channel for driving the C434T from macOS Recovery, where there is
no SSH. Start it with:  python3 server.py <this machine's LAN IP> 8000
The recovery Terminal then runs:  curl -s http://<IP>:8000/r | sh

GET /r        -> polling client loop (r.sh)
GET /c/<n>    -> queued command cmds/<n>.sh, 404 until it exists
GET /f/<name> -> a file from files/ (e.g. a new config.plist)
PUT /o/<n>    -> output of command n, stored as up/o<n>.txt
PUT /<name>   -> generic upload into up/
"""
import http.server, os, re, sys

BASE = os.path.dirname(os.path.abspath(__file__))
DIRS = {d: os.path.join(BASE, d) for d in ("up", "cmds", "files")}
for d in DIRS.values():
    os.makedirs(d, exist_ok=True)
NAME = r"[A-Za-z0-9_.-]{1,64}"


class H(http.server.BaseHTTPRequestHandler):
    def send_file(self, path):
        body = open(path, "rb").read()
        if path.endswith("r.sh"):
            body = body.replace(b"@URL@", f"http://{sys.argv[1]}:{sys.argv[2]}".encode())
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/r":
            return self.send_file(os.path.join(BASE, "r.sh"))
        m = re.fullmatch(r"/c/(\d{1,6})", self.path)
        if m and os.path.exists(os.path.join(DIRS["cmds"], m[1] + ".sh")):
            return self.send_file(os.path.join(DIRS["cmds"], m[1] + ".sh"))
        m = re.fullmatch(r"/f/(" + NAME + ")", self.path)
        if m and os.path.exists(os.path.join(DIRS["files"], m[1])):
            return self.send_file(os.path.join(DIRS["files"], m[1]))
        self.send_error(404)

    def do_PUT(self):
        m = re.fullmatch(r"/o/(\d{1,6})", self.path)
        name = f"o{m[1]}.txt" if m else self.path.lstrip("/")
        if not re.fullmatch(NAME, name):
            self.send_error(400)
            return
        n = int(self.headers.get("Content-Length", 0))
        with open(os.path.join(DIRS["up"], name), "wb") as f:
            while n > 0:
                chunk = self.rfile.read(min(n, 1 << 20))
                if not chunk:
                    break
                f.write(chunk)
                n -= len(chunk)
        self.send_response(201)
        self.end_headers()

    def log_message(self, fmt, *args):
        if not self.path.startswith("/c/") or "200" in (fmt % args):
            super().log_message(fmt, *args)


http.server.ThreadingHTTPServer((sys.argv[1], int(sys.argv[2])), H).serve_forever()
