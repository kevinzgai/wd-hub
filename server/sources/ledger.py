"""任务账本 —— 让「完成数」不再依赖盘上现存文件。

## 为什么需要这层

WorkBuddy 把任务存在 `~/.workbuddy/tasks/<会话id>/<任务id>.json`，
**删除任务就是直接删文件**。而 `files.tasks_stat` 原先只看盘上现存的
json，用 `updatedAt` 判断"今日完成"。后果很致命：

    AI 收尾时清空任务清单  ──►  今日完成立刻归零

2026-10-26 实测就踩了：当天上午完成了 4 个任务，会话收尾时把 tasks 目录
清空，屏上 `done` 从 4 掉回 0。历史数据不存在"墓碑"，删了就找不回来。

## 账本做什么

每轮采集把见到的 `(会话, 任务id, status, updatedAt)` 攒进本地 JSON：

  · 任务文件消失 **不删记录** —— 历史保住；
  · 首次见到 completed/done 记 `completed_at = 当时的 updatedAt`；
    任务被重开又完成时，`completed_at` 刷新成本次完成时间；
  · 只是改描述导致的 updatedAt 漂移，不会重复计入"今日完成"；
  · 90 天前的记录修剪掉，避免账本无限增长（长期约 1 万条以内）。

## 性能

采集周期 2s，不能每轮读写盘。账本常驻内存，只有内容真变了才落盘
（原子替换）。进程重启从盘上恢复。
"""

from __future__ import annotations

import json
import logging
import os
from typing import Any, Iterable, Optional

log = logging.getLogger("board.ledger")

# server/sources/ledger.py -> server/state/task_ledger.json
SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER_PATH = os.path.join(SERVER_DIR, "state", "task_ledger.json")

# 与 files.IN_PROGRESS_STATES 互补。这里只关心"算不算完成"。
COMPLETED = ("completed", "done")

# 账本只留 90 天。屏上"累计"本来就是滚动口径，不是终身成就。
RETENTION_MS = 90 * 24 * 3600 * 1000

_CACHE: Optional[dict[str, Any]] = None


def _load() -> dict[str, Any]:
    try:
        with open(LEDGER_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return {"version": 1, "tasks": {}}
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        # 账本坏掉就重来，绝不能因此拖垮任务统计
        log.warning("账本读取失败，重建: %s", e)
        return {"version": 1, "tasks": {}}
    if not isinstance(data, dict) or not isinstance(data.get("tasks"), dict):
        return {"version": 1, "tasks": {}}
    return data


def _state() -> dict[str, Any]:
    global _CACHE
    if _CACHE is None:
        _CACHE = _load()
    return _CACHE


def _save(data: dict[str, Any]) -> None:
    tmp = LEDGER_PATH + ".tmp"
    try:
        os.makedirs(os.path.dirname(LEDGER_PATH), exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
        os.replace(tmp, LEDGER_PATH)          # 原子替换，半截文件不会外泄
    except OSError as e:
        log.warning("账本写入失败（内存态仍生效）: %s", e)


def _key(row: dict[str, Any]) -> str:
    return f'{row.get("conversation") or "?"}/{row.get("id") or "?"}'


def merge(rows: Iterable[dict[str, Any]], now_ms: int) -> dict[str, dict[str, Any]]:
    """把本轮盘上任务并入账本，返回账本内的全部记录。

    rows 来自 `files._scan_tasks()`，字段已归一：status 小写去空格、
    updated_at 为毫秒。只做增量合并，**不因为盘上消失而删除记录**。
    """
    data = _state()
    tasks: dict[str, dict[str, Any]] = data["tasks"]
    dirty = False
    alive: set[str] = set()

    for r in rows:
        k = _key(r)
        alive.add(k)
        status = r.get("status") or ""
        updated = int(r.get("updated_at") or 0)
        cur = tasks.get(k)

        if cur is None:
            rec: dict[str, Any] = {"status": status, "updated_at": updated}
            if status in COMPLETED:
                rec["completed_at"] = updated
            tasks[k] = rec
            dirty = True
            continue

        if updated > int(cur.get("updated_at") or 0):
            prev = cur.get("status")
            cur["status"] = status
            cur["updated_at"] = updated
            # 只有在"非完成 -> 完成"这个跃迁上才刷新完成时间。
            # 一直 completed 只是改描述，updatedAt 会漂移，不能重复计今日。
            if status in COMPLETED and prev not in COMPLETED:
                cur["completed_at"] = updated
            dirty = True

    # 修剪只清"盘上已经不存在、且超过保留期"的记录。
    # 盘上还在的哪怕很老也必须留 —— 否则每轮 merge 加进来又被剪掉，
    # dirty 永远是 True，等于每 2s 重写一次盘（实测有 31 个 5 月的任务踩过）。
    cutoff = now_ms - RETENTION_MS
    for gone in [k for k, v in tasks.items()
                 if k not in alive and int(v.get("updated_at") or 0) < cutoff]:
        tasks.pop(gone, None)
        dirty = True

    if dirty:
        _save(data)
    return tasks


def stats(tasks: dict[str, dict[str, Any]], midnight_ms: int) -> dict[str, int]:
    """账本口径的统计。

    total / completed 都是**历史累计**（含已被删除的任务），所以
    `completed` 可能大于盘上现存任务数 —— 这是有意的，屏幕不该因为
    清理任务清单就把今天的成果抹掉。
    """
    total = completed = today = 0
    for v in tasks.values():
        total += 1
        if v.get("status") in COMPLETED:
            completed += 1
            if int(v.get("completed_at") or 0) >= midnight_ms:
                today += 1
    return {"total": total, "completed": completed, "today_completed": today}
