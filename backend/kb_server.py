#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
kid_buddy 家庭账号后端（方案 A：只做身份与路由，不存故事正文）

能做的事：
  - 家庭注册、家长登录（PBKDF2 口令 + HttpOnly 会话 cookie）
  - 家长给自己家的板子开设备、生成 broker 账号和 topic
  - 企业端：家庭列表、停用账号、审计日志、数据导出、家庭删除
  - 导出 mosquitto 的 password / ACL 文件（每台设备只能收发自己的 topic）

刻意不做的事：
  - 不保存任何故事正文、对话内容。表里根本没有这些字段。
    故事仍然只存在板子 /data 和家长的手机里。

只依赖 Python 3.8+ 标准库。数据库是一个 SQLite 文件，默认 ./data/kb.db。

用法：
  python3 kb_server.py --init
  python3 kb_server.py --create-admin you@example.com '口令' --family 运营
  python3 kb_server.py --export-broker ./broker-out
  python3 kb_server.py --serve

环境变量：
  KB_DB             数据库路径（默认 data/kb.db）
  KB_HOST           监听地址（默认 127.0.0.1，公网部署改 0.0.0.0 或交给 nginx）
  KB_PORT           端口（默认 8080）
  KB_SECURE         =1 时给会话 cookie 加 Secure（上了 HTTPS 必须开）
  KB_PUBLIC_ORIGIN  允许的前端来源，如 https://kid.example.com
  KB_BROKER         板子要连的 broker，默认 123.56.1.47:1883
  KB_WS_URL         家长网页连的 WebSocket 地址，默认 ws://123.56.1.47:8083/mqtt
  KB_STATIC         静态目录（家长端网页），默认 ../phone-app
"""

import argparse
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import sys
import threading
import time
from collections import defaultdict, deque
from contextlib import closing
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ---------------------------------------------------------------- 配置

def env(name, default=""):
    v = os.environ.get(name)
    return v if v not in (None, "") else default


DB_PATH = env("KB_DB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "kb.db"))
HOST = env("KB_HOST", "127.0.0.1")
PORT = int(env("KB_PORT", "8080"))
SECURE_COOKIE = env("KB_SECURE", "0") == "1"
PUBLIC_ORIGIN = env("KB_PUBLIC_ORIGIN", "")
BROKER = env("KB_BROKER", "123.56.1.47:1883")
WS_URL = env("KB_WS_URL", "ws://123.56.1.47:8083/mqtt")
STATIC_DIR = env("KB_STATIC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "phone-app"))

SESSION_DAYS = 7
PBKDF2_ROUNDS = 200_000
MAX_DEVICES_PER_FAMILY = 4
MAX_BODY = 64 * 1024
LOGIN_WINDOW = 600          # 10 分钟
LOGIN_MAX_FAILS = 10        # 同一 IP 10 分钟内最多 10 次失败
AUDIT_KEEP = 200_000        # 审计行上限，超出按时间删旧

_lock = threading.Lock()


# ---------------------------------------------------------------- 数据库

SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS families (
    id          TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS accounts (
    id            TEXT PRIMARY KEY,
    family_id     TEXT REFERENCES families(id) ON DELETE CASCADE,
    email         TEXT NOT NULL UNIQUE,
    pw_hash       TEXT NOT NULL,
    role          TEXT NOT NULL DEFAULT 'parent',   -- parent | admin
    disabled      INTEGER NOT NULL DEFAULT 0,
    created_at    INTEGER NOT NULL,
    last_login_at INTEGER
);

CREATE TABLE IF NOT EXISTS sessions (
    token_hash  TEXT PRIMARY KEY,
    account_id  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL,
    ip          TEXT
);

CREATE TABLE IF NOT EXISTS devices (
    id              TEXT PRIMARY KEY,          -- 8 位十六进制
    family_id       TEXT NOT NULL REFERENCES families(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,
    broker_user     TEXT NOT NULL UNIQUE,      -- kb<id>
    broker_password TEXT NOT NULL,             -- 明文，要能再次展示给家长
    topic_in        TEXT NOT NULL,
    topic_out       TEXT NOT NULL,
    created_at      INTEGER NOT NULL
);

-- 审计只记「谁在什么时候做了什么」，不记故事内容
CREATE TABLE IF NOT EXISTS audit (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          INTEGER NOT NULL,
    actor_id    TEXT,
    family_id   TEXT,
    action      TEXT NOT NULL,
    detail      TEXT,
    ip          TEXT
);

CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit(ts);
CREATE INDEX IF NOT EXISTS idx_sessions_exp ON sessions(expires_at);
"""


