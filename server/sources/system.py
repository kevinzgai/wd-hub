"""系统指标。psutil 缺失时全部降级为 0，不影响服务启动。"""

from __future__ import annotations

import os
import time
from typing import Any

try:
    import psutil
    _HAS_PSUTIL = True
except ImportError:
    psutil = None            # type: ignore[assignment]
    _HAS_PSUTIL = False

START_TS = time.time()


def system_stat() -> dict[str, Any]:
    if not _HAS_PSUTIL:
        return {"cpu": 0.0, "mem": 0.0, "disk": 0.0, "temp": 0, "proc": 0,
                "uptime": int(time.time() - START_TS)}

    cpu = float(psutil.cpu_percent(interval=None))
    mem = float(psutil.virtual_memory().percent)

    try:
        root = os.path.abspath(os.sep)
        disk = float(psutil.disk_usage(root).percent)
    except Exception:
        disk = 0.0

    temp = 0
    try:
        sensors = psutil.sensors_temperatures() or {}
        for key in ("coretemp", "k10temp", "cpu_thermal", "acpitz"):
            arr = sensors.get(key)
            if arr:
                temp = int(round(arr[0].current or 0))
                break
    except Exception:
        temp = 0

    try:
        proc = len(psutil.pids())
    except Exception:
        proc = 0

    return {"cpu": round(cpu, 1), "mem": round(mem, 1), "disk": round(disk, 1),
            "temp": temp, "proc": proc, "uptime": int(time.time() - START_TS)}


def prime() -> None:
    """cpu_percent 第一次调用返回 0.0，先摸一次让后续读数有意义。"""
    if _HAS_PSUTIL:
        try:
            psutil.cpu_percent(interval=None)
        except Exception:
            pass
