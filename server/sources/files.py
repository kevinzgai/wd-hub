"""任务 / 变更 / 产物 —— 三类基于 JSON 文件的数据源。

这些数据的产生方是 WorkBuddy 内部，schema 会随版本漂移，所以每个读取函数
都必须容忍：文件缺失、JSON 损坏、字段改名。宁可少算，不可崩溃。
"""

from __future__ import annotations

import json
import os
import re
import time
from typing import Any, Optional

from .db import ARTIFACT_DIR, CHANGES_DIR, TASKS_DIR, SourceError, local_midnight_ms

# 编译产物、依赖目录。今天改过的文件里这些占绝大多数（本机 build/ 有 700 个 .obj），
# 不排除的话"改动文件数"这个指标就废了。
NOISE_MARKERS = (
    os.sep + "build" + os.sep,
    os.sep + "node_modules" + os.sep,
    os.sep + "__pycache__" + os.sep,
    os.sep + ".git" + os.sep,
    os.sep + "managed_components" + os.sep,
    os.sep + "dist" + os.sep,
)

COMPLETED_STATES = ("completed", "done", "completed ", "Completed")
IN_PROGRESS_STATES = ("in_progress", "pending", "running")


def _read_json(path: str) -> Optional[dict[str, Any]]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        # 单个文件坏掉就跳过，不影响整体统计
        return None


# ---------------------------------------------------------------- 任务

# 任务文件有 300+ 个，tasks_stat 和 tasks_list 都扫一遍太浪费。
# 用目录 mtime 做失效判断：任何任务变更都会改动所在目录的 mtime。
# 缓存窗口 3s —— 比采集周期略长，接口请求基本都命中。
_TASKS_CACHE: dict[str, Any] = {"at": 0.0, "sig": "", "rows": []}
_TASKS_TTL = 3.0


def _tasks_signature() -> str:
    """用所有会话目录的 (名称, mtime, 文件数) 拼指纹，变了就重新扫。"""
    try:
        parts = []
        for conv in os.listdir(TASKS_DIR):
            d = os.path.join(TASKS_DIR, conv)
            if not os.path.isdir(d):
                continue
            try:
                parts.append(f"{conv}:{os.stat(d).st_mtime_ns}:{len(os.listdir(d))}")
            except OSError:
                continue
        return "|".join(sorted(parts))
    except OSError:
        return ""


def _scan_tasks() -> list[dict[str, Any]]:
    """把全部任务文件读成扁平列表，带缓存。"""
    now = time.time()
    sig = _tasks_signature()

    if (_TASKS_CACHE["rows"] and now - _TASKS_CACHE["at"] < _TASKS_TTL
            and sig == _TASKS_CACHE["sig"]):
        return _TASKS_CACHE["rows"]

    rows: list[dict[str, Any]] = []
    if os.path.isdir(TASKS_DIR):
        try:
            conv_dirs = os.listdir(TASKS_DIR)
        except OSError as e:
            raise SourceError(f"tasks-list-failed:{type(e).__name__}") from e

        for conv in conv_dirs:
            d = os.path.join(TASKS_DIR, conv)
            if not os.path.isdir(d):
                continue
            try:
                files = os.listdir(d)
            except OSError:
                continue
            for fn in files:
                if not fn.endswith(".json"):
                    continue
                data = _read_json(os.path.join(d, fn))
                if not isinstance(data, dict):
                    continue
                updated = data.get("updatedAt") or 0
                rows.append({
                    "id": str(data.get("id") or fn[:-5]),
                    "subject": str(data.get("subject") or ""),
                    "status": str(data.get("status") or "").strip().lower(),
                    "conversation": conv,
                    "updated_at": int(updated) if isinstance(updated, (int, float)) else 0,
                })

    rows.sort(key=lambda x: x["updated_at"], reverse=True)
    _TASKS_CACHE.update({"at": now, "sig": sig, "rows": rows})
    return rows


def tasks_stat() -> dict[str, int]:
    """任务统计：总数 / 累计完成 / 今日完成 / 进行中。"""
    if not os.path.isdir(TASKS_DIR):
        # 没装过任务功能：返回全 0，不算故障
        return {"total": 0, "completed": 0, "today_completed": 0, "in_progress": 0}

    midnight = local_midnight_ms()
    total = completed = today_completed = in_progress = 0

    for r in _scan_tasks():
        total += 1
        if r["status"] in ("completed", "done"):
            completed += 1
            if r["updated_at"] >= midnight:
                today_completed += 1
        elif r["status"] in IN_PROGRESS_STATES:
            in_progress += 1

    return {"total": total, "completed": completed,
            "today_completed": today_completed, "in_progress": in_progress}


def tasks_pending_by_conversation() -> set[str]:
    """还有 pending 任务的会话 id 集合。

    任务文件的目录名就是会话 id（实测 33/34 对得上），所以可以直接按
    目录聚合。给固件"等待操作"黄灯用：某个会话里活没干完（还有 pending
    任务）却停下来了，就是等人推进。
    """
    return {r["conversation"] for r in _scan_tasks() if r["status"] == "pending"}


