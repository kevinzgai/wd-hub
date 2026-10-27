"""快照采集器。

核心设计：**接口只读内存快照，永远不在请求路径上查库。**

理由有三个：
  1. 固件每 5s 轮询，多个前端同时刷新，每次都查库会让 SQLite 被反复建连；
  2. 库是 WAL 且被 WorkBuddy 主进程占用，高频读会拉高锁竞争概率；
  3. 快照模式让降级变简单 —— 采集失败就沿用上一份，屏幕不会闪。

采集协程每 2s 刷新一次。2s 是权衡：屏幕 5s 刷新一次，采集比它快一倍多，
既不会让数字滞后，也不会白烧 CPU。
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Optional

from . import sources
from .sources import db, files, system
from .sources.db import SourceError

log = logging.getLogger("board.collector")

REFRESH_SEC = 2.0
STALE_MS = 15_000       # 约 7 个周期没成功就认为数据陈旧


@dataclass
class Snapshot:
    """一次采集的完整结果。字段与 /api/v1/board 一一对应。"""

    generated_at_ms: int = 0
    working: int = 0
    today_done: int = 0
    automations: int = 0
    sessions: list[dict[str, Any]] = field(default_factory=list)
    tasks: dict[str, Any] = field(default_factory=lambda: {
        "total": 0, "completed": 0, "today_completed": 0, "in_progress": 0})
    automation_runs: dict[str, Any] = field(default_factory=lambda: {
        "runs_today": 0, "failed_today": 0, "pending_review": 0, "last_run_at": None})
    activity: dict[str, Any] = field(default_factory=lambda: {
        "date": "", "files": 0, "additions": 0, "deletions": 0, "projects": []})
    sys: dict[str, Any] = field(default_factory=lambda: {
        "cpu": 0.0, "mem": 0.0, "disk": 0.0, "temp": 0, "proc": 0, "uptime": 0})
    health: dict[str, dict[str, Any]] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)

    @property
    def degraded(self) -> bool:
        return bool(self.errors)


class Collector:
    def __init__(self, refresh_sec: float = REFRESH_SEC) -> None:
        self.refresh_sec = refresh_sec
        self._snap = Snapshot()
        self._lock = asyncio.Lock()
        self._task: Optional[asyncio.Task] = None
        self._stopping = asyncio.Event()

    # ------------------------------------------------------------ 对外

    async def snapshot(self) -> Snapshot:
        async with self._lock:
            return self._snap

    async def cache_age_ms(self) -> int:
        async with self._lock:
            if not self._snap.generated_at_ms:
                return -1
            return int(time.time() * 1000) - self._snap.generated_at_ms

    async def start(self) -> None:
        """启动前先同步采一次，避免首屏空白。"""
        await self._refresh_once()
        self._task = asyncio.create_task(self._loop(), name="collector")

    async def stop(self) -> None:
        self._stopping.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass

    # ------------------------------------------------------------ 内部

    async def _loop(self) -> None:
        while not self._stopping.is_set():
            try:
                await asyncio.sleep(self.refresh_sec)
                await self._refresh_once()
            except asyncio.CancelledError:
                break
            except Exception as e:                      # noqa: BLE001
                # 采集协程绝不能死。死了就永久停在最后一份快照上。
                log.exception("采集异常，继续下一轮: %s", e)

    async def _refresh_once(self) -> None:
        """所有阻塞 IO 丢进线程池，别卡事件循环。"""
        new = await asyncio.to_thread(_collect_sync)
        async with self._lock:
            # 采集整体失败时保留旧数据，只更新错误信息。
            #
            # ⚠ 判据必须是"有没有任何**业务**数据源成功"，不能用
            # "generated_at_ms 是不是 0"。_collect_sync() 末尾无条件给
            # generated_at_ms 赋值，那个条件恒为假，于是数据源全挂时反而会
            # 走到下面这行，把屏上三格数字全刷成 0
            # （注释说的"保留旧数据、屏幕不闪"从来没发生过）。
            #
            # "system" 不计入：它兜在 except Exception 里，几乎不会失败，
            # 把它算进去会让判据在真正的故障时依然为真。
            ok_count = sum(
                1 for name, h in new.health.items()
                if name != "system" and h.get("ok")
            )
            if new.errors and ok_count == 0:
                # 业务源全军覆没：保留旧快照，只更新错误信息
                self._snap.errors = new.errors
                self._snap.health = new.health
                return
            self._snap = new


def _collect_sync() -> Snapshot:
    """同步采集一轮。每个数据源独立 try/except，互不影响。"""
    s = Snapshot()
    s.health = {}
    s.errors = []

    # ---- 会话 + 自动化（同一个库）----
    try:
        s.working = db.sessions_working()
        s.sessions = db.sessions_active(limit=12)
        s.health["sessions"] = {"ok": True, "count": s.working, "detail": ""}
    except SourceError as e:
        s.errors.append(f"sessions:{e}")
        s.health["sessions"] = {"ok": False, "count": 0, "detail": str(e)}
        log.warning("会话采集失败: %s", e)

    try:
        s.automations = db.automations_active()
        s.automation_runs = db.automation_runs_stats()
        s.health["automations"] = {"ok": True, "count": s.automations, "detail": ""}
    except SourceError as e:
        s.errors.append(f"automations:{e}")
        s.health["automations"] = {"ok": False, "count": 0, "detail": str(e)}
        log.warning("自动化采集失败: %s", e)

    # ---- 任务 ----
    try:
        s.tasks = files.tasks_stat()
        s.today_done = s.tasks.get("today_completed", 0)
        s.health["tasks"] = {"ok": True, "count": s.tasks.get("total", 0), "detail": ""}
    except SourceError as e:
        s.errors.append(f"tasks:{e}")
        s.health["tasks"] = {"ok": False, "count": 0, "detail": str(e)}
        log.warning("任务采集失败: %s", e)

    # ---- 变更 ----
    try:
        s.activity = files.activity_stat()
        s.health["changes"] = {"ok": True, "count": s.activity.get("files", 0), "detail": ""}
    except SourceError as e:
        s.errors.append(f"changes:{e}")
        s.health["changes"] = {"ok": False, "count": 0, "detail": str(e)}
        log.warning("变更采集失败: %s", e)

    # ---- 系统（永不失败）----
    try:
        s.sys = system.system_stat()
        s.health["system"] = {"ok": True, "count": s.sys.get("proc", 0), "detail": ""}
    except Exception as e:                          # noqa: BLE001
        log.warning("系统指标采集失败: %s", e)
        s.health["system"] = {"ok": False, "count": 0, "detail": str(e)}

    s.generated_at_ms = int(time.time() * 1000)
    return s


def iso_now(ms: Optional[int] = None) -> str:
    ts = (ms or int(time.time() * 1000)) / 1000
    return datetime.fromtimestamp(ts).astimezone().isoformat(timespec="seconds")
