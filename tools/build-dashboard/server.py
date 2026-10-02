"""Read-only, loopback-only SnowDesktop build monitor. Python 3.8+, stdlib only."""
import argparse
import collections
import ctypes
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HEX = re.compile(r"^[a-f0-9]{32}$")
SECRET = re.compile(r"(?i)(password|passwd|token|secret|api[_-]?key|authorization)(\s*[:=]\s*)[^\s,;]+")
ASSETS = {"/app-icon.png": "app-icon.png", "/": "index.html", "/app.js": "app.js", "/style.css": "style.css"}
MIME = {".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".png": "image/png"}

def redact(text):
    return SECRET.sub(r"\1\2[redacted]", str(text))[:4000]

def safe_path(root, name):
    path = root / name
    if path.parent != root or path.is_symlink():
        raise ValueError("Unsafe metadata path")
    for part in (root, path):
        if part.exists() and getattr(part.stat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("Reparse points are not served")
    return path

def shared_open(path):
    # File.Replace must remain possible while the monitor reads. Python's
    # default Windows open omits FILE_SHARE_DELETE, so use an explicit handle.
    if os.name != "nt":
        return open(str(path), "rb")
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "Metadata unavailable")
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        kernel.CloseHandle(ctypes.c_void_p(handle))
        raise
    return os.fdopen(fd, "rb")

def read_json(root, name):
    try:
        with shared_open(safe_path(root, name)) as stream:
            data = stream.read(262145)
        if len(data) > 262144:
            raise ValueError("Metadata exceeds 256 KiB")
        return json.loads(data.decode("utf-8-sig"))
    except FileNotFoundError:
        return None

def owner_state(owner):
    if not owner:
        return "none"
    if os.name != "nt":
        return "unknown"
    try:
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        handle = kernel.OpenProcess(0x1000, False, int(owner["pid"]))
        if not handle:
            return "exited" if ctypes.get_last_error() in (87, 1168) else "unknown"
        values = [ctypes.c_ulonglong() for _ in range(4)]
        try:
            if not kernel.GetProcessTimes(ctypes.c_void_p(handle), *[ctypes.byref(v) for v in values]):
                return "unknown"
            ticks = values[0].value + 504911232000000000
            return "alive" if str(ticks) == str(owner["startTicks"]) else "exited"
        finally:
            kernel.CloseHandle(ctypes.c_void_p(handle))
    except (KeyError, ValueError, OSError):
        return "unknown"

def age(value):
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return max(0, int((dt.datetime.now(dt.timezone.utc) - parsed).total_seconds()))
    except (ValueError, TypeError):
        return None

def identity(value):
    if not isinstance(value, dict):
        return None
    return {k: value.get(k) for k in ("algorithm", "digest", "fileCount", "head")}

def task_view(entry):
    check = entry.get("check") or {}
    state = entry.get("state", "unknown")
    check_status = check.get("status", "not-recorded")
    if check_status == "running" and owner_state(check.get("owner")) == "exited":
        check_status = "interrupted"
    display = "checking" if state == "finished" and check_status in ("pending", "running") else (
        "attention" if state == "finished" and check_status in ("failed", "invalidated", "interrupted") else (
        "waiting" if state == "finished" else state))
    plan = entry.get("testPlan") or {}
    seconds = age(entry.get("registeredUtc"))
    return {"id": redact(entry.get("id", "unknown")), "state": state, "display": display,
            "editRevision": entry.get("editRevision", 0), "ageSeconds": seconds,
            "stale": seconds is not None and seconds > 86400,
            "reopenedUtc": entry.get("reopenedUtc"), "finishedUtc": entry.get("finishedUtc"),
            "ownedFiles": [redact(x) for x in entry.get("ownedFiles", [])[:80]],
            "unclaimedPeers": entry.get("unclaimedPeers", [])[:80],
            "withdrawalReason": redact(entry.get("withdrawalReason") or ""),
            "check": {"status": check_status, "source": check.get("source"),
                      "editRevision": check.get("editRevision"), "reason": redact(check.get("reason", "")),
                      "inputStart": identity(check.get("inputStart")), "inputEnd": identity(check.get("inputEnd")),
                      "issues": [redact(x) for x in (check.get("summary") or {}).get("issues", [])[:40]]},
            "plan": {k: (redact(plan.get(k,"")) if k=="reason" else plan.get(k)) for k in ("scope", "suites", "tests", "reason", "requiredFull", "source", "editRevision")}}

class LogReader:
    def __init__(self):
        self.offset = 0
        self.partial = b""
        self.lines = collections.deque(maxlen=180)
        self.stage = "unknown"
        self.done = None
        self.total = None
        self.targets = set()
        self.skipped_prefix = False
        self.last_modified = None

    def read(self, path):
        try:
            with shared_open(path) as stream:
                size = stream.seek(0, 2)
                if size < self.offset:
                    self.__init__()
                # First read of a very long log starts at a bounded tail.
                if not self.offset and size > 262144:
                    self.offset = size - 262144
                    self.skipped_prefix = True
                stream.seek(self.offset)
                chunk = stream.read(262144)
                self.offset += len(chunk)
            pieces = (self.partial + chunk).split(b"\n")
            self.partial = pieces.pop()[-8192:]
            for raw in pieces:
                line = redact(raw.decode("utf-8", "replace").rstrip("\r"))
                self.lines.append(line)
                if re.search(r"Configuring|Generating done", line, re.I):
                    self.stage = "configure"
                if re.search(r"\.cpp\b|\.cxx\b|Building .+target|ClCompile", line):
                    self.stage = "compile"
                if re.search(r" -> .+\.(exe|dll)|Linking .+", line):
                    self.stage = "link"
                    self.targets.add(line[:400])
                match = re.search(r"(\d+)/(\d+)\s+Test\s+#", line)
                if match:
                    self.stage = "test"
                    self.done, self.total = int(match[1]), int(match[2])
                elif re.search(r"Running \d+ selected test|Test project|Start:\s+\d+", line):
                    self.stage = "test"
            self.last_modified = dt.datetime.fromtimestamp(path.stat().st_mtime, dt.timezone.utc).isoformat()
            return {"lines": list(self.lines), "bytesRead": self.offset, "bytesTotal": size,
                    "caughtUp": self.offset == size, "stage": self.stage,
                    "testCompleted": self.done, "testTotal": self.total,
                    "linkedTargetsObserved": len(self.targets), "prefixOmitted": self.skipped_prefix,
                    "modifiedUtc": self.last_modified}
        except FileNotFoundError:
            return {"lines": [], "stage": "unknown", "testCompleted": None, "testTotal": None}

class Store:
    def __init__(self, root, fixture):
        self.root = root
        self.fixture = fixture
        self.logs = {}
        self.cache = None
        self.cache_time = 0
        self.lock = threading.Lock()

    def batch(self, batch_id, current=None):
        result = read_json(self.root, batch_id + ".json")
        if result and result.get("batchId") != batch_id:
            raise ValueError("Result batch mismatch")
        data = result or current
        if not data:
            return None
        if batch_id not in self.logs:
            if len(self.logs) > 30:
                self.logs.clear()
            self.logs[batch_id] = LogReader()
        log = self.logs[batch_id].read(safe_path(self.root, batch_id + ".log"))
        owner = owner_state(data.get("owner"))
        outcome = data.get("outcome")
        phase = outcome or data.get("phase", "unknown")
        if not result and phase == "building" and owner == "exited":
            phase = "interrupted"
        if result and current:
            phase = "retirement-interrupted"
        coverage = read_json(self.root, batch_id + ".coverage.json") or data.get("coverage") or {}
        if coverage and coverage.get("batchId") != batch_id:
            raise ValueError("Coverage batch mismatch")
        issues = read_json(self.root, batch_id + ".issues.json") or {}
        tasks = [task_view(x) for x in data.get("participants", [])[:80]]
        if result:
            for task in tasks:
                task["stale"] = False
                if task["state"] == "finished":
                    task["display"] = "verified" if outcome == "passed" else ("exempt" if outcome == "skipped" else "batch-ended")
        return {"id": batch_id, "phase": phase, "outcome": outcome, "historical": current is None,
                "protocolVersion": data.get("protocolVersion", 1), "ownerState": owner,
                "createdUtc": data.get("createdUtc"), "completedUtc": data.get("completedUtc"),
                "buildStartedUtc": data.get("buildStartedUtc"), "exitCode": data.get("exitCode"),
                "error": redact(data.get("error", "")), "inputCheck": data.get("inputCheck", "not-frozen"),
                "inputStart": identity(data.get("inputStart")), "inputEnd": identity(data.get("inputEnd")),
                "freshness": "simulation" if self.fixture else "recorded snapshot; current source is not continuously rehashed",
                "tasks": tasks,
                "planStatus": data.get("planStatus"), "planError": redact(data.get("planError", "")),
                "preflight": {"status": (data.get("preflight") or {}).get("status", "not-observed"),
                              "owners": [{k: x.get(k) for k in ("pid", "name", "kind")} for x in (data.get("preflight") or {}).get("owners", [])[:20]],
                              "observedUtc": (data.get("preflight") or {}).get("observedUtc")},
                "coverage": {"mode": coverage.get("mode"), "status": coverage.get("status", "not-recorded"),
                             "selected": coverage.get("selected", [])[:300],
                             "tasks": [{k: x.get(k) for k in ("participant", "requested", "status", "failed", "reason")} for x in coverage.get("tasks", [])[:80]],
                             "error": redact(coverage.get("error", ""))},
                "issues": [{k: redact(x.get(k, "")) for k in ("id", "reportedBy", "assignee", "state", "reason")} for x in issues.get("issues", [])[:40]],
                "binaryEvidence": {"capturedUtc": (data.get("binaryEvidence") or {}).get("capturedUtc"),
                                   "buildStagePassed": (data.get("binaryEvidence") or {}).get("buildStagePassed"),
                                   "executables": (data.get("binaryEvidence") or {}).get("executables", [])[:20]},
                "log": log}

    def snapshot(self):
        with self.lock:
            if self.cache and time.monotonic() - self.cache_time < 2:
                return self.cache
            state = read_json(self.root, "state.json") or {}
            current = state.get("current")
            active = None
            if current:
                if not HEX.fullmatch(str(current.get("id", ""))):
                    raise ValueError("Invalid current batch identity")
                active = self.batch(current["id"], current)
            files = sorted((x for x in self.root.glob("*.json") if HEX.fullmatch(x.stem) and not x.is_symlink()),
                           key=lambda x: x.stat().st_mtime, reverse=True)[:20]
            history = []
            for file in files:
                if active and file.stem == active["id"]:
                    continue
                data = read_json(self.root, file.name) or {}
                if data.get("batchId") == file.stem:
                    history.append({k: data.get(k) for k in ("batchId", "outcome", "completedUtc", "exitCode")})
            self.cache = {"fixture": self.fixture, "updatedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
                          "current": active, "history": history,
                          "limitation": "Only registered cooperating tasks are visible. Stale age is diagnostic; it never completes an editor. Build and link stages are inferred from logs; counts remain unknown until reported."}
            self.cache_time = time.monotonic()
            return self.cache

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, status, content, kind="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", kind + "; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
        self.end_headers()
        self.wfile.write(content)

    def do_GET(self):
        port = self.server.server_port
        allowed_hosts = ("127.0.0.1:" + str(port), "localhost:" + str(port))
        origin = self.headers.get("Origin")
        if (self.headers.get("Host") not in allowed_hosts or
            (origin and origin not in ("http://" + x for x in allowed_hosts)) or
            self.headers.get("Sec-Fetch-Site") == "cross-site"):
            return self.send(403, b'{"error":"Local origin required"}')
        parsed = urlsplit(self.path)
        path = parsed.path
        if parsed.query or parsed.fragment or "%" in path or ".." in path or "\\" in path:
            return self.send(400, b'{"error":"Fixed routes only"}')
        try:
            if path in ASSETS:
                asset = (Path(__file__).resolve().parents[2] / "assets/icon/icon.png") if path == "/app-icon.png" else self.server.assets / ASSETS[path]
                return self.send(200, asset.read_bytes(), MIME[asset.suffix])
            if path == "/api/health":
                data = {"service": "SnowDesktop build dashboard", "instance": self.server.nonce,
                        "fixture": self.server.store.fixture}
            elif path == "/api/status":
                data = self.server.store.snapshot()
            elif path == "/api/history":
                data = self.server.store.snapshot()["history"]
            elif path.startswith("/api/batches/") and HEX.fullmatch(path[len("/api/batches/"):]):
                bid = path[len("/api/batches/"):]
                with self.server.store.lock:
                    data = self.server.store.batch(bid)
                if data is None:
                    return self.send(404, b'{"error":"Unknown batch"}')
            else:
                return self.send(404, b'{"error":"Unknown route"}')
            return self.send(200, json.dumps(data, ensure_ascii=False).encode("utf-8"))
        except (OSError, ValueError, TypeError, KeyError) as error:
            # No arbitrary metadata, paths or traceback are exposed.
            return self.send(503, json.dumps({"error": type(error).__name__ + ": metadata unavailable"}).encode())

    def do_POST(self):
        self.send(405, b'{"error":"Read only"}')
    do_PUT = do_POST
    do_DELETE = do_POST
    do_PATCH = do_POST

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--fixture-root", type=Path)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("Port must be 1024..65535")
    root = (args.fixture_root or args.root).resolve()
    state = root / ".build" / "collaboration"
    # Do not follow a linked metadata parent to unrelated local data.
    for part in (root / ".build", state):
        if part.exists() and (part.is_symlink() or getattr(part.stat(), "st_file_attributes", 0) & 0x400):
            parser.error("State directory must not be a reparse point")
    nonce = secrets.token_hex(16)
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError:
        parser.exit(3, "Port unavailable; no existing listener was stopped.\n")
    server.daemon_threads = True
    server.assets = Path(__file__).resolve().parent
    server.store = Store(state, args.fixture_root is not None)
    server.nonce = nonce
    receipts = root / ".build" / "dashboard"
    receipts.mkdir(parents=True, exist_ok=True)
    if getattr(receipts.stat(), "st_file_attributes", 0) & 0x400:
        parser.error("Receipt directory must not be a reparse point")
    receipt = receipts / ("server-" + str(args.port) + ".json")
    stop = receipts / ("stop-" + nonce + ".request")
    receipt.write_bytes(json.dumps({"pid": os.getpid(), "port": args.port, "instance": nonce,
                                    "fixture": args.fixture_root is not None,
                                    "url": "http://127.0.0.1:" + str(args.port) + "/"}).encode())
    def watch():
        while True:
            time.sleep(0.5)
            if stop.exists():
                try:
                    if stop.read_text() == nonce:
                        server.shutdown()
                        return
                except OSError:
                    pass
    threading.Thread(target=watch, daemon=True).start()
    print("http://127.0.0.1:" + str(args.port) + "/", flush=True)
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        server.server_close()
        try:
            if json.loads(receipt.read_text()).get("instance") == nonce:
                receipt.unlink()
            if stop.exists():
                stop.unlink()
        except (OSError, ValueError):
            pass

if __name__ == "__main__":
    main()