def tasks_list(status: Optional[str] = None, since_ms: Optional[int] = None,
               limit: int = 50) -> tuple[int, list[dict[str, Any]]]:
    """任务明细。复用 _scan_tasks 的缓存，不重复扫盘。"""
    if not os.path.isdir(TASKS_DIR):
        return 0, []

    out = _scan_tasks()
    if status:
        want = status.lower()
        out = [r for r in out if r["status"] == want]
    if since_ms:
        out = [r for r in out if r["updated_at"] >= since_ms]

    # 加 unknown 兜底，避免状态为空时前端拿到空字符串
    items = [{**r, "status": r["status"] or "unknown"} for r in out[:limit]]
    return len(out), items


# ---------------------------------------------------------------- 变更统计


def _is_noise(path: str) -> bool:
    p = path.replace("/", os.sep)
    return any(m in p for m in NOISE_MARKERS)


_PROJ = re.compile(r"WorkBuddy[\\/]([^\\/]+)")


def _project_of(path: str) -> Optional[str]:
    m = _PROJ.search(path)
    return m.group(1) if m else None


def activity_stat(date_str: Optional[str] = None) -> dict[str, Any]:
    """今日代码改动：文件数 / 增删行 / 涉及项目。

    读 changes-index/*.json —— 每个文件是一次请求的变更索引。已排除编译产物，
    否则 build/ 下 700 个 .obj 会把数字彻底淹掉。
    """
    from datetime import datetime

    if not os.path.isdir(CHANGES_DIR):
        return {"date": date_str or datetime.now().strftime("%Y-%m-%d"),
                "files": 0, "additions": 0, "deletions": 0, "projects": []}

    midnight = local_midnight_ms()
    files_set: set[str] = set()
    additions = deletions = 0
    projects: set[str] = set()

    for fn in os.listdir(CHANGES_DIR):
        if not fn.endswith(".json"):
            continue
        idx = _read_json(os.path.join(CHANGES_DIR, fn))
        if not isinstance(idx, dict):
            continue
        for ch in idx.get("changes") or []:
            if not isinstance(ch, dict):
                continue
            ts = ch.get("createdAt") or 0
            if not isinstance(ts, (int, float)) or ts < midnight:
                continue
            for f in ch.get("files") or []:
                if not isinstance(f, dict):
                    continue
                fp = f.get("filePath") or ""
                if not fp or _is_noise(fp):
                    continue
                files_set.add(fp)
                additions += int(f.get("additions") or 0)
                deletions += int(f.get("deletions") or 0)
                proj = _project_of(fp)
                if proj:
                    projects.add(proj)

    return {
        "date": date_str or datetime.now().strftime("%Y-%m-%d"),
        "files": len(files_set),
        "additions": additions,
        "deletions": deletions,
        "projects": sorted(projects)[:8],
    }


# ---------------------------------------------------------------- 产物

# 文件名 → (mtime_ns, 解析结果)。只重读变动的文件。
_ARTIFACT_CACHE: dict[str, tuple[int, list[dict[str, Any]]]] = {}


def artifacts_list(limit: int = 20) -> tuple[int, list[dict[str, Any]]]:
    """产物列表。artifact-index 每个会话一个文件，内含 artifacts 数组。"""
    if not os.path.isdir(ARTIFACT_DIR):
        return 0, []

    # artifact-index 有 160+ 个文件，其中很多是历史产物且几乎不变。
    # 按 (文件名, mtime) 缓存已解析结果，只重读新变动的文件。
    cache = _ARTIFACT_CACHE
    fresh: list[dict[str, Any]] = []
    seen: set[str] = set()

    try:
        names = [f for f in os.listdir(ARTIFACT_DIR) if f.endswith(".json")]
    except OSError:
        return 0, []

    for fn in names:
        fp = os.path.join(ARTIFACT_DIR, fn)
        try:
            mtime = os.stat(fp).st_mtime_ns
        except OSError:
            continue
        seen.add(fn)
        hit = cache.get(fn)
        if hit and hit[0] == mtime:
            fresh.extend(hit[1])
            continue

        idx = _read_json(fp)
        rows: list[dict[str, Any]] = []
        if isinstance(idx, dict):
            for a in idx.get("artifacts") or []:
                if not isinstance(a, dict):
                    continue
                uri = a.get("uri") or ""
                rows.append({
                    "name": str(a.get("name") or ""),
                    "path": uri.replace("file:///", "").replace("%3A", ":").replace("%5C", "\\"),
                    "title": str(a.get("title") or a.get("name") or ""),
                    "mime": a.get("mimeType"),
                    "size": int(a.get("size") or 0),
                    "created_at": int(a.get("createdAt") or 0),
                })
        cache[fn] = (mtime, rows)
        fresh.extend(rows)

    # 清掉已消失文件的缓存，防止无限增长
    for gone in [k for k in cache if k not in seen]:
        cache.pop(gone, None)

    fresh.sort(key=lambda x: x["created_at"], reverse=True)
    return len(fresh), fresh[:limit]
