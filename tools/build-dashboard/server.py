"""Read-only, loopback-only SnowDesktop build monitor. Python 3.8+, stdlib only."""
import argparse
import collections
import ctypes
import datetime as dt
import hashlib
import json
import math
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
# Full-test results include coverage and retry evidence that can exceed 256 KiB.
# Keep a finite read bound without rejecting ordinary coordinator records.
MAX_METADATA_BYTES = 8 * 1024 * 1024

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
    # Windows File.Replace can briefly remove the destination or hold a
    # sharing lock while exchanging the old/new files. A missing live state
    # during that exchange is not evidence that its editors have disappeared.
    # Reopen the actual file; never substitute cached/backup registration data.
    for attempt in range(4):
        try:
            with shared_open(safe_path(root, name)) as stream:
                data = stream.read(MAX_METADATA_BYTES + 1)
            if len(data) > MAX_METADATA_BYTES:
                raise ValueError("Metadata exceeds 8 MiB")
            return json.loads(data.decode("utf-8-sig"))
        except OSError as error:
            missing = isinstance(error, FileNotFoundError) or error.errno in (2, 3)
            transient = missing and name == "state.json" or error.errno in (32, 33)
            if attempt == 3 or not transient:
                if missing:
                    return None
                raise
            time.sleep(.005 * (attempt + 1))

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
            exit_code = ctypes.c_ulong()
            if not kernel.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(exit_code)):
                return "unknown"
            if exit_code.value != 259:  # STILL_ACTIVE; a retained handle keeps an exited PID queryable.
                return "exited"
            if not kernel.GetProcessTimes(ctypes.c_void_p(handle), *[ctypes.byref(v) for v in values]):
                return "unknown"
            ticks = values[0].value + 504911232000000000
            return "alive" if str(ticks) == str(owner["startTicks"]) else "exited"
        finally:
            kernel.CloseHandle(ctypes.c_void_p(handle))
    except (KeyError, ValueError, OSError):
        return "unknown"

def timestamp(value):
    try:
        # PowerShell records seven fractional digits; Python 3.8 accepts six.
        value = re.sub(r"(\.\d{6})\d+(?=Z|[+-]\d{2}:\d{2}$)", r"\1", value)
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed if parsed.tzinfo is not None else None
    except (ValueError, TypeError, AttributeError):
        return None

def age(value, observed=None):
    parsed = timestamp(value)
    end = timestamp(observed) if observed else dt.datetime.now(dt.timezone.utc)
    return max(0, int((end - parsed).total_seconds())) if parsed and end else None

def coverage_view(coverage):
    selected = coverage.get("selected") or []
    results = {}
    for item in coverage.get("completed") or []:
        if isinstance(item, dict) and isinstance(item.get("name"), str):
            results[item["name"]] = item
    counts = collections.Counter(item.get("status", "unknown") for item in results.values())
    passed = sum(counts[x] for x in ("passed", "passed-after-retry"))
    failed = sum(counts[x] for x in ("failed", "exhausted-failed"))
    not_run = {name for name, item in results.items() if item.get("status") in ("not-run", "notrun", "environment-blocked")}
    for task in coverage.get("tasks") or []:
        if task.get("status") in ("not-run", "notrun", "environment-blocked"):
            not_run.update(name for name in task.get("requested", []) if name not in results)
    completed = []
    for name, item in list(results.items())[:300]:
        try:
            seconds = float(item["seconds"])
            if not math.isfinite(seconds) or seconds < 0:
                seconds = None
        except (KeyError, TypeError, ValueError):
            seconds = None
        completed.append({"name": redact(name), "status": redact(item.get("status", "unknown")), "seconds": seconds})
    return {"selectedCount": len(selected), "completed": completed,
            "summary": {"selected": len(selected), "executed": passed + failed, "passed": passed, "failed": failed,
                        "notRun": len(not_run), "skipped": counts["skipped"], "flaky": counts["passed-after-retry"],
                        "unreported": len(set(selected) - set(results) - not_run)},
            "completedTruncated": len(results) > len(completed)}