def db():
    d = sqlite3.connect(DB_PATH, timeout=10)
    d.row_factory = sqlite3.Row
    d.execute("PRAGMA foreign_keys=ON")
    return d


def init_db():
    ddir = os.path.dirname(DB_PATH)
    os.makedirs(ddir, exist_ok=True)
    os.chmod(ddir, 0o700)
    with closing(db()) as d, d:
        d.executescript(SCHEMA)
    os.chmod(DB_PATH, 0o600)


def now():
    return int(time.time())


def audit(d, actor_id, family_id, action, detail="", ip=""):
    d.execute(
        "INSERT INTO audit(ts, actor_id, family_id, action, detail, ip) VALUES(?,?,?,?,?,?)",
        (now(), actor_id, family_id, action, detail[:500], ip),
    )
    d.execute(
        "DELETE FROM audit WHERE id <= (SELECT MAX(id) - ? FROM audit)",
        (AUDIT_KEEP,),
    )


# ---------------------------------------------------------------- 口令与会话

def hash_password(pw):
    salt = secrets.token_bytes(16)
    h = hashlib.pbkdf2_hmac("sha256", pw.encode("utf-8"), salt, PBKDF2_ROUNDS)
    return "pbkdf2_sha256$%d$%s$%s" % (PBKDF2_ROUNDS, salt.hex(), h.hex())


def verify_password(pw, stored):
    try:
        algo, rounds, salt_hex, want = stored.split("$")
        if algo != "pbkdf2_sha256":
            return False
        got = hashlib.pbkdf2_hmac(
            "sha256", pw.encode("utf-8"), bytes.fromhex(salt_hex), int(rounds)
        )
        return hmac.compare_digest(got.hex(), want)
    except Exception:
        return False


def token_hash(tok):
    return hashlib.sha256(tok.encode("ascii")).hexdigest()


def new_session(d, account_id, ip):
    tok = secrets.token_urlsafe(32)
    d.execute(
        "INSERT INTO sessions(token_hash, account_id, created_at, expires_at, ip) VALUES(?,?,?,?,?)",
        (token_hash(tok), account_id, now(), now() + SESSION_DAYS * 86400, ip),
    )
    d.execute("DELETE FROM sessions WHERE expires_at < ?", (now(),))
    return tok


def session_account(d, tok):
    if not tok:
        return None
    row = d.execute(
        """SELECT a.*, f.name AS family_name FROM sessions s
           JOIN accounts a ON a.id = s.account_id
           LEFT JOIN families f ON f.id = a.family_id
           WHERE s.token_hash = ? AND s.expires_at > ?""",
        (token_hash(tok), now()),
    ).fetchone()
    if row and row["disabled"]:
        return None
    return row


# ---------------------------------------------------------------- 设备

def make_device_ids(d):
    while True:
        did = secrets.token_hex(4)
        if not d.execute("SELECT 1 FROM devices WHERE id=?", (did,)).fetchone():
            return did


def device_public(row):
    return {
        "id": row["id"],
        "name": row["name"],
        "broker_user": row["broker_user"],
        "topic_in": row["topic_in"],
        "topic_out": row["topic_out"],
        "created_at": row["created_at"],
    }


def provision_payload(row):
    """给家长看的一条命令 + 网页直接用的一组连接参数。"""
    cmd = "set_mqtt %s %s %s %s %s %s" % (
        BROKER, "kbd-" + row["id"], row["topic_in"], row["topic_out"],
        row["broker_user"], row["broker_password"],
    )
    return {
        "device": device_public(row),
        "password": row["broker_password"],
        "command": cmd,
        "broker": {
            "url": WS_URL,
            "username": row["broker_user"],
            "password": row["broker_password"],
            "topic_in": row["topic_in"],
            "topic_out": row["topic_out"],
        },
    }


