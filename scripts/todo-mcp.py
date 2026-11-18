#!/usr/bin/env python3
"""Todo MCP server: lets Claude agents on this host add tasks to Todoist.

Todoist is connected to Home Assistant (todo.* entities, same lists as the
dashboard "Tasks" card). Lists are the ones in HA_TODO_ENTITIES (same
whitelist as the backend, see backend/src/config.cpp); reading goes through
HA's REST API (HA_URL / HA_TOKEN from the dashboard .env).
Adding: HA's Todoist integration currently fails on create (404 from
api.todoist.com/api/v1/tasks), so if TODOIST_API_TOKEN is set, tasks for
Todoist lists are created directly via the Todoist API v1, in the project
whose name matches the list's HA friendly name (Inbox -> inbox project).
Non-Todoist lists (e.g. todo.shopping_list) and setups without the token
use HA's todo.add_item.

Two ways to use it (Python standard library only):
  * MCP stdio server (no arguments) - registered for Claude Code on the host:
      claude mcp add -s user todo -- /usr/bin/python3 -I \
          /srv/projekte/dashboard/scripts/todo-mcp.py
    Used by interactive Claude Code sessions and by the HomeCollect "Claude"
    tab (scripts/claude-bridge.py pre-allows the mcp__todo tools).
    Tools: add_todo, list_todos.
  * CLI:  todo-mcp.py add "Buy milk" [--list inbox] [--due 2026-11-20[T18:00]]
                       [--description "..."] [--label next ...]
          todo-mcp.py lists | list [--list inbox]

Settings: DASHBOARD_ENV (default: ../.env next to this script),
TODO_DEFAULT_LIST (default: first entry of HA_TODO_ENTITIES),
TODOIST_API_TOKEN (Todoist > Settings > Integrations > Developer).
"""

import json
import os
import re
import sys
import urllib.request

ENV_FILE = os.environ.get(
    "DASHBOARD_ENV", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".env"))


def load_env():
    env = {}
    try:
        with open(ENV_FILE) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    env.update({k: v for k, v in os.environ.items() if k.startswith(("HA_", "TODO_", "TODOIST_"))})
    return env


ENV = load_env()
HA_URL = ENV.get("HA_URL", "").rstrip("/")
HA_TOKEN = ENV.get("HA_TOKEN", "")
LISTS = [e.strip() for e in (ENV.get("HA_TODO_ENTITIES", "") + "," + ENV.get("HA_TODO_ENTITY", "")).split(",")
         if e.strip()]
DEFAULT_LIST = ENV.get("TODO_DEFAULT_LIST") or (LISTS[0] if LISTS else "")
TODOIST_TOKEN = ENV.get("TODOIST_API_TOKEN", "")
TODOIST_API = "https://api.todoist.com/api/v1"


class TodoError(Exception):
    pass


