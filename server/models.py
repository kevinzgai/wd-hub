"""响应模型 + 文本处理工具。

这里只放两样东西：给 FastAPI 用的 Pydantic 模型，以及固件契约要求的
文本截断/格式化工具。业务查询逻辑不在这里。
"""

from __future__ import annotations

import re
from typing import Optional

from pydantic import BaseModel, Field, field_validator

# ---------------------------------------------------------------- 文本工具

# WorkBuddy 给临时会话建的沙箱目录形如 2026-10-15-16-00-59，直接显示没有信息量
_SANDBOX_DIR = re.compile(r"^\d{4}-\d{2}-\d{2}-\d{2}-\d{2}-\d{2}$")

# 沙箱目录的显示名。别在别处拿这个字符串做逻辑判断 —— 用 is_sandbox_dir()。
SANDBOX_LABEL = "临时工作区"


def is_sandbox_dir(name: Optional[str]) -> bool:
    """目录名是不是 WorkBuddy 的临时沙箱（时间戳形态）。

    单独抽成谓词，是因为调用方需要"这是不是沙箱"这个**判断**，
    而不是那个显示用的中文串。之前 files.py 直接比较 == "临时工作区"，
    改一下文案就静默失效，太脆。
    """
    return bool(name) and bool(_SANDBOX_DIR.match(name))


def utf8_trunc(s: str, max_bytes: int) -> str:
    """按字节截断，不切碎 UTF-8 字符。

    固件 buffer 是定长 char[]，且 LVGL 拿到半个汉字会渲染成乱码。
    """
    if not s:
        return ""
    b = s.encode("utf-8")
    if len(b) <= max_bytes:
        return s
    return b[:max_bytes].decode("utf-8", errors="ignore")


def human_ago(sec: float) -> str:
    """秒数转 5m / 2h / 3d 形式，对应固件 elapsed 字段（≤12 字节）。"""
    if sec < 0:
        sec = 0
    if sec < 60:
        return f"{int(sec)}s"
    if sec < 3600:
        return f"{int(sec // 60)}m"
    if sec < 86400:
        return f"{int(sec // 3600)}h"
    return f"{int(sec // 86400)}d"


def project_name(cwd: Optional[str]) -> str:
    """从 cwd 取项目名，Windows 两种分隔符都要处理。"""
    if not cwd:
        return "root"
    parts = [p for p in re.split(r"[\\/]+", cwd) if p]
    if not parts:
        return "root"
    last = parts[-1]
    # 盘符根目录
    if re.fullmatch(r"[A-Za-z]:", last):
        return last
    if is_sandbox_dir(last):
        return SANDBOX_LABEL
    return utf8_trunc(last, 20)


def session_title(title: Optional[str], custom: Optional[str], cwd: Optional[str],
                  sid: str) -> str:
    """标题回退链：用户自定义 > AI 生成 > 项目名+ID 前缀。"""
    for cand in (custom, title):
        if cand and cand.strip():
            return cand.strip()
    return f"{project_name(cwd)}-{sid[:8]}"


# ---------------------------------------------------------------- Pydantic 模型
# 全部字段带 description —— /docs 里直接读得懂，省一份接口文档


def _to_ms(v):
    """库里的毫秒时间戳可能是浮点（实测 1783644600000.0027）。

    毫秒精度已足够，取整即可。Pydantic 对 int 字段遇到浮点会直接抛校验错，
    这里统一兜住，避免接口 500。
    """
    if v is None or isinstance(v, int):
        return v or 0
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


class SessionItem(BaseModel):
    id: str = Field(description="会话 ID")
    title: str = Field(description="会话标题（custom_title > title > 回退）")
    project: str = Field(description="所属项目名（cwd 末级目录）")
    status: str = Field(description="会话状态，已归一为小写")
    model: Optional[str] = Field(default=None, description="使用的模型")
    elapsed: str = Field(description="距最近活动的时间，如 5m")
    last_activity_at: int = Field(description="最近活动时间（毫秒 epoch）")
    is_background: bool = Field(default=False, description="是否为自动化后台会话")

    _round_ts = field_validator("last_activity_at", mode="before")(_to_ms)


