#!/usr/bin/env python3
"""Claude bridge: lets the dashboard backend run Claude Code on this server.

The dashboard runs in Docker as `nobody`, while Claude Code is installed and
logged in on the host. This tiny HTTP service (Python standard library only)
runs on the host, listens on 127.0.0.1 only and accepts requests only with
the shared secret CLAUDE_BRIDGE_TOKEN. The backend checks that the caller is
a logged-in admin and forwards:

  GET  /status                 -> {"enabled": true, "version": ..., ...}
  POST /jobs                   {"prompt", "session"?, "user"?} -> {"id"}
  GET  /jobs/<id>?after=N      -> status + events from index N on
  POST /jobs/<id>/cancel

Each job is one `claude -p` run (stream-json). "session" continues an
earlier conversation (--resume). Jobs run in the background so the iPhone
app can poll; finished jobs are kept for an hour.

Settings (environment, e.g. via EnvironmentFile=.../dashboard/.env):
  CLAUDE_BRIDGE_TOKEN      required, long random string (openssl rand -hex 32)
  CLAUDE_BRIDGE_BIND       default 127.0.0.1
  CLAUDE_BRIDGE_PORT       default 8091
  CLAUDE_WORKDIR           default /srv/projekte
  CLAUDE_BIN               default "claude"
  CLAUDE_PERMISSION_MODE   default "acceptEdits" (file edits yes, shell only
                           for CLAUDE_ALLOWED_TOOLS); "bypassPermissions"
                           allows everything - only if you really want that
  CLAUDE_ALLOWED_TOOLS     extra allowed tools, comma separated, e.g.
                           "Bash(git status:*),Bash(git diff:*)"
  CLAUDE_MODEL             optional model name/alias
  CLAUDE_TIMEOUT_MIN       default 30 (job is stopped after that)
  CLAUDE_MAX_JOBS          default 2 (running at the same time)
"""

import hmac
import json
import os
import re
import secrets
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

TOKEN = os.environ.get("CLAUDE_BRIDGE_TOKEN", "")
BIND = os.environ.get("CLAUDE_BRIDGE_BIND", "127.0.0.1")
PORT = int(os.environ.get("CLAUDE_BRIDGE_PORT", "8091"))
WORKDIR = os.environ.get("CLAUDE_WORKDIR", "/srv/projekte")
CLAUDE = os.environ.get("CLAUDE_BIN", "claude")
PERMISSION_MODE = os.environ.get("CLAUDE_PERMISSION_MODE", "acceptEdits")
ALLOWED_TOOLS = [t.strip() for t in os.environ.get("CLAUDE_ALLOWED_TOOLS", "").split(",") if t.strip()]
MODEL = os.environ.get("CLAUDE_MODEL", "")
TIMEOUT_S = int(os.environ.get("CLAUDE_TIMEOUT_MIN", "30")) * 60
MAX_JOBS = int(os.environ.get("CLAUDE_MAX_JOBS", "2"))

KEEP_FINISHED_S = 3600
MAX_KEPT_JOBS = 50
MAX_BODY = 64 * 1024
SESSION_RE = re.compile(r"^[0-9a-fA-F-]{8,64}$")
JOB_RE = re.compile(r"^/jobs/([0-9a-f]{32})(/cancel)?$")

SYSTEM_PROMPT = (
    "You are being prompted from the HomeCollect iPhone app by the dashboard "
    "admin '{user}'. Your working directory is {workdir} on their home server. "
    "Answers are read on a phone: keep them short and use simple Markdown. "
    "You cannot ask follow-up questions interactively; if something is unclear "
    "or needs a tool you are not allowed to use, say so in your answer."
)

PERMISSION_MODES = {"default", "acceptEdits", "plan", "bypassPermissions", "dontAsk", "auto"}


def log(msg):
    print(f"[claude-bridge] {msg}", flush=True)


def clip(s, n):
    s = s if isinstance(s, str) else json.dumps(s, ensure_ascii=False)
    return s if len(s) <= n else s[: n - 1] + "…"


def tool_detail(name, inp):
    """One readable line for a tool call (command, file, pattern...)."""
    if not isinstance(inp, dict):
        return ""
    for key in ("command", "file_path", "path", "pattern", "url", "query", "description", "prompt"):
        if isinstance(inp.get(key), str) and inp[key]:
            return clip(inp[key], 300)
    return clip(inp, 300)