def identity(value):
    if not isinstance(value, dict):
        return None
    return {k: value.get(k) for k in ("algorithm", "digest", "fileCount", "head")}

def task_view(entry, observed=None):
    check = entry.get("check") or {}
    state = entry.get("state", "unknown")
    check_status = check.get("status", "not-recorded")
    if check_status == "running" and owner_state(check.get("owner")) == "exited":
        check_status = "interrupted"
    display = "checking" if state == "finished" and check_status in ("pending", "running") else (
        "attention" if state == "finished" and check_status in ("failed", "invalidated", "interrupted") else (
        "waiting" if state == "finished" else state))
    plan = entry.get("testPlan") or {}
    seconds = age(entry.get("registeredUtc"), observed)
    return {"id": redact(entry.get("id", "unknown")), "state": state, "display": display,
            "name": redact(entry.get("displayName") or entry.get("id", "unknown")),
            "registeredUtc": entry.get("registeredUtc"),
            "editRevision": entry.get("editRevision", 0), "ageSeconds": seconds,
            "stale": seconds is not None and seconds > 86400,
            "reopenedUtc": entry.get("reopenedUtc"), "finishedUtc": entry.get("finishedUtc"),
            "ownedFiles": [redact(x) for x in entry.get("ownedFiles", [])[:80]],
            "fileRole": "active-editor" if state=="editing" else "origin-record",
            "unclaimedPeers": entry.get("unclaimedPeers", [])[:80],
            "withdrawalReason": redact(entry.get("withdrawalReason") or ""),
            "check": {"status": check_status, "source": check.get("source"),
                      "startedUtc": check.get("startedUtc"), "completedUtc": check.get("completedUtc"),
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

def record_seconds(value):
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
        return number if math.isfinite(number) and number >= 0 else None
    except (TypeError, ValueError, OverflowError):
        return None

def acceptance_view(ledger, batch_id=None):
    records = (ledger or {}).get("acceptances") or []
    if not isinstance(records, list):
        raise ValueError("Invalid acceptance records")
    result = []
    for entry in records[-100:]:
        if not isinstance(entry, dict) or not HEX.fullmatch(str(entry.get("id", ""))):
            raise ValueError("Invalid acceptance identity")
        if batch_id and entry.get("batchId") != batch_id:
            continue
        result.append({**{key: entry.get(key) for key in ("id", "task", "batchId", "runId", "status", "requirementsRevision", "editRevision", "startedUtc", "completedUtc", "durationStatus", "reporter", "sourceDigest")},
                       **{key: redact(entry.get(key, "")) for key in ("criterion", "reference", "notes")},
                       "elapsedSeconds": record_seconds(entry.get("elapsedSeconds")),
                       "artifactCount": len(entry.get("artifacts") or []),
                       "artifacts": [{key: redact(x.get(key, "")) for key in ("file", "sha256", "sourceAssociation")} for x in (entry.get("artifacts") or [])[:64]]})
    return result

def evidence_view(value):
    value = value or {}
    if value.get("schemaVersion") != 1:
        return {"status": "not-recorded", "tests": [], "totalCount": 0}
    entries = value.get("tests") or []
    if not isinstance(entries, list):
        raise ValueError("Invalid per-test evidence")
    rows = []
    for entry in entries[:300]:
        if not isinstance(entry, dict) or not HEX.fullmatch(str(entry.get("id", ""))):
            raise ValueError("Invalid per-test evidence identity")
        rows.append({**{key: entry.get(key) for key in ("id", "test", "status", "batchId", "runId", "logicalBatchId", "attempt", "bindingDigest", "inputCheck", "identityStatus")},
                     "seconds": record_seconds(entry.get("seconds")), "requesters": [redact(x) for x in (entry.get("requesters") or [])[:80]],
                     "binding": {key: (entry.get("binding") or {}).get(key) for key in ("sourceDigest", "configuration", "assertionBinary", "invocationDigest", "environmentDigest")},
                     "reuseAllowed": entry.get("reuseAllowed") is True,
                     "attempts": [{key: x.get(key) for key in ("attempt", "status", "exitCode", "runnerExitCode", "seconds", "reportSha256")} for x in (entry.get("attempts") or [])[:10]]})
    return {"status": value.get("status", "not-recorded"), "tests": rows, "totalCount": len(entries)}

class Store:
    def __init__(self, root, fixture):
        self.root = root
        self.fixture = fixture
        self.logs = {}
        self.cache = None
        self.cache_time = 0
        self.lock = threading.Lock()

    def run(self, run_id):
        if not HEX.fullmatch(str(run_id)):
            raise ValueError("Invalid Run identity")
        directory = safe_path(self.root, "runs")
        record = read_json(directory, run_id + ".json")
        if not record:
            return None
        request = read_json(directory, run_id + ".request.json")
        checkout = self.root.parent.parent
        if not isinstance(record, dict) or not isinstance(request, dict) or record.get("schemaVersion") != 1 or request.get("schemaVersion") != 1 or not HEX.fullmatch(str(request.get("nonce"))) or record.get("id") != run_id or request.get("id") != run_id or record.get("nonce") != request.get("nonce") or Path(request.get("repositoryRoot", "")).resolve() != checkout or record.get("repositoryRoot") != request.get("repositoryRoot"):
            raise ValueError("Run request/result identity mismatch")
        def number(value):
            return value if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0 else None
        operations = {}
        collectors = 0
        finished_collectors = 0
        unfinished = 0
        limited = False
        pending_line = False
        total_bytes = 0
        paths = sorted(directory.glob(run_id + ".stages.*.jsonl")) + sorted(directory.glob(run_id + ".msbuild.*.jsonl"))
        if len(paths) > 100:
            raise ValueError("Run observation file budget exceeded")
        pipeline_logs = []
        for path in paths:
            with shared_open(safe_path(directory, path.name)) as stream:
                content = stream.read(4 * 1024 * 1024 + 1)
            total_bytes += len(content)
            if total_bytes > 4 * 1024 * 1024:
                raise ValueError("Run observation byte budget exceeded")
            lines = content.decode("utf-8-sig").split("\n")
            pending_line = pending_line or bool(lines[-1])
            for line in lines[:-1]:
                if not line:
                    continue
                event = json.loads(line)
                if event.get("schemaVersion") != 1 or event.get("runId") != run_id:
                    raise ValueError("Run observation identity mismatch")
                kind = event.get("kind")
                if kind == "collector":
                    collectors += 1
                elif kind == "collector-end":
                    finished_collectors += 1
                    unfinished += event.get("unfinishedTasks", 0)
                elif kind == "collector-limit":
                    limited = True
                elif kind in ("start", "end"):
                    eid = event.get("id")
                    if not HEX.fullmatch(str(eid)):
                        raise ValueError("Invalid Run stage identity")
                    pair = operations.setdefault(eid, {})
                    if kind in pair:
                        raise ValueError("Duplicate Run stage identity")
                    pair[kind] = event
                    if kind == "start" and event.get("logPath") and event.get("name", "").endswith("-pipeline"):
                        path_hint = Path(event["logPath"])
                        if path_hint.parent == self.root and re.fullmatch(r"entry-[a-f0-9]{32}\.log", path_hint.name):
                            pipeline_logs.append((event.get("utc", ""), path_hint.name))
        segments = []
        summaries = {}
        units = 0
        unknown_units = 0
        for eid, pair in operations.items():
            start, end = pair.get("start"), pair.get("end")
            if not start:
                raise ValueError("Run stage ended without an observed start")
            if end and end.get("name") != start.get("name"):
                raise ValueError("Run stage name mismatch")
            elapsed = number((end or {}).get("elapsedSeconds"))
            name = redact(start.get("name", "unknown"))
            row = {"id": eid, "name": name, "parentId": start.get("parentId"), "startedUtc": start.get("utc"),
                   "sourceUtc": start.get("sourceUtc"), "elapsedSeconds": elapsed, "exitCode": (end or {}).get("exitCode"),
                   "status": "running" if not end else "passed" if end.get("exitCode") == 0 else "failed",
                   "project": redact(start.get("project", "")), "task": redact(start.get("task", "")),
                   "measurement": redact((end or {}).get("measurement", "producer monotonic interval"))}
            segments.append(row)
            summary = summaries.setdefault(name, {"name": name, "tasks": 0, "completed": 0, "running": 0, "incomplete": 0, "failed": 0, "taskSeconds": 0})
            summary["tasks"] += 1
            summary["running" if not end else "completed"] += 1
            summary["failed"] += bool(end and end.get("exitCode") != 0)
            if elapsed is not None:
                summary["taskSeconds"] += elapsed
            if name == "compile":
                if end and end.get("unitCountKnown") is True and number(end.get("attemptedUnits")) is not None:
                    units += end["attemptedUnits"]
                else:
                    unknown_units += 1
        process = owner_state(record.get("owner"))
        terminal = record.get("status") in ("passed", "failed", "cancelled", "start-failed", "invalidated", "interrupted")
        status = "interrupted" if not terminal and process == "exited" else record.get("status", "unknown")
        if terminal or status == "interrupted":
            for row in segments:
                if row["status"] == "running":
                    row["status"] = "incomplete"
            for summary in summaries.values():
                summary["incomplete"] = summary["running"]
                summary["running"] = 0
        resources = read_json(directory, run_id + ".resources.json") or {}
        accounting = resources.get("accountingAvailable")
        resource_view = {**{k: number(resources.get(k)) if accounting is not False else None for k in ("cpuSeconds", "peakJobMemoryBytes", "activeProcesses", "totalProcesses", "ioReadBytes", "ioWriteBytes")},
                         "sampledUtc": resources.get("sampledUtc"), "finished": resources.get("finished") is True,
                         "accountingAvailable": accounting if isinstance(accounting, bool) else None}
        if not resources:
            resource_view["status"] = "unrecorded"
        elif accounting is False:
            resource_view["status"] = "unavailable"
        elif accounting is not True:
            resource_view["status"] = "unknown"
        elif resource_view["cpuSeconds"] is None or resource_view["peakJobMemoryBytes"] is None:
            resource_view["status"] = "unavailable"
        elif resource_view["finished"]:
            resource_view["status"] = "complete"
        else:
            resource_view["status"] = "incomplete" if terminal or status == "interrupted" else "sampling"
        waits = record.get("waitIntervals") or []
        log_key = run_id + ".stdout.log"
        log_root = directory
        if pipeline_logs:
            log_root, log_key = self.root, sorted(pipeline_logs)[-1][1]
        cache_key = "run:" + run_id
        if cache_key not in self.logs:
            if len(self.logs) > 50:
                self.logs.clear()
            self.logs[cache_key] = LogReader()
        segments.sort(key=lambda item: (str(item["startedUtc"]), item["id"]))
        return {"id": run_id, "batchId": request.get("batch"), "task": redact(request.get("task", "")),
                "action": request.get("action"), "mode": request.get("mode"), "configuration": request.get("configuration"),
                "status": status, "persistedStatus": record.get("status"), "ownerState": process, "ownerPid": (record.get("owner") or {}).get("pid"),
                "createdUtc": record.get("createdUtc"), "updatedUtc": record.get("updatedUtc"), "completedUtc": record.get("completedUtc"),
                "exitCode": record.get("exitCode"), "error": redact(record.get("error", "")), "waitingFor": redact(record.get("waitingFor", "")) if not terminal and status != "interrupted" else "",
                "stages": [{**{k: stage.get(k) for k in ("id", "name", "startedUtc", "completedUtc", "exitCode")}, "elapsedSeconds": number(stage.get("elapsedSeconds"))} for stage in (record.get("stages") or [])[:40]],
                "waitIntervals": [{**{k: interval.get(k) for k in ("resource", "startedUtc", "completedUtc")}, "reason": redact(interval.get("reason", "")), "elapsedSeconds": number(interval.get("elapsedSeconds"))} for interval in waits[:100]],
                "waitIntervalsTruncated": record.get("waitIntervalsTruncated") is True,
                "resources": resource_view,
                "phaseSummary": list(summaries.values()), "segments": segments[-80:], "segmentCount": len(segments), "segmentsOmitted": max(0, len(segments)-80),
                "compiler": {"collectorCount": collectors, "attemptedUnits": units if collectors else None, "unknownTasks": unknown_units,
                             "complete": bool(terminal and collectors and finished_collectors == collectors and not unknown_units and not unfinished and not limited and not pending_line)},
                "collectionLimited": limited, "pendingLine": pending_line, "unfinishedTasks": unfinished,
                "log": self.logs[cache_key].read(safe_path(log_root, log_key)),
                "limitation": "Task intervals may overlap; taskSeconds is accumulated task time, not Run wall time. Compiler counts are attempted source units from observed CL commands; missing projections remain unknown."}

    def runs(self):
        directory = safe_path(self.root, "runs")
        files = []
        for path in directory.glob("*.json"):
            if not HEX.fullmatch(path.stem):
                continue
            try:
                files.append((path.stat().st_mtime, path.stem))
            except FileNotFoundError:
                continue
        result = []
        for _, rid in sorted(files, reverse=True)[:8]:
            try:
                value = self.run(rid)
                if value:
                    result.append(value)
            except (ValueError, OSError) as error:
                result.append({"id": rid, "status": "unavailable", "error": redact(error)})
        return result

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
        tasks = [task_view(x, data.get("completedUtc") if result else None) for x in data.get("participants", [])[:80]]
        if result:
            for task in tasks:
                task["stale"] = False
                if task["state"] == "finished":
                    task["display"] = "verified" if outcome == "passed" else ("exempt" if outcome == "skipped" else "batch-ended")
        run = None
        if data.get("runId"):
            try:
                run = self.run(data["runId"])
                if run is None:
                    run = {"id": data["runId"], "status": "unavailable", "error": "Associated Run record is not readable yet."}
            except (ValueError, OSError, TypeError) as error:
                run = {"id": data["runId"], "status": "unavailable", "error": redact(error)}
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
                             **coverage_view(coverage),
                             "selected": coverage.get("selected", [])[:300],
                             "tasks": [{k: x.get(k) for k in ("participant", "requested", "status", "failed", "reason")} for x in coverage.get("tasks", [])[:80]],
                             "error": redact(coverage.get("error", ""))},
                "issues": [{k: redact(x.get(k, "")) for k in ("id", "reportedBy", "assignee", "state", "reason")} for x in issues.get("issues", [])[:40]],
                "binaryEvidence": {"capturedUtc": (data.get("binaryEvidence") or {}).get("capturedUtc"),
                                   "buildStagePassed": (data.get("binaryEvidence") or {}).get("buildStagePassed"),
                                   "executables": (data.get("binaryEvidence") or {}).get("executables", [])[:20]},
                "logicalBatchId":data.get("logicalBatchId",batch_id),"attempt":data.get("attempt",1),"repairOf":data.get("repairOf"),
                "runId": data.get("runId"),
                "run": run,
                "evidence": evidence_view(data.get("evidence")),
                "acceptances": acceptance_view((read_json(self.root, "state.json") or {}).get("taskLedger"), batch_id),
                "retry": self.retry_view(batch_id),
                "log": log}

    def retry_view(self,bid):
        data=read_json(self.root,bid+'.retry.json') or {}
        status=data.get('status','not-recorded')
        if status in ('running','retrying') and owner_state(data.get('owner'))=='exited':status='interrupted'
        tests=[]
        for name,item in list((data.get('tests') or {}).items()):
            if not item.get('failureCount') and item.get('status')=='passed':continue
            attempts=[]
            for attempt in item.get('attempts',[])[:3]:
                number=attempt.get('attempt')
                attempts.append({'attempt':number,'status':attempt.get('status'),'reason':redact(attempt.get('reason','')),
                    'logUrl':'/api/retries/'+bid+'/'+name+'/'+str(number)+'/log' if name in ('build_dashboard','build_dashboard_browser') and number in (2,3) else None})
            tests.append({'name':redact(name),'status':item.get('status'),'failureCount':item.get('failureCount',0),
                'firstReason':redact(item.get('firstReason','')),'finalReason':redact(item.get('finalReason','')),'attempts':attempts})
        return {'status':status,'reason':redact(data.get('reason','')),'policy':data.get('policy'),'tests':tests[:40],'selectedCount':len(data.get('selected',[]))}

    def waits(self):
        files=sorted(self.root.glob('*.wait.json'),key=lambda x:x.stat().st_mtime,reverse=True)[:40]
        items=[]
        for file in files:
            if not HEX.fullmatch(file.name[:-10]):continue
            ticket=read_json(self.root,file.name) or {}
            receipt=read_json(self.root,ticket.get('id','')+'.wait-attempt'+str(ticket.get('attempt',1))+'.json')
            ticket=receipt or ticket
            status=ticket.get('status','unknown')
            if status=='waiting' and owner_state(ticket.get('owner'))=='exited':status='interrupted'
            items.append({**{k:ticket.get(k) for k in ('id','participant','condition','batchId','attempt','createdUtc','deadlineUtc','completedUtc','observations')},'status':status,'reason':redact(ticket.get('reason','')),'next':redact(ticket.get('next',''))})
        return items

    def retry_log(self,bid,name,attempt):
        if name not in ('build_dashboard','build_dashboard_browser') or attempt not in (2,3):raise ValueError('Unknown retry log')
        directory=safe_path(self.root,bid+'.retry')
        path=safe_path(directory,name+'.attempt'+str(attempt)+'.log')
        return LogReader().read(path)

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
                    history.append({**{k: data.get(k) for k in ("batchId", "outcome", "createdUtc", "buildStartedUtc", "completedUtc", "exitCode", "logicalBatchId", "attempt", "repairOf")},
                                    "participants": [redact(x.get("displayName") or x.get("id", "unknown")) for x in data.get("participants", [])[:8]]})
            ledger = state.get("taskLedger") or {}
            presence = []
            for task in (ledger.get("tasks") or [])[-200:]:
                presence.append({**{key: redact(str(task.get(key) or "")) for key in
                                    ("id", "displayName", "phase", "progress", "blocker", "adapter", "registrationOrigin")},
                                 **{key: task.get(key) for key in ("revision", "requirementsRevision", "openedUtc", "lastHeartbeatUtc", "lastProgressUtc", "closedUtc")},
                                 "heartbeatAgeSeconds": age(task.get("lastHeartbeatUtc")),
                                 "capabilities": {key: (task.get("capabilities") or {}).get(key) is True for key in
                                                  ("taskRegistration", "beforeWriteGate", "boundedWait", "liveProgress", "activeTurnSteering", "idleWake")}})
            events = []
            for event in (ledger.get("events") or [])[-100:]:
                recipients = event.get("recipients") or []
                receipts = event.get("receipts") or []
                events.append({**{key: redact(str(event.get(key) or "")) for key in ("key", "sender", "target", "kind", "importance", "text")},
                               **{key: event.get(key) for key in ("sequence", "createdUtc", "requirementsRevision")},
                               "delivery": "persisted", "recipientCount": len(recipients),
                               "adapterReceived": sum(any(r.get("task") == who and r.get("level") == "adapter" for r in receipts) for who in recipients),
                               "agentAcknowledged": sum(any(r.get("task") == who and r.get("level") == "agent" for r in receipts) for who in recipients)})
            self.cache = {"fixture": self.fixture, "updatedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
                          "current": active, "history": history, "waits": self.waits(),
                          "taskPresence": presence, "taskPresenceTotal": len(ledger.get("tasks") or []),
                          "events": events, "eventTotal": len(ledger.get("events") or []), "runs": self.runs(),
                          "acceptances": acceptance_view(ledger),
                          "limitation": "Only registered cooperating tasks are visible. Stale age never completes an editor. Run task intervals and compiler counts come from producer observations; unobserved phases remain unknown."}
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
            elif re.fullmatch(r'/api/retries/[a-f0-9]{32}/(build_dashboard|build_dashboard_browser)/[23]/log',path):
                parts=path.split('/')
                with self.server.store.lock:data=self.server.store.retry_log(parts[3],parts[4],int(parts[5]))
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