def http_json(what, url, token, method="GET", body=None):
    req = urllib.request.Request(
        url, method=method, data=json.dumps(body).encode() if body is not None else None,
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        raise TodoError(f"{what} HTTP {e.code}: {e.read()[:300].decode(errors='replace')}")
    except (urllib.error.URLError, TimeoutError) as e:
        raise TodoError(f"{what} unreachable: {e}")


def ha(method, path, body=None):
    if not HA_URL or not HA_TOKEN:
        raise TodoError(f"HA_URL/HA_TOKEN missing (read from {ENV_FILE})")
    return http_json("Home Assistant", HA_URL + path, HA_TOKEN, method, body)


def ha_state(entity):
    try:
        return ha("GET", "/api/states/" + entity) or {}
    except TodoError:
        return {}


def is_todoist(entity):
    """Todoist lists support due date/datetime + description (features 16|32|64);
    HA's local shopping list does not."""
    feats = ha_state(entity).get("attributes", {}).get("supported_features", 0)
    return (feats & 112) == 112


def todoist_project_id(friendly):
    """Todoist project whose name matches the HA list name (or the inbox)."""
    cursor, projects = None, []
    while True:
        q = "/projects?limit=200" + (f"&cursor={cursor}" if cursor else "")
        resp = http_json("Todoist", TODOIST_API + q, TODOIST_TOKEN) or {}
        projects += resp.get("results", [])
        cursor = resp.get("next_cursor")
        if not cursor:
            break
    for p in projects:
        if p.get("name", "").lower() == friendly.lower():
            return p["id"]
    for p in projects:
        if p.get("inbox_project") and friendly.lower() == "inbox":
            return p["id"]
    raise TodoError(f"no Todoist project named '{friendly}'")


def list_names():
    """entity_id -> friendly name for all whitelisted lists."""
    out = {}
    for e in LISTS:
        try:
            out[e] = ha("GET", "/api/states/" + e)["attributes"].get("friendly_name", e)
        except (TodoError, KeyError, TypeError):
            out[e] = e
    return out


def resolve_list(name):
    """Accepts "todo.inbox", "inbox" or a friendly name ("Inbox")."""
    if not name:
        if not DEFAULT_LIST:
            raise TodoError("no todo lists configured (HA_TODO_ENTITIES)")
        return DEFAULT_LIST
    n = name.strip().lower()
    for e in LISTS:
        if n in (e.lower(), e.lower().removeprefix("todo.")):
            return e
    for e, friendly in list_names().items():
        if friendly.lower() == n:
            return e
    raise TodoError(f"unknown list '{name}'. Allowed: {', '.join(LISTS)}")


DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
DATETIME_RE = re.compile(r"^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}(:\d{2})?$")


def add_todo(content, list_name=None, due=None, description=None, labels=None):
    content = (content or "").strip()
    if not content:
        raise TodoError("content is empty")
    content = content[:500]
    entity = resolve_list(list_name)
    due = (due or "").strip()
    if due and not (DATE_RE.match(due) or DATETIME_RE.match(due)):
        raise TodoError("due must be YYYY-MM-DD or YYYY-MM-DDTHH:MM (local time)")
    description = (description or "")[:2000]
    if isinstance(labels, str):
        labels = [labels]
    labels = [l.strip() for l in (labels or []) if l and l.strip()][:10]

    if TODOIST_TOKEN and is_todoist(entity):
        friendly = ha_state(entity).get("attributes", {}).get("friendly_name", entity)
        task = {"content": content, "project_id": todoist_project_id(friendly)}
        if due and DATE_RE.match(due):
            task["due_date"] = due
        elif due:  # "YYYY-MM-DD HH:MM" as due_string = floating local time
            task["due_string"], task["due_lang"] = due.replace("T", " "), "en"
        if description:
            task["description"] = description
        if labels:
            task["labels"] = labels
        http_json("Todoist", TODOIST_API + "/tasks", TODOIST_TOKEN, "POST", task)
        via = "Todoist"
    else:
        data = {"entity_id": entity, "item": content}
        if due and DATE_RE.match(due):
            data["due_date"] = due
        elif due:
            data["due_datetime"] = due.replace("T", " ")
        if description:
            data["description"] = description
        if labels:
            raise TodoError("labels need a Todoist list and TODOIST_API_TOKEN")
        try:
            ha("POST", "/api/services/todo/add_item", data)
        except TodoError as e:
            hint = "" if TODOIST_TOKEN else (
                " (HA's Todoist create is broken; set TODOIST_API_TOKEN in the dashboard .env)")
            raise TodoError(str(e) + hint)
        via = "Home Assistant"
    return (f"Added '{content}' to {entity} via {via}" + (f" (due {due})" if due else "")
            + (f" [labels: {', '.join(labels)}]" if labels else ""))


def list_todos(list_name=None):
    entity = resolve_list(list_name)
    resp = ha("POST", "/api/services/todo/get_items?return_response",
              {"entity_id": entity, "status": "needs_action"})
    items = (resp or {}).get("service_response", {}).get(entity, {}).get("items", [])
    if not items:
        return f"{entity}: no open items"
    lines = [f"{entity} ({len(items)} open):"]
    for it in items:
        due = it.get("due")
        lines.append(f"- {it.get('summary', '')}" + (f" (due {due})" if due else ""))
    return "\n".join(lines)


# ---------------------------------------------------------------- MCP (stdio)

TOOLS = [
    {
        "name": "add_todo",
        "description": "Add a task to the owner's Todoist (via Home Assistant). "
                       "Use for reminders/follow-ups the owner should do themselves. "
                       "Lists: " + ", ".join(LISTS) + f" (default {DEFAULT_LIST}).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "content": {"type": "string", "description": "Task title, short and actionable"},
                "list": {"type": "string", "description": "List entity or name, e.g. 'inbox'. Optional."},
                "due": {"type": "string", "description": "Optional due date YYYY-MM-DD or "
                                                         "YYYY-MM-DDTHH:MM (Europe/Zurich)"},
                "description": {"type": "string", "description": "Optional details/notes"},
                "labels": {"type": "array", "items": {"type": "string"},
                           "description": "Optional Todoist labels (Todoist lists only). Prefer "
                                          "existing ones: next, waiting, dated, Sometime"},
            },
            "required": ["content"],
        },
    },
    {
        "name": "list_todos",
        "description": "List open tasks of one Todoist list (check for duplicates before adding).",
        "inputSchema": {
            "type": "object",
            "properties": {"list": {"type": "string", "description": "List entity or name. Optional."}},
        },
    },
]


def handle(msg):
    method, mid = msg.get("method"), msg.get("id")
    if mid is None:
        return None  # notification (e.g. notifications/initialized)
    if method == "initialize":
        result = {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2024-11-05"),
                  "capabilities": {"tools": {}},
                  "serverInfo": {"name": "todo", "version": "1.0"}}
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        p = msg.get("params", {})
        args = p.get("arguments") or {}
        try:
            if p.get("name") == "add_todo":
                text = add_todo(args.get("content"), args.get("list"), args.get("due"),
                                args.get("description"), args.get("labels"))
            elif p.get("name") == "list_todos":
                text = list_todos(args.get("list"))
            else:
                raise TodoError(f"unknown tool {p.get('name')}")
            result = {"content": [{"type": "text", "text": text}]}
        except TodoError as e:
            result = {"content": [{"type": "text", "text": str(e)}], "isError": True}
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def serve():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            reply = handle(json.loads(line))
        except Exception as e:  # never die on one bad message
            reply = {"jsonrpc": "2.0", "id": None, "error": {"code": -32603, "message": str(e)}}
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


# ---------------------------------------------------------------------- CLI

def cli(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="todo-mcp.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add")
    a.add_argument("content")
    a.add_argument("--list")
    a.add_argument("--due")
    a.add_argument("--description")
    a.add_argument("--label", action="append", dest="labels", help="repeatable")
    l = sub.add_parser("list")
    l.add_argument("--list")
    sub.add_parser("lists")
    ns = ap.parse_args(argv)
    try:
        if ns.cmd == "add":
            print(add_todo(ns.content, ns.list, ns.due, ns.description, ns.labels))
        elif ns.cmd == "list":
            print(list_todos(ns.list))
        else:
            for e, n in list_names().items():
                print(f"{e}\t{n}" + ("\t(default)" if e == DEFAULT_LIST else ""))
    except TodoError as e:
        print("error:", e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(cli(sys.argv[1:]) if len(sys.argv) > 1 else serve())
