"""任务 / 变更 / 产物 —— 三类基于 JSON 文件的数据源。

这些数据的产生方是 WorkBuddy 内部，schema 会随版本漂移，所以每个读取函数
都必须容忍：文件缺失、JSON 损坏、字段改名。宁可少算，不可崩溃。
"""

from __future__ import annotations

import json
import logging
import os
import re
import time
from typing import Any, Optional
from urllib.parse import unquote

from . import ledger
from .db import ARTIFACT_DIR, CHANGES_DIR, TASKS_DIR, SourceError, local_midnight_ms

# ⚠ 顶层导入，不要放函数内。
# models.py 只依赖 re/typing/pydantic，不 import sources —— 没有循环依赖。
# 原来放在 _project_of() 和 activity_stat() 的函数体里（前者每个文件调用一次，
# 后者在 for 循环体内 —— 每个文件都执行一次 import），纯属白扔性能。
from ..models import is_sandbox_dir, project_name

log = logging.getLogger("board.files")

# 编译产物、依赖目录。今天改过的文件里这些占绝大多数（本机 build/ 有 700 个 .obj），
# 不排除的话"改动文件数"这个指标就废了。
#
# ⚠ 一律用 "/" 做归一后比对，不要用 os.sep：
#   os.sep 跟着平台走，会导致同一份路径在 Windows / macOS 上判定结果不同
#   （实测 `C:\proj\build\a.obj` 在 macOS 上会被判成"正常文件"）。
#   改动文件路径可能来自另一个平台，判定必须与当前平台无关。
NOISE_MARKERS = (
    "/build/",
    "/node_modules/",
    "/__pycache__/",
    "/.git/",
    "/managed_components/",
    "/dist/",
)

# 完成态判定统一由 ledger.COMPLETED 提供，别在两处各写一份。
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
    """任务统计：总数 / 累计完成 / 今日完成 / 进行中。

    total / completed / today_completed 走账本（ledger.py）：任务文件被删除
    后历史不丢，否则清空任务清单一瞬间"今日完成"就归零（2026-10-26 实测踩过）。

    in_progress 只统计盘上实时状态 —— 任务删了就不该再算"进行中"，
    否则账本里会永久挂着几十个僵尸 pending。
    """
    rows = _scan_tasks()
    in_progress = sum(1 for r in rows if r["status"] in IN_PROGRESS_STATES)

    try:
        led = ledger.stats(
            ledger.merge(rows, int(time.time() * 1000)), local_midnight_ms())
        return {
            "total": max(led["total"], len(rows)),
            "completed": led["completed"],
            "today_completed": led["today_completed"],
            "in_progress": in_progress,
        }
    except Exception as e:                          # noqa: BLE001
        # 账本出任何问题都退回"只看盘上"：宁可少算，不能整块任务统计挂掉。
        log.warning("账本统计失败，退回盘上统计: %s", e)
        midnight = local_midnight_ms()
        completed = today = 0
        for r in rows:
            if r["status"] in ledger.COMPLETED:
                completed += 1
                if r["updated_at"] >= midnight:
                    today += 1
        return {"total": len(rows), "completed": completed,
                "today_completed": today, "in_progress": in_progress}


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
    """路径是否落在编译产物 / 依赖目录里。

    反斜杠和正斜杠都归一成 "/" 再比，Windows 路径在 macOS 上也能正确判定。
    """
    p = path.replace("\\", "/")
    return any(m in p for m in NOISE_MARKERS)


# ---------------------------------------------------------------- 项目名归属
#
# 改动文件在磁盘上的绝对路径**推不出**项目名：
#   · 真实项目在 ~/WebstormProjects/<name>/，正则按目录名抓会漏得一干二净；
#   · 临时会话在 ~/WorkBuddy/<时间戳>/，抓到的是 "2026-10-25-23-57-47" 这种
#     时间戳目录，信息量为零。
#
# 可靠做法：**changes-index 的文件名就是会话 id**（实测 26/26 全部能匹配到
# sessions.cwd），再由 cwd 末级目录得到项目名。cwd 指向时间戳目录时，
# 那个会话压根没有真实项目名，降级成"临时工作区"。
#
# 会话 cwd 的读取走 db 模块的只读连接，这里缓存一份，避免每轮采集都查库。

_SESSION_CWD_CACHE: dict[str, Optional[str]] = {}
_SESSION_CWD_LOADED_AT = 0.0

# 会话 cwd 缓存有效期。与采集周期 2s 同量级，但会话 cwd 几乎不变，
# 给足 30s 也不影响准确性，却能把查库次数压到极低。
_SESSION_CWD_TTL = 30.0


def _session_cwds() -> dict[str, Optional[str]]:
    """会话 id → cwd。带 TTL 缓存。"""
    global _SESSION_CWD_LOADED_AT
    now = time.time()
    if _SESSION_CWD_LOADED_AT and now - _SESSION_CWD_LOADED_AT < _SESSION_CWD_TTL:
        return _SESSION_CWD_CACHE

    data: dict[str, Optional[str]] = {}
    try:
        from .db import session_cwd_map
        data = session_cwd_map()
    except Exception as e:                          # noqa: BLE001
        log.warning("读取会话 cwd 失败，改用路径回退: %s", e)
        data = {}

    _SESSION_CWD_CACHE.clear()
    _SESSION_CWD_CACHE.update(data)
    _SESSION_CWD_LOADED_AT = now
    return _SESSION_CWD_CACHE