# ---------------------------------------------------------------- HTTP

_EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")
_fails = defaultdict(deque)   # ip -> 失败时间戳


def client_ip(h):
    # 如果前面有 nginx，用 X-Real-IP；否则用对端地址
    return (h.headers.get("X-Real-IP") or h.client_address[0] or "").strip()


def rate_limited(ip):
    with _lock:
        q = _fails[ip]
        cutoff = now() - LOGIN_WINDOW
        while q and q[0] < cutoff:
            q.popleft()
        return len(q) >= LOGIN_MAX_FAILS


def note_fail(ip):
    with _lock:
        _fails[ip].append(now())


def note_ok(ip):
    with _lock:
        _fails[ip].clear()


class Handler(BaseHTTPRequestHandler):
    server_version = "kb-server/1.0"
    protocol_version = "HTTP/1.1"

    # ---- 基础响应

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        pending, self._pending_headers = getattr(self, "_pending_headers", []), []
        self.send_response(code)
        for k, v in pending:
            self.send_header(k, v)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or []):
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def err(self, code, msg):
        self._send(code, {"error": msg})

    def json_body(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise ValueError("bad length")
        if n < 0 or n > MAX_BODY:
            raise ValueError("body too large")
        raw = self.rfile.read(n) if n else b"{}"
        try:
            obj = json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            raise ValueError("bad json")
        if not isinstance(obj, dict):
            raise ValueError("bad json")
        return obj

    def cookie_token(self):
        for part in (self.headers.get("Cookie") or "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "kb_session":
                return v
        return ""

    def auth(self, d):
        return session_account(d, self.cookie_token())

    def check_origin(self):
        """跨站请求的基本防线：有 Origin 就必须和配置一致。"""
        origin = self.headers.get("Origin")
        if origin and PUBLIC_ORIGIN and origin != PUBLIC_ORIGIN:
            return False
        return True

    # ---- 路由

    def do_GET(self):
        self.route("GET")

    def do_HEAD(self):
        # _send() 对 HEAD 只发头不发体，这里复用 GET 的路由
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_DELETE(self):
        self.route("DELETE")

    def route(self, method):
        u = urlparse(self.path)
        path = u.path
        try:
            if method == "GET" and not path.startswith("/api/"):
                return self.serve_static(path)
            if not self.check_origin():
                return self.err(HTTPStatus.FORBIDDEN, "来源不允许")
            with closing(db()) as d, d:
                return self.dispatch(method, path, parse_qs(u.query), d)
        except ValueError as e:
            return self.err(HTTPStatus.BAD_REQUEST, str(e))
        except BrokenPipeError:
            pass
        except Exception as e:
            sys.stderr.write("[kb-server] %s %s: %r\n" % (method, path, e))
            return self.err(HTTPStatus.INTERNAL_SERVER_ERROR, "服务器错误")

    def dispatch(self, method, path, qs, d):
        # ---- 公开
        if method == "POST" and path == "/api/register":
            return self.api_register(d)
        if method == "POST" and path == "/api/login":
            return self.api_login(d)
        if method == "POST" and path == "/api/logout":
            self.set_cookie("", 0)
            return self._send(200, {"ok": True})
        if method == "GET" and path == "/api/health":
            return self._send(200, {"ok": True, "time": now()})

        me = self.auth(d)
        if not me:
            return self.err(HTTPStatus.UNAUTHORIZED, "请先登录")

        # ---- 家长端
        if method == "GET" and path == "/api/me":
            devs = d.execute(
                "SELECT * FROM devices WHERE family_id=? ORDER BY created_at",
                (me["family_id"],),
            ).fetchall() if me["family_id"] else []
            return self._send(200, {
                "account": {"id": me["id"], "email": me["email"], "role": me["role"]},
                "family": {"id": me["family_id"], "name": me["family_name"]},
                "devices": [device_public(r) for r in devs],
            })

        if method == "GET" and path == "/api/devices":
            rows = d.execute(
                "SELECT * FROM devices WHERE family_id=? ORDER BY created_at",
                (me["family_id"],),
            ).fetchall()
            return self._send(200, {"devices": [device_public(r) for r in rows]})

        if method == "POST" and path == "/api/devices":
            return self.api_create_device(d, me)

        m = re.match(r"^/api/devices/([0-9a-f]{8})(/rotate|/provision)?$", path)
        if m:
            row = d.execute(
                "SELECT * FROM devices WHERE id=? AND family_id=?",
                (m.group(1), me["family_id"]),
            ).fetchone()
            if not row:
                return self.err(HTTPStatus.NOT_FOUND, "没有这台设备")
            sub = m.group(2)
            if method == "GET" and sub in (None, "/provision"):
                return self._send(200, provision_payload(row))
            if method == "POST" and sub == "/provision":
                return self._send(200, provision_payload(row))
            if method == "POST" and sub == "/rotate":
                pw = secrets.token_urlsafe(12)
                d.execute("UPDATE devices SET broker_password=? WHERE id=?", (pw, row["id"]))
                audit(d, me["id"], me["family_id"], "device.rotate", row["id"], client_ip(self))
                row = d.execute("SELECT * FROM devices WHERE id=?", (row["id"],)).fetchone()
                return self._send(200, provision_payload(row))
            if method == "DELETE" and sub is None:
                d.execute("DELETE FROM devices WHERE id=?", (row["id"],))
                audit(d, me["id"], me["family_id"], "device.delete", row["id"], client_ip(self))
                return self._send(200, {"ok": True})

        # ---- 企业端
        if path.startswith("/api/admin/"):
            if me["role"] != "admin":
                return self.err(HTTPStatus.FORBIDDEN, "需要管理员")
            return self.admin_dispatch(method, path, qs, d, me)

        return self.err(HTTPStatus.NOT_FOUND, "没有这个接口")

    # ---- 公开接口

    def api_register(self, d):
        ip = client_ip(self)
        if rate_limited(ip):
            return self.err(HTTPStatus.TOO_MANY_REQUESTS, "尝试太频繁，稍后再试")
        b = self.json_body()
        email = str(b.get("email", "")).strip().lower()
        pw = str(b.get("password", ""))
        family = str(b.get("family_name", "")).strip() or "我的家"
        if not _EMAIL_RE.match(email):
            return self.err(400, "邮箱格式不对")
        if len(pw) < 8:
            return self.err(400, "口令至少 8 位")
        if len(family) > 40:
            return self.err(400, "家庭名称太长")
        if d.execute("SELECT 1 FROM accounts WHERE email=?", (email,)).fetchone():
            return self.err(409, "这个邮箱已经注册过了")

        fid = "f" + secrets.token_hex(6)
        aid = "a" + secrets.token_hex(6)
        t = now()
        d.execute("INSERT INTO families(id, name, created_at) VALUES(?,?,?)", (fid, family, t))
        d.execute(
            "INSERT INTO accounts(id, family_id, email, pw_hash, role, created_at) VALUES(?,?,?,?,?,?)",
            (aid, fid, email, hash_password(pw), "parent", t),
        )
        audit(d, aid, fid, "register", family, ip)
        tok = new_session(d, aid, ip)
        self.set_cookie(tok, SESSION_DAYS * 86400)
        return self._send(200, {"ok": True, "family_id": fid, "role": "parent"})

    def api_login(self, d):
        ip = client_ip(self)
        if rate_limited(ip):
            return self.err(HTTPStatus.TOO_MANY_REQUESTS, "尝试太频繁，稍后再试")
        b = self.json_body()
        email = str(b.get("email", "")).strip().lower()
        pw = str(b.get("password", ""))
        row = d.execute("SELECT * FROM accounts WHERE email=?", (email,)).fetchone()
        if not row or not verify_password(pw, row["pw_hash"]):
            note_fail(ip)
            audit(d, None, row["family_id"] if row else None, "login.fail", email[:80], ip)
            return self.err(HTTPStatus.UNAUTHORIZED, "邮箱或口令不对")
        if row["disabled"]:
            audit(d, row["id"], row["family_id"], "login.disabled", email[:80], ip)
            return self.err(HTTPStatus.FORBIDDEN, "账号已停用")
        note_ok(ip)
        d.execute("UPDATE accounts SET last_login_at=? WHERE id=?", (now(), row["id"]))
        tok = new_session(d, row["id"], ip)
        audit(d, row["id"], row["family_id"], "login", "", ip)
        self.set_cookie(tok, SESSION_DAYS * 86400)
        return self._send(200, {"ok": True, "role": row["role"]})

    # ---- 家长端设备

    def api_create_device(self, d, me):
        b = self.json_body()
        name = str(b.get("name", "")).strip() or "我的板子"
        if len(name) > 40:
            return self.err(400, "名字太长")
        n = d.execute(
            "SELECT COUNT(*) AS c FROM devices WHERE family_id=?", (me["family_id"],)
        ).fetchone()["c"]
        if n >= MAX_DEVICES_PER_FAMILY:
            return self.err(409, "一个家庭最多 %d 台设备" % MAX_DEVICES_PER_FAMILY)
        did = make_device_ids(d)
        user = "kb" + did
        pw = secrets.token_urlsafe(12)
        d.execute(
            """INSERT INTO devices(id, family_id, name, broker_user, broker_password,
                                   topic_in, topic_out, created_at)
               VALUES(?,?,?,?,?,?,?,?)""",
            (did, me["family_id"], name, user, pw,
             "kb/%s/in" % user, "kb/%s/out" % user, now()),
        )
        audit(d, me["id"], me["family_id"], "device.create", name, client_ip(self))
        row = d.execute("SELECT * FROM devices WHERE id=?", (did,)).fetchone()
        return self._send(200, provision_payload(row))

    # ---- 企业端

    def admin_dispatch(self, method, path, qs, d, me):
        if method == "GET" and path == "/api/admin/families":
            rows = d.execute(
                """SELECT f.id, f.name, f.created_at,
                          (SELECT COUNT(*) FROM devices v WHERE v.family_id=f.id) AS devices,
                          (SELECT COUNT(*) FROM accounts a WHERE a.family_id=f.id) AS accounts
                   FROM families f ORDER BY f.created_at DESC"""
            ).fetchall()
            out = []
            for r in rows:
                accounts = d.execute(
                    "SELECT id, email, disabled, last_login_at FROM accounts WHERE family_id=?",
                    (r["id"],),
                ).fetchall()
                out.append({
                    "id": r["id"], "name": r["name"], "created_at": r["created_at"],
                    "devices": r["devices"], "accounts": [dict(a) for a in accounts],
                })
            return self._send(200, {"families": out})

        if method == "GET" and path == "/api/admin/stats":
            def one(sql, *args):
                return d.execute(sql, args).fetchone()[0]
            return self._send(200, {
                "families": one("SELECT COUNT(*) FROM families"),
                "accounts": one("SELECT COUNT(*) FROM accounts WHERE role='parent'"),
                "devices": one("SELECT COUNT(*) FROM devices"),
                "logins_7d": one("SELECT COUNT(*) FROM audit WHERE action='login' AND ts>?", now() - 7 * 86400),
            })

        if method == "GET" and path == "/api/admin/audit":
            n = min(int(qs.get("limit", ["100"])[0] or 100), 1000)
            rows = d.execute(
                "SELECT * FROM audit ORDER BY id DESC LIMIT ?", (n,)
            ).fetchall()
            return self._send(200, {"audit": [dict(r) for r in rows]})

        m = re.match(r"^/api/admin/families/(f[0-9a-f]{12})(/export|/purge)?$", path)
        if m:
            fid = m.group(1)
            fam = d.execute("SELECT * FROM families WHERE id=?", (fid,)).fetchone()
            if not fam:
                return self.err(404, "没有这个家庭")
            sub = m.group(2)
            if method == "GET" and sub == "/export":
                # 只导出账号与设备元数据。系统里没有故事正文可导。
                accs = d.execute(
                    "SELECT email, role, disabled, created_at, last_login_at FROM accounts WHERE family_id=?",
                    (fid,),
                ).fetchall()
                devs = d.execute(
                    "SELECT id, name, broker_user, created_at FROM devices WHERE family_id=?",
                    (fid,),
                ).fetchall()
                audit(d, me["id"], fid, "admin.export", "", client_ip(self))
                return self._send(200, {
                    "family": {"id": fid, "name": fam["name"], "created_at": fam["created_at"]},
                    "accounts": [dict(a) for a in accs],
                    "devices": [dict(v) for v in devs],
                    "note": "本系统不保存故事正文；故事只在板子和家长手机上。",
                })
            if method == "POST" and sub == "/purge":
                b = self.json_body()
                if b.get("confirm") != "DELETE":
                    return self.err(400, "需要 confirm=DELETE")
                d.execute("DELETE FROM sessions WHERE account_id IN (SELECT id FROM accounts WHERE family_id=?)", (fid,))
                d.execute("DELETE FROM devices WHERE family_id=?", (fid,))
                d.execute("DELETE FROM accounts WHERE family_id=?", (fid,))
                d.execute("DELETE FROM families WHERE id=?", (fid,))
                audit(d, me["id"], None, "admin.purge", fid, client_ip(self))
                return self._send(200, {"ok": True})

        m = re.match(r"^/api/admin/accounts/(a[0-9a-f]{12})/disable$", path)
        if m and method == "POST":
            aid = m.group(1)
            b = self.json_body()
            disabled = 1 if b.get("disabled") else 0
            row = d.execute("SELECT * FROM accounts WHERE id=?", (aid,)).fetchone()
            if not row:
                return self.err(404, "没有这个账号")
            d.execute("UPDATE accounts SET disabled=? WHERE id=?", (disabled, aid))
            if disabled:
                d.execute("DELETE FROM sessions WHERE account_id=?", (aid,))
            audit(d, me["id"], row["family_id"], "admin.disable" if disabled else "admin.enable", aid, client_ip(self))
            return self._send(200, {"ok": True, "disabled": bool(disabled)})

        return self.err(404, "没有这个接口")

    # ---- 静态文件（家长端网页）

    def serve_static(self, path):
        # /admin/ 走企业端页面（nginx 部署时可以改成 alias，两条路都行）
        if path.startswith("/admin/"):
            root = os.path.realpath(
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "admin"))
            rel = path[len("/admin/"):] or "admin.html"
        else:
            root = os.path.realpath(STATIC_DIR)
            rel = path.lstrip("/") or "index.html"
        if not os.path.isdir(root):
            return self.err(404, "静态目录不存在")
        full = os.path.realpath(os.path.join(root, rel))
        if not full.startswith(root + os.sep) and full != root:
            return self.err(403, "不允许")
        if os.path.isdir(full):
            full = os.path.join(full, "index.html")
        if not os.path.isfile(full):
            return self.err(404, "没有这个文件")
        ctype = {
            ".html": "text/html; charset=utf-8",
            ".js": "application/javascript; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".json": "application/json; charset=utf-8",
            ".png": "image/png",
            ".svg": "image/svg+xml",
            ".ico": "image/x-icon",
            ".webmanifest": "application/manifest+json",
        }.get(os.path.splitext(full)[1].lower(), "application/octet-stream")
        with open(full, "rb") as f:
            body = f.read()
        self._send(200, body, ctype)

    # ---- cookie

    def set_cookie(self, tok, max_age):
        parts = [
            "kb_session=%s" % tok,
            "Path=/",
            "HttpOnly",
            "SameSite=Lax",
            "Max-Age=%d" % max_age,
        ]
        if SECURE_COOKIE:
            parts.append("Secure")
        # _send() 会把它带上；这里不能直接 send_header，状态行还没写。
        self._pending_headers = getattr(self, "_pending_headers", [])
        self._pending_headers.append(("Set-Cookie", "; ".join(parts)))


# ---------------------------------------------------------------- 命令行

def export_broker(outdir):
    """生成 mosquitto 用的 password / ACL 文件。

    password_file 需要哈希，交给 mosquitto_passwd 生成；这里输出明文清单，
    由 deploy/apply-broker.sh 转成哈希并 reload。
    """
    os.makedirs(outdir, exist_ok=True)
    with closing(db()) as d, d:
        rows = d.execute("SELECT * FROM devices ORDER BY created_at").fetchall()

    plain = os.path.join(outdir, "passwd_plain.txt")
    acl = os.path.join(outdir, "acl.conf")
    devs = os.path.join(outdir, "devices.tsv")

    with open(plain, "w", encoding="utf-8") as f:
        for r in rows:
            f.write("%s:%s\n" % (r["broker_user"], r["broker_password"]))
    with open(devs, "w", encoding="utf-8") as f:
        f.write("family_id\tdevice_id\tname\tuser\ttopic_in\ttopic_out\n")
        for r in rows:
            f.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (
                r["family_id"], r["id"], r["name"], r["broker_user"],
                r["topic_in"], r["topic_out"]))
    with open(acl, "w", encoding="utf-8") as f:
        f.write("# 由 backend/kb_server.py --export-broker 生成\n")
        f.write("# 每台设备只能读写自己那一对 topic；%u 是 broker 用户名。\n")
        f.write("# 板子和手机共用同一组设备凭据，所以 in/out 都要 readwrite：\n")
        f.write("# 板子往 out 发、手机从 out 收；手机往 in 发、板子从 in 收。\n")
        f.write("pattern readwrite kb/%u/in\n")
        f.write("pattern readwrite kb/%u/out\n")

    os.chmod(plain, 0o600)
    print("已生成：")
    print("  %s   （明文口令，仅用于生成 password_file，用完删掉）" % plain)
    print("  %s   （ACL，直接放进 mosquitto.conf 的 acl_file）" % acl)
    print("  %s   （设备清单，给运维/企业端对账）" % devs)
    print("设备数：%d" % len(rows))


