#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WorkBuddy 状态看板 · 数据桥

只读扫描 WorkBuddy 本地 SQLite，聚合出看板需要的指标，
通过局域网 HTTP 提供给 ESP32 设备轮询。

设计要点：
- 只读打开数据库（mode=ro），绝不写、绝不锁
- WAL 模式下 SQLite 允许并发读，不影响 WorkBuddy 正常运行
- 零第三方依赖，仅需 Python 3.10+
- 输出为固定 schema，设备端解析简单

启动：python server.py [--port 8080]
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

DB_PATH = Path(os.path.expanduser("~")) / ".workbuddy" / "workbuddy.db"

# 看板一次性显示的会话条数上限（240x240 屏幕实际只能放 3 条）
MAX_SESSIONS = 3
# 标题截断长度，超出后设备端显示省略号
TITLE_MAX = 22
# 会话最后活动超过这个秒数就视为僵尸会话，不再算作"正在工作"
# WorkBuddy 崩溃或强退后 status 会留在 working/active，必须靠时间过滤
STALE_SECONDS = 2 * 3600


# ---------------------------------------------------------------------------
# 数据聚合
# ---------------------------------------------------------------------------

def _ms_to_dt(ms: int | None) -> datetime | None:
    if not ms:
        return None
    return datetime.fromtimestamp(ms / 1000.0)


def _elapsed_text(start_ms: int | None, now_ms: int) -> str:
    """把毫秒时间戳转成 '12m' / '2h3m' 这类紧凑时长，屏幕友好。"""
    if not start_ms:
        return "-"
    delta = max(0, now_ms - start_ms) // 1000
    if delta < 60:
        return f"{delta}s"
    if delta < 3600:
        return f"{delta // 60}m"
    hours = delta // 3600
    minutes = (delta % 3600) // 60
    return f"{hours}h{minutes}m" if minutes else f"{hours}h"


def _truncate(text: str | None, limit: int = TITLE_MAX) -> str:
    if not text:
        return "(无标题)"
    text = text.strip().replace("\n", " ")
    if len(text) <= limit:
        return text
    return text[: limit - 1] + "…"


def collect(db_path: Path = DB_PATH) -> dict:
    """
    读取数据库并聚合成看板数据。
    任何异常都吞掉并放进 error 字段，保证设备端永远能拿到合法 JSON。
    """
    now_ms = int(time.time() * 1000)
    # 今日 00:00 的毫秒时间戳
    today_start = int(
        datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000
    )

    payload: dict = {
        "ok": True,
        "ts": now_ms,
        "clock": datetime.now().strftime("%H:%M"),
        "date": datetime.now().strftime("%m-%d"),
        "error": None,
        "summary": {
            "working": 0,
            "today_done": 0,
            "automations": 0,
            "skills_today": 0,
        },
        "sessions": [],
        "updated": datetime.now().strftime("%H:%M:%S"),
    }

    if not db_path.exists():
        payload["ok"] = False
        payload["error"] = f"db not found: {db_path}"
        return payload

    try:
        # 只读 + WAL 并发读，不干扰 WorkBuddy 主进程
        uri = f"file:{db_path.as_posix()}?mode=ro"
        conn = sqlite3.connect(uri, uri=True, timeout=3.0)
        conn.row_factory = sqlite3.Row
        try:
            cur = conn.cursor()

            # --- 正在工作的会话 ---
            # 关键：last_activity_at 必须够新。WorkBuddy 异常退出会把 status
            # 永久留在 working，只靠 status 过滤会显示几天前的僵尸会话。
            stale_cutoff = now_ms - STALE_SECONDS * 1000
            rows = cur.execute(
                """
                SELECT id, title, custom_title, cwd, model, status,
                       created_at, last_activity_at
                FROM sessions
                WHERE deleted_at IS NULL
                  AND status IN ('working', 'active')
                  AND last_activity_at >= ?
                ORDER BY last_activity_at DESC
                LIMIT ?
                """,
                (stale_cutoff, MAX_SESSIONS),
            ).fetchall()

            for r in rows:
                name = r["custom_title"] or r["title"]
                project = Path(r["cwd"]).name if r["cwd"] else "-"
                model = (r["model"] or "-").split(":")[-1]
                payload["sessions"].append(
                    {
                        "title": _truncate(name),
                        "project": _truncate(project, 14),
                        "model": _truncate(model, 20),
                        "elapsed": _elapsed_text(r["last_activity_at"], now_ms),
                        "status": r["status"],
                    }
                )

            payload["summary"]["working"] = len(payload["sessions"])

            # --- 今日完成数 ---
            payload["summary"]["today_done"] = cur.execute(
                """
                SELECT COUNT(*) FROM sessions
                WHERE deleted_at IS NULL
                  AND status IN ('completed', 'archived')
                  AND last_activity_at >= ?
                """,
                (today_start,),
            ).fetchone()[0]

            # --- 启用的自动化 ---
            payload["summary"]["automations"] = cur.execute(
                "SELECT COUNT(*) FROM automations WHERE status = 'ACTIVE'"
            ).fetchone()[0]

        finally:
            conn.close()

        # --- 今日技能使用数（来自 usage-log.json，独立于数据库）---
        payload["summary"]["skills_today"] = _count_skills_today()

    except sqlite3.Error as exc:
        payload["ok"] = False
        payload["error"] = f"sqlite: {exc}"
    except Exception as exc:  # noqa: BLE001 - 看板服务必须永不崩
        payload["ok"] = False
        payload["error"] = f"{type(exc).__name__}: {exc}"

    return payload


