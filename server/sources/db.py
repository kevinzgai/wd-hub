"""WorkBuddy 内置数据的只读访问。

设计要点：
  1. 一律 `mode=ro` 打开 —— 物理上禁止写入。库是 WAL 模式，WorkBuddy 主进程
     正在读写，任何写操作都可能让主进程卡住或触发锁冲突。
  2. 每次查询新建连接并关闭。连接很便宜，但持有一个长连接会在 WorkBuddy
     做 checkpoint 时互相等待。实测采集周期 2s，建连开销可忽略。
  3. 所有公开函数只抛 SourceError，由 collector 统一降级，不向 HTTP 层泄漏异常。
"""

from __future__ import annotations

import os
import sqlite3
import time
from datetime import datetime
from typing import Any, Optional

# ---------------------------------------------------------------- 路径

WB_HOME = os.path.expanduser("~/.workbuddy")
DB_PATH = os.path.join(WB_HOME, "workbuddy.db")
TASKS_DIR = os.path.join(WB_HOME, "tasks")
CHANGES_DIR = os.path.join(WB_HOME, "changes-index")
ARTIFACT_DIR = os.path.join(WB_HOME, "artifact-index")


class SourceError(Exception):
    """数据源不可用。message 会进 /status 的 error 字段，必须短且无中文。"""


# ---------------------------------------------------------------- 连接


def _connect() -> sqlite3.Connection:
    if not os.path.isfile(DB_PATH):
        raise SourceError("db-not-found")
    try:
        conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=2.0)
        conn.row_factory = sqlite3.Row
        # 被 WorkBuddy 占着锁时最多等 200ms，超时就跳过本轮，不拖累请求
        conn.execute("PRAGMA busy_timeout=200")
        return conn
    except sqlite3.Error as e:
        raise SourceError(f"db-open-failed:{type(e).__name__}") from e


def _query(sql: str, params: tuple = ()) -> list[sqlite3.Row]:
    conn = _connect()
    try:
        return conn.execute(sql, params).fetchall()
    except sqlite3.Error as e:
        raise SourceError(f"db-query-failed:{type(e).__name__}") from e
    finally:
        conn.close()


# ---------------------------------------------------------------- 时间


