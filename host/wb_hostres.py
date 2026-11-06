#!/usr/bin/env python3
"""电脑资源采集（CPU / 内存 / 磁盘），给设备「主机资源」页用。

设计前提：**不引入第三方依赖**。
  host/wb_app.py 是在用户机器上跑的守护进程，加一个 psutil 会让没装它的人
  直接起不来 —— 而它恰恰是"查看电脑状态"这种锦上添花的功能。所以这里
  只用标准库，按平台分支：

  * CPU：优先 psutil（装了就用，最准），否则
      - Linux/macOS：os.getloadavg() 折算成占用率
      - macOS 额外可读 mach host_statistics（这里不展开，getloadavg 已够）
  * 内存：优先 psutil；否则 macOS 走 vm_stat，Linux 读 /proc/meminfo，
    Windows 用 ctypes GlobalMemoryStatusEx
  * 磁盘：shutil.disk_usage(path) 全平台可用

返回的字典与固件 wb_hostres_t 一一对应（键名就是协议键名），值为 int。
拿不到的项一律给 -1，由固件显示「--」——绝不填 0，因为"没有数据"和
"占用 0%"在屏幕上是两种完全不同的含义。

CPU 占用率的坑：
  psutil.cpu_percent(interval=None) 第一次调用恒返回 0.0（它需要两次采样
  求差）。本模块自己保存上一次的总/空闲 tick，用差值算，第一次调用时
  报 -1（未就绪），下一轮（30s 后）开始就有真实值。
"""

from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
import sys

# CPU 差值采样的历史状态：(总秒数, 空闲秒数)，None 表示还没有基线。
_last_cpu: tuple[float, float] | None = None
# psutil 是否可用。只在首次探测一次，避免每轮 30s 都跑一遍 import 失败路径。
_psutil = None
_psutil_probed = False

# 磁盘统计的根路径。给 Windows 留 C:\，其余用 /（macOS 上 / 就是数据卷所在）。
DISK_ROOT = {"win32": "C:\\"}.get(sys.platform, "/")


def _load_psutil():
    """惰性探测 psutil；没装就记住结果，后面所有分支直接走标准库。"""
    global _psutil, _psutil_probed
    if _psutil_probed:
        return _psutil
    _psutil_probed = True
    try:
        import psutil  # noqa: PLC0415  —— 故意延迟导入，缺库不能影响启动
        _psutil = psutil
    except Exception:
        _psutil = None
    return _psutil


def _pct(used: float, total: float) -> int:
    if total <= 0:
        return -1
    v = int(round(used * 100.0 / total))
    return max(0, min(100, v))


def _cpu_via_mach() -> int:
    """macOS：读 mach 的 host_cpu_load_info，用两次 tick 差值算占用率。

    为什么不满足于 getloadavg：load average 把"等待 IO / 不可中断睡眠"的
    线程也算进去了，磁盘忙的时候会读到 80%，而 CPU 其实闲得很。mach 给的是
    纯 CPU tick（user / system / idle / nice），差值就是真实占用率。
    Apple Silicon 和 Intel 上字段相同，不需要分架构。

    两个必须处理的细节：
      1) mach_host_self() 每次都拿一个新的 port 引用，不 deallocate 就是
         泄漏 —— 本进程按 30s 周期跑一年会漏约 100 万个 port，必须回收。
      2) tick 是 uint32，100Hz 下约 497 天回绕一次。差值出现负数时只重置
         基线、返回 -1，不能让负数传进百分比计算。
    """
    global _last_cpu
    if sys.platform != "darwin":
        return -1
    try:
        libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib")

        class CpuTicks(ctypes.Structure):
            # CPU_STATE_MAX == 4：USER / SYSTEM / IDLE / NICE
            _fields_ = [("ticks", ctypes.c_uint32 * 4)]

        host = libc.mach_host_self()
        try:
            info = CpuTicks()
            count = ctypes.c_uint32(4)      # HOST_CPU_LOAD_INFO_COUNT
            HOST_CPU_LOAD_INFO = 3
            if libc.host_statistics64(host, HOST_CPU_LOAD_INFO,
                                      ctypes.byref(info),
                                      ctypes.byref(count)) != 0:
                return -1
            t = info.ticks
            user, system, idle, nice = t[0], t[1], t[2], t[3]
        finally:
            # MACH_PORT_NULL == 0；失败时也别漏回收。
            if host:
                libc.mach_port_deallocate(libc.mach_task_self(), host)

        busy = float(user + system + nice)
        total = busy + float(idle)
        prev = _last_cpu
        _last_cpu = (total, float(idle))
        if prev is None:
            return -1                     # 首帧没有基线，报「未就绪」
        d_total = total - prev[0]
        d_idle = float(idle) - prev[1]
        if d_total <= 0:
            return -1                     # 采样间隔内没跑过 tick，或已回绕
        return _pct(d_total - d_idle, d_total)
    except Exception:
        return -1