def _project_of(path: str) -> Optional[str]:
    """从绝对路径回退推断项目名。

    只在拿不到会话 cwd 时才走这条路。规则：
      · 路径落在某个已知工作区根目录下 → 取它的下一级目录名（真实项目名）；
      · 路径落在 ~/WorkBuddy/<时间戳>/ 下 → 返回 None（沙箱没有项目名）。
    """
    parts = [p for p in re.split(r"[\\/]+", path) if p]
    if not parts:
        return None

    # 找 "WebstormProjects" / "Projects" 这类工作区根，取下一级目录名
    for i, p in enumerate(parts[:-1]):
        if p in ("WebstormProjects", "Projects", "workspace", "workspaces"):
            nxt = parts[i + 1]
            if nxt and not is_sandbox_dir(nxt):
                return project_name(nxt)
            return None

    # ~/WorkBuddy/<名字>/ —— 名字不是时间戳的话就是真实项目名
    for i, p in enumerate(parts[:-1]):
        if p == "WorkBuddy":
            nxt = parts[i + 1]
            if nxt and not is_sandbox_dir(nxt):
                return project_name(nxt)
            return None

    return None


def activity_stat(date_str: Optional[str] = None) -> dict[str, Any]:
    """今日代码改动：文件数 / 增删行 / 涉及项目。

    读 changes-index/*.json —— 每个文件是一次请求的变更索引。已排除编译产物，
    否则 build/ 下 700 个 .obj 会把数字彻底淹掉。

    项目名口径见上方说明：**按会话 id 关联 session.cwd** 优先，
    路径回退只在会话缺失时兜底。

    ⚠ date_str 以前是摆设：过滤条件恒用 local_midnight_ms()（今天午夜），
    传"昨天"进去返回的还是今天的数。现在按传入日期算窗口，
    不传才用今天。
    """
    from datetime import datetime, timedelta

    if not os.path.isdir(CHANGES_DIR):
        return {"date": date_str or datetime.now().strftime("%Y-%m-%d"),
                "files": 0, "additions": 0, "deletions": 0, "projects": []}

    if date_str:
        try:
            day = datetime.strptime(date_str, "%Y-%m-%d")
        except ValueError:
            log.warning("activity_stat: 日期格式不对 %r，按今天算", date_str)
            day = datetime.now()
        midnight = int(day.replace(hour=0, minute=0, second=0,
                                   microsecond=0).timestamp() * 1000)
        # 上界也要算，否则今天的改动会被算进"昨天"（只要过了午夜）
        end = midnight + 24 * 3600 * 1000
    else:
        midnight = local_midnight_ms()
        end = None

    files_set: set[str] = set()
    additions = deletions = 0
    projects: set[str] = set()

    cwds = _session_cwds()

    for fn in os.listdir(CHANGES_DIR):
        if not fn.endswith(".json"):
            continue
        conv_id = fn[:-5]                    # 文件名就是会话 id
        idx = _read_json(os.path.join(CHANGES_DIR, fn))
        if not isinstance(idx, dict):
            continue

        # 本会话的项目名（有 cwd 且 cwd 不是沙箱目录才作数）
        cwd = cwds.get(conv_id)
        conv_project: Optional[str] = None
        if cwd:
            # cwd 末级是时间戳 → 沙箱，没有真实项目名，交给路径回退
            last = [p for p in re.split(r"[\\/]+", cwd) if p]
            if last and not is_sandbox_dir(last[-1]):
                conv_project = project_name(cwd)

        for ch in idx.get("changes") or []:
            if not isinstance(ch, dict):
                continue
            ts = ch.get("createdAt") or 0
            if not isinstance(ts, (int, float)) or ts < midnight:
                continue
            if end is not None and ts >= end:
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
                proj = conv_project or _project_of(fp)
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

def _artifact_path(uri: str) -> str:
    """把 artifact 的 uri 还原成本地路径。

    ⚠ 不能只 replace("%3A", ":") / replace("%5C", "\\")。
    那样只能还原盘符和反斜杠，剩下这些全还原不出来：
        %20  空格          → "my report.html" 变成 "my%20report.html"
        %E4%B8%AD  中文    → 一串百分号
        %23  #             → 变成片段标识符
    路径还原不出来 = 用户点了打不开 = 功能等于没有。

    用 urllib.parse.unquote 一次性解全部转义，再去掉 file:/// 前缀。"""
    if not uri:
        return ""
    p = uri
    if p.startswith("file:///"):
        p = p[len("file:///"):]
    elif p.startswith("file://"):
        p = p[len("file://"):]
    # unquote 只解 %XX，不解 '+'（那是表单编码，不是路径编码）
    return unquote(p)


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
                    "path": _artifact_path(uri),
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