def create_admin(email, password, family_name):
    email = email.strip().lower()
    if not _EMAIL_RE.match(email):
        sys.exit("邮箱格式不对")
    if len(password) < 8:
        sys.exit("口令至少 8 位")
    with closing(db()) as d, d:
        if d.execute("SELECT 1 FROM accounts WHERE email=?", (email,)).fetchone():
            sys.exit("这个邮箱已经存在")
        fid = "f" + secrets.token_hex(6)
        aid = "a" + secrets.token_hex(6)
        d.execute("INSERT INTO families(id, name, created_at) VALUES(?,?,?)",
                  (fid, family_name or "运营", now()))
        d.execute(
            "INSERT INTO accounts(id, family_id, email, pw_hash, role, created_at) VALUES(?,?,?,?,?,?)",
            (aid, fid, email, hash_password(password), "admin", now()),
        )
        audit(d, aid, fid, "admin.create", email, "")
    print("管理员已创建：%s" % email)


def main():
    ap = argparse.ArgumentParser(description="kid_buddy 家庭账号后端")
    ap.add_argument("--init", action="store_true", help="建库")
    ap.add_argument("--create-admin", nargs=2, metavar=("EMAIL", "PASSWORD"))
    ap.add_argument("--family", default="运营")
    ap.add_argument("--export-broker", metavar="DIR")
    ap.add_argument("--serve", action="store_true", help="启动 HTTP 服务")
    args = ap.parse_args()

    if args.init:
        init_db()
        print("数据库：%s" % DB_PATH)
    if args.create_admin:
        init_db()
        create_admin(args.create_admin[0], args.create_admin[1], args.family)
    if args.export_broker:
        export_broker(args.export_broker)
    if args.serve:
        init_db()
        httpd = ThreadingHTTPServer((HOST, PORT), Handler)
        print("kb-server 监听 http://%s:%d（静态目录 %s）" % (HOST, PORT, os.path.realpath(STATIC_DIR)))
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n退出")
    if not any([args.init, args.create_admin, args.export_broker, args.serve]):
        ap.print_help()


if __name__ == "__main__":
    main()