def _count_skills_today() -> int:
    """统计今天用过的技能数。usage-log.json 结构简单且是原子写入，可直接读。"""
    log = Path(os.path.expanduser("~")) / ".workbuddy" / "usage-log.json"
    if not log.exists():
        return 0
    try:
        data = json.loads(log.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return 0
    today = datetime.now().strftime("%Y-%m-%d")
    count = 0
    for bucket in ("skills", "connectors"):
        for item in (data.get(bucket) or {}).values():
            dates = [item.get("lastUsedDate")] + (item.get("recentDates") or [])
            if today in dates:
                count += 1
    return count


# ---------------------------------------------------------------------------
# HTTP 服务
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "WorkBuddyBoard/1.0"

    def _send_json(self, obj: dict, code: int = 200) -> None:
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?")[0].rstrip("/") or "/"

        if path in ("/status", "/"):
            self._send_json(collect())
        elif path == "/health":
            self._send_json({"ok": True, "db": str(DB_PATH), "exists": DB_PATH.exists()})
        else:
            self._send_json({"ok": False, "error": "not found"}, code=404)

    def log_message(self, fmt: str, *args) -> None:
        # 设备每秒轮询一次，默认日志会把控制台刷爆，只记录错误
        if args and str(args[1]).startswith(("4", "5")):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def local_ips() -> list[str]:
    """尽力枚举本机局域网 IP，方便直接告诉用户该在设备里填哪个地址。"""
    ips: list[str] = []
    try:
        import socket

        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except Exception:  # noqa: BLE001
        pass
    return ips


def main() -> int:
    parser = argparse.ArgumentParser(description="WorkBuddy dashboard data bridge")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="0.0.0.0", help="0.0.0.0 让局域网设备可访问")
    parser.add_argument("--once", action="store_true", help="只打印一次聚合结果，用于调试")
    args = parser.parse_args()

    if args.once:
        print(json.dumps(collect(), ensure_ascii=False, indent=2))
        return 0

    if not DB_PATH.exists():
        print(f"[!] 找不到数据库：{DB_PATH}", file=sys.stderr)
        print("    确认 WorkBuddy 已启动过，或调整 DB_PATH。", file=sys.stderr)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print("=" * 56)
    print("  WorkBuddy 看板数据桥")
    print("=" * 56)
    print(f"  数据库 : {DB_PATH}")
    print(f"  监听   : {args.host}:{args.port}")
    for ip in local_ips():
        print(f"  设备填 : http://{ip}:{args.port}/status")
    print("=" * 56)
    print("  Ctrl+C 停止")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