class Job:
    def __init__(self, prompt, session, user):
        self.id = secrets.token_hex(16)
        self.prompt = prompt
        self.user = user
        self.session = session  # filled from the init event for new chats
        self.status = "running"  # running | done | error | cancelled
        self.events = []
        self.result = None
        self.error = None
        self.cost_usd = None
        self.started = time.time()
        self.finished = None
        self.proc = None
        self.stderr_tail = ""
        self.lock = threading.Lock()

    def add(self, event):
        with self.lock:
            self.events.append(event)

    def view(self, after):
        with self.lock:
            return {
                "id": self.id,
                "status": self.status,
                "session": self.session,
                "events": self.events[after:],
                "next": len(self.events),
                "result": self.result,
                "error": self.error,
                "costUsd": self.cost_usd,
                "started": int(self.started * 1000),
                "finished": int(self.finished * 1000) if self.finished else None,
            }


jobs = {}
jobs_lock = threading.Lock()


def prune():
    now = time.time()
    with jobs_lock:
        old = [j.id for j in jobs.values() if j.finished and now - j.finished > KEEP_FINISHED_S]
        for i in old:
            del jobs[i]
        finished = sorted((j for j in jobs.values() if j.finished), key=lambda j: j.finished)
        while len(jobs) > MAX_KEPT_JOBS and finished:
            del jobs[finished.pop(0).id]


def handle_line(job, line):
    try:
        msg = json.loads(line)
    except ValueError:
        return
    t = msg.get("type")
    if t == "system" and msg.get("subtype") == "init":
        if msg.get("session_id"):
            job.session = msg["session_id"]
    elif t == "assistant":
        for block in (msg.get("message") or {}).get("content") or []:
            if block.get("type") == "text" and block.get("text", "").strip():
                job.add({"kind": "text", "text": block["text"]})
            elif block.get("type") == "tool_use":
                name = block.get("name", "tool")
                job.add({"kind": "tool", "name": name, "detail": tool_detail(name, block.get("input"))})
    elif t == "user":
        for block in (msg.get("message") or {}).get("content") or []:
            if isinstance(block, dict) and block.get("type") == "tool_result" and block.get("is_error"):
                content = block.get("content")
                if isinstance(content, list):
                    content = " ".join(c.get("text", "") for c in content if isinstance(c, dict))
                job.add({"kind": "tool_error", "text": clip(content or "", 400)})
    elif t == "result":
        with job.lock:
            job.result = msg.get("result")
            job.cost_usd = msg.get("total_cost_usd")
            if msg.get("session_id"):
                job.session = msg["session_id"]
            if msg.get("is_error"):
                job.error = clip(msg.get("result") or msg.get("subtype") or "error", 2000)
        denials = msg.get("permission_denials") or []
        for d in denials[:10]:
            job.add({"kind": "denied", "name": d.get("tool_name", "tool"),
                     "detail": tool_detail(d.get("tool_name", ""), d.get("tool_input"))})


def run_job(job):
    args = [CLAUDE, "-p", "--output-format", "stream-json", "--verbose",
            "--permission-mode", PERMISSION_MODE,
            "--append-system-prompt", SYSTEM_PROMPT.format(user=job.user or "admin", workdir=WORKDIR)]
    if job.session:
        args += ["--resume", job.session]
    if MODEL:
        args += ["--model", MODEL]
    if ALLOWED_TOOLS:
        args += ["--allowedTools", ",".join(ALLOWED_TOOLS)]
    log(f"job {job.id} by {job.user or '?'}: {'resume ' + job.session if job.session else 'new chat'}")
    try:
        job.proc = subprocess.Popen(
            args, cwd=WORKDIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1, start_new_session=True)
    except OSError as e:
        with job.lock:
            job.status, job.error, job.finished = "error", f"cannot start claude: {e}", time.time()
        return

    def read_stderr():
        for line in job.proc.stderr:
            job.stderr_tail = (job.stderr_tail + line)[-2000:]

    threading.Thread(target=read_stderr, daemon=True).start()
    timer = threading.Timer(TIMEOUT_S, lambda: kill(job, "timeout"))
    timer.daemon = True
    timer.start()
    try:
        job.proc.stdin.write(job.prompt)
        job.proc.stdin.close()
        for line in job.proc.stdout:
            handle_line(job, line)
        code = job.proc.wait()
    finally:
        timer.cancel()
    with job.lock:
        job.finished = time.time()
        if job.status == "running":
            if code == 0 and not job.error:
                job.status = "done"
            else:
                job.status = "error"
                job.error = job.error or clip(job.stderr_tail.strip() or f"claude exited with {code}", 2000)
    log(f"job {job.id} {job.status} after {job.finished - job.started:.0f}s")
    prune()