def _cpu_via_ticks() -> int:
    """用两次采样的 (总, 空闲) 差值算 CPU 占用率，全平台通用。

    第一次没有基线，返回 -1（未就绪）；从第二次起返回真实值。
    只有 Linux 的 /proc/stat 和 macOS 的 host_statistics 能直接拿到 tick，
    所以这条路径实际只在 Linux 生效，macOS 落到 getloadavg。
    """
    global _last_cpu
    try:
        with open("/proc/stat", "r", encoding="utf-8") as f:
            line = f.readline()
    except Exception:
        return -1
    parts = line.split()
    if len(parts) < 5:
        return -1
    # user nice system idle iowait irq softirq steal
    values = [float(p) for p in parts[1:] if p.isdigit()]
    if len(values) < 4:
        return -1
    idle = values[3] + (values[4] if len(values) > 4 else 0.0)
    total = sum(values)
    prev = _last_cpu
    _last_cpu = (total, idle)
    if prev is None:
        return -1
    d_total = total - prev[0]
    d_idle = idle - prev[1]
    if d_total <= 0:
        return -1
    return _pct(d_total - d_idle, d_total)


def _cpu_via_loadavg(cores: int) -> int:
    """用 1 分钟平均负载折算占用率。

    注意这是近似值：load average 包含等待 IO 的进程，会略高于真实 CPU 占用。
    在没有 psutil 的机器上，这个近似值比"永远显示 0%"有用得多。
    """
    try:
        load = os.getloadavg()[0]
    except Exception:
        return -1
    if cores <= 0:
        return -1
    return _pct(load, float(cores))