def local_midnight_ms() -> int:
    """今日 00:00:00 的毫秒时间戳（本地时区）。

    所有库内时间戳都是毫秒 epoch。不能用 utcfromtimestamp 截断 ——
    CST-8 下会差 8 小时，凌晨统计直接错一天。
    """
    return int(datetime.now().astimezone().replace(
        hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)


# ---------------------------------------------------------------- 会话

# 状态是大小写混用的（实测同时有 completed/Completed、working/active），
# 一律 LOWER() 归一；白名单外的状态一律不计数。
WORKING_STATES = ("working", "active")
ACTIVE_WINDOW_MS = 30 * 60 * 1000


def sessions_working() -> int:
    """近 30 分钟活跃的交互式会话数。

    三个过滤条件缺一不可：
      deleted_at IS NULL        软删除的会话不算（实测 103 条）
      is_background_automation  自动跑出来的不算"人在干活"（实测 59 条）
      30 分钟窗口                status 会残留，靠窗口自愈
    """
    now = int(time.time() * 1000)
    rows = _query(
        """SELECT COUNT(*) FROM sessions
           WHERE LOWER(status) IN ('working','active')
             AND deleted_at IS NULL
             AND (is_background_automation IS NULL OR is_background_automation = 0)
             AND last_activity_at >= ?""",
        (now - ACTIVE_WINDOW_MS,),
    )
    return int(rows[0][0]) if rows else 0


def sessions_active(limit: int = 20, include_background: bool = False) -> list[dict[str, Any]]:
    """活跃会话列表，按最近活动倒序。"""
    now = int(time.time() * 1000)
    cond_bg = "" if include_background else \
        "AND (is_background_automation IS NULL OR is_background_automation = 0)"
    rows = _query(
        f"""SELECT id, title, custom_title, cwd, status, model,
                   last_activity_at, is_background_automation
            FROM sessions
            WHERE LOWER(status) IN ('working','active')
              AND deleted_at IS NULL
              AND last_activity_at >= ?
              {cond_bg}
            ORDER BY last_activity_at DESC
            LIMIT ?""",
        (now - ACTIVE_WINDOW_MS, limit),
    )
    return [dict(r) for r in rows]


def sessions_recent(limit: int = 20, state: Optional[str] = None,
                    project: Optional[str] = None) -> tuple[int, list[dict[str, Any]]]:
    """会话分页（不限于活跃窗口），用于 /api/v1/sessions。

    project 过滤只能在内存里做：cwd 末级目录要同时处理 `\\` 和 `/` 两种分隔符，
    SQL 里没有可移植的做法。项目数量是几十级，全量取回再筛没有性能问题。
    """
    from ..models import project_name       # 放函数内避免包初始化时的循环导入

    where = ["deleted_at IS NULL"]
    params: list[Any] = []
    if state:
        where.append("LOWER(status) = ?")
        params.append(state.lower())
    clause = " AND ".join(where)

    rows = _query(
        f"""SELECT id, title, custom_title, cwd, status, model,
                   last_activity_at, is_background_automation
            FROM sessions WHERE {clause}
            ORDER BY last_activity_at DESC""",
        tuple(params),
    )
    items = [dict(r) for r in rows]
    if project:
        items = [i for i in items if project_name(i.get("cwd")).lower() == project.lower()]
    return len(items), items[:limit]


# ---------------------------------------------------------------- 自动化

def automations_active() -> int:
    """已启用的自动化数。

    ⚠ 不要加 deleted_at IS NULL：实测 5 条记录 status 全为 ACTIVE，
    但 deleted_at 全部非空。该字段在这张表里是同步占位，不是删除标记，
    加过滤会得到 0（与事实相反）。
    """
    rows = _query("SELECT COUNT(*) FROM automations WHERE status = 'ACTIVE'")
    return int(rows[0][0]) if rows else 0


def automation_runs_stats() -> dict[str, Any]:
    """自动化运行态：今日次数 / 失败 / 待审阅 / 最近运行时间。"""
    midnight = local_midnight_ms()
    rows = _query(
        """SELECT
             SUM(CASE WHEN created_at >= ? THEN 1 ELSE 0 END) AS runs_today,
             SUM(CASE WHEN created_at >= ? AND result_success = 0 THEN 1 ELSE 0 END) AS failed_today,
             SUM(CASE WHEN status = 'PENDING_REVIEW' THEN 1 ELSE 0 END) AS pending,
             MAX(created_at) AS last_run
           FROM automation_runs""",
        (midnight, midnight),
    )
    r = rows[0] if rows else None
    return {
        "runs_today": int(r["runs_today"] or 0) if r else 0,
        "failed_today": int(r["failed_today"] or 0) if r else 0,
        "pending_review": int(r["pending"] or 0) if r else 0,
        "last_run_at": int(r["last_run"]) if r and r["last_run"] else None,
    }


def automations_list() -> list[dict[str, Any]]:
    """自动化列表 + 每个最近 5 次运行。"""
    autos = _query(
        """SELECT id, name, status, schedule_type, rrule, next_run_at, last_run_at
           FROM automations WHERE status = 'ACTIVE' ORDER BY last_run_at DESC"""
    )
    running = {r["automation_id"] for r in
               _query("SELECT automation_id FROM automation_runtime_state WHERE running = 1")}
    out = []
    for a in autos:
        runs = _query(
            """SELECT thread_id, status, result_success, created_at
               FROM automation_runs WHERE automation_id = ?
               ORDER BY created_at DESC LIMIT 5""",
            (a["id"],),
        )
        item = dict(a)
        item["running"] = a["id"] in running
        item["recent_runs"] = [
            {"thread_id": r["thread_id"], "status": r["status"],
             "success": None if r["result_success"] is None else bool(r["result_success"]),
             "created_at": r["created_at"]}
            for r in runs
        ]
        out.append(item)
    return out