class Summary(BaseModel):
    working: int = Field(description="近 30 分钟活跃的交互式会话数")
    today_done: int = Field(description="今日完成的任务数（账本口径，删除任务不清零）")
    automations: int = Field(description="已启用的自动化数（status=ACTIVE）")


class TaskStat(BaseModel):
    total: int = Field(description="任务总数（含已删除的历史任务，来自本地账本）")
    completed: int = Field(description="累计已完成（含已删除任务）")
    today_completed: int = Field(description="今日完成（按完成时刻算，删除任务不影响）")
    in_progress: int = Field(description="进行中（仅统计盘上现存任务）")


class AutomationStat(BaseModel):
    active: int = Field(description="已启用自动化数")
    runs_today: int = Field(description="今日运行次数")
    failed_today: int = Field(description="今日失败次数")
    pending_review: int = Field(description="待审阅运行数")
    last_run_at: Optional[int] = Field(default=None, description="最近运行时间（毫秒）")


class SystemStat(BaseModel):
    cpu: float = Field(description="CPU 占用 %")
    mem: float = Field(description="内存占用 %")
    disk: float = Field(description="磁盘占用 %")
    temp: int = Field(description="温度 °C，取不到为 0")
    proc: int = Field(description="总进程数")
    uptime: int = Field(description="服务运行秒数")


class ActivityStat(BaseModel):
    date: str = Field(description="统计日期 YYYY-MM-DD")
    files: int = Field(description="今日改动文件数（已排除 build/node_modules）")
    additions: int = Field(description="新增行数")
    deletions: int = Field(description="删除行数")
    projects: list[str] = Field(default_factory=list, description="涉及的项目")


class ArtifactItem(BaseModel):
    name: str = Field(description="文件名")
    path: str = Field(description="完整路径")
    title: str = Field(description="显示标题")
    mime: Optional[str] = Field(default=None, description="MIME 类型")
    size: int = Field(description="字节数")
    created_at: int = Field(description="创建时间（毫秒）")

    _round_ts = field_validator("created_at", mode="before")(_to_ms)


class BoardResponse(BaseModel):
    generated_at: str = Field(description="快照生成时间（ISO 8601，含时区）")
    cache_age_ms: int = Field(description="缓存年龄，超过 15000 视为 stale")
    degraded: bool = Field(description="是否有数据源不可用")
    summary: Summary
    sessions: list[SessionItem] = Field(description="活跃会话列表")
    tasks: TaskStat
    automations: AutomationStat
    system: SystemStat
    activity: ActivityStat


class SessionListResponse(BaseModel):
    total: int
    items: list[SessionItem]


class TaskItem(BaseModel):
    id: str
    subject: str = Field(description="任务标题")
    status: str
    conversation: str = Field(description="所属会话 ID")
    updated_at: int = Field(description="更新时间（毫秒）")

    _round_ts = field_validator("updated_at", mode="before")(_to_ms)


class TaskListResponse(BaseModel):
    total: int
    items: list[TaskItem]


class AutomationRun(BaseModel):
    thread_id: str
    status: str
    success: Optional[bool] = None
    created_at: int

    _round_ts = field_validator("created_at", mode="before")(_to_ms)


class AutomationItem(BaseModel):
    id: str
    name: str
    status: str
    schedule_type: str
    rrule: Optional[str] = None
    next_run_at: Optional[int] = None
    last_run_at: Optional[int] = None
    running: bool = False
    recent_runs: list[AutomationRun] = Field(default_factory=list)

    _round_ts = field_validator("next_run_at", "last_run_at", mode="before")(_to_ms)


class AutomationListResponse(BaseModel):
    total: int
    items: list[AutomationItem]


class ArtifactListResponse(BaseModel):
    total: int
    items: list[ArtifactItem]


class SourceHealth(BaseModel):
    ok: bool
    count: int = 0
    detail: str = ""


class HealthResponse(BaseModel):
    ok: bool
    version: str
    generated_at: Optional[str] = None
    cache_age_ms: int
    stale: bool
    sources: dict[str, SourceHealth]


class ErrorResponse(BaseModel):
    ok: bool = Field(default=False)
    error: str = Field(description="机器可读错误码")
    detail: str = Field(description="人话描述")
    source: Optional[str] = None