def _mem_psutil(ps) -> tuple[int, int, int]:
    m = ps.virtual_memory()
    return _pct(m.used, m.total), int(m.used // (1024 * 1024)), int(m.total // (1024 * 1024))


def _mem_linux() -> tuple[int, int, int]:
    info = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as f:
            for line in f:
                k, _, v = line.partition(":")
                parts = v.split()
                if parts and parts[0].isdigit():
                    info[k] = int(parts[0])  # 单位 kB
    except Exception:
        return -1, -1, -1
    total = info.get("MemTotal", 0)
    avail = info.get("MemAvailable", info.get("MemFree", 0))
    if total <= 0:
        return -1, -1, -1
    used_kb = total - avail
    return (_pct(used_kb, total),
            int(used_kb // 1024),
            int(total // 1024))


def _mem_macos() -> tuple[int, int, int]:
    """解析 vm_stat。Page size 在首行给出（Apple Silicon 上是 16384）。

    这里把 "free + inactive + speculative" 算作可用：macOS 会把大量内存
    用作缓存，"仅 free" 读出来会常年 95%+，对用户毫无参考价值。
    """
    try:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return -1, -1, -1
    if not out:
        return -1, -1, -1
    page = 4096
    stats = {}
    for line in out.splitlines():
        if "page size of" in line:
            digits = "".join(ch for ch in line.split("page size of")[-1] if ch.isdigit())
            if digits:
                page = int(digits)
        k, _, v = line.partition(":")
        key = k.strip()
        if key and v.strip().rstrip(".").isdigit():
            stats[key] = int(v.strip().rstrip("."))
    total = (stats.get("Pages free", 0) + stats.get("Pages active", 0)
             + stats.get("Pages inactive", 0) + stats.get("Pages speculative", 0)
             + stats.get("Pages wired down", 0) + stats.get("Pages occupied by compressor", 0))
    available = (stats.get("Pages free", 0) + stats.get("Pages inactive", 0)
                 + stats.get("Pages speculative", 0))
    if total <= 0:
        return -1, -1, -1
    total_b = total * page
    used_b = (total - available) * page
    return (_pct(used_b, total_b),
            int(used_b // (1024 * 1024)),
            int(total_b // (1024 * 1024)))


def _mem_windows() -> tuple[int, int, int]:
    class MemStatus(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    try:
        st = MemStatus()
        st.dwLength = ctypes.sizeof(MemStatus)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return -1, -1, -1
    except Exception:
        return -1, -1, -1
    total_mb = int(st.ullTotalPhys // (1024 * 1024))
    used_mb = int((st.ullTotalPhys - st.ullAvailPhys) // (1024 * 1024))
    return int(st.dwMemoryLoad), used_mb, total_mb


def _cpu_cores() -> int:
    ps = _load_psutil()
    if ps is not None:
        try:
            return ps.cpu_count(logical=True) or 1
        except Exception:
            pass
    return os.cpu_count() or 1


def collect() -> dict:
    """采一次数据，返回可直接并进 host 帧的字典。

    任何一项采不到都给 -1，绝不抛异常 —— 调用方是 30s 一跑的推送循环，
    一次采集失败不该让整帧消失（那会让设备上的数据停在旧值且无提示）。
    """
    ps = _load_psutil()
    out = {
        "cpu_pct": -1,
        "mem_pct": -1,
        "mem_used_mb": -1,
        "mem_total_mb": -1,
        "disk_pct": -1,
        "disk_used_mb": -1,
        "disk_total_mb": -1,
    }

    # ---- CPU ----
    if ps is not None:
        try:
            # interval=None + 自己保存基线：本函数被 30s 周期调用，
            # 差值天然覆盖这 30 秒的平均占用。
            v = ps.cpu_percent(interval=None)
            out["cpu_pct"] = _pct(v, 100.0)
        except Exception:
            out["cpu_pct"] = -1
    if out["cpu_pct"] < 0:
        if sys.platform == "darwin":
            out["cpu_pct"] = _cpu_via_mach()
        elif sys.platform.startswith("linux"):
            out["cpu_pct"] = _cpu_via_ticks()
        if out["cpu_pct"] < 0:
            out["cpu_pct"] = _cpu_via_loadavg(_cpu_cores())

    # ---- 内存 ----
    try:
        if ps is not None:
            pct, used, total = _mem_psutil(ps)
        elif sys.platform == "darwin":
            pct, used, total = _mem_macos()
        elif sys.platform.startswith("linux"):
            pct, used, total = _mem_linux()
        elif sys.platform == "win32":
            pct, used, total = _mem_windows()
        else:
            pct, used, total = -1, -1, -1
        out["mem_pct"], out["mem_used_mb"], out["mem_total_mb"] = pct, used, total
    except Exception:
        pass

    # ---- 磁盘 ----
    try:
        # 目标目录所在卷比根卷更贴近"用户关心的那块盘"：
        # 工程目录通常在数据分区，而 / 在 macOS 上是只读系统卷。
        target = os.environ.get("WB_DISK_PATH") or os.path.expanduser("~")
        if not os.path.isdir(target):
            target = DISK_ROOT
        u = shutil.disk_usage(target)
        total_mb = int(u.total // (1024 * 1024))
        used_mb = int(u.used // (1024 * 1024))
        out["disk_pct"] = _pct(u.used, u.total)
        out["disk_used_mb"] = used_mb
        out["disk_total_mb"] = total_mb
    except Exception:
        pass

    return out
