#!/usr/bin/env python3
"""Tiny local web server for the Social Network Analysis UI.

It runs your real C programs (social_network_seq / social_network_parallel)
and shows the SAME numbers the terminal prints. Nothing is re-timed here.

Usage:   python3 ui_server.py        then open http://localhost:8000
Needs:   Python 3 + gcc (no extra packages).
"""
import json, os, re, subprocess, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(ROOT, "data")
PORT = int(os.environ.get("PORT", 8000))
PROGRAMS = {"seq": "social_network_seq", "par": "social_network_parallel"}
RUN_LOCK = threading.Lock()   # only one benchmark at a time, so runs never compete for the CPU
HEADER = os.path.join(ROOT, "social_common.h")


def ensure_built(mode):
    """Compile with the same command you use in the terminal if needed."""
    exe = os.path.join(ROOT, PROGRAMS[mode])
    src = exe + ".c"
    newest = max(os.path.getmtime(src), os.path.getmtime(HEADER))
    if os.path.exists(exe) and os.path.getmtime(exe) >= newest:
        return exe
    r = subprocess.run(["gcc", "-O2", "-fopenmp", src, "-o", exe],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("Compile failed:\n" + r.stderr)
    return exe


def list_datasets():
    out = []
    for base, _, files in os.walk(DATA):
        for f in sorted(files):
            if f.endswith(".txt"):
                p = os.path.join(base, f)
                out.append({"path": os.path.relpath(p, ROOT).replace(os.sep, "/"),
                            "kb": round(os.path.getsize(p) / 1024)})
    out.sort(key=lambda d: (d["path"].count("/"), os.path.getsize(os.path.join(ROOT, d["path"]))))
    return out


def run_program(mode, dataset, threads):
    if mode not in PROGRAMS:
        raise ValueError("Unknown version")
    full = os.path.realpath(os.path.join(ROOT, dataset))
    if not full.startswith(os.path.realpath(DATA) + os.sep) or not os.path.isfile(full):
        raise ValueError("Dataset must be a file inside the data/ folder")
    exe = ensure_built(mode)
    env = dict(os.environ)
    cmd_prefix = ""
    if mode == "par":
        threads = max(2, min(int(threads), os.cpu_count() or 2))
        env["OMP_NUM_THREADS"] = str(threads)
        env["OMP_PROC_BIND"] = "true"     # keep threads on their cores -> steadier timings
        cmd_prefix = f"OMP_NUM_THREADS={threads} "
    with RUN_LOCK:
        r = subprocess.run([exe, os.path.relpath(full, ROOT)], cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=300)
    raw = r.stdout
    if r.returncode != 0 or "Execution time" not in raw:
        raise RuntimeError(raw.strip() or r.stderr.strip() or "Program failed")

    def grab(pattern, cast=str):
        m = re.search(pattern, raw, re.M)
        return cast(m.group(1)) if m else None

    top = [{"rank": int(a), "user": int(b), "connections": int(c)} for a, b, c in
           re.findall(r"Rank (\d+) -> User (\d+) : (\d+) connections", raw)]
    return {
        "mode": mode,
        "command": f"{cmd_prefix}./{PROGRAMS[mode]} {os.path.relpath(full, ROOT)}",
        "top": top,
        "components": grab(r"Connected components\s*:\s*(\d+)", int),
        "largest": grab(r"Largest component\s*:\s*(\d+)", int),
        "users": grab(r"^Users\s*:\s*(\d+)", int),
        "relationships": grab(r"^Relationships\s*:\s*(\d+)", int),
        "threads": grab(r"OpenMP threads\s*:\s*(\d+)", int) if mode == "par" else None,
        "triangles": grab(r"Triangles\s*:\s*(\d+)", int),
        "clustering": grab(r"Clustering coeff\.\s*:\s*([\d.]+)"),
        "avgpath": grab(r"Avg shortest path\s*:\s*([\d.]+)"),
        "diameter": grab(r"Estimated diameter\s*:\s*(\d+)", int),
        "seconds": grab(r"Execution time\s*:\s*([\d.]+) seconds"),  # exact text printed by the C program
        "raw": raw,
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            with open(os.path.join(ROOT, "ui", "index.html"), "rb") as f:
                return self.send(200, f.read(), "text/html; charset=utf-8")
        if path == "/api/info":
            return self.send(200, {"cores": os.cpu_count(), "datasets": list_datasets()})
        self.send(404, {"error": "Not found"})

    def do_POST(self):
        if urlparse(self.path).path != "/api/run":
            return self.send(404, {"error": "Not found"})
        try:
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
            self.send(200, run_program(req.get("mode"), req.get("dataset", ""), req.get("threads", 1)))
        except Exception as e:
            self.send(400, {"error": str(e)})


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"Social Network Analysis UI running at http://localhost:{PORT}  (Ctrl+C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