def kill(job, why):
    with job.lock:
        if job.status != "running" or not job.proc:
            return
        job.status = "cancelled"
        job.error = "Stopped (time limit reached)." if why == "timeout" else "Stopped."
    try:
        os.killpg(job.proc.pid, signal.SIGTERM)
    except OSError:
        pass


class Handler(BaseHTTPRequestHandler):
    server_version = "claude-bridge"

    def log_message(self, fmt, *args):
        pass  # no access log (prompts stay out of the journal)

    def reply(self, code, body):
        raw = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def authorized(self):
        got = self.headers.get("Authorization", "")
        ok = hmac.compare_digest(got.encode(), f"Bearer {TOKEN}".encode())
        if not ok:
            self.reply(401, {"error": "unauthorized"})
        return ok

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_BODY:
            return None
        try:
            data = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return None
        return data if isinstance(data, dict) else None

    def do_GET(self):
        if not self.authorized():
            return
        url = urlparse(self.path)
        if url.path == "/status":
            with jobs_lock:
                running = sum(1 for j in jobs.values() if j.status == "running")
            return self.reply(200, {"enabled": True, "version": VERSION, "workdir": WORKDIR,
                                    "permissionMode": PERMISSION_MODE, "allowedTools": ALLOWED_TOOLS,
                                    "running": running})
        m = JOB_RE.match(url.path)
        if m and not m.group(2):
            after = parse_qs(url.query).get("after", ["0"])[0]
            after = int(after) if after.isdigit() else 0
            with jobs_lock:
                job = jobs.get(m.group(1))
            if not job:
                return self.reply(404, {"error": "unknown_job"})
            return self.reply(200, job.view(after))
        self.reply(404, {"error": "not_found"})

    def do_POST(self):
        if not self.authorized():
            return
        path = urlparse(self.path).path
        body = self.body()
        if body is None:
            return self.reply(400, {"error": "invalid_json"})
        if path == "/jobs":
            prompt = body.get("prompt")
            session = body.get("session") or None
            user = str(body.get("user") or "")[:64]
            if not isinstance(prompt, str) or not prompt.strip():
                return self.reply(400, {"error": "empty_prompt"})
            if session is not None and (not isinstance(session, str) or not SESSION_RE.match(session)):
                return self.reply(400, {"error": "invalid_session"})
            with jobs_lock:
                running = [j for j in jobs.values() if j.status == "running"]
                if session and any(j.session == session for j in running):
                    return self.reply(409, {"error": "session_busy"})
                if len(running) >= MAX_JOBS:
                    return self.reply(429, {"error": "too_many_jobs"})
                job = Job(prompt, session, user)
                jobs[job.id] = job
            threading.Thread(target=run_job, args=(job,), daemon=True).start()
            return self.reply(201, {"id": job.id})
        m = JOB_RE.match(path)
        if m and m.group(2):
            with jobs_lock:
                job = jobs.get(m.group(1))
            if not job:
                return self.reply(404, {"error": "unknown_job"})
            kill(job, "cancel")
            return self.reply(200, job.view(len(job.events)))
        self.reply(404, {"error": "not_found"})


def claude_version():
    try:
        out = subprocess.run([CLAUDE, "--version"], capture_output=True, text=True, timeout=30)
        return out.stdout.strip() or None
    except (OSError, subprocess.TimeoutExpired):
        return None


VERSION = None

if __name__ == "__main__":
    if len(TOKEN) < 32:
        sys.exit("CLAUDE_BRIDGE_TOKEN must be set (at least 32 characters, e.g. openssl rand -hex 32)")
    if PERMISSION_MODE not in PERMISSION_MODES:
        sys.exit(f"CLAUDE_PERMISSION_MODE must be one of {sorted(PERMISSION_MODES)}")
    VERSION = claude_version()
    if not VERSION:
        sys.exit(f"cannot run '{CLAUDE} --version' - is Claude Code installed and on PATH?")
    log(f"{VERSION}, workdir {WORKDIR}, permission mode {PERMISSION_MODE}, listening on {BIND}:{PORT}")
    ThreadingHTTPServer((BIND, PORT), Handler).serve_forever()
