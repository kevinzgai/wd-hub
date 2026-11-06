#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wb_app.py —— WorkBuddy 控制台（单一入口，daemon + 本地 Web UI）。

运行 `python wb_app.py` 即完成：
  * 拉起 daemon：BLE 连接 Xiaozhi BOX + 状态推流 + 周期积分 + 审批遥控内核（TCP IPC :48765）
  * 启动内置 Web UI：浏览器打开 http://127.0.0.1:8080
       - 设备连接状态 + daemon 启停按钮
       - 数据看板（会话列表 + 积分余额/使用率）
       - 实时日志面板

整合自 wb_daemon.py / wb_status.py / wb_balance.py，功能等价，单一入口，自包含，不依赖外部脚本。

用法：
  python wb_app.py                    # 启动 daemon + Web UI（默认）
  python wb_app.py --mac CC:..:E6     # 指定设备地址（否则扫描）
  python wb_app.py --web-port 8080    # 覆盖 Web 端口（默认 8080）
  python wb_app.py --credit-interval 0# 禁用周期积分
  python wb_app.py --no-daemon        # 只起 Web UI，不自动启动 daemon
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import webbrowser
# 子进程调用时隐藏控制台窗口（Windows）：netstat/pip 等每次调用默认会闪一个
# 黑色 cmd 框，尤其 ACP 发现循环每 10s 跑一次 netstat，不隐藏会持续弹窗。
SUB_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
import base64
import wave

from collections import deque
import array
import ctypes
import queue
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
try:
    import winreg
except ImportError:  # pragma: no cover - 非 Windows 环境
    winreg = None

try:
    import pystray
    from PIL import Image, ImageDraw, ImageFont
    HAS_TRAY = True
except ImportError:
    pystray = None
    HAS_TRAY = False

# ===================== 常量 =====================

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
# 主机端同时服务两套固件，扫描只按广播名匹配（不按 service uuid），所以不能
# 写死单名：passport-fw(esp32c3) 广播 "Workbuddy BOX"，firmware(esp32s3)
# 广播 "Xiaozhi BOX"。6f4b6d5 为 s3 改成后者后，本机 c3 一直「本轮未命中」。
# DEVICE_NAME 保留为主显示名，供外部模块 import 时行为不变。
DEVICE_NAMES = ("Workbuddy BOX", "Xiaozhi BOX")
DEVICE_NAME = DEVICE_NAMES[0]
CONNECT_TIMEOUT = 15.0
# 单帧写 BLE 必须在此限内完成。Windows BLE 栈在链路异常时会让写调用
# 永久挂起（不返回、不报错、连接仍显示已连），必须靠 wait_for 兜底，
# 否则 status/credit 推流循环会静默僵死。
WRITE_TIMEOUT = 8.0
# 断开重连时给 disconnect 的兜底超时，防止其本身也挂起。
DISCONNECT_TIMEOUT = 5.0
# B-02：写超时**连续**这么多次才判定链路僵死并主动断开。
# 单次超时在 BLE 上很常见（连接间隔抖动、mbuf 瞬时耗尽、射频瞬断），
# 一超时就 disconnect 会把一次 100ms 的卡顿升级成 3~5s 的断链重连，
# 反过来又让下一次写更容易超时，形成"越修越断"的正反馈。
# 实测（2026-10-30）4Hz 语音轮询下平均 60–90s 就会掉一次，正是这个机制。
WRITE_TIMEOUT_STRIKES = 3
# 判定僵死后到重新建连之间留出的静默期：立刻重连往往撞在对方还在
# 拆链的路上（BLE 的 disconnect 是异步的），等连接间隔几拍再连成功率更高。
RESET_RECONNECT_DELAY = 2.0
# 连续连不上这么多次，就认为记住的地址已失效，清掉退回"按名字扫描"。
# 依据：macOS 对未配对设备用随机地址，设备刷固件/复位后会变（实测
# 0E9EAF43… → D843E746…）。而看护循环每 15s 重试一次，死抱旧地址
# 就永远看不到设备的广播。取 3 次约 45s，既不会因偶发瞬断误清，
# 也不会让用户对着"设备明明开着却说找不到"等太久。
CONN_FAILS_BEFORE_RESCAN = 3
SCAN_TIMEOUT = 20.0
IPC_PORT = 48765
STATUS_INTERVAL = 2.0
CREDIT_INTERVAL = 60.0
MAX_LINE_BYTES = 64 * 1024
LOG_MAX = 200

# ---- 设备语音录入（阶段 3）----
# 设备只在收到 {"voice":"poll"} 时才回一行语音数据；按下录音键后有 2 秒窗口，
# 窗口内没被 poll 到这次按键就作废，所以空闲也得按 0.25s 一直问。
VOICE_POLL_FRAME = b'{"voice":"poll"}\n'
# 空闲轮询间隔。B-02 判定 0.25s（4Hz）是链路不稳的主因之一，改为 1Hz。
# 依据：设备按下录音键后有 2 秒窗口等 poll 把它领走，1Hz 仍留有 1 次余量；
# 而空闲期 4Hz 意味着每秒 4 次带响应写的 GATT 事务，持续占用 NimBLE 的
# mbuf 池，恰好与 2s 一次的状态帧推流叠加，是"空闲也会掉线"的来源。
VOICE_IDLE_INTERVAL = 1.0
VOICE_ACTIVE_INTERVAL = 0.10
VOICE_FRAME_BYTES = 163          # 3 字节头 + 320/2 个 nibble
VOICE_FRAME_SAMPLES = 320
VOICE_SAMPLE_RATE = 16000
VOICE_DIR = os.path.expanduser("~/.workbuddy/voice")
# IPC 与 BLE 解耦后：daemon 启动即监听 IPC，设备由后台看护循环发现/重连。
# 尚无设备地址时，扫描失败后间隔多久再扫（扫描本身占用蓝牙，不宜过频）。
RESCAN_INTERVAL = 25.0
# 已有地址时，看护循环检查连接状态的周期。
WATCH_INTERVAL = 5.0
# Notification(permission_prompt) / Stop(提问) 事件提醒去重窗口。
EVENT_DEDUP_TTL = 30.0

# ---- wb_status 数据源 ----
DB = os.path.expanduser("~/.workbuddy/workbuddy.db")
PROJECTS = os.path.expanduser("~/.workbuddy/projects")
MSG_MAX = 120
MODEL_MAX = 30
# ---- 设备第 2 屏（SESSION RACK）的字段宽度 ----
# 卡片内容宽 214px，12px 中文字宽约 12px/字 → 最多显示 17 个汉字。
# 超出屏幕的部分设备只会画省略号，白占 480B 的帧预算，所以在 PC 端
# 就直接按"屏幕装得下多少"来截。
#
# 注意 to_display_text() 是按**字符**截的，不是按字节：17 个汉字 = 51 字节，
# 17 个 ASCII = 17 字节，两者都在 214px 内，用它一个数就够。
#
# 为什么不直接改 MSG_MAX：那个变量还被提问页的 hint 用着（不上设备，
# 需要保留完整长度给 Web UI 和 ACP 作答），压小会连带砍掉不该砍的内容。
DEV_FIELD_MAX = 17
TRANS_TAIL_BYTES = 20000

# 增量扫描状态：path -> {"off": 已读到的字节偏移, "size": 上次见到的文件大小}
# 见 read_transcript 的注释（要解决"提问被推出尾扫窗口"的问题）。
_TRANS_CURSOR: dict[str, dict] = {}
_TRANS_CURSOR_LOCK = threading.Lock()
# 增量扫描一次最多读多少字节。故意给得比 TRANS_TAIL_BYTES 大得多：
# 一轮对话动辄写几十上百 KB，窗口太小时"刚发生的提问"又会掉到窗口外，
# 等于把同一个坑换个位置再踩一遍。
TRANS_INCR_MAX = 1 << 20          # 1 MiB
# **首次**遇到某个转录时，从文件末尾向前回退多远开始扫。
#
# ⚠ 为什么首扫不能从字节 0 开始（这里踩过一次）：
#   本项目自己的转录已有 23.6MB。若首扫从 0 起、一次最多读 TRANS_INCR_MAX，
#   要 23 轮（每轮 ~2 秒的状态推流）才能追到文件末尾 —— 期间发生的提问
#   全部延迟几十秒才可能上屏，等于把"提问框不出现"换个形式重演。
#   更严重的是：首扫从 0 起会把几十轮对话之前的历史提问也当成"待答"，
#   一旦那些提问没被 _ask_skipped 拦住，设备会被一堆陈年残题刷屏。
#
#   所以首扫**只回退 TRANS_FIRST_WINDOW 就够**：daemon 重启后本来就不该
#   把旧提问重新弹出来（那是残留，不是"用户现在等着答的题"）。
#   之后靠增量游标持续跟进，任何新增的提问都在发生当下被捕获。
TRANS_FIRST_WINDOW = 2 << 20      # 2 MiB
# 增量扫描保留的尾部行数：只用于算 active_tools / has_reasoning 这类
# "近况"指标，提问判定靠 _asks（累积状态，不看窗口）。
TRANS_RECENT_LINES = 60
# 设备提问页（ask 帧）的协议容量，须与固件 main/wb_parse.h 一致。
# 超容量的 text/tool 会让整帧被设备拒收（extract_string 返回 false），
# 所以必须在 PC 端按 UTF-8 边界预截断；opts 超长会被设备宽容截断。
ASK_TOOL_MAX = 24
ASK_TEXT_MAX = 120
ASK_OPT_LEN = 31
ASK_OPT_MAX = 6
# 提问页保活重推间隔（秒）：设备端提问页 90 秒无操作会自动退出且不通知 PC
# （固件 demo_workbuddy.c: WB_ASK_TIMEOUT_MS=90000），按此间隔原样重发同一
# ask 帧可让设备重新进入提问页并重置其本机计时器（固件对重复帧幂等）。
# 必须小于 90，留 30 秒余量抗 BLE/调度抖动。
ASK_KEEPALIVE_SEC = 60.0
# 设备接收缓冲 512B（固件 main/wb_parse.h: WB_RECV_MAX）。超长帧会被
# ATT 层直接拒收（write_gatt_char 报 Invalid Attribute Value Length），故留余量自检。
BLE_FRAME_MAX = 480

# ---- 设备第 7 屏（今日消耗）----
# 数据源不是 workbuddy.db：那张 session_usage 表只有"会话累计 used"，
# 既没有按天维度也没有模型维度（会话跨天时无法拆分）。请求级真实 usage
# 只在会话转录 jsonl 里，每轮一条 providerData.rawUsage —— 这也正是
# token-dashboard 技能的实现原理。
#
# 全量扫 142MB / 50 个文件实测 0.42s，30s 一轮完全可接受，不做增量索引：
# jsonl 是追加写的，增量要靠记住每个文件的字节偏移，一旦日志轮转或
# 用户删了目录就会错位，维护成本远高于省下的 0.4 秒。
USAGE_INTERVAL = 30.0
# 上设备的模型条数。屏幕内容区 188px，扣掉顶部两行 KPI 后只够 5 行。
# 多于 5 条的部分并进"其它"合计，保证屏幕上的数字加起来等于总量。
USAGE_MODELS_MAX = 5
# 模型名上设备的宽度上限。模型条左侧 96px 给名字，12px 字号约 8 个 ASCII
# 字符；超长在设备侧只会画省略号，不如在 PC 端按屏幕宽度截。
USAGE_MODEL_NAME_MAX = 16

# ---- 设备能力位（与固件 main/wb_proto.h 的 WB_CAP_* 一一对应）----
# 设备连上后会主动发一帧 {"type":"hello","fw":"1.2.0","proto":2,"caps":N}，
# 主机据此决定下发哪些帧、以及 status_list.count 用什么语义。
# 老固件不发 hello，所以 caps 缺省为 0 = 全部按"兼容档"处理：
# 不假设设备能认任何新特性。
CAP_APPROVAL_PAGE = 1 << 0
CAP_ASK_PAGE = 1 << 1
CAP_ASK_MULTI = 1 << 2
# bit3：设备认「count = 会话总数」的新语义。本固件未开（wb_proto.h 注明），
# 所以老/新固件都仍收到"已发条数"，等固件打开这一位后再切换。
CAP_COUNT_TOTAL = 1 << 3
CAP_AUTO_PAGE = 1 << 4
CAP_VOICE_UP = 1 << 5
# bit6：设备有「主机大盘」屏，能解析 usage / host 帧。
# 只有声明这一位才推 host 帧；旧固件不认识无 type 的 host 帧会回 ERR_JSON。
CAP_HOST_DASH = 1 << 6

CAP_LABELS = {
    CAP_APPROVAL_PAGE: "审批页",
    CAP_ASK_PAGE: "提问页",
    CAP_ASK_MULTI: "多选",
    CAP_COUNT_TOTAL: "总数语义",
    CAP_AUTO_PAGE: "自动切页",
    CAP_VOICE_UP: "语音上行",
    CAP_HOST_DASH: "主机大盘",
}

# ---- 本地 ACP（设备端直接作答通道）----
# 桌面端 worker 为 loopback 自动化免网关密码：仅需安全头 + 连接 id。
ACP_SEC_HDR = {"x-codebuddy-request": "1"}
ACP_CONNECT_PATH = "/api/v1/acp/connect"
ACP_PATH_STR = "/api/v1/acp"
ACP_DISCOVER_INTERVAL = 10.0
# 长订阅的 socket 读超时（秒）。服务端 GET 订阅每 30s 发一次
# ": heartbeat"，90s 是 3 倍余量；仍保留重连兜底以防服务端重启/连接失效。
ACP_STREAM_IDLE = 90.0
# session/load 的 POST 是一次性回放（实测约 74KB 后 EOF），给足读取时间。
ACP_LOAD_TIMEOUT = 60.0
# 长流断开后的重连退避：base * n，上限 ACP_REMOUNT_MAX。
ACP_REMOUNT_BASE = 3.0
ACP_REMOUNT_MAX = 30.0
CONSOLE_CFG = os.path.expanduser("~/.workbuddy/wb-console.json")
# 设备提示音音量（0..100，0 = 静音）。缺省 100，与固件端 WB_BEEP_VOLUME_DEFAULT
# 一致——老配置文件里没有 beep_vol 键时，行为与升级前完全相同。
BEEP_VOL_DEFAULT = 100
# 用户主动跳过的提问 cid 名单（持久化）：重启后仍不重推，防止幽灵提问回潮。
SKIP_FILE = os.path.expanduser("~/.workbuddy/wb-skip.json")
SKIP_MAX_STORE = 500

# ---- wb_balance ----
BAL_API_URL = "https://copilot.tencent.com/v2/billing/meter/get-user-resource"
# 官方客户端真正用来显示"积分余额"的汇总接口。⚠ 没有 /v2 前缀（实测 /v2/... 404）。
# 返回 TotalCredits + Packages + CreditGroups（套餐基础/套餐赠送/购买/平台奖励）。
BAL_SUMMARY_URL = "https://copilot.tencent.com/billing/meter/get-user-resource-summary"
BAL_PRODUCT_CODE = "p_tcaca"
BAL_RANGE_BEGIN = "2024-12-01 21:25:00"
# 无凭据时的重试间隔：WorkBuddy 登录态为加密信封时靠它等用户补一次 login 后自愈，
# 不用 60s 空转刷日志。
CREDIT_AUTH_RETRY = 300.0
# 同一积分错误的最短重复打印间隔（其余合并计数），避免信封态每 60s 刷屏。
CREDIT_ERR_REPEAT = 1800.0

# ===================== 共享状态 =====================

class AppState:
    """daemon 与 Web 线程共享的运行状态 + 日志。"""
    def __init__(self, log_max: int = LOG_MAX):
        self._lock = threading.Lock()
        self.running = False
        self.connected = False
        self.address = ""
        self.sessions: list[dict] = []
        # 会话总数（status_list.count 的语义由设备 caps 决定）。
        # UI 用它区分「设备上有几条」和「总共有几个会话」。
        self.sess_total: int = 0
        self.credit: dict | None = None
        # 积分不可用的可读原因（无凭据 / token 失效 / 登录态为加密信封等），
        # 由 daemon 维护，UI 在积分卡片内直接展示，避免用户翻日志。
        self.credit_reason: str = ""
        self.battery: int | None = None
        # 设备能力握手（hello 帧）。收到前为 None：表示"还没握手"，
        # 与"握手了但 caps=0（老固件）"要能区分——前者不该在 UI 上
        # 显示成"设备不支持任何功能"。
        self.device_fw: str = ""
        self.device_proto: int = 0
        self.device_caps: int | None = None
        # 一键登录状态：idle / running / done / error，及结果文案（前端轮询展示）。
        self.login_state: str = "idle"
        self.login_msg: str = ""
        self._logs: deque = deque(maxlen=log_max)
        # 单调递增的日志序号。环形缓冲写满后 recent_logs() 长度恒为 log_max，
        # 前端用「长度是否变化」判增量会永久失效（日志面板冻结），故额外给序号。
        self._log_seq: int = 0
        # 设备扫描状态：scanning 表示正在扫，scan_since 为本轮起始 monotonic 秒，
        # 供前端显示「正在搜索设备…」与倒计时（无地址时最坏需
        # SCAN_TIMEOUT + RESCAN_INTERVAL ≈ 45s，不能让用户干等）。
        self.scanning: bool = False
        self.scan_since: float = 0.0
        # 重扫等待：find_device 未命中后 sleep(RESCAN_INTERVAL) 期间，
        # 记录本轮等待的起始时刻与总时长，snapshot 时实时算剩余秒数，
        # 供前端显示「未找到设备，N 秒后重试」——否则这 25s 是空白。
        self.rescan_since: float = 0.0
        self.rescan_total: float = 0.0
        # 暂停自动搜索/自动连接：设备连不上时可临时暂停找设备，
        # 保留积分推送、IPC 审批、状态推流等其它功能。纯运行时状态，
        # 不持久化 —— 重启后自动恢复搜索，避免设备开机后反而连不上。
        self.search_paused: bool = False

    def log(self, msg: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line, flush=True)
        with self._lock:
            self._logs.append(line)
            self._log_seq += 1

    def begin_scan(self) -> None:
        with self._lock:
            self.scanning = True
            self.scan_since = time.monotonic()

    def end_scan(self) -> None:
        with self._lock:
            self.scanning = False
            self.scan_since = 0.0

    def begin_rescan(self, total: float) -> None:
        with self._lock:
            self.rescan_since = time.monotonic()
            self.rescan_total = total

    def end_rescan(self) -> None:
        with self._lock:
            self.rescan_since = 0.0
            self.rescan_total = 0.0

    def snapshot(self) -> dict:
        with self._lock:
            addr = self.address
            # 控制台配置只读一次：本函数每 2s 被前端轮询一次，逐键各读一遍
            # 会重复打开文件。
            cfg = console_cfg_load()
            # 首次配对判定（前端据此弹「首次连接」引导）：
            #   有地址：按「该地址是否记录过」判 —— 换新设备仍会提示。
            #   无地址：daemon 刚启动、还没扫描到设备，此时不能一律当成首次，
            #     否则每次启动都会误弹（设备明明连过）；只有这台机器从未成功
            #     连接过任何设备（known_devices.json 为空）才算首次。
            fp = (not is_known(addr)) if addr else (not _known_load())
            return {
                "running": self.running,
                "connected": self.connected,
                "address": addr,
                "sessions": list(self.sessions),
                "sess_total": self.sess_total,
                "credit": self.credit,
                "credit_reason": self.credit_reason,
                "battery": self.battery,
                # 能力握手：caps 为 None 表示设备还没发过 hello（老固件或未握手），
                # 前端据此显示"未握手"而不是"不支持"。
                "device_fw": self.device_fw,
                "device_proto": self.device_proto,
                "device_caps": self.device_caps,
                "device_caps_text": (
                    "未握手" if self.device_caps is None
                    else ("、".join(lbl for bit, lbl in CAP_LABELS.items()
                                    if self.device_caps & bit) or "无")),
                "login_state": self.login_state,
                "login_msg": self.login_msg,
                "first_pair": fp,
                # 扫描进度：供前端显示「正在搜索设备…+ 倒计时」。
                "scanning": self.scanning,
                "scan_elapsed": (round(time.monotonic() - self.scan_since, 1)
                                 if self.scanning and self.scan_since else 0.0),
                # 重扫倒计时：扫描未命中后的等待剩余秒数（无等待时为 0）。
                "rescan_remaining": (round(max(0.0, self.rescan_total - (time.monotonic() - self.rescan_since)), 1)
                                     if self.rescan_since else 0.0),
                # 是否暂停自动搜索/自动连接（前端开关）。
                "search_paused": self.search_paused,
                "log_seq": self._log_seq,
                "autostart": autostart_status() == "开启",
                "device_answer": bool(cfg.get("device_answer", True)),
                # 提示音音量（0..100，0 = 静音）。真源是本地配置文件，
                # 设备端不落 NVS，前端滑块据此回显。
                "beep_vol": beep_vol_get(cfg),
            }

    def recent_logs(self) -> list:
        with self._lock:
            return list(self._logs)

    def caps(self) -> int | None:
        """设备能力位（未握手返回 None）。加锁读，避免与 daemon 写入竞争。"""
        with self._lock:
            return self.device_caps

    def log_bundle(self) -> dict:
        """/api/log 的响应体：序号 + 行。

        前端必须按 seq 判增量而非 len(lines) —— 环形缓冲满载后长度恒定。
        """
        with self._lock:
            return {"seq": self._log_seq, "lines": list(self._logs)}

    def set(self, **kw) -> None:
        with self._lock:
            for k, v in kw.items():
                setattr(self, k, v)

# ===================== wb_status 内联 =====================

def encode_cwd(path: str) -> str:
    """把工作目录路径编码成 projects/ 下的转录目录名。

    ⚠ 这个函数原来只按 **Windows 盘符路径** 写，在 macOS 上全错 ——
    这是 2026-10-31 实机排查「设备只有声音、没有提问框」的**根因**：

        /Users/kevin/WebstormProjects/xiaozhi-workbuddy-board
          p[0]  → "/"          ← 把开头的斜杠当成了盘符
          p[2:] → "sers-kevin/…"  ← 连带吃掉了 "U"
          结果  → "/-sers-kevin-WebstormProjects-xiaozhi-workbuddy-board"

        而真实目录名是：
          "Users-kevin-WebstormProjects-xiaozhi-workbuddy-board"

    后果链条：read_transcript 拼出的路径不存在 → 直接返回 None →
    get_status_list 的 pending 恒为空 → 会话永远不进 NEED_ANSWER →
    _process_questions 没有任何 current 提问可推 →
    **设备上永远不弹提问框**。

    最阴险的地方是它**完全不报错**：read_transcript 对"文件不存在"
    本来就有 return None 的正常分支，所以日志里连一条异常都没有。
    状态、积分、今日消耗这些帧都不走这条路，全部照常工作 ——
    整台设备看起来"一切正常，就是不弹提问框"。

    正确规则（与本机 projects/ 下真实目录名逐一对齐）：
      Windows  C:\\Users\\foo\\bar   →  c-Users-foo-bar
      POSIX    /Users/kevin/foo     →  Users-kevin-foo
    即：把分隔符统一成 "-"，去掉开头的分隔符；Windows 保留小写盘符作前缀。
    """
    if not path:
        return ""
    p = path.replace("\\", "/")
    # Windows 盘符：形如 C:/Users/...（盘符后必跟冒号）
    is_drive = len(p) > 2 and p[1] == ":" and p[0].isalpha()
    if is_drive:
        drive = p[0].lower()
        rest = p[2:].replace("/", "-").strip("-")
        return f"{drive}-{rest}"
    # POSIX：去掉前导 "/"，其余分隔符换成 "-"
    return p.strip("/").replace("/", "-")

def map_state(status: str) -> str:
    s = (status or "").strip().lower()
    if s in ("completed", "done", "success", "finished"):
        return "COMPLETED"
    if s in ("running", "working", "executing", "in_progress", "active"):
        return "RUNNING"
    if s in ("idle", "waiting", "paused", "stale", "none", "pending"):
        return "IDLE"
    if s in ("error", "failed", "crashed"):
        return "ERROR"
    if s in ("needs_input", "awaiting_input", "question", "asking"):
        return "NEED_ANSWER"
    if s in ("needs_approval", "awaiting_approval", "pending_approval"):
        return "NEED_APPROVAL"
    return "RUNNING"

def to_display_text(text, maxlen: int) -> str:
    out = []
    for ch in text or "":
        o = ord(ch)
        if ch == " " or (o < 128 and ch.isprintable()):
            out.append(ch)
        elif 0x4E00 <= o <= 0x9FFF:
            out.append(ch)
    s = " ".join("".join(out).split())
    if len(s) > maxlen:
        s = s[:maxlen]
    return s

def _one_question(q: dict, idx: int) -> dict | None:
    """把 questions[] 里的一项归一化成设备提问页可直接用的结构。

    返回 {"qid", "hint", "options", "multi", "answerable"}；qid 优先取题目自带的
    id（ACP schema 里形如 "q_0"），缺失时按序号补 "q_<idx>"——**必须与
    resolveInterruption 的 answers 键一致**，否则服务端映射不回问题原文。
    """
    if not isinstance(q, dict):
        return None
    header = str(q.get("header") or "").strip()
    text = str(q.get("question") or "").strip()
    hint = f"{header}：{text}" if header else text
    options = []
    for opt in q.get("options") or []:
        label = str(opt.get("label") if isinstance(opt, dict) else opt or "").strip()
        if label:
            options.append(label)
    multi = q.get("multiSelect") is True
    qid = str(q.get("id") or "").strip() or f"q_{idx}"
    # 可作答门槛：1~6 个选项。多选同样可答——设备用勾选态（multi=1），
    # 旧固件不认multi 键时会退化成单选（见 _push_ask_item 的兼容处理）。
    return {"qid": qid, "hint": hint[:MSG_MAX], "options": options,
            "multi": multi,
            "answerable": 1 <= len(options) <= ASK_OPT_MAX}


def _question_info(arguments) -> dict:
    """从 AskUserQuestion 的 arguments 取出设备提问页所需信息。

    arguments 在转录里是 JSON 字符串（也兼容已是 dict）。只取标题/问题与选项
    label，不取 description——设备屏小、字库有限。

    返回 {"hint", "options", "answerable", "count", "multi", "qlist"}：
    · hint/options/answerable 是**第0 题**的视图（保持既有调用方语义不变）
    · qlist 是全部题目的归一化列表，供"多题串行化"逐题下发现场使用
      （设备一次只能显示一题，见 UnifiedDaemon._ask_queue）
    · answerable 仍要求单题：多题由队列逐题推送，每题各自判定
    """
    empty = {"hint": "", "options": [], "answerable": False,
             "count": 0, "multi": True, "qlist": []}
    try:
        args = json.loads(arguments) if isinstance(arguments, str) else arguments
        questions = args.get("questions") if isinstance(args, dict) else None
        if not isinstance(questions, list) or not questions:
            return dict(empty)
        qlist = []
        for i, q in enumerate(questions):
            one = _one_question(q, i)
            if one and one["options"]:
                qlist.append(one)
        if not qlist:
            return dict(empty)
        first = qlist[0]
        return {"hint": first["hint"], "options": first["options"],
                "answerable": (len(qlist) == 1 and first["answerable"]),
                "count": len(qlist), "multi": first["multi"], "qlist": qlist}
    except (ValueError, TypeError, AttributeError):
        return dict(empty)

def trunc_utf8(s: str, limit: int) -> str:
    """按 UTF-8 字节数截断字符串，绝不切碎多字节字符。

    设备端 extract_string() 在字符串放不进缓冲时会让整个 ask 帧解析失败，
    因此 text/tool 必须在这里就截到协议容量以内。
    """
    data = s.encode("utf-8")
    if len(data) <= limit:
        return s
    return data[:limit].decode("utf-8", errors="ignore")

def _sse_json_objects(text: str) -> list:
    """从 SSE 文本里解析出所有 JSON 对象（逐行取 data: 后的内容）。

    POST /api/v1/acp 的响应体是 SSE 形状（":ok" 注释行、"event: message"、
    "data: {...}"），不能对整段做 json.loads——那样永远解析失败（实测）。
    """
    out: list = []
    for raw in (text or "").splitlines():
        s = raw.strip()
        if s.startswith("data:"):
            s = s[5:].strip()
        if not s.startswith("{"):
            continue
        try:
            obj = json.loads(s)
        except ValueError:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    if not out:
        # 兜底：整段就是一个 JSON（无 SSE 包装，如部分 HTTP 错误体）。
        s = (text or "").strip()
        if s.startswith("{"):
            try:
                obj = json.loads(s)
            except ValueError:
                obj = None
            if isinstance(obj, dict):
                out.append(obj)
    return out

def read_transcript(cwd: str, session_id: str):
    """读会话转录，返回 {"active_tools", "has_reasoning", "pending"}。

    ⚠ 这里曾经只用「文件末尾 20KB + 最后 60 行」判定待答提问，踩了一个
    真实的坑（2026-10-31 实机排查）：

      会话转录会持续增长（本项目自己的转录已达 23.6MB）。提问发出后
      助手往往还在继续输出，几秒到几十秒内就能把那条 function_call
      推出 20KB / 60 行的窗口 —— 之后每 2 秒一轮的状态推流都看不到它，
      pending 恒为空，主机什么也不发。

      现象因此极具迷惑性：设备**有声音**（那是别的帧触发的提示音），
      但提问框永远不出现。实测证据：20:12:20 / 20:20:05 两次真实提问
      距文件末尾 416 / 315 行、21.9 / 22.1MB，全部落在窗口外；
      daemon 日志里一整天没有任何一条「提问页已下发」，只有手动测试。

    现在拆成两路：

      1) **近况**（active_tools / has_reasoning）：仍只看尾部最近 60 行。
         它们本来就代表"现在在干什么"，看历史没有意义。

      2) **待答提问**（pending）：走 `_ask_open` 全量增量状态。每个文件
         记住已消费到的字节偏移，只读新增部分，把 function_call /
         function_call_result 累积成"当前仍未闭合的提问集合"。
         这样无论文件多大、提问被推出窗口多远，只要没闭合就一直可见。

    增量游标放在模块级缓存里（_TRANS_CURSOR），按 (path, size) 判断能否续读：
      文件变小（被截断/重建）、或 inode 换掉 → 从头重扫该文件。
    """
    if not cwd or not session_id:
        return None
    path = os.path.join(PROJECTS, encode_cwd(cwd), session_id + ".jsonl")
    if not os.path.exists(path):
        return None
    try:
        size = os.path.getsize(path)
    except OSError:
        return None

    # ---- 近况：尾部窗口，保持原语义 ----
    try:
        with open(path, "rb") as f:
            f.seek(max(0, size - TRANS_TAIL_BYTES))
            tail = f.read().decode("utf-8", errors="replace")
    except OSError:
        return None
    lines = [ln for ln in tail.splitlines() if ln.strip()]

    open_tools = set()
    has_reasoning = False
    for line in lines[-TRANS_RECENT_LINES:]:
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        t = ev.get("type")
        if t == "reasoning":
            has_reasoning = True
        elif t == "function_call":
            cid = str(ev.get("callId") or ev.get("id") or ev.get("timestamp") or line)
            open_tools.add(cid)
        elif t == "function_call_result":
            cid = str(ev.get("callId") or ev.get("id") or "")
            if cid:
                open_tools.discard(cid)

    # ---- 待答提问：全量增量扫描 ----
    pending = _ask_scan_incremental(path, size)

    return {"active_tools": len(open_tools), "has_reasoning": has_reasoning,
            "pending": pending}


def _ask_scan_incremental(path: str, size: int) -> list[dict]:
    """增量扫描转录，返回仍未闭合的 AskUserQuestion 列表。

    为什么必须增量（而不是每次全量读）：这函数被状态推流每 ~2 秒调用一次，
    一次要处理 3 个会话的转录。全量读 23MB × 3 × 每 2 秒 = 每秒 35MB 的
    IO + JSON 解析，直接把 daemon 拖死。增量只读"上次之后新增的字节"，
    稳态下每次只读几 KB。

    状态存在 _TRANS_CURSOR[path]["asks"] 里：{cid: {cid, tool, **问题信息}}。
    看见 function_call 就加入，看见同 cid 的 function_call_result 就移除。
    这是**累积状态**，与"最后 N 行"无关 —— 因此提问被推出尾窗也不会消失。
    """
    state = _TRANS_CURSOR.get(path)
    if state is None or size < state.get("size", 0):
        # 首次见到 / 文件变小（被截断或重建）→ 丢弃旧状态。
        #
        # 首扫从末尾回退 TRANS_FIRST_WINDOW 开始（不是从 0）：见
        # TRANS_FIRST_WINDOW 的注释 —— 从 0 起既追得慢，又会把陈年残题
        # 当成待答重新弹出来。回退点若落在半行中间，靠下面的
        # "只消费到最后一个换行" 自动对齐到下一条完整记录。
        start = max(0, size - TRANS_FIRST_WINDOW)
        # 落在半行时，跳到下一个换行后，保证第一条记录是完整的
        if start > 0:
            try:
                with open(path, "rb") as f:
                    f.seek(start - 1)
                    if f.read(1) != b"\n":
                        with open(path, "rb") as g:
                            g.seek(start)
                            rest = g.read(min(64, size - start))
                        pos = rest.find(b"\n")
                        if pos >= 0:
                            start += pos + 1
            except OSError:
                pass
        state = {"off": start, "size": size, "asks": {}, "open": set(),
                 "first_off": start}
        _TRANS_CURSOR[path] = state

    off = state["off"]
    if size < off:
        off = 0
        state["asks"] = {}
        state["open"] = set()

    todo = size - off
    if todo > 0:
        # 一轮就把新增部分追平（不设上限）：内部按 TRANS_INCR_MAX 分块，
        # 既不会一次读爆内存，也不会因为"读不完"把事件推迟到下一轮。
        # 稳态下 todo 通常只有几 KB。
        while state["off"] < size:
            off = state["off"]
            limit = min(size - off, TRANS_INCR_MAX)
            try:
                with open(path, "rb") as f:
                    f.seek(off)
                    chunk = f.read(limit)
            except OSError:
                break
            if not chunk:
                break
            nl = chunk.rfind(b"\n")
            if nl < 0:
                if off + limit < size:
                    # 一整块都是半行且还有剩余 → 等它写完整再来
                    break
                nl = len(chunk) - 1
            consumed = nl + 1
            state["off"] = off + consumed

            text = chunk[:consumed].decode("utf-8", errors="replace")
            for line in text.splitlines():
                if not line.strip():
                    continue
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                t = ev.get("type")
                if t == "function_call":
                    cid = str(ev.get("callId") or ev.get("id")
                              or ev.get("timestamp") or line)
                    state["open"].add(cid)
                    if str(ev.get("name") or "").strip().lower() == "askuserquestion":
                        state["asks"][cid] = {
                            "cid": cid, "tool": str(ev.get("name") or ""),
                            **_question_info(ev.get("arguments")),
                        }
                elif t == "function_call_result":
                    cid = str(ev.get("callId") or ev.get("id") or "")
                    if cid:
                        state["open"].discard(cid)
                        state["asks"].pop(cid, None)
            if consumed <= 0:
                break

    state["size"] = size
    return list(state["asks"].values())

def get_usage_tokens(session_id: str) -> int:
    if not session_id:
        return 0
    try:
        con = sqlite3_connect_ro(DB)
        row = con.execute(
            "SELECT used FROM session_usage WHERE session_id=?",
            (session_id,)).fetchone()
        con.close()
        return int(row[0]) if row else 0
    except Exception:
        return 0

def get_usage_credit(session_id: str) -> float:
    """会话消耗积分：解析 session_usage.credit_json（JSON {账户id:金额}）求和。"""
    if not session_id:
        return 0.0
    try:
        con = sqlite3_connect_ro(DB)
        row = con.execute(
            "SELECT credit_json FROM session_usage WHERE session_id=?",
            (session_id,)).fetchone()
        con.close()
        if not row or not row[0]:
            return 0.0
        import json as _json
        d = _json.loads(row[0])
        return float(sum(d.values())) if isinstance(d, dict) else 0.0
    except Exception:
        return 0.0


def usage_scan_today(day_start: int) -> dict:
    """扫描会话转录 jsonl，汇总「从 day_start 起到现在」的请求级 token 用量。

    day_start 是本机自然日 0 点的毫秒时间戳（调用方给，不在这里算 ——
    这里自己算的话，一轮跑在 23:59:59、下一轮在 00:00:01 会跳变，
    而调用方持有跨轮状态，能保证同一轮里口径一致）。

    返回 {today, req, cache, models:[{n,v,hit}], total_all}：
      today  今日 total_tokens 之和
      req    今日请求数
      cache  今日缓存命中的 prompt token 数
      models 今日按模型 TOP N（已含"其它"合并，见 USAGE_MODELS_MAX）
      total_all 今日所有请求的 total_tokens（= today 的别名，留给 PC 端算占比）

    ---- 为什么要按 conversationRequestId 去重 ----
    同一次请求会在转录里出现多条带 usage 的行（工具调用、快照、补充记录
    都会重复挂同一份 rawUsage）。不去重会把一次请求算 3~5 次。这里优先用
    providerData.conversationRequestId，缺失时退回行 id：两者都是请求级
    唯一值，比"时间戳 + token 数"可靠（同一秒并发两个请求很常见）。
    """
    by_model: dict[str, int] = {}
    by_model_hit: dict[str, int] = {}
    today = 0
    req = 0
    cache = 0
    seen: set[str] = set()

    for path in _usage_files():
        try:
            fh = open(path, encoding="utf-8", errors="ignore")
        except OSError:
            continue
        with fh:
            for line in fh:
                # 先做字符串预筛：parse 一行 142MB 里的绝大多数行都是白花的。
                if '"usage"' not in line and 'rawUsage' not in line:
                    continue
                try:
                    o = json.loads(line)
                except Exception:
                    continue
                pd = o.get("providerData") or {}
                us = pd.get("rawUsage") or pd.get("usage")
                if not isinstance(us, dict):
                    continue
                ts = o.get("timestamp")
                if not isinstance(ts, (int, float)) or ts < day_start:
                    continue
                total = int(us.get("total_tokens") or 0)
                if total <= 0:
                    continue
                rid = pd.get("conversationRequestId") or o.get("id")
                if rid:
                    if rid in seen:
                        continue
                    seen.add(str(rid))
                model = str(pd.get("model") or o.get("model") or "").strip() or "(未标模型)"
                hit = int((us.get("prompt_tokens_details") or {}).get("cached_tokens") or 0)
                by_model[model] = by_model.get(model, 0) + total
                by_model_hit[model] = by_model_hit.get(model, 0) + hit
                today += total
                cache += hit
                req += 1

    # 按用量降序，前 N-1 条单独列出，其余并进"其它"。
    # 为什么 N-1：屏上 5 行位，若恰好 6 个模型就会剩 1 条无名可归；
    # 留一格给"其它"能让屏幕上的数之和恒等于 today，对账不会差。
    ranked = sorted(by_model.items(), key=lambda kv: kv[1], reverse=True)
    models: list[dict] = []
    keep = max(1, USAGE_MODELS_MAX - 1)
    for name, val in ranked[:keep]:
        models.append({"n": _clip_model(name), "v": val,
                       "hit": by_model_hit.get(name, 0)})
    rest = ranked[keep:]
    if rest:
        models.append({"n": "其它",
                       "v": sum(v for _, v in rest),
                       "hit": sum(by_model_hit.get(k, 0) for k, _ in rest)})

    # 截断后必须去重：'trae/deepseek-v4.1-flash' 和 'deepseek-v4.1-flash'
    # 去掉前缀后前 15 字符完全一样，屏幕上会出现两行一模一样的
    # 'deepseek-v4.1-f…' 而数字不同，看着像 bug。
    _dedup_model_names(models, ranked[:keep])

    return {"today": today, "req": req, "cache": cache,
            "models": models, "total_all": today}


def _dedup_model_names(models: list[dict], originals: list[tuple[str, int]]) -> None:
    """把 models[].n 里的重名就地消歧，保持"前几位更重要"的直觉。

    策略：重名的第二条起改用**尾截断**（保留末 USAGE_MODELS_MAX-1 字符）。
    模型名尾巴通常是型号/档位（-flash / -flashx / -preview），比开头
    'deepseek' 这种公用前缀更能区分。尾截断仍冲突时退回编号，
    编号至少能保证不重名（数字对不上时用户也不会误读成同一个模型）。
    """
    seen: dict[str, int] = {}
    for i, md in enumerate(models):
        name = md["n"]
        if name not in seen:
            seen[name] = 1
            continue
        # 撞名：用原始全名做尾截断
        orig = originals[i][0] if i < len(originals) else name
        tail = orig[-(USAGE_MODEL_NAME_MAX - 1):]
        cand = "…" + tail
        if cand in seen:
            seen[name] += 1
            cand = f"{name[:USAGE_MODEL_NAME_MAX - 3]}·{seen[name]}"
        seen[cand] = 1
        md["n"] = cand


def _clip_model(name: str) -> str:
    """模型名压到 USAGE_MODEL_NAME_MAX 个字符以内，尽量保住区分度。

    这里按**字符**截（不是字节）：设备是 LVGL 按字宽排版，12px 字号下
    ASCII 与汉字各占不同宽度，按字节截反而会让"混排名"被砍得莫名其妙。

    ⚠ 不做"去供应商前缀"的优化。试过把 trae/ 和 custom-local: 剥掉，
    结果 `trae/deepseek-v4.1-flash` 和 `deepseek-v4.1-flash` 变成同一个
    字符串 —— 同一台机器上同时跑官方和第三方通道是常态，前缀正是
    唯一的区分点。宁可让名字长一点、中间省略，也不能把两个模型抹成一个。
    """
    s = str(name or "").strip()
    if len(s) <= USAGE_MODEL_NAME_MAX:
        return s
    return _fit_middle(s)


def _fit_middle(s: str, keep_head: int = 6) -> str:
    """中段省略：保留开头 keep_head 个字符 + 结尾若干，中间用"…"。

    单纯尾截断会得到 '…seek-v4.1-flash'，看着像被切坏的字符串；
    单纯头截断会得到 'deepseek-v4.1-f…'，丢掉了最有区分度的档位后缀。
    两种都保留正好能同时看到"哪个系列 + 哪个档位"。
    """
    room = USAGE_MODEL_NAME_MAX - 1          # 减去省略号本身
    if room <= keep_head:
        return s[:USAGE_MODEL_NAME_MAX - 1] + "…"
    tail = room - keep_head
    return s[:keep_head] + "…" + s[-tail:]


def _usage_files() -> list[str]:
    """列出所有会话转录 jsonl。子代理日志（<session>/subagents/*.jsonl）
    也在其中 —— 子代理的 token 是真消耗，不能漏。"""
    out: list[str] = []
    try:
        for root, _dirs, files in os.walk(PROJECTS):
            for f in files:
                if f.endswith(".jsonl"):
                    out.append(os.path.join(root, f))
    except OSError:
        pass
    return out

def get_status_list(limit: int = 3, hide_cids: set | None = None,
                    total_semantics: bool = False) -> dict:
    """汇总最近会话状态。hide_cids：已作答的提问 cid（转录尚未落 result
    时也要从 pending 里剔除，避免会话被误标 NEED_ANSWER）。

    total_semantics：`count` 用哪种语义（C2 / caps bit3）。
      False（默认）：count = 本次下发的条数，即 len(tasks)。老固件的
        head 显示「N PIPELINES」，实际是"这台设备上显示了几条"。
      True：count = 会话总数（同过滤条件），设备据此显示真实总数，
        tasks 仍只发最多 3 条。
    为什么不用"多一个字段"来区分：设备侧 JSON 节点池是固定的，
    同键改义只能靠 caps 协商，不能靠"老固件忽略新键"。
    """
    tasks: list[dict] = []
    total = 0
    try:
        con = sqlite3_connect_ro(DB)
        rows = con.execute(
            "SELECT id,cwd,title,custom_title,status,model "
            "FROM sessions WHERE deleted_at IS NULL "
            "ORDER BY last_activity_at DESC LIMIT ?",
            (int(limit),)).fetchall()
        if total_semantics:
            # 第二条查询只在需要时才发：老路径完全不碰 COUNT，
            # 避免为用不到的数字多开一次游标。
            row = con.execute(
                "SELECT COUNT(*) AS n FROM sessions "
                "WHERE deleted_at IS NULL").fetchone()
            total = int(row["n"]) if row else 0
        con.close()
        for s in rows:
            state = map_state(s["status"])
            tr = read_transcript(s["cwd"] or "", s["id"] or "")
            pending = (tr or {}).get("pending") or []
            if hide_cids:
                pending = [q for q in pending
                           if str(q.get("cid") or "") not in hide_cids]
            if tr and tr.get("active_tools") and state in ("IDLE", "COMPLETED"):
                state = "RUNNING"
            name = to_display_text(s["custom_title"] or s["title"] or "",
                                   DEV_FIELD_MAX) or "task"
            model = to_display_text(s["model"] or "", MODEL_MAX)
            credit = get_usage_credit(s["id"])
            # 有未闭合的 AskUserQuestion：库里状态往往仍是 IDLE/COMPLETED，
            # 这里按转录纠正为 NEED_ANSWER，并把问题摘要带出来。
            msg = ""
            if pending:
                state = "NEED_ANSWER"
                msg = to_display_text(pending[0].get("hint") or "", DEV_FIELD_MAX)
            tasks.append({"state": state, "model": model, "name": name,
                          "credit": credit, "msg": msg, "questions": pending,
                          "sid": s["id"], "cwd": s["cwd"] or ""})
    except Exception:
        tasks = []
        total = 0
    # 总数语义下若 COUNT 失败（total=0）但明明有 tasks，退回条数：
    # 显示 0 比显示"3"更误导。
    if total_semantics and total < len(tasks):
        total = len(tasks)
    return {"type": "status_list",
            "count": (total if total_semantics else len(tasks)),
            "tasks": tasks}

def sqlite3_connect_ro(path: str):
    """只读连接 sqlite（按需导入 sqlite3，避免顶层依赖影响其它路径）。"""
    import sqlite3
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=1)
    con.row_factory = sqlite3.Row
    return con

# ===================== 控制台本地配置 =====================

def console_cfg_load() -> dict:
    try:
        with open(CONSOLE_CFG, encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}

def console_cfg_save(d: dict) -> bool:
    try:
        with open(CONSOLE_CFG, "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False)
        return True
    except Exception:
        return False

def beep_vol_clamp(v) -> int:
    """把提示音音量夹到 0..100（容忍字符串/浮点/越界；坏值退回缺省）。"""
    try:
        iv = int(float(v))
    except (TypeError, ValueError):
        return BEEP_VOL_DEFAULT
    return max(0, min(100, iv))

def beep_vol_get(cfg: dict | None = None) -> int:
    """取提示音音量（缺键或坏值走缺省 100，与固件默认值对齐）。"""
    if cfg is None:
        cfg = console_cfg_load()
    return beep_vol_clamp(cfg.get("beep_vol", BEEP_VOL_DEFAULT))

# ---- 设备发消息：新任务默认参数（只存 PC 端，随控制台配置落盘） ----
# 键名 wb_job_cwd / wb_job_agent / wb_job_model；空串表示不传（走 WorkBuddy 默认）。
WB_JOB_CFG_KEYS = ("cwd", "agent", "model")
# 默认工作目录：WorkBuddy 派发后台任务时的 cwd，缺省取当前工作区。
WB_JOB_CWD_DEFAULT = "D:/wpslinxi/foloty"


def wb_job_cfg_get(cfg: dict | None = None) -> dict:
    """取新任务默认参数（cwd/agent/model），缺键走缺省，坏值忽略。"""
    if cfg is None:
        cfg = console_cfg_load()
    out = {"cwd": WB_JOB_CWD_DEFAULT, "agent": "", "model": ""}
    for k in WB_JOB_CFG_KEYS:
        v = cfg.get(f"wb_job_{k}")
        if isinstance(v, str) and v.strip():
            out[k] = v.strip()
    return out


def wb_job_cfg_set(patch: dict) -> bool:
    """写入新任务默认参数（只认 cwd/agent/model 三个键，其余忽略）。"""
    cfg = console_cfg_load()
    changed = False
    for k in WB_JOB_CFG_KEYS:
        if isinstance(patch.get(k), str):
            cfg[f"wb_job_{k}"] = patch[k].strip()
            changed = True
    return console_cfg_save(cfg) if changed else True

# ---- 提问"不再重推"名单（持久化） ----
# 两类入账：① 用户主动跳过（长按 DOWN）；② 已成功受理（resolved=true，
# 防转录不落 result 时重启回潮）。自动判定的失效不落盘——那是启发式，
# 重启后应给它一次翻盘机会（可能是时序问题造成的误判）。

def skip_list_load() -> list:
    try:
        with open(SKIP_FILE, encoding="utf-8") as f:
            d = json.load(f)
        items = d.get("skipped") if isinstance(d, dict) else None
        return [c for c in items if isinstance(c, str)] if isinstance(items, list) else []
    except Exception:
        return []

def skip_list_save(items: list) -> bool:
    try:
        os.makedirs(os.path.dirname(SKIP_FILE), exist_ok=True)
        with open(SKIP_FILE, "w", encoding="utf-8") as f:
            json.dump({"skipped": items[-SKIP_MAX_STORE:]}, f, ensure_ascii=False)
        return True
    except Exception:
        return False

def skip_list_add(cid: str) -> None:
    items = skip_list_load()
    if cid in items:
        items.remove(cid)
    items.append(cid)
    skip_list_save(items)

# ===================== 本地 ACP worker 发现 =====================

def _process_image_name(pid: int) -> str:
    """取进程映像完整路径（只读，失败返回 ''）。

    macOS 用 ps -o comm=（Electron 会给出 .app/Contents/MacOS/Electron 全路径）；
    Windows 用 QueryFullProcessImageNameW。
    """
    if sys.platform == "darwin":
        try:
            return subprocess.run(["ps", "-p", str(pid), "-o", "comm="],
                                  capture_output=True, text=True,
                                  timeout=5).stdout.strip()
        except Exception:
            return ""
    import ctypes
    from ctypes import wintypes
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not h:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return buf.value
    finally:
        k32.CloseHandle(h)
    return ""

def _local_listen_port_pid() -> dict[int, int]:
    """枚举 127.0.0.1 上处于 LISTEN 的 TCP 端口 → 进程 pid。

    Windows 解析 netstat -ano；macOS/Linux 用 lsof（BSD netstat 的字段和
    含义都不同，不能直接套用）。两台都是系统自带命令，零第三方依赖。
    """
    port_pid: dict[int, int] = {}
    if sys.platform == "darwin":
        try:
            out = subprocess.run(["lsof", "-nP", "-iTCP", "-sTCP:LISTEN", "-F", "pn"],
                                 capture_output=True, text=True, timeout=8).stdout
        except Exception:
            return {}
        pid = 0
        for line in out.splitlines():
            if line.startswith("p"):
                try:
                    pid = int(line[1:])
                except ValueError:
                    pid = 0
            elif line.startswith("n") and pid:
                addr = line[1:]
                if not addr.startswith("127.0.0.1:"):
                    continue
                try:
                    port_pid[int(addr.rsplit(":", 1)[1])] = pid
                except ValueError:
                    continue
        return port_pid
    try:
        # Windows 中文系统下 netstat 走 CREATE_NO_WINDOW 时会输出 GBK（表头
        # "活动连接/协议…"），而进程环境 PYTHONUTF8=1 时 text=True 按 UTF-8
        # 解码会失败，读线程报 UnicodeDecodeError 并让 stdout 变成 None。
        # errors="replace" 保证按 ASCII 的 TCP 行仍能正常解析；stdout or ""
        # 兜住任何解码异常，避免 None.splitlines() 把整个 /api/wb-live 打挂。
        out = subprocess.run(["netstat", "-ano", "-p", "tcp"],
                             capture_output=True, text=True, timeout=8,
                             creationflags=SUB_NO_WINDOW,
                             encoding="utf-8", errors="replace").stdout or ""
    except Exception:
        return {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 5 or parts[0].upper() != "TCP":
            continue
        local = parts[1]
        if not local.startswith("127.0.0.1:"):
            continue
        if parts[3].upper() != "LISTENING":
            continue
        try:
            port_pid[int(local.rsplit(":", 1)[1])] = int(parts[4])
        except ValueError:
            continue
    return port_pid


def _is_workbuddy_image(img: str) -> bool:
    """按进程映像路径判断是否 WorkBuddy 系进程（桌面端或它拉起的 worker）。

    macOS 桌面端进程名就是 Electron，只能看 .app 内的完整路径；Windows 看
    映像名，另外也认 WorkBuddy 自带 node 运行时跑起来的本地网关 worker。
    """
    low = img.lower()
    base = os.path.basename(low)
    if base in _WB_WORKER_IMAGES:
        return True
    if "workbuddy" in low or "codebuddy" in low:
        return True
    # macOS：/Applications/WorkBuddy.app/Contents/MacOS/Electron 这类路径里
    # 没有小写 workbuddy 的情况已由上面的小写判断覆盖；这里再兜一层 .app 名。
    return False


def _workbuddy_pid_ports() -> list[tuple[int, int]]:
    """枚举 127.0.0.1 上属于 WorkBuddy 的监听端口，返回 (pid, port)。

    保留 pid 是给投递层用的：桌面端把网关密码经环境变量注入 worker 进程，
    密码不落盘，只有拿到 pid 才能从它的环境块里读出来（Windows 读 PEB，
    macOS 读 ps eww）。
    """
    result = []
    for port, pid in _local_listen_port_pid().items():
        if _is_workbuddy_image(_process_image_name(pid) or ""):
            result.append((pid, port))
    return sorted(set(result))

_WB_WORKER_IMAGES = {"workbuddy.exe", "codebuddy.exe", "cbc.exe", "cbc-prewarm.exe"}

def _list_workbuddy_ports() -> list[int]:
    """枚举 127.0.0.1 上属于 WorkBuddy 的监听 TCP 端口（仅端口号）。"""
    return sorted({port for _pid, port in _workbuddy_pid_ports()})

# ===================== WorkBuddy 投递层（设备发消息的出口） =====================
# 与 ACP 完全无关：一律走 REST，绝不碰 ACP writer —— session/load 会抢会话的
# agent connection（写者角色），导致模型续跑事件流向本连接、桌面 UI 断流
# （2026-09-30 教训）。POST /api/v1/sessions/{id}/reply 的规格原文即「向当前
# 活会话投递回复（不占用 ACP writer）」，实测 delivered:true 且 writerOccupied
# 保持 true；ACP 那套继续只做「端口哨兵」（发现 + 状态）。
#
# 鉴权：--serve 默认 --auth password。密码由桌面端每次启动随机生成（32B
# base64url），只经环境变量注入 worker 进程（CODEBUDDY_GATEWAY_AUTH /
# CODEBUDDY_GATEWAY_PASSWORD），不落盘、settings.json 里也没有，所以只能从
# worker 进程的 PEB 环境块读（纯读操作，不写目标进程任何内存）。
#
# 实测（2026-10-09）：GET /api/v1/sessions/live、POST /sessions/{id}/reply、
# POST /api/v1/jobs 三个端点在本机 worker 上全部可用。

WB_SECRET_TTL = 60.0        # 网关密钥缓存（桌面重启即换密钥，过期重读）
WB_LIVE_TTL = 3.0           # 活会话探测缓存（别被前端轮询打爆 worker）
WB_HTTP_TIMEOUT = 8.0
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
PEB_PROCESS_PARAMETERS_OFFSET = 0x20
PARAMS_ENVIRONMENT_OFFSET = 0x80
ENV_READ_SIZE = 0x8000

_wb_lock = threading.Lock()
_wb_cache: dict = {"secret": "", "secret_at": 0.0, "live": [], "live_at": 0.0}


def _wb_read_process_env(pid: int) -> dict:
    """读目标进程的环境块（Windows 走 PEB；macOS 走 ps eww）。

    Windows 没有公开 API 能枚举「别的进程」的环境，只能按 PEB 结构取；
    这里只读内存，不注入、不写、不挂起目标进程。
    """
    if sys.platform == "darwin":
        return _darwin_read_process_env(pid)
    import ctypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")

    class _PBI(ctypes.Structure):
        _fields_ = [("Reserved1", ctypes.c_void_p),
                    ("PebBaseAddress", ctypes.c_void_p),
                    ("Reserved2", ctypes.c_void_p * 2),
                    ("UniqueProcessId", ctypes.c_void_p),
                    ("Reserved3", ctypes.c_void_p)]

    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION
                             | PROCESS_QUERY_LIMITED_INFORMATION
                             | PROCESS_VM_READ, False, int(pid))
    if not handle:
        raise OSError(f"OpenProcess({pid}) 失败: {ctypes.get_last_error()}")

    def _read(addr: int, size: int) -> bytes:
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), buf,
                                     ctypes.c_size_t(size), ctypes.byref(got)):
            raise OSError(f"ReadProcessMemory 失败: {ctypes.get_last_error()}")
        return buf.raw[:got.value]

    try:
        pbi = _PBI()
        ret = ctypes.c_ulong(0)
        st = ntdll.NtQueryInformationProcess(ctypes.c_void_p(handle), 0,
                                             ctypes.byref(pbi), ctypes.sizeof(pbi),
                                             ctypes.byref(ret))
        if st != 0 or not pbi.PebBaseAddress:
            raise OSError(f"NtQueryInformationProcess 失败: 0x{st & 0xffffffff:x}")
        params = int.from_bytes(_read(int(pbi.PebBaseAddress)
                                      + PEB_PROCESS_PARAMETERS_OFFSET, 8), "little")
        if not params:
            raise OSError("ProcessParameters 为空")
        env_ptr = int.from_bytes(_read(params + PARAMS_ENVIRONMENT_OFFSET, 8), "little")
        if not env_ptr:
            raise OSError("Environment 指针为空")
        raw = _read(env_ptr, ENV_READ_SIZE)
    finally:
        k32.CloseHandle(handle)

    text = raw.decode("utf-16-le", "replace")
    end = text.find("\x00\x00")
    if end != -1:
        text = text[:end]
    out: dict = {}
    for item in text.split("\x00"):
        if "=" in item:
            k, _, v = item.partition("=")
            out[k] = v
    return out


def _darwin_read_process_env(pid: int) -> dict:
    """macOS 读同用户进程的环境变量：ps eww 会把「命令行 + 环境」一起打出来。

    只用于取网关密钥（base64url，无空格）；带空格的变量值会被按空白切开，
    属已知限制，不影响 CODEBUDDY_GATEWAY_* 两项。
    """
    try:
        out = subprocess.run(["ps", "eww", "-p", str(pid)],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return {}
    env: dict = {}
    for token in out.split():
        if token.count("=") and token.split("=", 1)[0].replace("_", "").isalnum():
            k, _, v = token.partition("=")
            env.setdefault(k, v)
    return env


def wb_gateway_secret(force: bool = False) -> str:
    """取网关密钥（密码明文）：本进程环境 → settings.json → worker 进程环境。

    实测密码只在 worker 进程环境里，前两处只是兜底（自建 --serve 的场景）。
    """
    with _wb_lock:
        now = time.monotonic()
        if not force and now - _wb_cache["secret_at"] < WB_SECRET_TTL:
            return _wb_cache["secret"]
    secret = os.environ.get("CODEBUDDY_GATEWAY_PASSWORD") or ""
    if not secret:
        for path in (os.path.expanduser("~/.codebuddy/settings.json"),
                     os.path.expanduser("~/.workbuddy/settings.json")):
            try:
                with open(path, encoding="utf-8") as f:
                    pw = (json.load(f) or {}).get("gateway", {}).get("password")
                if pw:
                    secret = str(pw)
                    break
            except Exception:
                continue
    if not secret:
        for pid, _port in _workbuddy_pid_ports():
            try:
                env = _wb_read_process_env(pid)
            except OSError:
                continue
            if (env.get("CODEBUDDY_GATEWAY_AUTH") == "password"
                    and env.get("CODEBUDDY_GATEWAY_PASSWORD")):
                secret = env["CODEBUDDY_GATEWAY_PASSWORD"]
                break
    with _wb_lock:
        _wb_cache["secret"] = secret
        # 只在**取到**时才记时间戳：取不到就让它保持过期，下次调用立刻重试。
        # 否则一次瞬时失败（worker 换代的窗口期）会被缓存成整整 60 秒的
        # 「网关未运行」，把一两秒的抖动放大成一分钟。
        _wb_cache["secret_at"] = time.monotonic() if secret else 0.0
    return secret


def wb_rest(port: int, path: str, secret: str, method: str = "GET",
            body: dict | None = None, timeout: float = WB_HTTP_TIMEOUT):
    """向本机 worker 发一次 REST 请求，返回 (status, text)；网络异常 status 为 None。"""
    data = None
    headers = {"Authorization": f"Bearer {secret}", "x-codebuddy-request": "1"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                                 headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # 有些 worker 端口会返回 400 且 body 迟迟不结束，e.read() 超时会抛
        # TimeoutError —— 它发生在 except 块内，下面那层 except 接不住，会
        # 直接冒到 /api/wb-live。读 body 再兜一层，坏端口只算这一条失败。
        try:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception:
            return e.code, ""
    except Exception as e:
        return None, str(e)


def wb_live_sessions(force: bool = False) -> list[dict]:
    """遍历 worker 端口问 /api/v1/sessions/live，返回活会话列表。

    每项 {pid, port, sessionId, writerOccupied}。多个活会话时按
    「writerOccupied 优先 + 端口升序」稳定排序 —— REST 面不给活跃时间，
    这是当前能拿到的最稳的启发式（桌面交互会话通常 writerOccupied=true）。
    """
    with _wb_lock:
        if not force and time.monotonic() - _wb_cache["live_at"] < WB_LIVE_TTL:
            return list(_wb_cache["live"])
    secret = wb_gateway_secret()
    out: list[dict] = []
    if secret:
        for pid, port in _workbuddy_pid_ports():
            code, text = wb_rest(port, "/api/v1/sessions/live", secret, timeout=4.0)
            if code != 200:
                continue
            try:
                d = json.loads(text).get("data") or {}
            except Exception:
                continue
            sid = str(d.get("sessionId") or "")
            if sid:
                out.append({"pid": pid, "port": port, "sessionId": sid,
                            "writerOccupied": bool(d.get("writerOccupied"))})
    out.sort(key=lambda s: (not s["writerOccupied"], s["port"]))
    with _wb_lock:
        _wb_cache["live"] = out
        _wb_cache["live_at"] = time.monotonic()
    return list(out)


def wb_reply_live(text: str, session: dict | None = None) -> dict:
    """把一句话投进当前活会话（不占用 ACP writer）。"""
    secret = wb_gateway_secret()
    if not secret:
        return {"ok": False, "kind": "reply",
                "err": "未取到 WorkBuddy 网关密钥（WorkBuddy 桌面端未在运行？）"}
    live = wb_live_sessions(force=True)
    if not live:
        return {"ok": False, "kind": "reply",
                "err": "当前没有活会话（WorkBuddy 里没有正在进行的对话）"}
    tgt = session or live[0]
    code, body = wb_rest(tgt["port"], f"/api/v1/sessions/{tgt['sessionId']}/reply",
                         secret, method="POST", body={"text": text}, timeout=15.0)
    if code == 200:
        return {"ok": True, "kind": "reply", "port": tgt["port"],
                "sessionId": tgt["sessionId"], "detail": "已投递到活会话"}
    if code == 409:
        return {"ok": False, "kind": "reply",
                "err": "该会话已不是活会话（SESSION_FOLLOW_NOT_LIVE），可改用新建任务"}
    return {"ok": False, "kind": "reply", "err": f"HTTP {code}: {body[:200]}"}


# -------------------------------------------------------------------- ACP 投递
# 为什么需要这条通道：REST `POST /api/v1/sessions/{id}/reply` 只在「该会话仍是
# 当前活会话」时才收（canDeliverFollowReply），会话空闲被 worker 回收后
# `/api/v1/sessions/live` 返回 null，这条路就只剩 409。
# ACP `session/prompt` 不要求会话是活的（实测 live=null 时照样投递成功），
# 代价是要先 `session/resume`（带 cwd）把会话 hydrate 起来。
# clientInfo.name 必须挂 "codebuddy-web-ui"：服务端有「外部 writer 抢占」拦截
# （foreignWriterBlocked），但请求方与持有方同为 webUi 客户端时该拦截不生效，
# 挂别的名字会被 -32000 writer_occupied 直接拒掉。
WB_ACP_CLIENT_NAME = "codebuddy-web-ui"
ACP_DELIVER_TIMEOUT = 180.0


def _wb_http(port: int, path: str, secret: str, method: str = "GET",
             body: dict | None = None, headers: dict | None = None,
             timeout: float = 15.0) -> tuple[int | None, str]:
    """带自定义 header 的一次性 REST 调用（ACP 要 acp-connection-id，wb_rest 不支持）。"""
    h = {"Authorization": f"Bearer {secret}", "x-codebuddy-request": "1"}
    if headers:
        h.update(headers)
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        h["Content-Type"] = "application/json"
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                                 headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # 有些 worker 端口会返回 400 且 body 迟迟不结束，e.read() 超时会抛
        # TimeoutError —— 它发生在 except 块内，下面那层 except 接不住，会
        # 直接冒到 /api/wb-live。读 body 再兜一层，坏端口只算这一条失败。
        try:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception:
            return e.code, ""
    except Exception as e:
        return None, str(e)


def _wb_acp_rpc(port: int, secret: str, cid: str, rid: int, method: str,
                params: dict, timeout: float = 60.0,
                abort_after: int = 0) -> tuple[dict | None, dict]:
    """发一条 ACP JSON-RPC，返回 (响应对象, 通知类型计数)。

    响应流是 SSE/NDJSON（一行一个 JSON），本条 rid 的 Response 出现即视为结束。
    abort_after>0 时收到这么多条通知就停读返回（不等 Response）——
    实测 prompt 被接受后 `session_info_update` 约 0.1s 就到，而**提前断开连接
    不会掉断正在生成的回答**（模型照常跑完并落库），所以投递不必干等整轮跑完。
    """
    body = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params}
    data = json.dumps(body).encode("utf-8")
    headers = {"Authorization": f"Bearer {secret}", "x-codebuddy-request": "1",
               "Content-Type": "application/json", "acp-connection-id": cid,
               "Accept": "application/json, text/event-stream"}
    req = urllib.request.Request(f"http://127.0.0.1:{port}/api/v1/acp", data=data,
                                 headers=headers, method="POST")
    resp_obj, notes = None, {}
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            for raw in r:
                s = raw.decode("utf-8", "replace").strip()
                if not s:
                    continue
                if s.startswith("data:"):
                    s = s[5:].strip()
                if not s.startswith("{"):
                    continue
                try:
                    obj = json.loads(s)
                except ValueError:
                    continue
                if obj.get("id") == rid and ("result" in obj or "error" in obj):
                    resp_obj = obj
                    break
                k = ((obj.get("params") or {}).get("update") or {}).get("sessionUpdate")
                if k:
                    notes[k] = notes.get(k, 0) + 1
                    if abort_after and sum(notes.values()) >= abort_after:
                        break
    except urllib.error.HTTPError as e:
        return ({"error": {"code": e.code,
                          "message": e.read().decode("utf-8", "replace")[:200]}}, notes)
    except Exception as e:
        return {"error": {"code": None, "message": str(e)}}, notes
    return resp_obj, notes


def _wb_sessions_db() -> str:
    return os.path.join(os.path.expanduser("~"), ".workbuddy", "workbuddy.db")


def _wb_session_row(session_id: str = "") -> dict | None:
    """从 workbuddy.db 取一个会话行（指定 id / 最近活动的那条）。

    只 SELECT 不写；读不到（DB 不存在 / 被锁）就返回 None，交给上层回退。
    """
    p = _wb_sessions_db()
    if not os.path.isfile(p):
        return None
    try:
        con = sqlite3.connect(p, timeout=2.0)
        con.row_factory = sqlite3.Row
        try:
            if session_id:
                row = con.execute(
                    "SELECT id, cwd, title FROM sessions "
                    "WHERE id = ? AND deleted_at IS NULL", (session_id,)).fetchone()
            else:
                row = con.execute(
                    "SELECT id, cwd, title FROM sessions "
                    "WHERE deleted_at IS NULL "
                    "ORDER BY COALESCE(last_activity_at, updated_at, created_at) DESC "
                    "LIMIT 1").fetchone()
        finally:
            con.close()
        return dict(row) if row else None
    except Exception:
        return None


def _wb_session_target() -> dict | None:
    """投递目标：优先活会话；一个活的都没有就取 DB 里最近活动的那个会话。

    返回 {sessionId, cwd, title, port(可空), src}；一个都找不到返回 None。
    """
    live = wb_live_sessions(force=True)
    if live:
        s = live[0]
        row = _wb_session_row(s["sessionId"]) or {}
        return {"sessionId": s["sessionId"], "cwd": str(row.get("cwd") or ""),
                "title": str(row.get("title") or ""), "port": s["port"],
                "src": "live"}
    row = _wb_session_row()
    if not row or not row.get("id"):
        return None
    return {"sessionId": str(row["id"]), "cwd": str(row.get("cwd") or ""),
            "title": str(row.get("title") or ""), "port": None, "src": "db"}


def _wb_port_for_cwd(cwd: str) -> int | None:
    """找一个 cwd 与该会话一致的 worker 端口（resume 要带 cwd，得投给对的人）。"""
    if not cwd:
        return None
    secret = wb_gateway_secret()
    if not secret:
        return None
    want = os.path.normcase(os.path.normpath(cwd.replace("/", os.sep)))
    for _pid, port in _workbuddy_pid_ports():
        code, text = wb_rest(port, "/api/v1/info", secret, timeout=4.0)
        if code != 200:
            continue
        try:
            got = str((json.loads(text).get("data") or {}).get("cwd") or "")
        except Exception:
            continue
        if got and os.path.normcase(os.path.normpath(got.replace("/", os.sep))) == want:
            return port
    return None


def wb_acp_deliver(text: str, session: dict | None = None) -> dict:
    """走 ACP `session/prompt` 把一句话投进会话（不要求该会话是活会话）。

    与 REST reply 的差别：REST 是静默注入（要求活会话、不广播）；ACP 是桌面
    界面自己在用的通道，落库记录的 providerData 带 startsNewUserRequest=true，
    与界面手打的消息同构。

    注意：WorkBuddy 5.8.2 起，界面只在「自己刷新会话时」读会话数据（worker 以
    acp=false 运行，界面侧不持有 ACP 连接），所以消息落库后要切一下会话才可见。
    """
    text = (text or "").strip()
    if not text:
        return {"ok": False, "kind": "reply", "err": "内容为空"}
    secret = wb_gateway_secret()
    if not secret:
        return {"ok": False, "kind": "reply",
                "err": "未取到 WorkBuddy 网关密钥（WorkBuddy 桌面端未在运行？）"}
    tgt = session or _wb_session_target()
    if not tgt or not tgt.get("sessionId"):
        return {"ok": False, "kind": "reply",
                "err": "找不到可投递的会话（WorkBuddy 里还没有任何对话）"}
    sid, cwd = tgt["sessionId"], tgt.get("cwd") or ""
    port = tgt.get("port") or _wb_port_for_cwd(cwd)
    if not port:
        ports = _list_workbuddy_ports()
        port = ports[0] if ports else None
    if not port:
        return {"ok": False, "kind": "reply", "err": "没有可用的 WorkBuddy worker 端口"}

    code, body = _wb_http(port, "/api/v1/acp/connect", secret, method="POST",
                          timeout=8.0)
    if code != 200:
        return {"ok": False, "kind": "reply", "port": port,
                "err": f"ACP connect 失败：HTTP {code} {body[:120]}"}
    try:
        cid = json.loads(body).get("connectionId") or ""
    except Exception:
        cid = ""
    if not cid:
        return {"ok": False, "kind": "reply", "port": port,
                "err": "ACP connect 没返回 connectionId"}
    try:
        _wb_acp_rpc(port, secret, cid, 1, "initialize",
                    {"protocolVersion": 1, "capabilities": {},
                     "clientInfo": {"name": WB_ACP_CLIENT_NAME, "version": "1.0"}},
                    timeout=20.0)
        if cwd:
            _wb_acp_rpc(port, secret, cid, 2, "session/resume",
                        {"sessionId": sid, "cwd": cwd}, timeout=40.0)
        resp, notes = _wb_acp_rpc(port, secret, cid, 3, "session/prompt",
                                  {"sessionId": sid,
                                   "prompt": [{"type": "text", "text": text}]},
                                  timeout=ACP_DELIVER_TIMEOUT, abort_after=1)
    finally:
        _wb_http(port, "/api/v1/acp", secret, method="DELETE",
                 headers={"acp-connection-id": cid}, timeout=5.0)

    err = (resp or {}).get("error")
    if err:
        return {"ok": False, "kind": "reply", "port": port, "sessionId": sid,
                "err": f"ACP prompt 被拒：{err.get('code')} {str(err.get('message'))[:120]}"}
    if not resp and not notes:
        # 既没收到响应、也没收到任何轮次通知，说明服务端根本没认领这次 prompt
        return {"ok": False, "kind": "reply", "port": port, "sessionId": sid,
                "err": "ACP prompt 没有响应（超时）"}
    # notes 非空 = 服务端已开始这一轮（接受了 prompt）；提前断开不影响它跑完
    return {"ok": True, "kind": "reply", "port": port, "sessionId": sid,
            "via": "acp", "accepted": bool(notes), "events": notes,
            "detail": "已投递到会话（在 WorkBuddy 里切一下会话即可看到）"}


def wb_cli_path() -> str:
    """WorkBuddy CLI 入口：返回**能直接交给 node 跑**的那个 js/mjs 文件。

    别死认 `bin/codebuddy`：它只是个薄包装，内部 require('../dist/codebuddy')，
    而 WorkBuddy 2026-10-14 更新后 dist 里只剩 `codebuddy-lite-wb.mjs`
    （改版换名），照旧路径跑只会得到 "Cannot find module '../dist/codebuddy'"。
    这里按打包形态从"完整包"到"精简包"再到"老包装"依次探测，谁在就用谁。
    """
    la = os.environ.get("LOCALAPPDATA") or ""
    if not la:
        return ""
    cli = os.path.join(la, "Programs", "WorkBuddy", "resources",
                       "app.asar.unpacked", "cli")
    for rel in (("dist", "codebuddy.js"),
                ("dist", "codebuddy-headless.js"),
                ("dist", "codebuddy-lite-wb.mjs"),
                ("bin", "codebuddy")):
        p = os.path.join(cli, *rel)
        if os.path.isfile(p):
            return p
    return ""


def wb_cli_new_job(text: str, cwd: str | None = None) -> dict:
    """CLI 回退：没有活的 serve 实例时，直接起一个 headless 会话（每次独立会话）。"""
    cli = wb_cli_path()
    if not cli:
        return {"ok": False, "kind": "job",
                "err": "没有可用的 worker 端口，也没找到 WorkBuddy CLI"}
    node = shutil.which("node") or "node"
    try:
        # encoding 必须显式给 utf-8：系统默认是 GBK，会把子进程输出里的中文解成乱码
        # （报错信息里的用户名 忘尘 → 蹇樺皹，之前排查时被这个误导过）。
        r = subprocess.run([node, cli, "-p", text], cwd=cwd or None,
                           capture_output=True, text=True, timeout=180,
                           encoding="utf-8", errors="replace",
                           creationflags=SUB_NO_WINDOW)
    except Exception as e:
        return {"ok": False, "kind": "job", "err": f"CLI 调用失败：{e}"}
    if r.returncode != 0:
        tail = (r.stderr or r.stdout or "").strip()
        return {"ok": False, "kind": "job",
                "err": f"CLI 退出码 {r.returncode}：{tail[:200]}"}
    return {"ok": True, "kind": "job", "detail": "已通过 CLI 新建任务（无网关 worker）",
            "text": (r.stdout or "").strip()[:200]}


def wb_new_job(text: str, cwd: str | None = None, name: str | None = None) -> dict:
    """派发一个新任务：优先 REST POST /api/v1/jobs，无可用 worker 时回退 CLI。"""
    cfg = wb_job_cfg_get()
    cwd = (cwd or cfg.get("cwd") or "").strip()
    name = (name or "").strip() or text.strip()[:24] or "设备语音任务"
    secret = wb_gateway_secret()
    if secret:
        cands = [s["port"] for s in wb_live_sessions()]
        for p in _list_workbuddy_ports():
            if p not in cands:
                cands.append(p)
        job: dict = {"prompt": text, "name": name}
        if cwd:
            job["cwd"] = cwd
        for k in ("agent", "model"):
            if cfg.get(k):
                job[k] = cfg[k]
        err = "没有可用的 WorkBuddy worker 端口"
        for port in cands:
            code, body = wb_rest(port, "/api/v1/jobs", secret, method="POST",
                                 body=job, timeout=30.0)
            if code == 200:
                try:
                    data = json.loads(body).get("data") or {}
                except Exception:
                    data = {}
                return {"ok": True, "kind": "job", "port": port,
                        "jobId": data.get("id"), "sessionId": data.get("sessionId"),
                        "detail": "已派发新任务"}
            if code == 404:
                continue            # 该端口不是 REST worker，换下一个
            if code in (401, 403):
                with _wb_lock:      # 密钥可能因桌面重启换了，清缓存下次重取
                    _wb_cache["secret"] = ""
                    _wb_cache["secret_at"] = 0.0
                return {"ok": False, "kind": "job",
                        "err": f"网关鉴权失败（HTTP {code}），请稍后重试"}
            err = f"HTTP {code}: {body[:160]}"
        if cands:
            return {"ok": False, "kind": "job", "err": err}
    return _wb_cli_fallback(text, cwd or None)

def _wb_fallback_hint(raw: str) -> str:
    """把回退失败翻译成"缺什么 + 下一步做什么"。这句话会原样进设备回执。"""
    if not wb_gateway_secret(force=True):
        return ("WorkBuddy 本地网关没在运行（取不到网关密钥）："
                "请在 WorkBuddy 里打开或跑一个会话后重试")
    if not wb_cli_path():
        return "没有可用的投递通道：网关不在线，也没找到 WorkBuddy CLI"
    if "Authentication required" in raw or "/login" in raw:
        return "CLI 回退不可用（需先在终端 /login 一次），且没有可用的网关端口"
    if "Cannot find module" in raw:
        return "WorkBuddy CLI 入口已变更（升级后换名），CLI 回退不可用"
    return "投递失败：" + (raw[:80] or "未知原因")

def _wb_cli_fallback(text: str, cwd: str | None) -> dict:
    """没有网关 worker 时的兜底：起一个 headless CLI 会话。

    失败一律换成可执行的提示（见 _wb_fallback_hint），原始错误塞进 detail ——
    设备屏幕只有一行字，node 的堆栈放上去等于没说。
    """
    res = wb_cli_new_job(text, cwd)
    if res.get("ok"):
        return res
    raw = str(res.get("err") or "")
    res["detail"] = raw
    res["err"] = _wb_fallback_hint(raw)
    return res


def wb_deliver(text: str, target: str = "reply") -> dict:
    """统一投递入口。target: reply=回复当前会话 / job=新建任务。

    reply 走 ACP 优先（不要求会话是活的），失败再回退 REST reply ——
    桌面端没打开会话时只有 ACP 这条路能进。
    """
    text = (text or "").strip()
    if not text:
        return {"ok": False, "kind": target, "err": "内容为空"}
    if target == "job":
        return wb_new_job(text)
    res = wb_acp_deliver(text)
    if res.get("ok"):
        return res
    res2 = wb_reply_live(text)
    if res2.get("ok"):
        res2["via"] = "rest"
        res2["detail"] = str(res2.get("detail") or "") + "（ACP 未成功，已回退 REST）"
        res2["acp_err"] = res.get("err")
        return res2
    res["rest_err"] = res2.get("err")
    return res


def wb_status_report() -> None:
    """把投递层现状一次性打出来：候选端口 → 是否 ACP worker → 是否 REST → 密钥/活会话。

    「投递失败」九成是因为没有本地网关 worker（没有它就拿不到密钥，REST 走不了，
    会一路回退到 CLI）。这个报告就是用来一眼分清"是 worker 没起"还是别的问题。
    """
    print("=" * 68)
    print("  WorkBuddy 投递层自检")
    print("=" * 68)
    pairs = _workbuddy_pid_ports()
    print("[1] 候选监听端口（WorkBuddy / node 相关进程）: %d 个" % len(pairs))
    for pid, port in pairs:
        print("     :%-6d pid=%-6d %s" % (port, pid, _process_image_name(pid) or "?"))
    if not pairs:
        print("     ** 一个都没有：WorkBuddy 桌面可能没在运行 **")
    workers = []
    for pid, port in pairs:
        cid = probe_worker(port)
        mark = "ACP worker" if cid else "-"
        code_live, _b = wb_rest(port, "/api/v1/sessions/live", "", timeout=4.0)
        code_jobs, _b2 = wb_rest(port, "/api/v1/jobs", "", method="POST",
                                 body={}, timeout=4.0)
        is_rest = code_live in (200, 401, 403)
        if cid:
            workers.append(port)
        print("     :%-6d %-10s REST(live)=%-5s REST(jobs)=%-5s %s"
              % (port, mark, code_live, code_jobs,
                 "← REST 网关" if is_rest else ""))
    print("[2] 网关密钥：%s" % ("已拿到（长度 %d）" % len(wb_gateway_secret(force=True))
                                if wb_gateway_secret(force=True) else "**没拿到**"))
    if not wb_gateway_secret(force=True):
        print("     密钥来自 worker 进程的环境块（CODEBUDDY_GATEWAY_AUTH=password + "
              "CODEBUDDY_GATEWAY_PASSWORD）；没有 --serve worker 就没有它。")
    print("[3] 活会话：%s" % (json.dumps(wb_live_sessions(force=True), ensure_ascii=False)
                              if wb_gateway_secret() else "（无密钥，跳过）"))
    print("[4] CLI 回退入口：%s" % (wb_cli_path() or "**没找到**"))
    print("[5] 新任务默认参数：%s" % json.dumps(wb_job_cfg_get(), ensure_ascii=False))
    print("=" * 68)

def wb_gateway_state() -> dict:
    """投递层现状：候选端口 / 有没有网关密钥 / 网关算不算在线。

    网关密钥只在有 `--serve` worker 时才读得到（它由桌面端随机生成后经环境变量
    注入 worker 进程），所以"拿得到密钥"就等于"本地网关在线"。没有它，REST
    投递（回复活会话 / 新建任务）全都走不了，只能退到 CLI。
    """
    ports = _list_workbuddy_ports()
    secret = bool(wb_gateway_secret())
    return {"ports": ports, "count": len(ports), "secret": secret,
            "online": secret, "cli": bool(wb_cli_path())}

def wb_live_overview() -> dict:
    """前端用的一次性概览：网关状态 + 密钥/CLI 可用性 + 活会话 + 新任务默认参数。"""
    gateway = wb_gateway_state()
    live = wb_live_sessions(force=True) if gateway["secret"] else []
    return {"ok": True, "secret": gateway["secret"], "gateway": gateway,
            "cli": gateway["cli"], "sessions": live, "job": wb_job_cfg_get()}


# ===================== 本地 ACP 客户端 =====================

def probe_worker(port: int):
    """探测端口是否为 ACP worker：POST connect。

    返回 connectionId（并立即 DELETE 释放）；非 worker（404 等）返回 None。
    """
    base = f"http://127.0.0.1:{port}"
    cid = None
    try:
        req = urllib.request.Request(
            base + ACP_CONNECT_PATH, data=b"",
            headers=ACP_SEC_HDR, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            d = json.loads(r.read().decode("utf-8"))
        cid = str(d.get("connectionId") or "") or None
    except Exception:
        return None
    if cid:
        try:
            req = urllib.request.Request(
                base + ACP_PATH_STR,
                headers={**ACP_SEC_HDR, "acp-connection-id": cid},
                method="DELETE")
            urllib.request.urlopen(req, timeout=5).read()
        except Exception:
            pass
    return cid


class AcpConnection:
    """单 worker + 单会话的 ACP 连接（线程化，按需创建）。

    流程：connect → initialize（POST）→ session/load（POST，一次性回放该会话
    已有事件后 EOF）→ GET standalone SSE 长开订阅（30s 心跳）。服务端把
    notification 与服务端扩展请求（如 _codebuddy.ai/question）都推到该订阅上。

    设备作答走另一条官方通道：任何连接都能用 post_request 发
    _codebuddy.ai/resolveInterruption（服务端 handleResolveInterruption 只按
    sessionId 查会话，不校验连接归属），因此不必成为会话 owner、也不与桌面端
    抢连接。详见设计文档「十二、作答通道修正」。
    """

    def __init__(self, port: int, session_id: str, cwd: str,
                 on_event, loop) -> None:
        self._base = f"http://127.0.0.1:{port}"
        self._port = port
        self._sid = session_id
        self._cwd = cwd
        self._on_event = on_event
        self._loop = loop
        self._cid = ""
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._ok = False
        self._thread: threading.Thread | None = None
        self._rpc_seq = 0
        self._req_seq = 0          # post_request 的请求 id 自增（形如 ans-1）

    @property
    def port(self) -> int:
        return self._port

    @property
    def session_id(self) -> str:
        return self._sid

    @property
    def alive(self) -> bool:
        """长流是否仍在（daemon 据此决定复用还是重建）。"""
        return (self._ok and not self._stop.is_set()
                and self._thread is not None and self._thread.is_alive())

    def start(self) -> bool:
        self._thread = threading.Thread(
            target=self._run,
            name=f"acp-{self._port}-{self._sid[:8] or 'sentry'}", daemon=True)
        self._thread.start()
        self._ready.wait(timeout=25.0)
        return self._ok

    def _release(self) -> None:
        """尽力 DELETE 释放服务端连接（幂等，失败忽略）。"""
        if not self._cid:
            return
        try:
            req = urllib.request.Request(
                self._base + ACP_PATH_STR,
                headers={**ACP_SEC_HDR, "acp-connection-id": self._cid},
                method="DELETE")
            urllib.request.urlopen(req, timeout=5).read()
        except Exception:
            pass

    def stop(self) -> None:
        self._stop.set()
        self._ready.set()   # 避免 start() 空等到超时
        self._release()

    def post_result(self, rpc_id, result: dict) -> None:
        """向 worker 回 JSON-RPC result（独立短连接 + 线程）。"""
        body = {"jsonrpc": "2.0", "id": rpc_id, "result": result}
        threading.Thread(target=self._post_result_blocking,
                         args=(body,), daemon=True).start()

    def post_request(self, method: str, params: dict, tag: str = "req") -> None:
        """向 worker 发一条客户端→服务端的 JSON-RPC 请求（短连接 + 线程）。

        设备作答走这里：method="_codebuddy.ai/resolveInterruption"，params=
        {sessionId, toolCallId, decision, answers}。请求 id 用 "<tag>-N" 字符串，
        daemon 在 _acp_post 事件里按前缀识别这条响应用途。
        服务端只按 sessionId 查会话，不要求我们把该会话挂载在自己名下。
        """
        self._req_seq += 1
        body = {"jsonrpc": "2.0", "id": f"{tag}-{self._req_seq}",
                "method": method, "params": params}
        threading.Thread(target=self._post_result_blocking,
                         args=(body,), daemon=True).start()

    # ---- 内部实现 ----

    def _emit(self, event: dict) -> None:
        # 标记来源端口/会话，daemon 据此找到投递答案的连接
        try:
            event["_src_port"] = self._port
            event["_src_sid"] = self._sid
        except (TypeError, AttributeError):
            pass
        try:
            self._loop.call_soon_threadsafe(self._safe_emit, event)
        except RuntimeError:
            pass

    def _safe_emit(self, event: dict) -> None:
        try:
            r = self._on_event(event)
            if asyncio.iscoroutine(r):
                asyncio.ensure_future(r)
        except Exception:
            pass

    def _post_result_blocking(self, body: dict) -> None:
        headers = {**ACP_SEC_HDR, "Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream",
                   "acp-connection-id": self._cid}
        req = urllib.request.Request(
            self._base + ACP_PATH_STR,
            data=json.dumps(body).encode("utf-8"), headers=headers, method="POST")
        resp = b""
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                resp = r.read()
        except urllib.error.HTTPError as exc:
            # 4xx/5xx 的正文里才是 JSON-RPC 错误（如 writer_occupied），读出来
            try:
                resp = exc.read() or b""
            except Exception:
                resp = b""
            if not resp:
                self._emit({"_acp_error": f"HTTP {exc.code} {exc.reason}"})
                return
        except Exception as exc:
            self._emit({"_acp_error": str(exc)})
            return
        self._emit({"_acp_post": {"id": body.get("id"),
                                  "resp": resp.decode("utf-8", errors="replace")}})

    def _do_connect(self) -> bool:
        req = urllib.request.Request(
            self._base + ACP_CONNECT_PATH, data=b"",
            headers=ACP_SEC_HDR, method="POST")
        with urllib.request.urlopen(req, timeout=8) as r:
            d = json.loads(r.read().decode("utf-8"))
        self._cid = str(d.get("connectionId") or "")
        return bool(self._cid)

    def _short_post(self, message: dict, timeout: float = 15.0):
        """短连接 POST 一条 JSON-RPC，取同 id 响应后返回。"""
        data = json.dumps(message).encode("utf-8")
        headers = {**ACP_SEC_HDR, "Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream",
                   "acp-connection-id": self._cid}
        req = urllib.request.Request(
            self._base + ACP_PATH_STR, data=data, headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            for raw in r:
                obj = self._parse_line(raw)
                if obj and obj.get("id") == message["id"]:
                    return obj
        return None

    @staticmethod
    def _parse_line(raw: bytes):
        s = raw.decode("utf-8", errors="replace").strip()
        if not s:
            return None
        if s.startswith("data:"):
            s = s[5:].strip()
        if not s.startswith("{"):
            return None
        try:
            return json.loads(s)
        except ValueError:
            return None

    def _mount(self) -> None:
        """connect → initialize → session/load（含该会话的一次性回放）。

        走到这里的三步都是 POST（请求-响应），服务端按 acp-connection-id 关联
        连接状态（实测：同 id 的新连接可直接发请求，不会报未初始化）。
        真正的推送通道是 GET standalone SSE，见 _subscribe（load 的 POST 响应
        实测约 74KB 回放后即 EOF，不能当长连接用）。
        """
        if not self._do_connect():
            raise RuntimeError("connect failed")
        self._rpc_seq += 1
        resp = self._short_post(
            {"jsonrpc": "2.0", "id": self._rpc_seq, "method": "initialize",
             "params": {"protocolVersion": 1, "capabilities": {},
                        "clientInfo": {"name": "wb-console", "version": "1.0"}}})
        if not resp or "result" not in resp:
            raise RuntimeError("initialize failed")

        if not self._sid:
            return   # 端口哨兵：无会话，initialize 后直接去订阅（保持连接存活）

        # session/load：响应流里会回放该会话已有事件，读完即结束。
        self._rpc_seq += 1
        load_req = urllib.request.Request(
            self._base + ACP_PATH_STR,
            data=json.dumps(
                {"jsonrpc": "2.0", "id": self._rpc_seq, "method": "session/load",
                 "params": {"sessionId": self._sid, "cwd": self._cwd,
                            "mcpServers": []}}).encode("utf-8"),
            headers={**ACP_SEC_HDR, "Content-Type": "application/json",
                     "Accept": "application/json, text/event-stream",
                     "acp-connection-id": self._cid}, method="POST")
        with urllib.request.urlopen(load_req, timeout=ACP_LOAD_TIMEOUT) as r:
            for raw in r:
                if self._stop.is_set():
                    break
                obj = self._parse_line(raw)
                if obj:
                    self._emit(obj)

    def _subscribe(self):
        """GET standalone SSE：服务端→客户端的推送通道（30s 一次心跳）。

        必须带 Accept: text/event-stream，否则服务端 406；同一 connectionId
        只允许一条订阅（重复 GET 会 409）。
        """
        req = urllib.request.Request(
            self._base + ACP_PATH_STR,
            headers={**ACP_SEC_HDR, "Accept": "text/event-stream",
                     "acp-connection-id": self._cid}, method="GET")
        return urllib.request.urlopen(req, timeout=ACP_STREAM_IDLE)

    def _run(self) -> None:
        """挂载（connect→initialize→load→GET 订阅）；断线后整体重挂载。

        首次挂载失败直接退出（daemon 下一轮会再试）；已挂载过则自行退避重挂载，
        否则服务端重启/连接失效后该会话永久收不到提问。
        """
        delay = ACP_REMOUNT_BASE
        attached = False
        while not self._stop.is_set():
            try:
                self._mount()
                r = self._subscribe()
            except Exception as exc:
                self._ok = False
                self._ready.set()
                if not attached:
                    self._emit({"_acp_error": f"startup: {exc}"})
                    return
                self._emit({"_acp_error":
                            f"remount 失败: {exc}（{delay:.0f}s 后重试）"})
                if self._stop.wait(delay):
                    break
                delay = min(delay * 2, ACP_REMOUNT_MAX)
                continue
            self._ok = True
            self._ready.set()
            if attached:
                self._emit({"_acp_info": f"已重连 :{self._port} {self._sid[:8]}"})
            attached = True
            delay = ACP_REMOUNT_BASE
            try:
                with r:
                    for raw in r:
                        if self._stop.is_set():
                            break
                        obj = self._parse_line(raw)
                        if obj:
                            self._emit(obj)
                reason = "服务端关闭订阅流"
            except Exception as exc:
                reason = f"{type(exc).__name__}: {exc}"
            self._ok = False
            if self._stop.is_set():
                break
            self._emit({"_acp_error":
                        f"sub: {reason}（{delay:.0f}s 后重挂载）"})
            self._release()   # 旧 cid 已失效，释放后整体重建
            if self._stop.wait(delay):
                break
        self._ready.set()

# ===================== wb_balance 内联 =====================

# 凭据读取已迁到 wb_credential.py（官方 OAuth 明文凭据 + 自动刷新）：
# 原先在此直读 workbuddy-desktop.info 的 find_auth_path / load_auth 已删除——
# WorkBuddy 5.6 起该文件的 accessToken 变成 $wbEncrypted 加密信封，读出来
# 也只是个 dict，无法再当 Bearer 用。

def _system_proxy() -> str | None:
    if winreg is None:
        return None
    try:
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Internet Settings",
        )
        enable, _ = winreg.QueryValueEx(key, "ProxyEnable")
        server, _ = winreg.QueryValueEx(key, "ProxyServer")
        winreg.CloseKey(key)
    except OSError:
        return None
    if enable and server:
        return server
    return None

def _origin_of(url: str) -> str:
    """取 URL 的 scheme://host，用于拼与请求 host 一致的 Origin/Referer。"""
    try:
        scheme, rest = url.split("://", 1)
        return f"{scheme}://{rest.split('/', 1)[0]}"
    except ValueError:
        return "https://copilot.tencent.com"

def _balance_headers(auth: dict) -> dict:
    """billing 接口的公共请求头。

    官方桌面端调 billing 会带浏览器形态的 Origin/Referer 与客户端标识，
    缺失时网关的一致性校验会拒（返回 HTML 错误页而非 JSON）。
    """
    origin = _origin_of(BAL_API_URL)
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/plain, */*",
        "User-Agent": "Go-http-client/1.1",
        "Authorization": f"Bearer {auth['access_token']}",
        "X-User-Id": str(auth["uid"]),
        "X-Client-Platform": "web",
        "Origin": origin,
        "Referer": f"{origin}/profile/plans-usage",
    }
    if auth.get("domain"):
        headers["X-Domain"] = auth["domain"]
    if auth.get("enterprise_id"):
        headers["X-Enterprise-Id"] = str(auth["enterprise_id"])
        headers["X-Tenant-Id"] = str(auth["enterprise_id"])
    return headers


def _balance_post(url: str, body: dict, headers: dict, timeout: float) -> dict:
    req = urllib.request.Request(
        url, data=json.dumps(body).encode("utf-8"), headers=headers, method="POST")
    proxy = _system_proxy()
    if proxy:
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": proxy, "https": proxy}))
    else:
        opener = urllib.request.build_opener()
    try:
        with opener.open(req, timeout=timeout) as resp:
            if resp.status != 200:
                raise RuntimeError(f"WorkBuddy 余额接口返回 {resp.status}")
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"WorkBuddy 余额接口返回 {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"WorkBuddy 接口请求失败: {exc.reason}") from exc


def _fetch_balance_summary(auth: dict, timeout: float) -> tuple[float, float]:
    """按官方客户端口径取余额（汇总接口）。

    客户端 plans-usage 页用的是 `/billing/meter/get-user-resource-summary`
    —— 注意**没有 /v2 前缀**，写成 /v2/... 会 404（实测）。

    它直接返回 `TotalCredits` = 积分余额（和客户端侧栏那个数完全一致），
    并按用途拆成 4 组：套餐基础积分 / 套餐赠送积分 / 购买积分 / 平台奖励积分。

    为什么不能用资源明细逐条加：明细接口（get-user-resource）按 `Status:[0,3]`
    只给"资源记录"，同一个包的多条记录里既有已用尽的、也有还在有效期的，
    累加出来是 3517，而真实余额是 6323 —— 差的那部分正是"平台奖励积分"
    （76 个奖励包合并后的 3371）。客户端自己也只在三源都成功时才说完整
    （代码里有 "credits are incomplete" 的告警）。
    """
    payload = _balance_post(BAL_SUMMARY_URL, {}, _balance_headers(auth), timeout)
    data = payload.get("data") or {}
    packages = data.get("Packages") or []
    if not packages:
        raise RuntimeError("余额汇总接口没有返回 Packages")

    remain = data.get("TotalCredits")
    if remain is None:
        # 老版本可能没有 TotalCredits，按客户端 sumSummaryCapacity 的算法自己加。
        remain = sum(float(p.get("CycleRemainCapacity") or 0.0) for p in packages)
    remain = float(remain or 0.0)
    total = sum(float(p.get("CycleTotalCapacity") or 0.0) for p in packages)
    if total < remain:
        total = remain
    return remain, total


def _fetch_balance_accounts(auth: dict, timeout: float) -> tuple[float, float]:
    """资源明细口径（兜底用）：逐条累加 Status==0 的包。

    只在汇总接口不可用时才走这里。注意这个口径会算少 —— 见
    _fetch_balance_summary 的说明。
    """
    body = {
        "PageNumber": 1,
        "PageSize": 100,
        "ProductCode": BAL_PRODUCT_CODE,
        "Status": [0, 3],
        "PackageStartTimeRangeBegin": BAL_RANGE_BEGIN,
        "PackageStartTimeRangeEnd": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    }
    payload = _balance_post(BAL_API_URL, body, _balance_headers(auth), timeout)
    try:
        accounts = payload["data"]["Response"]["Data"]["Accounts"]
    except (KeyError, TypeError):
        raise RuntimeError("WorkBuddy 余额格式无法识别（平台可能改版）")

    remaining = 0.0
    total = 0.0
    all_remaining = 0.0
    all_total = 0.0
    active = 0
    for account in accounts or []:
        remain = float(account.get("CycleCapacityRemainPrecise") or 0.0)
        size = float(account.get("CycleCapacitySizePrecise") or 0.0)
        all_remaining += remain
        all_total += size
        # 只累加 Status==0（当前有效）的包：失效包的 CycleCapacitySizePrecise
        # 仍保留原值，全加会把分母撑大（实测 16054 vs 真实 4541）。
        if int(account.get("Status") or 0) != 0:
            continue
        active += 1
        remaining += remain
        total += size
    if active == 0:
        remaining, total = all_remaining, all_total
    if total < remaining:
        total = remaining
    return remaining, total


def fetch_balance(auth: dict, timeout: float = 15.0) -> tuple[float, float]:
    """取 (剩余积分, 总额度)。优先走官方汇总口径，失败才退回资源明细。"""
    token = auth["access_token"]
    uid = auth["uid"]
    if isinstance(token, dict) and token.get("$wbEncrypted"):
        raise RuntimeError(
            "WorkBuddy 登录态已加密（$wbEncrypted 信封），密钥运行时下发、磁盘不可得；"
            "请执行 python wb_credential.py login 让 AIWK 持有自己的明文凭据")
    if not token or not uid:
        raise RuntimeError("WorkBuddy 登录信息不可用（accessToken/uid 缺失）")

    try:
        return _fetch_balance_summary(auth, timeout)
    except Exception as exc:
        summary_err = exc
    try:
        return _fetch_balance_accounts(auth, timeout)
    except Exception as exc:
        raise RuntimeError(
            f"余额查询失败（汇总: {summary_err}；明细: {exc}）") from exc

def _load_credit_auth() -> tuple[dict | None, str]:
    """解析周期积分所需凭据，返回 (auth, reason)。

    凭据优先取 AIWK 自有副本（wb_credential.py，走官方 OAuth 拿明文 token）。
    WorkBuddy 桌面端 5.6 起把 workbuddy-desktop.info 的 accessToken 改成
    ``$wbEncrypted`` 加密信封（AES-256-GCM，密钥运行时由原生模块下发、磁盘
    不可得），因此该文件不再作为凭据来源，只用于给出可读的失败原因。
    """
    try:
        import wb_credential
    except Exception as exc:  # 模块缺失 / 语法错误都不该拖垮 daemon
        return None, f"wb_credential 模块不可用: {exc}"
    try:
        auth, reason = wb_credential.resolve()
    except Exception as exc:
        return None, f"凭据解析失败: {exc}"
    return auth, (reason or "")

# ===================== daemon 核心 =====================


# ===================== 设备语音的文字链路 =====================
# 设备录到的音频 → 虚拟声卡（VB-CABLE）播放端 → 微信输入法听写 → 本进程取字
# → 按设备上选的目标投递（reply / job / manual）。
#
# 三个基础件全部只用标准库 + ctypes：
#   _CableWriter  WinMM(MME) 流式播放，把 16k 单声道 PCM 写进 CABLE Input
#   _press_hotkey SendInput 敲一下输入法的语音快捷键（默认 Ctrl+Alt+V）
#   _RelayWindow  原生 EDIT 窗口，抢焦点接输入法打出来的字，供桥读走
#
# 参考实现（zhongdan110/ai-passport-workbuddy）用 sounddevice 播音频，但
# PortAudio 初始化会把线程的 COM 单元模型钉成 STA，而 bleak 的 WinRT 后端要求
# MTA —— 两者同进程会互相破坏（参考实现得在 import sounddevice 之前
# CoInitializeEx(MTA)）。我们和 bleak 同进程，就不引这个隐患了。

# 虚拟声卡播放端名里包含的字符串。
# ⚠ 必须短：WinMM 的设备名上限是 32 个字符，完整名
# "CABLE Input (VB-Audio Virtual Cable)" 会被截成 "CABLE Input (VB-Audio Virtual C"，
# 拿完整长串去搜永远搜不到（第一次就是这么踩的）。
CABLE_HINT = "CABLE Input"              # 标准版播放端（内部环回到 CABLE Output）
CABLE_HINT_ALT = "VB-Audio"             # 兜底：任何 VB-Audio 播放端
CABLE_HINT_SKIP = "16ch"                # 多声道变体，写进去 CABLE Output 收不到
CABLE_RATE = 16000                      # 设备采样率；MME 接受 16k，由系统重采样
CABLE_CHANNELS = 2                      # 统一立体声：单声道直接塞进去会"音调全错"
CABLE_BUF_SECONDS = 2.0                 # 缓冲上限；超了丢最旧（说话时新鲜度优先）
CABLE_PERIOD_MS = 100                   # 每个 WinMM 缓冲块的时长
CABLE_QUEUE_BLOCKS = 4                  # 在途块数（4×100ms ≈ 400ms 余量）
# 文字链路总开关：关掉后行为退回"只存 WAV + 回执已存录音"。
# 自检/单元测试必须置 False —— 传动流程会抢焦点、弹中转窗口、注入组合键，
# 在测试里做这些动作会打扰用户桌面。
VOICE_ASR_ENABLED = True
# 输入法语音输入的唤起快捷键（可用控制台配置 voice_hotkey 覆盖）。
#
#   微信输入法（WeType 2.1.4.6，2026-10-14 装的那版）：ctrl+win（当前默认）
#       「语音输入」页里有两个快捷键：
#         启动语音输入（免按）= Ctrl + Win + Shift —— 按一次开始、再按一次或 Enter 结束
#         按住说话          = Ctrl + Win       —— 整段按住，松手结束
#       两个都能用程序唤起（都验过），但**选"按住说话"那个**：它只有一次按下/松开，
#       不用关心"这一轮到底是谁开的听写"，也不会因为第二次注入失败而一直开着麦。
#       麦克风要在同一个页面里显式选成 CABLE Output。
#       **还必须确认当前生效输入法就是微信输入法**（见 ime_ensure）——
#       否则它会把识别结果丢进剪贴板，永远不进输入框。
#   豆包输入法：**不能用**（2026-10-14 实测结论，别再试了）
#       - 物理按「右Alt+空格」（免按模式）或按住「右Alt」（长按模式）都正常，
#         豆包会听写并把字打进焦点控件；
#       - 但**任何程序注入的按键都唤不起它的语音输入**：同时按下/拟人时序
#         （Alt 先按 350ms 再敲空格）/长按 2 秒/只给硬件扫描码，四组全无反应，
#         状态栏麦克风图标用 PostMessage、SendMessage、真鼠标点击也点不动；
#       - 对照组证明不是"注入被整体过滤"：注入一次 Shift，它状态栏的「中」照样
#         变成「英」。所以它只对"语音快捷键"这一条路做了拒绝（大概率是特意挡
#         自动化开麦）。
#       结论：豆包这条路走不通，默认值换成微信输入法。
VOICE_HOTKEY_DEFAULT = "ctrl+win"
RELAY_QUIET_S = 1.0                     # 文字静默多久算"说完了"
# 等字上限刻意小于设备的 20 秒等回执上限：宁可回一条"没听清"，也别让设备先超时
# 弹"未收到回执"，那样用户会以为链路坏了。
RELAY_MAX_WAIT_S = 12.0
MIN_INJECT_GAP = 0.3

# 虚拟声卡侧的自动增益：设备麦克风电平偏低，直接喂给输入法容易被 VAD 当静音。
# 目标峰值按"正常说话"来定，最大放大倍数留个上限，免得把底噪一起吹起来。
VOICE_AGC = True
VOICE_TARGET_PEAK = 15000
VOICE_MAX_GAIN = 8.0
VOICE_CABLE_GAIN = 1.0        # 关掉 AGC 时用的固定倍数
VOICE_AGC_FLOOR = 150         # 一块的峰值低于这个数就认为没人在说话，不参与估增益
VOICE_HOLD_TAIL_S = 0.25      # 松手前再留一点：参考实现用 250ms，别把尾音剪掉
VOICE_HPF_HZ = 120.0          # 写入前的高通截止：去掉设备录音里的低频隆隆声
VOICE_RELEASE_GUARD_S = 5.0   # 录音结束后多久还没松手就强制松开（兜底，不是收尾计时）
VOICE_HOLD_MAX_S = 135.0      # 按住的硬上限：设备单次录音 120 秒封顶，留点余量                    # 两次热键注入的最小间隔（防抖）
RELAY_TITLE = "WorkBuddy 语音中转（请勿关闭）"

# 中转窗口的观感：右下角一小块 + 半透明。原来是个 520×150 的大窗压在屏幕底部正中，
# 录音时很挡视线；窗口只在取字期间露一下（用完即收），所以做小做淡就够。
RELAY_WIN_W = 340          # 宽：够放下两三行听写文字
RELAY_WIN_H = 120          # 高
RELAY_WIN_MARGIN = 24      # 距工作区右边/下边
RELAY_WIN_ALPHA = 210      # 不透明度 0-255（210≈82%：字看得清，又不挡底下内容）


# ---- WinMM 常量与结构 ----
_WAVE_FORMAT_PCM = 1
_CALLBACK_NULL = 0
_WAVE_MAPPER = -1
_WHDR_DONE = 0x00000001
_MMSYSERR_NOERROR = 0
_MAXPNAMELEN = 32


class _WAVEFORMATEX(ctypes.Structure):
    _pack_ = 1          # C 里是 18 字节（自然对齐会补成 20；偏移本来就一致）
    _fields_ = [
        ("wFormatTag", ctypes.c_ushort),
        ("nChannels", ctypes.c_ushort),
        ("nSamplesPerSec", ctypes.c_uint32),
        ("nAvgBytesPerSec", ctypes.c_uint32),
        ("nBlockAlign", ctypes.c_ushort),
        ("wBitsPerSample", ctypes.c_ushort),
        ("cbSize", ctypes.c_ushort),
    ]


class _WAVEOUTCAPSW(ctypes.Structure):
    _fields_ = [
        ("wMid", ctypes.c_ushort),
        ("wPid", ctypes.c_ushort),
        ("vDriverVersion", ctypes.c_uint32),
        ("szPname", ctypes.c_wchar * _MAXPNAMELEN),
        ("dwFormats", ctypes.c_uint32),
        ("wChannels", ctypes.c_ushort),
        ("wReserved1", ctypes.c_ushort),
        ("dwSupport", ctypes.c_uint32),
    ]


class _WAVEHDR(ctypes.Structure):
    pass


_WAVEHDR._fields_ = [
    ("lpData", ctypes.c_void_p),
    ("dwBufferLength", ctypes.c_uint32),
    ("dwBytesRecorded", ctypes.c_uint32),
    ("dwUser", ctypes.c_void_p),
    ("dwFlags", ctypes.c_uint32),
    ("dwLoops", ctypes.c_uint32),
    ("lpNext", ctypes.POINTER(_WAVEHDR)),
    ("reserved", ctypes.c_void_p),
]

if sys.platform == "win32":
    _winmm = ctypes.WinDLL("winmm", use_last_error=True)
    _winmm.waveOutGetNumDevs.restype = ctypes.c_uint
    _winmm.waveOutGetDevCapsW.argtypes = (ctypes.c_void_p, ctypes.POINTER(_WAVEOUTCAPSW),
                                          ctypes.c_uint)
    _winmm.waveOutOpen.argtypes = (ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p,
                                   ctypes.POINTER(_WAVEFORMATEX), ctypes.c_void_p,
                                   ctypes.c_void_p, ctypes.c_uint)
    _winmm.waveOutPrepareHeader.argtypes = (ctypes.c_void_p, ctypes.POINTER(_WAVEHDR),
                                            ctypes.c_uint)
    _winmm.waveOutWrite.argtypes = (ctypes.c_void_p, ctypes.POINTER(_WAVEHDR),
                                    ctypes.c_uint)
    _winmm.waveOutUnprepareHeader.argtypes = (ctypes.c_void_p, ctypes.POINTER(_WAVEHDR),
                                              ctypes.c_uint)
    _winmm.waveOutClose.argtypes = (ctypes.c_void_p,)
    _winmm.waveOutReset.argtypes = (ctypes.c_void_p,)
else:  # pragma: no cover - 只为让非 Windows 上能 import
    _winmm = None


def _mono_to_stereo(pcm: bytes) -> bytes:
    """单声道 → 立体声（L=R）。

    设备回传的是单声道，而虚拟声卡的流是立体声。**必须显式复制**：
    直接把单声道样本塞进去，每两个样本会被当成一对 (L,R)，波形被拆到两个声道、
    等效速率减半 —— 表现是"有声音但音调全错"。参考实现踩过这个坑，单测抓不到。
    """
    if not pcm:
        return b""
    src = array.array("h")
    src.frombytes(pcm)
    out = array.array("h")
    for value in src:
        out.extend((value, value))
    return out.tobytes()


def find_cable_device(hint: str = "") -> tuple[int, str]:
    """按名字找虚拟声卡的播放端，返回 (设备号, 设备名)；找不到回 (-1, "")。

    只枚举、不打开 —— 单独提供出来是为了让自检和日志能说清"到底看到了什么设备"。
    优先级：hint（默认 "CABLE Input"）→ 任何 VB-Audio 播放端；一律跳过 16ch 变体。
    """
    if _winmm is None:
        return -1, ""
    primary = (hint or CABLE_HINT).lower()
    caps = _WAVEOUTCAPSW()
    fallback: tuple[int, str] = (-1, "")
    for dev in range(_winmm.waveOutGetNumDevs()):
        if _winmm.waveOutGetDevCapsW(dev, ctypes.byref(caps), ctypes.sizeof(caps)) != _MMSYSERR_NOERROR:
            continue
        name = caps.szPname or ""
        low = name.lower()
        if CABLE_HINT_SKIP in low:
            continue
        if primary in low:
            return dev, name
        if fallback[0] < 0 and CABLE_HINT_ALT.lower() in low:
            fallback = (dev, name)
    return fallback


def _clip16(v: float) -> int:
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return int(v)

def _apply_gain(pcm_mono: bytes, gain: float) -> bytes:
    """整块乘一个倍数（带硬限幅）。只用于写虚拟声卡的那份，不动存档的 WAV。"""
    if gain == 1.0 or not pcm_mono:
        return pcm_mono
    src = array.array("h")
    src.frombytes(pcm_mono[:len(pcm_mono) // 2 * 2])
    return array.array("h", (_clip16(x * gain) for x in src)).tobytes()

class _HighPass:
    """单路二阶巴特沃斯高通（Direct Form I），按块调用、状态跨块保留。

    为什么要有它：真机录音的 0–150Hz 占了约 11% 的能量（干净语音样本是 0%），
    放大之后这段隆隆声会先把 AGC 的峰值占掉，语音反而没被抬起来。先砍掉再放大。
    """

    def __init__(self, rate: int, cutoff: float) -> None:
        import math as _m
        w = 2.0 * _m.pi * cutoff / rate
        cw, sw = _m.cos(w), _m.sin(w)
        q = 1.0 / _m.sqrt(2.0)              # Butterworth 的 Q
        alpha = sw / (2.0 * q)
        b0, b1, b2 = (1 + cw) / 2, -(1 + cw), (1 + cw) / 2
        a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
        self._b = (b0 / a0, b1 / a0, b2 / a0)
        self._a = (a1 / a0, a2 / a0)
        self._x1 = self._x2 = self._y1 = self._y2 = 0.0

    def reset(self) -> None:
        self._x1 = self._x2 = self._y1 = self._y2 = 0.0

    def process(self, pcm_mono: bytes) -> bytes:
        if not pcm_mono:
            return pcm_mono
        src = array.array("h")
        src.frombytes(pcm_mono[: len(pcm_mono) // 2 * 2])
        b0, b1, b2 = self._b
        a1, a2 = self._a
        out = array.array("h", bytes(len(src) * 2))
        for i, x in enumerate(src):
            y = (b0 * x + b1 * self._x1 + b2 * self._x2
                 - a1 * self._y1 - a2 * self._y2)
            self._x2, self._x1 = self._x1, x
            self._y2, self._y1 = self._y1, y
            out[i] = 32767 if y > 32767 else (-32768 if y < -32768 else int(y))
        return out.tobytes()


class _CableWriter:
    """把 16kHz 单声道 PCM 持续写进虚拟声卡的播放端（WinMM/MME，流式）。

    输入法是实时听麦克风的流，不是读文件 —— 所以必须收到就写。缓冲上限 2 秒，
    超了**丢最旧的**：说话场景里"最新"比"完整"重要，宁可漏字也不能越积越延迟。
    没数据时补静音，保持流不中断（否则输入法会因为输入流停摆而中止听写）。
    """

    def __init__(self, hint: str = CABLE_HINT, enabled: bool = True) -> None:
        self._hint = hint
        self._enabled = enabled
        self._lock = threading.Lock()
        self._buffer = bytearray()
        self._handle = None
        self._device_name = ""
        self._device_id = -1
        self._in_flight: list[tuple[_WAVEHDR, ctypes.Array]] = []
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._failed = False
        self._max_bytes = int(CABLE_BUF_SECONDS * CABLE_RATE) * CABLE_CHANNELS * 2
        self._block_bytes = (CABLE_PERIOD_MS * CABLE_RATE // 1000) * CABLE_CHANNELS * 2
        self.bytes_written = 0
        self.blocks_dropped = 0
        self.agc = VOICE_AGC
        self.gain = VOICE_CABLE_GAIN
        self.hpf = _HighPass(CABLE_RATE, VOICE_HPF_HZ)
        self._peak = 0.0
        self._gain_now = 1.0

    # --- 状态 ---
    @property
    def enabled(self) -> bool:
        return self._enabled and _winmm is not None

    @property
    def is_open(self) -> bool:
        return self._handle is not None

    def stats(self) -> dict:
        with self._lock:
            buffered = len(self._buffer)
        return {
            "enabled": self.enabled, "open": self.is_open, "device": self._device_name,
            "bytes_written": self.bytes_written, "blocks_dropped": self.blocks_dropped,
            "buffered_bytes": buffered, "pending_blocks": len(self._in_flight),
            "gain": round(self._gain_now, 2),
        }

    def wait_drained(self, timeout: float) -> bool:
        """等缓冲里的**真实音频**都交给 WinMM 了（不含在途块的播放时间）。

        停听写之前要留这段时间：热键一按输入法就收尾，缓冲里还没播出去的
        尾巴会被剪掉。
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if not self._buffer:
                    return True
            time.sleep(0.05)
        return False

    def open(self) -> bool:
        """显式打开输出流（正常链路不需要：write() 会在第一帧自动开）。

        自检要先单独确认"声卡打得开"，才能把"声卡打不开"和"输入法不出字"分开报。
        """
        if self._handle is not None:
            return True
        return self._open()

    def retry(self) -> None:
        """清掉"永久失败"标记，下次写入时重新尝试打开。

        一次暂时性失败（设备被占用 / 驱动刚装好还没就绪）不该让语音永远哑掉。
        """
        self._failed = False

    # --- 写入 ---
    def write(self, pcm_mono: bytes) -> bool:
        if not self.enabled or not pcm_mono:
            return False
        if self._handle is None and not self._open():
            return False
        pcm_mono = self.hpf.process(pcm_mono)      # 先去掉低频隆隆声，再谈增益
        if self.agc:
            pcm_mono = self._apply_agc(pcm_mono)
        elif self.gain != 1.0:
            pcm_mono = _apply_gain(pcm_mono, self.gain)
        pcm = _mono_to_stereo(pcm_mono)
        with self._lock:
            self._buffer.extend(pcm)
            if len(self._buffer) > self._max_bytes:
                excess = len(self._buffer) - self._max_bytes
                del self._buffer[:excess]
                self.blocks_dropped += 1
        return True

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.5)
            self._thread = None
        for hdr, _buf in self._in_flight:
            try:
                _winmm.waveOutUnprepareHeader(self._handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
            except Exception:
                pass
        self._in_flight.clear()
        if self._handle is not None:
            try:
                _winmm.waveOutReset(self._handle)
                _winmm.waveOutClose(self._handle)
            except Exception:
                pass
            self._handle = None
        with self._lock:
            self._buffer.clear()
        self.hpf.reset()
        self._stop = threading.Event()

    def _apply_agc(self, pcm_mono: bytes) -> bytes:
        """按块估增益：涨得快、落得慢，避免一块一块地跳。

        峰值太低（安静的间隙）时不更新估计 —— 否则会把底噪吹成"说话"。
        """
        src = array.array("h")
        src.frombytes(pcm_mono[:len(pcm_mono) // 2 * 2])
        if not len(src):
            return pcm_mono
        peak = max(max(src), -min(src))
        if peak >= VOICE_AGC_FLOOR:
            if peak > self._peak:
                self._peak = float(peak)
            else:
                self._peak = self._peak * 0.7 + peak * 0.3
        gain = 1.0
        if self._peak > VOICE_AGC_FLOOR:
            gain = min(VOICE_MAX_GAIN, VOICE_TARGET_PEAK / self._peak)
        # 平滑一下增益本身，避免音量忽大忽小（识别对突变的包络不友好）
        self._gain_now = self._gain_now * 0.6 + gain * 0.4
        self._gain_now = max(1.0, min(VOICE_MAX_GAIN, self._gain_now))
        if self._gain_now <= 1.02:
            return pcm_mono
        return array.array("h", (_clip16(x * self._gain_now) for x in src)).tobytes()

    # --- 内部 ---
    def _open(self) -> bool:
        if self._failed or _winmm is None:
            return False
        dev, name = find_cable_device(self._hint)
        if dev < 0:
            self._failed = True   # 只报一次，不刷日志
            return False
        handle = ctypes.c_void_p()
        fmt = _WAVEFORMATEX(
            wFormatTag=_WAVE_FORMAT_PCM,
            nChannels=CABLE_CHANNELS,
            nSamplesPerSec=CABLE_RATE,
            nAvgBytesPerSec=CABLE_RATE * CABLE_CHANNELS * 2,
            nBlockAlign=CABLE_CHANNELS * 2,
            wBitsPerSample=16,
            cbSize=0,
        )
        rc = _winmm.waveOutOpen(ctypes.byref(handle), ctypes.c_void_p(dev),
                                ctypes.byref(fmt), None, None, _CALLBACK_NULL)
        if rc != _MMSYSERR_NOERROR:
            self._failed = True
            return False
        self._handle = handle
        self._device_id = dev
        self._device_name = name
        self._stop.clear()
        self._thread = threading.Thread(target=self._pump, name="wb-cable", daemon=True)
        self._thread.start()
        return True

    def _pump(self) -> None:
        """WinMM 喂数据线程：回收播完的块、从缓冲取数据补静音填满在途块。"""
        while not self._stop.is_set():
            try:
                # 1) 回收已播完的块
                for item in list(self._in_flight):
                    hdr = item[0]
                    if hdr.dwFlags & _WHDR_DONE:
                        try:
                            _winmm.waveOutUnprepareHeader(self._handle, ctypes.byref(hdr),
                                                          ctypes.sizeof(hdr))
                        except Exception:
                            pass
                        self._in_flight.remove(item)
                # 2) 补满在途块
                while len(self._in_flight) < CABLE_QUEUE_BLOCKS and not self._stop.is_set():
                    with self._lock:
                        take = min(self._block_bytes, len(self._buffer))
                        chunk = bytes(self._buffer[:take]) if take else b""
                        del self._buffer[:take]
                    if len(chunk) < self._block_bytes:
                        chunk += b"\x00" * (self._block_bytes - len(chunk))
                    buf = ctypes.create_string_buffer(chunk, len(chunk))
                    hdr = _WAVEHDR()
                    hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
                    hdr.dwBufferLength = len(chunk)
                    if _winmm.waveOutPrepareHeader(self._handle, ctypes.byref(hdr),
                                                   ctypes.sizeof(hdr)) != _MMSYSERR_NOERROR:
                        break
                    if _winmm.waveOutWrite(self._handle, ctypes.byref(hdr),
                                           ctypes.sizeof(hdr)) != _MMSYSERR_NOERROR:
                        _winmm.waveOutUnprepareHeader(self._handle, ctypes.byref(hdr),
                                                      ctypes.sizeof(hdr))
                        break
                    # buf 必须跟着 hdr 一起保活，否则被 GC 后 WinMM 读到野内存
                    self._in_flight.append((hdr, buf))
                    self.bytes_written += len(chunk)
            except Exception:
                pass
            time.sleep(0.02)

# ---- SendInput 注入快捷键 ----
_INPUT_KEYBOARD = 1
_KEYEVENTF_KEYUP = 0x0002

# 修饰键 → (虚拟键码, 附加标志)。带 0x0001 的是"扩展键"标志。
#
# 通用码（ctrl=0x11 / alt=0x12 / shift=0x10）注入时系统会解析成**左侧**那个键，
# 这是标准做法（参考实现实测：微信输入法的快捷键捕获框把注入结果识别成
# 「左Ctrl + 左Alt + V」）。左右变体则直接用专用虚拟键码，不走扩展标志 —— 少一层
# 不确定性。豆包输入法的「右 Alt」对应的就是 ralt。
_KEYEVENTF_EXTENDEDKEY = 0x0001
_MAPVK_VK_TO_VSC = 0
_E = _KEYEVENTF_EXTENDEDKEY
_MODIFIERS = {
    "ctrl": (0x11, 0), "control": (0x11, 0),
    "lctrl": (0xA2, 0), "rctrl": (0xA3, _E),
    "alt": (0x12, 0), "lalt": (0xA4, 0), "ralt": (0xA5, _E),
    "shift": (0x10, 0), "lshift": (0xA0, 0), "rshift": (0xA1, 0),
    "win": (0x5B, 0), "lwin": (0x5B, 0), "rwin": (0x5C, _E),
}

# 扩展键集合：注入时必须带 0x0001，否则会被钩子当成左侧键。
_EXT_VKS = frozenset((
    0xA3, 0xA5,            # 右 Ctrl / 右 Alt
    0x5B, 0x5C, 0x5D,      # 左 Win / 右 Win / 菜单键
    0x2D, 0x2E,            # Insert / Delete
    0x24, 0x23, 0x21, 0x22,  # Home / End / PageUp / PageDown
    0x25, 0x26, 0x27, 0x28,  # 方向键
    0x90, 0x2C,            # NumLock / PrintScreen
))
_NAMED_KEYS = {
    "space": 0x20, "enter": 0x0D, "return": 0x0D, "tab": 0x09, "esc": 0x1B,
    "escape": 0x1B, "backspace": 0x08, "delete": 0x2E, "insert": 0x2D,
    "home": 0x24, "end": 0x23, "pageup": 0x21, "pagedown": 0x22,
    "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
}
for _i in range(1, 25):
    _NAMED_KEYS["f%d" % _i] = 0x70 + _i - 1

if sys.platform == "win32":
    _ULONG_PTR = ctypes.c_uint64 if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_uint32

    class _KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                    ("dwFlags", ctypes.c_uint32), ("time", ctypes.c_uint32),
                    ("dwExtraInfo", _ULONG_PTR)]

    class _MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                    ("mouseData", ctypes.c_uint32), ("dwFlags", ctypes.c_uint32),
                    ("time", ctypes.c_uint32), ("dwExtraInfo", _ULONG_PTR)]

    class _HARDWAREINPUT(ctypes.Structure):
        _fields_ = [("uMsg", ctypes.c_uint32), ("wParamL", ctypes.c_ushort),
                    ("wParamH", ctypes.c_ushort)]

    class _INPUTUNION(ctypes.Union):
        _fields_ = [("mi", _MOUSEINPUT), ("ki", _KEYBDINPUT), ("hi", _HARDWAREINPUT)]

    class _INPUT(ctypes.Structure):
        _fields_ = [("type", ctypes.c_uint32), ("u", _INPUTUNION)]

    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _user32.SendInput.argtypes = (ctypes.c_uint, ctypes.POINTER(_INPUT), ctypes.c_int)
    _user32.SendInput.restype = ctypes.c_uint
else:  # pragma: no cover
    _INPUT = None
    _user32 = None


def parse_hotkey(spec: str) -> tuple[list[tuple[int, int]], int]:
    """把 "ctrl+win" 解析成（[(修饰键虚拟码, 标志), ...], 主键）。解析失败抛 ValueError。

    示例（豆包输入法的三种唤起方式）：
        "ralt"            → 长按模式下的右 Alt（免提模式下按一次即可）
        "ralt+space"      → 右 Alt + 空格
        "ctrl+win"        → 左 Ctrl + Win（推荐：两个都是标准码，注入最稳）
    """
    parts = [p.strip().lower() for p in (spec or "").replace(" ", "").split("+") if p.strip()]
    if not parts:
        raise ValueError("快捷键为空")
    modifiers: list[tuple[int, int]] = []
    keys: list[str] = []
    for part in parts:
        if part in _MODIFIERS:
            item = _MODIFIERS[part]
            if item not in modifiers:
                modifiers.append(item)
        else:
            keys.append(part)
    # 主键最多一个；没有主键时必须是纯修饰键组合（豆包的「Ctrl+Win」「右 Alt」）。
    if len(keys) > 1:
        raise ValueError("快捷键最多只能有一个主键：%r" % spec)
    if not keys:
        return modifiers, 0
    name = keys[0]
    if len(name) == 1 and ("a" <= name <= "z" or "0" <= name <= "9"):
        return modifiers, ord(name.upper())
    if name in _NAMED_KEYS:
        return modifiers, _NAMED_KEYS[name]
    raise ValueError("不认识的按键：%r" % name)


_SCAN_CACHE: dict = {}

def _scan_code(vk: int) -> int:
    """虚拟键码 → 硬件扫描码（MapVirtualKey）。查不到就回 0，让系统自己填。"""
    if vk in _SCAN_CACHE:
        return _SCAN_CACHE[vk]
    code = 0
    if _user32 is not None:
        try:
            _user32.MapVirtualKeyW.argtypes = (ctypes.c_uint, ctypes.c_uint)
            _user32.MapVirtualKeyW.restype = ctypes.c_uint
            code = int(_user32.MapVirtualKeyW(vk, _MAPVK_VK_TO_VSC)) & 0xFF
        except Exception:
            code = 0
    _SCAN_CACHE[vk] = code
    return code

def _hotkey_events(spec: str) -> tuple:
    """把快捷键解析成（按下事件, 松开事件）。

    无主键（纯修饰键组合）时不发主键事件；有主键时修饰键先按、主键最后按，
    松开顺序完全相反 —— 部分 IME 在钩子里按顺序判定，乱序会被当成单个按键。
    """
    modifiers, key = parse_hotkey(spec)

    def ev(vk: int, up: bool, extra_flags: int = 0) -> "_INPUT":
        flags = extra_flags | (_KEYEVENTF_EXTENDEDKEY if vk in _EXT_VKS else 0) \
            | (_KEYEVENTF_KEYUP if up else 0)
        return _INPUT(type=_INPUT_KEYBOARD,
                      u=_INPUTUNION(ki=_KEYBDINPUT(wVk=vk, wScan=_scan_code(vk),
                                                   dwFlags=flags, time=0, dwExtraInfo=0)))

    down = [ev(vk, False, flags) for vk, flags in modifiers]
    up: list = []
    if key:
        down.append(ev(key, False))
        up.append(ev(key, True))
    up += [ev(vk, True, flags) for vk, flags in reversed(modifiers)]
    return down, up

def _send_inputs(events: list) -> bool:
    if _user32 is None or not events:
        return False
    arr = (_INPUT * len(events))(*events)
    return _user32.SendInput(len(events), arr, ctypes.sizeof(_INPUT)) == len(events)

def hotkey_down(spec: str) -> bool:
    """只按下、不松开 —— 微信输入法「按住说话」要的就是这个。

    用 SendInput 而不是 keyboard/pyautogui：后者要装全局键盘钩子（杀软会拦，
    对 IME 的兼容性反而更差）。SendInput 是官方 API，不需要管理员、不装钩子。

    注意：这里按下的是 **Win 键**，会一直保持按下状态到 hotkey_up。所以调用方
    必须保证一定配得上一次 hotkey_up（见 _asr_release_guard）。
    """
    if _user32 is None:
        return False
    try:
        down, _up = _hotkey_events(spec)
    except ValueError:
        return False
    return _send_inputs(down)

def hotkey_up(spec: str) -> bool:
    """松开之前按下的组合键。"""
    if _user32 is None:
        return False
    try:
        _down, up = _hotkey_events(spec)
    except ValueError:
        return False
    return _send_inputs(up)

def press_hotkey(spec: str, hold_s: float = 0.04) -> bool:
    """敲一下组合键（修饰键先按、主键最后按，松开顺序相反）。"""
    if _user32 is None:
        return False
    try:
        down, up = _hotkey_events(spec)
    except ValueError:
        return False
    if not _send_inputs(down):
        return False
    if hold_s > 0:
        time.sleep(hold_s)      # 太短时少数 IME 会漏掉组合判定
    return _send_inputs(up)

# ---- 当前生效输入法（TSF）----
# 微信输入法的语音识别结果，**只有在它自己是"当前生效输入法"时才会打进输入框，
# 否则只放进剪贴板**（参考实现 inputegg 的 ime 模块把这条写在文档里）。中文输入法
# 共用同一个键盘布局 0804，ActivateKeyboardLayout 那种办法分不开，必须按输入法
# CLSID + profile GUID 调 ITfInputProcessorProfileMgr::ActivateProfile，且它作用于
# 整个桌面会话。
#
# 不引 comtypes：桥要保持"无第三方依赖"，所以这里手写 vtable 调用（纯 ctypes）。
_ole32 = None
if sys.platform == "win32":
    _ole32 = ctypes.WinDLL("ole32")
    _ole32.CoInitializeEx.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    _ole32.CoInitializeEx.restype = ctypes.c_long

_LANG_ZH_CN = 0x0804
_TF_PROFILETYPE_INPUTPROCESSOR = 1
_TF_IPPMF_FORSESSION = 0x20000000
_TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE = 0x00000004
_CLSID_TF_InputProcessorProfiles = "{33C53A50-F456-4884-B049-85FD643ECFED}"
_IID_ITfInputProcessorProfileMgr = "{71C6E74C-0F28-11D8-A82A-00065B84435C}"
_GUID_TFCAT_TIP_KEYBOARD = "{34745C63-B2F0-4784-8B67-5E12C8701A31}"
# 语音引擎 → 输入法 CLSID。微信输入法这一个是本机注册表里读出来的。
IME_ENGINE_TIPS = {"wechat": "{86598FB9-66A2-463E-B9C2-AEB906D477AD}"}

class _GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_uint32), ("Data2", ctypes.c_uint16),
                ("Data3", ctypes.c_uint16), ("Data4", ctypes.c_ubyte * 8)]

    def __init__(self, text: str = "") -> None:
        super().__init__()
        if not text:
            return
        p = text.strip().strip("{}").split("-")
        self.Data1 = int(p[0], 16)
        self.Data2 = int(p[1], 16)
        self.Data3 = int(p[2], 16)
        rest = p[3] + p[4]
        for i in range(8):
            self.Data4[i] = int(rest[i * 2:i * 2 + 2], 16)

    def text(self) -> str:
        return "{%08X-%04X-%04X-%s-%s}" % (
            self.Data1, self.Data2, self.Data3,
            "".join("%02X" % self.Data4[i] for i in range(2)),
            "".join("%02X" % self.Data4[i] for i in range(2, 8)))

class _TF_INPUTPROCESSORPROFILE(ctypes.Structure):
    _fields_ = [("dwProfileType", ctypes.c_uint32), ("langid", ctypes.c_uint16),
                ("clsid", _GUID), ("guidProfile", _GUID), ("catid", _GUID),
                ("hklSubstitute", ctypes.c_void_p), ("dwCaps", ctypes.c_uint32),
                ("hkl", ctypes.c_void_p), ("dwFlags", ctypes.c_uint32)]

if sys.platform == "win32":
    _ole32.CoCreateInstance.argtypes = (ctypes.POINTER(_GUID), ctypes.c_void_p,
                                        ctypes.c_uint32, ctypes.POINTER(_GUID),
                                        ctypes.POINTER(ctypes.c_void_p))
    _ole32.CoCreateInstance.restype = ctypes.c_long

_ime_mgr_ptr = None

def _ime_mgr():
    """拿到 ITfInputProcessorProfileMgr；失败返回 None（调用方一律"失败就不管"）。"""
    global _ime_mgr_ptr
    if _ime_mgr_ptr is not None:
        return _ime_mgr_ptr
    if _ole32 is None:
        return None
    try:
        hr = _ole32.CoInitializeEx(None, 0x2)      # APARTMENTTHREADED
        # S_OK / S_FALSE / RPC_E_CHANGED_MODE 三种都能继续用
        if hr not in (0, 1, -2147417850):
            return None
        ptr = ctypes.c_void_p()
        hr = _ole32.CoCreateInstance(
            ctypes.byref(_GUID(_CLSID_TF_InputProcessorProfiles)), None, 0x1,
            ctypes.byref(_GUID(_IID_ITfInputProcessorProfileMgr)), ctypes.byref(ptr))
        if hr != 0 or not ptr:
            return None
        _ime_mgr_ptr = ptr
        return ptr
    except Exception:
        return None

def _ime_vtbl(ptr, index: int, restype, *argtypes):
    """按下标取 vtable 里的函数指针，包成可直接调用的 ctypes 函数。"""
    vtbl = ctypes.cast(ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    proto = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)
    return proto(vtbl[index])

def ime_profile_guid(clsid: str) -> str:
    """某输入法在中文（0804）下注册的第一个 profile GUID。读不到返回空串。"""
    path = r"SOFTWARE\Microsoft\CTF\TIP\%s\LanguageProfile\0x00000804" % clsid
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as key:
            for i in range(winreg.QueryInfoKey(key)[0]):
                return winreg.EnumKey(key, i)
    except OSError:
        return ""
    return ""

def ime_active_tip() -> str:
    """当前生效的键盘类输入法 CLSID；读不到返回空串。"""
    mgr = _ime_mgr()
    if mgr is None:
        return ""
    try:
        prof = _TF_INPUTPROCESSORPROFILE()
        fn = _ime_vtbl(mgr, 10, ctypes.c_long,      # GetActiveProfile
                       ctypes.POINTER(_GUID), ctypes.POINTER(_TF_INPUTPROCESSORPROFILE))
        if fn(mgr, ctypes.byref(_GUID(_GUID_TFCAT_TIP_KEYBOARD)), ctypes.byref(prof)) != 0:
            return ""
        return prof.clsid.text()
    except Exception:
        return ""

def ime_ensure(engine: str = "wechat") -> bool:
    """确保当前生效输入法就是语音引擎那个；已经对了返回 True，切换成功也返回 True。

    失败一律返回 False，调用方只记日志、不中断流程 —— 切换失败时语音输入仍然
    能开麦，只是识别结果会进剪贴板（等于取不到字，日志里能看出来）。
    """
    clsid = IME_ENGINE_TIPS.get(engine)
    if not clsid:
        return False
    cur = ime_active_tip()
    if cur.upper() == clsid.upper():
        return True
    mgr = _ime_mgr()
    if mgr is None:
        return False
    prof = ime_profile_guid(clsid)
    if not prof:
        return False
    try:
        fn = _ime_vtbl(mgr, 3, ctypes.c_long,       # ActivateProfile
                       ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(_GUID),
                       ctypes.POINTER(_GUID), ctypes.c_void_p, ctypes.c_uint32)
        hr = fn(mgr, _TF_PROFILETYPE_INPUTPROCESSOR, _LANG_ZH_CN,
                ctypes.byref(_GUID(clsid)), ctypes.byref(_GUID(prof)), None,
                _TF_IPPMF_FORSESSION | _TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE)
        return hr == 0
    except Exception:
        return False

# ---- 抢前台 ----
_VK_F24 = 0x87

def _force_foreground(u32, k32, hwnd) -> bool:
    """把窗口抢到前台并**如实返回**结果。

    背景：SetForegroundWindow 有一道"前台锁"——只有当前前台进程、或刚接收过
    用户输入事件的进程才能抢成功。托盘守护进程两条都不满足，直接调它等于没调。
    所以按三段来：
      ① 直接抢（用户主动跑自检、进程本来就在前台时，这一步就够）
      ② 附加到前台线程再抢（借它的输入队列资格）
      ③ 先敲一下无害的 F24 —— 注入事件同样算"我接收过输入"，借此拿到许可
    第③步用 F24 而不是 Alt：Alt 会把前台窗口的菜单栏点出来，F24 在 Windows 上
    基本没人用，没有可见副作用。
    """
    u32.GetForegroundWindow.restype = ctypes.c_void_p
    u32.SetForegroundWindow(ctypes.c_void_p(hwnd))
    if u32.GetForegroundWindow() == hwnd:
        return True

    fg = u32.GetForegroundWindow()
    if fg:
        me = k32.GetCurrentThreadId()
        u32.GetWindowThreadProcessId.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
        u32.GetWindowThreadProcessId.restype = ctypes.c_uint32
        tid = u32.GetWindowThreadProcessId(ctypes.c_void_p(fg), None)
        if tid and tid != me:
            u32.AttachThreadInput.argtypes = (ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int)
            u32.AttachThreadInput(tid, me, True)
            u32.SetForegroundWindow(ctypes.c_void_p(hwnd))
            u32.AttachThreadInput(tid, me, False)
            if u32.GetForegroundWindow() == hwnd:
                return True

    press_hotkey("f24")
    u32.SetForegroundWindow(ctypes.c_void_p(hwnd))
    return u32.GetForegroundWindow() == hwnd

# ---- 中转窗口（取字）----
class _RelayWindow:
    """一个原生 EDIT 窗口：录音期间抢焦点，接住输入法打出来的字，供桥读走。

    为什么需要：我们的方案是"桥自己取字 + REST 投递"，而不是像参考实现那样让
    输入法直接把字打进前台应用。代价是这个窗口在录音期间会抢焦点 —— 反过来说
    也省掉了"必须让 WorkBuddy 输入框在前台"的限制。

    用原生 EDIT 而不是 Tk：编辑器控件是输入法最熟的落点（参考实现的目标应用就是
    各类标准控件），兼容性最好。

    所有 Win32 调用都走 SendMessageW，跨线程是同步的；窗口线程只负责泵消息。
    """

    def __init__(self, title: str = RELAY_TITLE) -> None:
        self._title = title
        self._hwnd = None
        self._edit = None            # 子 EDIT 控件：真正的取字/落字窗口
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._failed = False
        self._commands: "queue.Queue[tuple[str, object]]" = queue.Queue()

    @property
    def available(self) -> bool:
        return self._hwnd is not None

    def start(self) -> bool:
        if self._hwnd is not None:
            return True
        if self._failed or sys.platform != "win32":
            return False
        self._thread = threading.Thread(target=self._run, name="wb-relay", daemon=True)
        self._thread.start()
        self._ready.wait(timeout=3.0)
        return self._hwnd is not None

    def stop(self) -> None:
        if self._hwnd is not None:
            self._commands.put(("quit", None))
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None
        self._hwnd = None

    # --- 对外动作 ---
    def focus(self) -> bool:
        """显示并抢焦点 —— 必须在下发快捷键之前完成，否则字会打到别处。

        返回值是**真的抢到了没有**（用 GetForegroundWindow 核对过）：托盘里的
        守护进程属于后台程序，Windows 前台锁会拦下它，所以必须确认到位再注入
        快捷键，否则输入法的字会打进用户当前那个窗口。
        """
        result: list = []
        if self._call(("focus", result), timeout=2.5) and result:
            return bool(result[0])
        return False

    def hide(self) -> bool:
        """收起来（不销毁）：窗口只在取字时需要，空闲时不该一直占着桌面。

        manual 档是例外 —— 那一档的语义就是把字留在窗口里给人看/复制。
        窗口不带 WS_VISIBLE，下次 focus() 会重新显示，所以收了不丢状态。
        """
        return self._call(("hide", None))

    def clear(self) -> bool:
        return self._call(("clear", None))

    def text(self) -> str:
        result: list[str] = []
        if self._call(("get", result), timeout=2.0):
            return result[0] if result else ""
        return ""

    # --- 内部 ---
    def _call(self, cmd: tuple[str, object], timeout: float = 3.0):
        if self._hwnd is None and not self.start():
            return False
        self._commands.put(cmd)
        if cmd[0] in ("get", "focus") and timeout:
            # 按"是否已回填"判断，不能按内容真值 —— 确实没有字时也要立刻返回。
            placeholder = cmd[1]
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if isinstance(placeholder, list) and placeholder:
                    return True
                time.sleep(0.02)
            return False
        return True

    def _run(self) -> None:
        """窗口线程：建窗口 → 泵消息 → 处理命令。"""
        try:
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            u32 = ctypes.WinDLL("user32", use_last_error=True)
            g32 = ctypes.WinDLL("gdi32", use_last_error=True)
            hinst = k32.GetModuleHandleW(None)
            u32.CreateWindowExW.restype = ctypes.c_void_p
            _WS_EX_TOPMOST = 0x00000008
            _WS_EX_TOOLWINDOW = 0x00000080
            _WS_EX_LAYERED = 0x00080000
            _LWA_ALPHA = 0x00000002
            _WS_POPUP = 0x80000000
            _WS_VISIBLE = 0x10000000
            _WS_BORDER = 0x00800000
            _WS_VSCROLL = 0x00200000
            _ES_MULTILINE = 0x0004
            _ES_AUTOVSCROLL = 0x0040
            _EM_SETSEL = 0x00B1
            _WM_SETTEXT = 0x000C
            _WM_GETTEXT = 0x000D
            _WM_GETTEXTLENGTH = 0x000E
            _WM_SETFONT = 0x0030
            _SW_SHOW = 5
            _SW_HIDE = 0
            _HWND_TOPMOST = -1
            _SWP_NOSIZE = 0x0001
            _SWP_NOMOVE = 0x0002

            # 刻意不带 WS_VISIBLE：窗口由 focus() 按需显示，空闲时不占屏幕。
            _WS_CHILD = 0x40000000
            # 右下角一小块：按**工作区**（而不是全屏）定位，免得到压在任务栏下面。
            sw = u32.GetSystemMetrics(0)
            sh = u32.GetSystemMetrics(1)
            win_w, win_h = min(RELAY_WIN_W, max(200, sw - 80)), RELAY_WIN_H

            class _RECT(ctypes.Structure):
                _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                            ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

            work = _RECT()
            _SPI_GETWORKAREA = 0x0030
            if u32.SystemParametersInfoW(_SPI_GETWORKAREA, 0, ctypes.byref(work), 0):
                right, bottom = work.right, work.bottom
            else:
                right, bottom = sw, sh
            win_x = max(0, right - win_w - RELAY_WIN_MARGIN)
            win_y = max(0, bottom - win_h - RELAY_WIN_MARGIN)
            # 容器（STATIC，用来显示标题）+ 子 EDIT（真正接字的地方）。
            # EDIT 的"窗口文本"就是编辑内容，所以标题必须留在容器上。
            hwnd = u32.CreateWindowExW(
                _WS_EX_TOPMOST | _WS_EX_TOOLWINDOW | _WS_EX_LAYERED, "STATIC", self._title,
                _WS_POPUP | _WS_BORDER, win_x, win_y, win_w, win_h, None, None, hinst, None)
            if not hwnd:
                self._failed = True
                self._ready.set()
                return
            # 分层窗口必须设一次透明度，否则可能整块不显示。
            u32.SetLayeredWindowAttributes(ctypes.c_void_p(hwnd), 0, RELAY_WIN_ALPHA, _LWA_ALPHA)
            edit = u32.CreateWindowExW(
                0, "EDIT", "",
                _WS_CHILD | _WS_VISIBLE | _WS_VSCROLL | _ES_MULTILINE | _ES_AUTOVSCROLL,
                10, 28, win_w - 20, win_h - 38, ctypes.c_void_p(hwnd), None, hinst, None)
            if not edit:
                u32.DestroyWindow(ctypes.c_void_p(hwnd))
                self._failed = True
                self._ready.set()
                return
            font = g32.CreateFontW(-16, 0, 0, 0, 400, 0, 0, 0, 134, 0, 0, 0, 0, "Microsoft YaHei")
            u32.SendMessageW(ctypes.c_void_p(edit), _WM_SETFONT, ctypes.c_void_p(font), 1)
            self._hwnd = hwnd
            self._edit = edit
            self._ready.set()

            msg = ctypes.create_string_buffer(64)   # MSG 结构够大即可
            while True:
                try:
                    kind, payload = self._commands.get_nowait()
                except queue.Empty:
                    kind = ""
                    payload = None
                if kind == "quit":
                    break
                if kind == "focus":
                    u32.ShowWindow(ctypes.c_void_p(hwnd), _SW_SHOW)
                    u32.SetWindowPos(ctypes.c_void_p(hwnd), ctypes.c_void_p(_HWND_TOPMOST),
                                     0, 0, 0, 0, _SWP_NOSIZE | _SWP_NOMOVE)
                    u32.BringWindowToTop(ctypes.c_void_p(hwnd))
                    grabbed = _force_foreground(u32, k32, hwnd)
                    # 焦点必须落在子 EDIT 上：输入法只往有焦点的控件里落字。
                    u32.SetFocus(ctypes.c_void_p(edit))
                    if isinstance(payload, list):
                        payload.append(bool(grabbed))
                elif kind == "hide":
                    u32.ShowWindow(ctypes.c_void_p(hwnd), _SW_HIDE)
                elif kind == "clear":
                    u32.SendMessageW(ctypes.c_void_p(edit), _WM_SETTEXT, None, "")
                elif kind == "get":
                    length = u32.SendMessageW(ctypes.c_void_p(edit), _WM_GETTEXTLENGTH, 0, 0)
                    buf = ctypes.create_unicode_buffer(int(length) + 4)
                    u32.SendMessageW(ctypes.c_void_p(edit), _WM_GETTEXT,
                                     ctypes.c_void_p(len(buf)), buf)
                    if isinstance(payload, list):
                        payload.append(buf.value.strip())
                # 泵一轮消息：拖动/重绘都靠它
                while u32.PeekMessageW(msg, None, 0, 0, 1):
                    u32.TranslateMessage(msg)
                    u32.DispatchMessageW(msg)
                time.sleep(0.02)

            u32.DestroyWindow(ctypes.c_void_p(hwnd))   # 子控件随父窗口一起销毁
            self._hwnd = None
            self._edit = None
        except Exception:
            self._failed = True
            self._ready.set()

# ---- 自检与取字共用的小工具 ----
def list_output_devices() -> list[str]:
    """把系统里所有播放设备列出来（诊断用：装完 VB-CABLE 到底认没认出来）。"""
    if _winmm is None:
        return []
    caps = _WAVEOUTCAPSW()
    names: list[str] = []
    for dev in range(_winmm.waveOutGetNumDevs()):
        if _winmm.waveOutGetDevCapsW(dev, ctypes.byref(caps), ctypes.sizeof(caps)) != _MMSYSERR_NOERROR:
            continue
        names.append(f"[{dev}] {caps.szPname}")
    return names


def _resample_linear_mono(samples, src_rate: int, dst_rate: int):
    """线性插值重采样（只用于自检灌音，不进设备主链路）。"""
    if src_rate == dst_rate or len(samples) < 2:
        return samples
    out_len = max(1, int(len(samples) * dst_rate / src_rate))
    step = (src_rate << 16) // dst_rate
    out = array.array("h", bytes(out_len * 2))
    for i in range(out_len):
        pos = i * step
        idx = pos >> 16
        if idx >= len(samples) - 1:
            idx, frac = len(samples) - 1, 0
        else:
            frac = (pos & 0xFFFF) >> 8
        nxt = idx + 1 if idx + 1 < len(samples) else idx
        a, b = samples[idx], samples[nxt]
        out[i] = a + (((b - a) * frac) >> 8)
    return out


def read_wav_mono16k(path: str) -> tuple[bytes, int, float]:
    """读 WAV → (16kHz 单声道 int16 PCM, 原始采样率, 时长秒)。

    多声道只取第一路；非 16kHz 用线性插值转过去（自检用的输入文件什么格式都有）。
    """
    import wave as _wave
    with _wave.open(path, "rb") as w:
        channels, width, rate, frames = (w.getnchannels(), w.getsampwidth(),
                                         w.getframerate(), w.getnframes())
        raw = w.readframes(frames)
    if width != 2:
        raise ValueError(f"只支持 16bit PCM 的 WAV（当前 {width * 8}bit）")
    src = array.array("h")
    src.frombytes(raw)
    if channels > 1:
        src = array.array("h", (src[i] for i in range(0, len(src), channels)))
    if rate != CABLE_RATE:
        src = _resample_linear_mono(src, rate, CABLE_RATE)
    return src.tobytes(), rate, len(src) / CABLE_RATE


def collect_relay_text(relay: "_RelayWindow", quiet_s: float = RELAY_QUIET_S,
                       timeout_s: float = RELAY_MAX_WAIT_S) -> str:
    """等输入法把字打完：文字静默 quiet_s 秒即认为说完了。"""
    deadline = time.monotonic() + timeout_s
    text = ""
    last_change = time.monotonic()
    while time.monotonic() < deadline:
        now = relay.text()
        if now != text:
            text, last_change = now, time.monotonic()
        elif text and (time.monotonic() - last_change) >= quiet_s:
            return text
        time.sleep(0.1)
    return text


def voice_selftest(wav_path: str, hotkey: str = "", deliver: str = "none",
                   manual: bool = False, manual_seconds: float = 40.0) -> int:
    """文字链路自检：WAV → 虚拟声卡 → 输入法听写 → 取字 →（可选）投递。

    **不碰设备**。设备、声卡、输入法三个变量里只验后两个：出字说明链路通，
    不出字就能直接判定问题在"声卡/输入法/焦点"这一侧。
    """
    spec = hotkey or VOICE_HOTKEY_DEFAULT
    print("=" * 62)
    print("  文字链路自检（不接设备）")
    print("=" * 62)

    try:
        pcm, src_rate, seconds = read_wav_mono16k(wav_path)
    except Exception as exc:
        print(f"[x] 读 WAV 失败：{exc}")
        return 1
    print(f"[1/6] 音频 {wav_path}：{seconds:.1f}s，原始 {src_rate}Hz → 16kHz 单声道")

    dev, name = find_cable_device()
    if dev < 0:
        print(f"[x] 没找到含 {CABLE_HINT!r} 的播放设备 —— 需要先装 VB-CABLE（管理员 + 重启）")
        print("    当前枚举到的播放设备：")
        for item in list_output_devices():
            print(f"      {item}")
        return 2
    print(f"[2/6] 虚拟声卡就绪：[{dev}] {name}")

    relay = _RelayWindow()
    if not relay.start():
        print("[x] 中转窗口创建失败")
        return 3
    relay.clear()
    if not relay.focus():
        print("[3/6] 中转窗口抢焦点失败 —— 就停在这里，不往下注入")
        print("      抢不到焦点还敲快捷键，听写的字会打进用户当前那个窗口，宁可不出字。")
        print("      常见原因：前台是全屏独占程序（游戏/投屏），或另一个提权窗口在顶层。")
        relay.stop()
        return 3
    print(f"[3/6] 中转窗口已就绪，且核实抢到了前台焦点（当前输入法 {ime_active_tip() or '读不到'}）")
    print(f"      音频通路：高通 {VOICE_HPF_HZ:.0f}Hz → "
          + ("AGC 目标峰值 %d（上限 %.1f×）" % (VOICE_TARGET_PEAK, VOICE_MAX_GAIN)
             if VOICE_AGC else "固定增益 %.1f×" % VOICE_CABLE_GAIN))

    writer = _CableWriter()
    if not writer.open():
        print("[x] 虚拟声卡打开失败")
        relay.stop()
        return 4
    chunk = CABLE_PERIOD_MS * CABLE_RATE // 1000 * 2      # 100ms 的单声道字节数

    if manual:
        print("[4/6] 手动模式：开始循环灌音（你不用说话，话在音频里）")
        print(f"      请在这 {manual_seconds:.0f} 秒内，到屏幕上那个小窗口里按一下你的语音快捷键")
        print("      （豆包设置里当前生效模式的快捷键；按一次开始，再按一次结束）")
        deadline = time.monotonic() + manual_seconds
        while time.monotonic() < deadline:
            sent = 0
            while sent < len(pcm) and time.monotonic() < deadline:
                writer.write(pcm[sent:sent + chunk])
                sent += chunk
                time.sleep(CABLE_PERIOD_MS / 1000.0)     # 按真实速度喂，循环续播
        print("[5/6] 手动窗口结束")
    else:
        print(f"[4/6] 按住 {spec} 开始听写，并按实时速度灌音（{seconds:.1f}s）…")
        ime_ensure("wechat")
        hotkey_down(spec)
        sent = 0
        try:
            while sent < len(pcm):
                writer.write(pcm[sent:sent + chunk])
                sent += chunk
                time.sleep(CABLE_PERIOD_MS / 1000.0)     # 按真实速度喂，别一口气灌完
            writer.wait_drained(1.0)
        except KeyboardInterrupt:
            print("\n[!] 被中断，直接收尾")
        print(f"[5/6] 灌音结束，松开 {spec} 停止听写")
        time.sleep(VOICE_HOLD_TAIL_S)
        hotkey_up(spec)

    text = collect_relay_text(relay)
    print(f"[6/6] 取到文字 {len(text)} 个：")
    print("      " + (text if text else "（空 —— 输入法没出字）"))

    if text and deliver in ("reply", "job"):
        print(f"      投递 {deliver}：{wb_deliver(text, deliver)}")
    writer.close()
    relay.stop()
    print("=" * 62)
    if not text:
        print("没出字时按顺序查：①VB-CABLE 装了吗、微信输入法的麦克风是否选了 CABLE Output")
        print("②当前生效输入法是不是微信输入法（不是的话识别结果只进剪贴板）")
        print("③中转窗口是否抢到了焦点（锁屏 / 全屏独占时抢不到）")
        print("④音频是不是太弱：设备录音电平低、底噪大时，输入法的 VAD 会整段丢掉")
    return 0 if text else 5


# ===================== 设备语音录入（阶段 3）=====================
# 设备每收到一次 poll 回一行 JSON，行里带若干 163 字节的 IMA ADPCM 帧：
#   {"voice":1,"session":N,"recording":bool,"sequence":N,"pending":N,"peak":N,
#    "dropped":N,"stop_reason":N,"clipped":N,"samples":N,"adpcm":"<base64>"}
# 帧格式：3 字节头（int16 predictor + int8 step_index，都是**编码前**的状态快照）
# + 160 字节 nibble（低半字节在前）。解码器与固件 main/wb_adpcm.c、参考实现
# wbb/adpcm.py 逐字节一致（已用 40 组随机帧交叉验证）。
#
# 这里负责：攒音频 → 停录后落 WAV → 给设备回执。文字那一步（虚拟声卡灌音 +
# 输入法听写 + 中转窗口取字）还没接，接口留在 _voice_deliver。

_ADPCM_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767,
)
_ADPCM_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

VOICE_STOP_REASONS = {
    0: "未停止", 1: "正常松开", 2: "主机停止轮询", 3: "达到120秒上限", 4: "链路已断开",
}


def _adpcm_decode(frame: bytes) -> bytes:
    """解一帧（含 3 字节头）。每帧都从帧头重建解码器状态，丢帧不污染后续音频。"""
    if len(frame) < 3:
        return b""
    predictor = int.from_bytes(frame[0:2], "little", signed=True)
    index = frame[2] if frame[2] < 128 else frame[2] - 256
    index = max(0, min(88, index))
    out = bytearray()
    for byte in frame[3:]:
        for nib in (byte & 0x0F, byte >> 4):
            step = _ADPCM_STEP_TABLE[index]
            diff = step >> 3
            if nib & 1:
                diff += step >> 2
            if nib & 2:
                diff += step >> 1
            if nib & 4:
                diff += step
            predictor += -diff if nib & 8 else diff
            predictor = max(-32768, min(32767, predictor))
            index = max(0, min(88, index + _ADPCM_INDEX_TABLE[nib]))
            out += predictor.to_bytes(2, "little", signed=True)
    return bytes(out)


class VoiceSession:
    """一次录音会话的收流状态与统计。"""

    def __init__(self, session_id: int, target: str) -> None:
        self.session_id = session_id
        self.target = target          # reply / job / manual，来自设备的 msg_target 帧
        self.pcm = bytearray()
        self.frames = 0
        self.samples = 0
        self.peak = 0
        self.dropped = 0
        self.clipped = 0
        self.stop_reason = 0
        self.seq_expect: int | None = None
        self.seq_gap = 0
        self.started = time.time()
        self.first_line_at = 0.0
        self.last = self.started

    def add(self, obj: dict) -> None:
        """把一帧数据解码后累加进 self.pcm。**不返回数据**：调用方要用增量的话，
        自己记 len(self.pcm) 的前后差（别指望返回值，这里曾经因此把整条音频链路写哑）。"""
        now = time.time()
        self.last = now
        if not self.first_line_at:
            self.first_line_at = now

        raw = obj.get("adpcm") or ""
        blobs = base64.b64decode(raw) if raw else b""
        count = len(blobs) // VOICE_FRAME_BYTES

        # sequence 是"本批首帧的序号"：上一批首帧 + 上一批帧数 应等于本批首帧，
        # 不等就是链路上丢了批（设备不会多发）。
        seq = _as_int(obj.get("sequence"))
        if self.seq_expect is None:
            self.seq_expect = seq
        if seq > self.seq_expect:
            self.seq_gap += seq - self.seq_expect
        self.seq_expect = seq + count

        for i in range(count):
            self.pcm += _adpcm_decode(blobs[i * VOICE_FRAME_BYTES:(i + 1) * VOICE_FRAME_BYTES])
        self.frames += count
        self.peak = max(self.peak, _as_int(obj.get("peak")))
        self.dropped = max(self.dropped, _as_int(obj.get("dropped")))
        self.clipped = max(self.clipped, _as_int(obj.get("clipped")))
        self.samples = max(self.samples, _as_int(obj.get("samples")))
        self.stop_reason = _as_int(obj.get("stop_reason"))

    @property
    def seconds(self) -> float:
        return len(self.pcm) / 2 / VOICE_SAMPLE_RATE

    @property
    def wall_seconds(self) -> float:
        base = self.first_line_at or self.started
        return max(1e-6, self.last - base)

    @property
    def lost(self) -> int:
        return self.seq_gap + self.dropped


def _ble_alive(ble) -> bool:
    """链路是否还活着。

    bleak 的 is_connected 在连接被关掉后返回 False，属性本身也可能抛异常，
    统一收口，别让"判断动作"自己变成异常源。
    """
    try:
        return bool(ble is not None and ble.is_connected)
    except Exception:
        return False


def _as_int(value, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


class UnifiedDaemon:
    """状态推流 + 周期积分 + 审批遥控，共享单条 BLE 连接，状态回写 AppState。"""

    def __init__(self, st: AppState, address: str, port: int = IPC_PORT,
                 status_interval: float = STATUS_INTERVAL,
                 credit_interval: float = CREDIT_INTERVAL) -> None:
        self._st = st
        self._address = address or ""
        self._port = port
        self._status_interval = status_interval
        self._credit_interval = credit_interval
        # 状态推流日志降噪：只在状态串变化时打印，避免每 2s 刷屏。
        self._last_states: str | None = None
        # 最近一次"压到极限仍超长"的状态帧尺寸：尺寸不变时不再重复打日志。
        self._last_oversize = 0

        self._ble = None
        self._write_lock = asyncio.Lock()
        # B-02：写超时**连续**计数。任何一次写成功都清零——只有"连续的"
        # 超时才说明链路真的死了；中间夹一次成功就说明只是抖动。
        self._write_timeouts: int = 0
        # 最近一次判定僵死后设置的静默截止时刻（monotonic）。在此之前
        # _ensure_ble 直接返回 False，避免刚断开就抢着重连又撞上拆链过程。
        self._reconnect_not_before: float = 0.0
        # 连续连接失败次数。达到 CONN_FAILS_BEFORE_RESCAN 就丢掉旧地址退回扫描
        # ——设备复位/重配后 BLE 随机地址会变，死抱着旧地址永远连不上。
        self._conn_fails: int = 0
        # 语音上行按行拼装：语音行带音频、会长过单次 notify 载荷（MTU 512
        # → 509B），逐 notify 解析会在语音一开始就整帧失败。
        self._rx_buf = bytearray()
        self._voice_sess: VoiceSession | None = None
        self._voice_target = "reply"      # 设备菜单里选的目标，由 msg_target 帧更新
        self._voice_write_warned = False  # 轮询写失败只报一次，避免 4Hz 刷屏
        # 文字链路：虚拟声卡写入 + 中转窗口取字。设备/输入法不在时逐级降级，
        # 最差也就是回到"只存 WAV + 回执已存录音"的老行为。
        self._cable = _CableWriter()
        self._relay = _RelayWindow()
        self._asr_last_inject = 0.0
        self._asr_active = False     # 本轮是不是我们启动的听写（开始/结束必须配对）
        # 连接建立过程的互斥：看护循环与审批路径可能同时触发 _ensure_ble。
        self._conn_lock = asyncio.Lock()
        self._notify_events: dict[int, asyncio.Future] = {}
        self._next_id = 1
        self._start_time = time.time()
        self._server: asyncio.AbstractServer | None = None
        self._stop = asyncio.Event()
        # 正在阻塞等待按键的审批项（session|kind|tool），提醒事件不重复推送。
        self._active_approvals: set[str] = set()
        # 设备提问页状态：一次只显示一个提问，用 id 关联 ask_end 与设备回传。
        self._ask_shown_cid: str | None = None
        self._ask_shown_id = -1
        # 已下发到设备的提问上下文（toolCallId / 会话 / 完整选项）；设备作答时
        # 用它反查完整 label（设备只回序号，绝不能回截断文案）。
        self._ask_shown: dict | None = None
        # 保活重推：最近一次下发时刻（monotonic）与完整载荷；设备端提问页 90 秒
        # 无操作会自动退出且不发任何帧，到间隔就原样重发让提问页保持可见。
        self._ask_shown_at: float = 0.0
        self._ask_shown_payload: dict | None = None
        # 多题串行化队列：一次 AskUserQuestion 含 N 题时，设备一次只能显示一题，
        # 故把归一化后的 qlist 拆成队列逐题下发。元素结构见 _build_ask_queue。
        # 队列内已按"可作答优先"排好序（只读题在末尾），队首是当前显示的那一题。
        self._ask_queue: list[dict] = []
        # 队列所属的提问（cid）；空表示无排队。用于转录闭合/跳过时整体清空。
        self._ask_queue_cid: str | None = None
        # 多题累积答案：cid -> {qid: [label, ...]}。中间题只累积，队列排空时
        # 一次性投递（resolveInterruption 发一次即视为整个中断已解决）。
        self._ask_accum: dict[str, dict] = {}
        # 用户已在设备上长按跳过的提问：电脑端回答前不再重复推送。
        self._ask_skipped: set[str] = set(skip_list_load())
        # 事件提醒去重：key -> monotonic 过期时刻。
        self._event_dedup: dict[str, float] = {}
        # 音量只在 PC 侧持久化，设备一断电就回固件默认值：每次连接成功补推一次。
        # 用连接代次记"这条链路推过没有"，避免同一链路上反复重推。
        self._conn_gen: int = 0
        # 已推送过 cfg 的连接代次（等于 _conn_gen 表示本链路已推过）。
        self._cfg_pushed_gen: int = -1
        # 旧固件对 cfg 帧回 ERR_JSON：只提示一次，避免每次重连补推都刷日志。
        self._cfg_unsupported_logged: bool = False

        # ---- 本地 ACP（设备直接作答）----
        # 会话级订阅已于 2026-09-30 删除（writer 抢占问题，见 _process_questions
        # 内注释）；作答投递统一走端口哨兵 _acp_sentries（从未 session/load，
        # 不抢会话写者角色）。
        self._acp_questions: dict[str, dict] = {}
        # 已作答/已闭合的 toolCallId：重连后服务端会重放会话事件，
        # 避免旧提问再次弹到设备上。
        self._answered_cids: set[str] = set()
        # 设备作答的确认窗口：cid -> {"expect": 投递连接数, "fail": 未受理数}。
        # 全部连接都回 resolved=false 说明服务端已无该提问的活跃中断（多为
        # 转录里的残留提问），标记失效、停止重推，否则会每 2s 重推一次。
        self._ans_watch: dict[str, dict] = {}
        # 端口哨兵：每个发现的 worker 端口维持一条"无会话"轻连接，是设备
        # 作答的**唯一**投递通道——哨兵从未 session/load，不抢会话写者角色，
        # 模型续跑事件得以流向桌面 UI（2026-09-30 实测：会话挂载端口可能
        # "能 load 但不是活跃 runtime"，且 load 本身就会抢占 agent connection）。
        self._acp_sentries: dict[int, AcpConnection] = {}
        self._sentry_flight: set[int] = set()
        self._worker_ports: set[int] = set()
        self._nonworker_ports: set[int] = set()
        self._device_answer_enabled: bool = bool(
            console_cfg_load().get("device_answer", True))

        self._auth, self._auth_reason = _load_credit_auth()
        # 今日消耗：记上一次推成功的 today 值，只在变化时打日志 ——
        # 30s 一轮、数字常常整分钟不动，每次都打会把日志刷成噪音。
        self._last_usage_today: int | None = None
        # 积分错误降噪：记录上次打印的错误文本与时刻，重复错误合并计数。
        self._credit_err: str = ""
        self._credit_err_at: float = 0.0
        self._credit_err_count: int = 0
        self._st.set(credit_reason=self._auth_reason if not self._auth else "")
        if not self._auth:
            self._st.log(
                f"[daemon] 周期积分禁用：{self._auth_reason or '无可用凭据'}；"
                "如需启用请执行 python wb_credential.py login")

    def _log(self, msg: str) -> None:
        self._st.log(msg)

    def request_stop(self) -> None:
        self._stop.set()

    # ---- BLE ----

    def _handle_hello(self, obj: dict) -> None:
        """能力握手：设备连上后报固件版本 + caps 位图。

        为什么需要它：`status_list.count` 的语义要改（C2：从"已发条数"
        改成"会话总数"），但这是**同键改义**，老固件无法靠"忽略未知键"
        兼容——它只会把 5 显示成 3。所以靠 caps bit3 区分：
        设备声明支持才发新语义，否则退回旧语义。

        老固件根本不发 hello，caps 缺省 None。这里把它当成 caps=0，
        即"只按最保守的兼容档下发"，与握手前行为一致。
        """
        fw = obj.get("fw")
        proto = _as_int(obj.get("proto"))
        # caps 缺失/非数字时按 0 处理（最保守）：宁可少下发，也不要发设备不认的帧。
        caps = _as_int(obj.get("caps"))
        caps = max(0, caps)
        self._st.set(device_fw=(fw if isinstance(fw, str) else ""),
                     device_proto=proto,
                     device_caps=caps)
        # hello 紧跟 BLE 连接建立，用它当「已连接」起点（与 Web 顶栏 uptime 同义）。
        self._ble_since = time.time()
        names = "、".join(lbl for bit, lbl in CAP_LABELS.items() if caps & bit) or "无"
        self._log(f"[daemon] 设备握手：fw={fw or '?'} proto={proto} "
                  f"caps=0x{caps:x}（{names}）")
        # 设备不支持语音上行却仍在轮询，纯属给链路添堵。
        if not caps & CAP_VOICE_UP:
            self._log("[daemon] 设备未声明语音上行能力（caps bit5），"
                      "语音轮询降为空闲频率，避免无效写占用链路")

    def _handle_ack_text(self, text: str) -> None:
        """处理设备回传的裸文本 ACK（非 JSON）。

        只有两类值得留下痕迹：
          OK_CFG:<vol> —— 设备确认音量配置，回显夹取后的实际生效值；
          ERR_JSON     —— 设备不认识该帧，多半是旧固件不支持 cfg。
        其余（OK_ASK / OK_STATUS / OK_APPROVAL …）是既有流程的常规应答，
        逐条打印只会淹没日志，一律静默。
        """
        if text.startswith("OK_CFG:"):
            try:
                vol = int(text.split(":", 1)[1])
            except ValueError:
                self._log(f"[daemon] 设备音量回执无法解析: {text}")
                return
            self._log(f"[daemon] 设备已确认提示音音量 {vol}")
            return
        if text == "ERR_JSON":
            if not self._cfg_unsupported_logged:
                self._cfg_unsupported_logged = True
                self._log("[daemon] 设备回 ERR_JSON：固件不认识该帧（常见于旧固件不支持提示音音量配置），"
                          "烧录新版固件后重连即可生效")
            return

    def _on_notify(self, _sender: int, data: bytearray) -> None:
        # 固件侧上行帧一律以 '\n' 收尾（这轮已统一补），但语音帧带音频、
        # 会长过单次 notify 的载荷上限，必须按行拼装后逐行解析。
        self._rx_buf.extend(data)
        while True:
            cut = self._rx_buf.find(b"\n")
            if cut < 0:
                break
            raw = bytes(self._rx_buf[:cut])
            del self._rx_buf[:cut + 1]
            if raw.strip():
                self._dispatch_line(raw)
        if len(self._rx_buf) > MAX_LINE_BYTES:
            self._rx_buf.clear()
        elif self._rx_buf:
            # 兼容没有换行的旧固件：攒不出换行但缓冲本身已是合法 JSON，就当一帧。
            # （语音长行的前几片必然是半截 JSON，走不到这里。）
            text = self._rx_buf.decode("utf-8", errors="replace").strip()
            try:
                obj = json.loads(text)
            except (ValueError, UnicodeDecodeError):
                return
            self._rx_buf.clear()
            if isinstance(obj, dict):
                self._dispatch_obj(obj)

    def _dispatch_line(self, raw: bytes) -> None:
        """一行完整上行：JSON 帧走 JSON，裸文本 ACK 走 _handle_ack_text。

        设备应答有两种形态：JSON 帧（key / ask_answer / battery / voice）与裸
        文本 ACK（OK_ASK / OK_CFG:<vol> / ERR_JSON）。后者 json.loads 必然失败，
        早期版本直接 return —— 于是 ACK 被整段丢弃（OK_CFG 的回显就收不到了）。
        """
        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            return
        try:
            obj = json.loads(text)
        except (ValueError, UnicodeDecodeError):
            self._handle_ack_text(text)
            return
        if isinstance(obj, dict):
            self._dispatch_obj(obj)

    def _dispatch_obj(self, obj: dict) -> None:
        # 语音行没有 "type" 键（字段与参考实现逐字对齐），先单独认。
        if obj.get("voice") == 1:
            self._voice_on_line(obj)
            return
        t = obj.get("type")
        if t == "hello":
            self._handle_hello(obj)
            return
        if t == "msg_target":
            tgt = obj.get("target")
            if tgt in ("reply", "job", "manual"):
                self._voice_target = tgt
                self._log(f"[voice] 设备选定目标 {tgt}")
            return
        if t == "key":
            aid = obj.get("approval_id")
            # 设备长按 DOWN 跳过提问：设备已自行回状态页，这里只需抑制重复推送。
            if obj.get("key") == "skip":
                if isinstance(aid, int) and aid == self._ask_shown_id:
                    if self._ask_shown_cid is not None:
                        self._ask_skipped.add(self._ask_shown_cid)
                        skip_list_add(self._ask_shown_cid)
                    # 多题序列整队作废：跳过一题不该让后面的题继续冒出来。
                    self._ask_queue.clear()
                    self._ask_queue_cid = None
                    self._ask_accum.clear()
                    self._clear_ask_shown()
                    self._log(f"[daemon] 用户在设备上跳过提问 #{aid}（已持久化），不再重复推送")
                return
            fut = self._notify_events.get(aid) if isinstance(aid, int) else None
            if fut is not None and not fut.done():
                fut.set_result(obj.get("key"))
        elif t == "ask_answer":
            self._handle_ask_answer(obj)
        elif t == "ack":
            pass
        elif t == "battery":
            soc = obj.get("soc")
            if isinstance(soc, int):
                self._st.set(battery=soc)

    # ---- 语音录入（阶段 3）----

    def _note_write_ok(self) -> None:
        """一次成功写入：清零超时连击，解除静默期。

        必须在**每一次**写成功后调用，否则上一轮录音残留的计数会让
        后面一次偶发超时就凑满 3 次，等于退回"单次即重置"的老行为。
        """
        if self._write_timeouts:
            self._write_timeouts = 0
        self._voice_write_warned = False

    async def _note_write_timeout(self, what: str) -> None:
        """一次写超时：累计连击，达到阈值才判定僵死并断开。

        B-02：单次超时只记录不动作。BLE 的写超时绝大多数是瞬时拥塞，
        立刻 disconnect 会把 100ms 的卡顿放大成数秒断链；而断链本身
        又会让后续写更容易超时，是老版本"越用越断"的根因。
        """
        self._write_timeouts += 1
        n = self._write_timeouts
        if n < WRITE_TIMEOUT_STRIKES:
            self._log(f"{what} 写超时（连续 {n}/{WRITE_TIMEOUT_STRIKES} 次），"
                      f"先不断开，继续观察")
            return
        self._log(f"{what} 写超时连续 {n} 次（阈值 {WRITE_TIMEOUT_STRIKES}），"
                  f"判定链路僵死，主动断开重连")
        await self._force_reset_ble()

    async def _write_raw(self, data: bytes) -> bool:
        """原样写一段字节（语音轮询用：它不是 JSON 对象）。

        刻意不在这里触发连接：连接归 _device_watch_loop 管，否则设备不在时
        4Hz 的轮询循环会把"连接失败"刷满日志。
        """
        ble = self._ble
        if ble is None or not ble.is_connected:
            return False
        try:
            async with self._write_lock:
                await asyncio.wait_for(
                    ble.write_gatt_char(CHAR_UUID, data, response=True),
                    timeout=WRITE_TIMEOUT)
            self._note_write_ok()
            return True
        except asyncio.TimeoutError:
            await self._note_write_timeout("[voice] 轮询")
            return False
        except Exception as exc:
            # 轮询循环频率高：失败要报，但只报一次，别把日志刷满。
            if not self._voice_write_warned:
                self._voice_write_warned = True
                self._log(f"[voice] 轮询写失败: {exc}")
            return False

    async def _voice_loop(self) -> None:
        """持续轮询设备语音状态。

        设备按下录音键后只有 2 秒窗口，窗口内必须被主机 poll 到这次按键才生效
        （参考实现的硬约束），所以未录音时也要周期问；录音中 0.10s，
        跟设备 100ms 一批的出帧节奏对齐。

        空闲频率是 1Hz（原 0.25s / B-02）：2 秒窗口对 1Hz 仍有 1 次余量，
        而 4Hz 的空闲轮询等于每秒 4 次带响应写，长期占用 NimBLE mbuf，
        与 2s 一次的状态帧叠加后会让链路周期性僵死。

        2026-10-31：设备固件已下线语音上行（不声明 caps bit5）。这里不再
        发轮询帧 —— 写一次 poll 设备也不会回任何 voice 行，纯占写带宽。
        hello 帧到来前/后各睡 5s 兜底；若未来固件重新声明 bit5，本循环
        自动恢复原节奏，无需改代码。
        """
        while True:
            if self._stop.is_set():
                return
            # 设备明确声明不支持语音上行（caps bit5 = 0）时，轮询毫无意义：
            # 它不会回任何 voice 行，只是白占写带宽。完全跳过写入。
            caps = self._st.caps()
            no_voice = (caps is not None and not (caps & CAP_VOICE_UP))
            t0 = time.monotonic()
            if not no_voice and not await self._write_raw(VOICE_POLL_FRAME):
                await asyncio.sleep(0.5)
                continue
            if self._voice_sess is not None:
                interval = VOICE_ACTIVE_INTERVAL
            else:
                interval = 5.0 if no_voice else VOICE_IDLE_INTERVAL
            delay = interval - (time.monotonic() - t0)
            if delay > 0:
                await asyncio.sleep(delay)

    def _voice_on_line(self, obj: dict) -> None:
        """收一行语音数据。

        recording 的下降沿是唯一可靠的"收完"信号：设备停录后若环形缓冲里还有
        帧没发完，仍会报 recording=true，直到排空才转为 false。
        """
        sid = _as_int(obj.get("session"))
        rec = bool(obj.get("recording"))
        sess = self._voice_sess
        if sess is not None and sess.session_id != sid:
            # 上一轮没等到下降沿（掉线/漏帧）就换了会话：先把上一轮收掉。
            self._voice_finish(sess)
            sess = None
        if rec and sess is None:
            sess = VoiceSession(sid, self._voice_target)
            self._voice_sess = sess
            self._log(f"[voice] 开始录音 session={sid} target={self._voice_target}")
            # 起听写要抢焦点 + 敲热键，都是阻塞动作：丢线程，别卡住 BLE 回调。
            self._spawn(self._asr_start)
        if sess is None:
            return
        # add() 只往 sess.pcm 累加、不返回数据（签名就是 -> None），所以这里取增量。
        # 之前写成 `pcm = sess.add(obj)` 再 `if pcm:` —— 恒为 None，声卡一次都没写过。
        before = len(sess.pcm)
        sess.add(obj)
        if len(sess.pcm) > before:
            # 边收边写：输入法是实时听麦克风的流，不是读文件。
            self._cable.write(bytes(sess.pcm[before:]))
        if not rec and sess.frames:
            # 先等音频放完再敲"结束"，否则尾巴会被剪掉。
            self._spawn(self._asr_stop)
            self._voice_finish(sess)

    def _voice_finish(self, sess: VoiceSession) -> None:
        self._voice_sess = None
        path = ""
        if sess.pcm:
            name = time.strftime("voice-%Y%m%d-%H%M%S") + f"-{sess.session_id}.wav"
            path = os.path.join(VOICE_DIR, name)
            try:
                os.makedirs(VOICE_DIR, exist_ok=True)
                with wave.open(path, "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(VOICE_SAMPLE_RATE)
                    w.writeframes(bytes(sess.pcm))
            except (OSError, wave.Error) as exc:
                self._log(f"[voice] 落盘失败: {exc}")
                path = ""
        reason = VOICE_STOP_REASONS.get(sess.stop_reason, sess.stop_reason)
        self._log(f"[voice] 录音结束 target={sess.target} {sess.seconds:.1f}s "
                  f"帧={sess.frames} 丢帧={sess.lost} 削顶={sess.clipped} "
                  f"实时率={sess.seconds / sess.wall_seconds:.3f} 停止={reason}"
                  + (f" -> {path}" if path else "（无音频）"))
        # 录音已经结束，这时候才起"松手兜底"计时：正常路径几百毫秒内就松了，
        # 走到这里还没松说明是异常路径。
        if self._asr_active:
            self._asr_guard_start(VOICE_RELEASE_GUARD_S, "录音已结束仍未松手")
        # 交付＝听写取字 + REST 投递，可能好几秒：放后台任务，别卡住 BLE 回调。
        try:
            asyncio.get_running_loop().create_task(self._voice_deliver_task(sess, path))
        except RuntimeError:
            self._log("[voice] 不在事件循环里，交付流程未启动")

    # ---- 文字链路：虚拟声卡灌音 + 输入法听写 + 中转窗口取字 ----

    def _spawn(self, fn, *args) -> None:
        """把阻塞动作丢到守护线程。

        BLE 通知回调与事件循环都不能被按住：热键注入要按住 40ms、等音频放完
        要几百毫秒、等输入法出字要好几秒、REST 投递是同步 urllib。
        """
        def _run() -> None:
            try:
                fn(*args)
            except Exception as exc:
                self._log(f"[voice] 后台动作异常：{exc}")
        threading.Thread(target=_run, name="wb-asr", daemon=True).start()

    def _asr_hotkey(self) -> str:
        """输入法语音快捷键；允许用控制台配置 voice_hotkey 覆盖。

        默认 ctrl+win = 微信输入法「按住说话」：按下开始录音、松手结束。
        控制台那份配置优先级更高，改了配置就按配置走（比如临时换回免按模式）。
        """
        try:
            cfg = console_cfg_load() or {}
            spec = str(cfg.get("voice_hotkey") or "").strip()
        except Exception:
            spec = ""
        return spec or VOICE_HOTKEY_DEFAULT

    def _asr_link_ready(self) -> bool:
        """文字链路是否可以参与：总开关 + 虚拟声卡存在。

        虚拟声卡不存在就完全不参与（不抢焦点、不注入按键）—— 它同时是"前置
        装好了没"的判断：没装 VB-CABLE 时那次录音只是多抢一次焦点、多敲一次
        无用的组合键，收益为零还打扰人。
        """
        return bool(VOICE_ASR_ENABLED) and find_cable_device()[0] >= 0

    def _asr_start(self) -> None:
        """设备开始收音：中转窗口抢焦点 → 敲热键开始听写。

        顺序不能反：字是打到**焦点控件**里的，先敲热键而窗口还没在前台，
        这一轮的字就丢了。
        """
        if not self._asr_link_ready():
            self._log("[voice] 文字链路未就绪（缺虚拟声卡），本轮只存 WAV")
            return
        self._cable.retry()          # 每次开录都给音频设备一次重来的机会
        # 先把流拉起来（内部会持续写静音），这样输入法开麦时 CABLE Output 上
        # 已经是一条活流；等第一帧音频到了再开，会白白多一段"死流"。
        if not self._cable.open():
            self._log("[voice] 虚拟声卡打不开，本轮取不到字")
            return
        if not self._relay.start():
            self._log("[voice] 中转窗口不可用，本轮取不到字")
            return
        self._relay.clear()
        if not self._relay.focus():
            # 抢不到焦点就**不注入**：字会打进用户当前那个窗口，比取不到字更糟。
            self._log("[voice] 中转窗口抢焦点失败，本轮不启动听写（免得把字打到别的窗口）")
            self._relay.hide()   # focus 里已经 ShowWindow 过了，抢不到焦点也要收掉
            return
        # 微信语音的结果只在"微信输入法是当前生效输入法"时进输入框，否则进剪贴板。
        # 这一步失败不致命（可能本来就是它），失败时日志里能看出取不到字的原因。
        if not ime_ensure("wechat"):
            self._log("[voice] 没能确认/切换当前输入法为微信输入法（取字可能落空）")
        self._asr_active = self._asr_inject("按住开始说话", hold=True)
        if self._asr_active:
            # 录音期间只挂"硬上限"，**绝不能用固定短计时**：整段说话都要按着，
            # 早松一秒输入法就只听到半句话（这里踩过一次，7.7 秒的录音第 3 秒被松开）。
            self._asr_guard_start(VOICE_HOLD_MAX_S, "按住超过硬上限")

    def _asr_stop(self) -> None:
        """设备停录：等音频放完 → 敲热键结束听写。

        只在**开始注入成功过**的前提下才注入结束 —— 免按模式下按一次是"开始
        说话"，开始没轮到我们按、结束却按了，等于在用户当前窗口凭空开一次听写。
        """
        if not self._asr_active:
            return
        self._asr_active = False
        if not self._asr_link_ready():
            return
        if not self._cable.wait_drained(0.6):
            self._log("[voice] 音频缓冲未排空就收尾（可能有尾巴被剪）")
        time.sleep(VOICE_HOLD_TAIL_S)     # 松手前留一点，别把尾音剪掉
        self._asr_inject("松开结束说话", hold=False)

    def _asr_guard_start(self, delay_s: float, why: str) -> None:
        """保险丝：按下的是 Win 键，万一没人来松手，到点强制释放。

        delay_s 必须**大于**它要覆盖的那一段的正常时长 —— 这是兜底，不是收尾计时。
        正常收尾由 _asr_stop 在"缓冲排空 + 留尾"之后松手；这里只兜
        "录音中途掉线/异常导致没人来松手"。
        """
        def _fire() -> None:
            if self._asr_active:
                self._asr_active = False
                spec = self._asr_hotkey()
                if hotkey_up(spec):
                    self._log(f"[voice] 热键 {spec} 未正常松开，已强制释放（{why}）")
        timer = threading.Timer(delay_s, _fire)
        timer.daemon = True
        timer.start()

    def _asr_inject(self, why: str, hold: bool = False) -> bool:
        spec = self._asr_hotkey()
        now = time.monotonic()
        if now - self._asr_last_inject < MIN_INJECT_GAP:
            self._log(f"[voice] 注入太密，跳过（{why}）")
            return False
        self._asr_last_inject = now
        ok = hotkey_down(spec) if hold else hotkey_up(spec)
        if ok:
            self._log(f"[voice] 已{'按下' if hold else '松开'}快捷键 {spec}（{why}）")
            return True
        self._log(f"[voice] 快捷键注入失败：{spec}（{why}）")
        return False

    def _asr_collect(self) -> str:
        """等输入法把字打完（实现见模块级 collect_relay_text，自检模式共用）。"""
        return collect_relay_text(self._relay)

    async def _voice_deliver_task(self, sess: VoiceSession, path: str) -> None:
        """交付一轮录音：先取字、再按目标投递，最后把结果回给设备。"""
        if not path:
            await self._send_voice_receipt(False, "没有录到音频")
            return
        if sess.seconds < 0.3:
            await self._send_voice_receipt(False, "录音太短")
            return
        text, ok = await asyncio.to_thread(self._asr_deliver, sess, path)
        await self._send_voice_receipt(ok, text)

    def _asr_deliver(self, sess: VoiceSession, path: str) -> tuple[str, bool]:
        """取字 → 投递（在工作线程里跑）。

        目标语义（设备菜单里选的）：
          reply  -> ACP session/prompt 投进会话（不要求会话是活的；失败回退 REST reply）
          job    -> POST /api/v1/jobs（新建任务）
          manual -> 只把字留在中转窗口，不投递
        """
        try:
            return self._asr_deliver_impl(sess, path)
        finally:
            # 中转窗口在开录时就亮出来了（抢焦点接字），除 manual 档外用完即收 ——
            # 它抢过焦点、又占着屏幕底部，不该一直挂在桌面上。
            if sess.target != "manual":
                self._relay.hide()

    def _asr_deliver_impl(self, sess: VoiceSession, path: str) -> tuple[str, bool]:
        """取字与投递的实现；收窗口的收尾由 _asr_deliver 统一负责。"""
        if not self._asr_link_ready():
            self._log("[voice] 文字链路未就绪（缺虚拟声卡或总开关关了），本轮只存 WAV")
            return (f"已存录音 {sess.seconds:.0f}s", True)
        text = self._asr_collect()
        stats = self._cable.stats()
        self._log(f"[voice] 取字 {len(text)} 个：{text[:60] or '（空）'}"
                  f"｜声卡 open={stats['open']} 写入={stats['bytes_written']}B "
                  f"增益={stats['gain']} 丢块={stats['blocks_dropped']}")
        if not text:
            note = "" if stats["open"] else "，虚拟声卡未就绪"
            return (f"未听清（已存 {sess.seconds:.0f}s{note}）", False)
        if sess.target == "manual":
            self._log(f"[voice] 手动模式，未投递：{text[:60]}")
            return (f"已听写 {len(text)} 字", True)
        res = wb_deliver(text, sess.target)
        if res.get("ok"):
            # 端口/会话一定打出来：worker 是按会话临时起的，端口每次都不一样，
            # 排"投到哪去了"的问题全靠这两个字段。
            where = " ".join(str(x) for x in (
                ("端口 " + str(res.get("port"))) if res.get("port") else "",
                ("会话 " + str(res.get("sessionId"))[:8]) if res.get("sessionId") else "",
            ) if x)
            self._log(f"[voice] 已投递 {sess.target}：{text[:60]}"
                      f"（{res.get('detail') or '已投递'}{' ' + where if where else ''}）")
            return (text[:44], True)
        raw = res.get("detail")
        self._log(f"[voice] 投递失败 {sess.target}：{res.get('err') or res}"
                  + (f"｜原始：{str(raw)[:160]}" if raw else ""))
        return (f"投递失败：{res.get('err') or res}", False)

    async def _send_voice_receipt(self, ok: bool, text: str) -> None:
        await self._write({"type": "msg", "ok": 1 if ok else 0, "text": text[:48]})

    def _on_ble_disconnected(self, client) -> None:
        """bleak 的断链回调：设备断电 / 走远时立刻作废句柄。

        没有这个回调时，主机只能靠 is_connected 轮询 + 写超时三连才发现链路没了：
        Windows 上链路监督超时要几十秒才判死，之后再拿旧地址连 3 次
        （每次最多 CONNECT_TIMEOUT=15s）才肯清地址重新扫描，加起来一两分钟。
        表现就是"换一台设备开机，主机像没反应、不会自动连"。

        注意别把新链路误清：旧 client 迟到的回调要丢掉。
        """
        if self._ble is not None and client is not self._ble:
            return
        was_connected = self._ble is not None
        self._ble = None
        self._write_timeouts = 0
        self._st.set(connected=False, device_caps=None,
                     device_fw="", device_proto=0)
        if was_connected:
            self._log("[daemon] 链路已断开，立即重连/重扫")

    async def _ensure_ble(self) -> bool:
        if self._ble is not None and self._ble.is_connected:
            return True
        # B-02：僵死判定后的静默期。刚 disconnect 就立刻重连会撞在
        # 对方还没拆完链的窗口里，重连成功率低，且会再次写超时。
        if time.monotonic() < self._reconnect_not_before:
            return False
        async with self._conn_lock:
            if self._ble is not None:
                if self._ble.is_connected:
                    return True
                try:
                    await self._ble.disconnect()
                except Exception:
                    pass
                self._ble = None
            # 地址未知时扫描由 _device_watch_loop 负责，这里安静失败，
            # 避免状态循环每 2s 用空地址连接并刷错误日志。
            if not self._address:
                return False
            try:
                from bleak import BleakClient
                client = BleakClient(self._address, self._on_ble_disconnected,
                                     timeout=CONNECT_TIMEOUT)
                await client.connect()
                # start_notify 会顺带做服务发现；但 macOS 上 connect() 返回时
                # 特征表可能还没就绪，紧接着的第一次 write_gatt_char 会抛
                # "Service Discovery has not been performed yet"。
                # 显式等一次 get_services 再订阅，把这个竞态关掉。
                try:
                    await asyncio.wait_for(client.get_services(), timeout=CONNECT_TIMEOUT)
                except Exception:
                    pass
                await client.start_notify(CHAR_UUID, self._on_notify)
                self._ble = client
                self._st.set(connected=True, address=self._address)
                mark_known(self._address)  # 记录该地址已成功连接，之后不再视为首次
                self._log(f"[daemon] BLE 已连接 {self._address}")
                # 新链路从干净的计数起步：上一轮僵死留下的连击数不能带过来。
                self._write_timeouts = 0
                self._conn_fails = 0
                self._reconnect_not_before = 0.0
                # 音量只在 PC 侧持久化，设备一断电就回固件默认值：
                # 每次连接成功都必须补推一次。
                #
                # 用"连接代次"而不是布尔标志：_ensure_ble() 每 2s 就被状态循环
                # 调一次，只要某次判断成"未连接"就会重新置位布尔标志，
                # 于是一条链路上会连推 6 次 cfg（实测 17:39 刷了 6 行
                # 「设备已确认提示音音量 100」）。代次只在新连接真正建立时 +1，
                # 同一个代次内无论调多少次都只推一次。
                self._conn_gen += 1
                return True
            except Exception as exc:
                self._ble = None
                self._st.set(connected=False)
                self._log(f"[daemon] BLE 连接失败: {exc}")
                # 地址失效后必须退回扫描，否则永远连不上。
                #
                # macOS 的 CoreBluetooth 对未配对的设备使用**随机地址**
                # （每设备一换、重新配对就变；实测刷固件/复位后
                # 0E9EAF43… 变成了 D843E746…）。而 self._address 一旦被
                # 记住就再没被清过 —— 看护循环拿着旧地址每 15 秒重试一次，
                # 日志刷满 "Device with address … was not found"，
                # 却永远不会重新扫一次。设备明明在广播，主机却说找不到。
                #
                # 连续失败到这里才清地址：单次失败可能只是信号瞬断或
                # 设备还在重启，立刻丢掉地址反而会触发无谓的全量扫描。
                self._conn_fails += 1
                if self._conn_fails >= CONN_FAILS_BEFORE_RESCAN and self._address:
                    self._log(f"[daemon] 连续 {self._conn_fails} 次连不上 "
                              f"{self._address}，丢弃该地址并重新扫描"
                              f"（设备可能复位/重配，随机地址已变）")
                    self._address = ""
                    self._st.set(address="")
                    self._conn_fails = 0
                return False

    async def _write(self, payload: dict) -> bool:
        if not await self._ensure_ble():
            return False
        # 取本地引用：这里到真正写之间还有一个 await（拿写锁），期间看门狗或
        # 语音轮询的写超时会调 _force_reset_ble() 把 self._ble 置空。老写法在
        # 锁后继续用 self._ble，会炸成 NoneType 属性错误，盖掉真正的失败原因。
        ble = self._ble
        if ble is None:
            return False
        data = json.dumps(payload, ensure_ascii=False).encode()
        # 超长帧会被设备 ATT 层拒收，这里提前定位是哪类帧（否则只能看到
        # Invalid Attribute Value Length，不知起因）。
        if len(data) > BLE_FRAME_MAX:
            self._log(f"[daemon] 帧超长 {len(data)}B > {BLE_FRAME_MAX}B "
                      f"type={payload.get('type')}，会被设备拒收")
        try:
            async with self._write_lock:
                await asyncio.wait_for(
                    ble.write_gatt_char(CHAR_UUID, data, response=True),
                    timeout=WRITE_TIMEOUT)
            self._note_write_ok()
            return True
        except asyncio.TimeoutError:
            await self._note_write_timeout(f"[daemon] {payload.get('type') or 'frame'}")
            return False
        except Exception as exc:
            if not _ble_alive(ble):
                # 链路已在别处被重置，不是这一帧的问题，别当异常刷出来
                self._log("[daemon] 写帧放弃：链路已被重置")
                return False
            self._log(f"[daemon] 写帧失败: {exc}")
            return False

    async def _force_reset_ble(self) -> None:
        """写超时后主动断开并清空 BLE 句柄，避免后续复用假活的连接。

        先置 self._ble = None，即使 disconnect 本身也挂起（有 wait_for 兜底），
        也不影响下次 _ensure_ble 重新建立连接。

        同时设一个静默期（B-02）：BLE 的断开是异步的，刚 disconnect 就
        立刻重连往往撞在对方还没拆完链的窗口里，重连成功率低且会再次超时。
        等 RESET_RECONNECT_DELAY 再连，成功率明显更高。
        """
        ble = self._ble
        self._ble = None
        self._write_timeouts = 0
        self._reconnect_not_before = time.monotonic() + RESET_RECONNECT_DELAY
        # 能力位随链路一起作废：下次连上的可能是另一台设备（或同一台刷了
        # 新固件），沿用旧 caps 会按错误的能力档下发。等新的 hello 再填。
        self._st.set(connected=False, device_caps=None,
                     device_fw="", device_proto=0)
        if ble is not None:
            try:
                await asyncio.wait_for(ble.disconnect(), timeout=DISCONNECT_TIMEOUT)
            except Exception:
                pass

    # ---- 设备看护（IPC 与 BLE 解耦的核心） ----

    async def _device_watch_loop(self) -> None:
        """后台保证设备连接：无地址则周期扫描发现，有地址则保持/重连。

        IPC server 已先行启动，所以即使设备迟迟不开机，WorkBuddy 的审批 hook
        也能连上本 daemon（设备不在线时审批 fail-open）；设备一旦开机广播，
        下一轮扫描即可发现并自动连接，无需重启程序。
        """
        while not self._stop.is_set():
            # 新连上的设备要先补推提示音音量。放在暂停判定之前：已连接的设备
            # 即便用户暂停了自动连接，也应收到配置。未连接时不主动发起连接
            # （交给下面的常规分支按自己的节奏重连），避免 1s 一次的空连。
            #
            # 用连接代次判重：_push_beep_cfg 内部会走 _write → _ensure_ble，
            # 而 _ensure_ble 成功时会把代次 +1 —— 那会把"已推送"又变回"未推送"，
            # 于是无限重推。所以先记下推送前的代次，成功就标成"这条链路推过了"。
            if self._conn_gen > self._cfg_pushed_gen:
                ble = self._ble
                if ble is not None and ble.is_connected:
                    gen = self._conn_gen
                    if await self._push_beep_cfg():
                        self._cfg_pushed_gen = max(self._cfg_pushed_gen, gen)
            if self._st.search_paused:
                # 暂停自动搜索/自动连接：只轮询等待恢复，不做任何连接动作。
                # 已建立的 BLE 连接保持，断线后不自动重连；扫描/重扫一并停。
                await asyncio.sleep(1.0)
                continue
            try:
                if self._address:
                    ble = self._ble
                    if ble is None or not ble.is_connected:
                        await self._ensure_ble()
                    await asyncio.sleep(WATCH_INTERVAL)
                else:
                    self._log("[daemon] 未配置设备地址，扫描设备…")
                    # 标记扫描中：前端据此显示「正在搜索设备…」+ 倒计时，
                    # 避免用户面对 20s 扫描 + 25s 重扫的空窗（约 45s）干等。
                    self._st.begin_scan()
                    try:
                        addr = await find_device()
                    except RuntimeError:
                        addr = ""
                    finally:
                        self._st.end_scan()
                    if addr:
                        if self._st.search_paused:
                            # 扫描进行中用户点了暂停：本次已发现的设备不连接，
                            # 交回循环顶部的暂停分支，等用户恢复后再连。
                            self._log(f"[daemon] 已暂停自动连接，本次发现的设备暂不连接: {addr}")
                        else:
                            self._address = addr
                            self._st.set(address=addr)
                            await self._ensure_ble()
                    else:
                        # 重扫等待：扫描未命中，前端显示「未找到设备，N 秒后重试」。
                        self._st.begin_rescan(RESCAN_INTERVAL)
                        try:
                            await asyncio.sleep(RESCAN_INTERVAL)
                        finally:
                            self._st.end_rescan()
            except Exception as exc:
                self._log(f"[daemon] 设备看护出错: {exc}")
                await asyncio.sleep(RESCAN_INTERVAL)

    async def _push_beep_cfg(self) -> bool:
        """把提示音音量下发给设备。

        音量真源在 PC 的 wb-console.json（设备不落 NVS——合并镜像烧录会清 NVS，
        "设备自持"不可靠），所以每次连接建立后都要补推，否则设备重启即回默认。
        """
        vol = beep_vol_get(console_cfg_load())
        return await self._write({"type": "cfg", "beep_vol": vol})

    # ---- 状态推流 ----

    async def _push_status(self) -> None:
        # hide_cids：本进程内已确认作答的提问——转录可能长期不落 result
        # （resolveInterruption 路径的已知服务端行为），不剔除会让会话
        # 一直显示 NEED_ANSWER，造成"作答未送达"的误判。
        # total_semantics：只有设备在 hello 里声明 caps bit3 时才改语义。
        # 同键改义无法靠"忽略未知键"兼容（老固件会把 5 显示成 3），必须协商。
        caps = self._st.caps()
        st = get_status_list(hide_cids=self._answered_cids,
                             total_semantics=bool(caps and caps & CAP_COUNT_TOTAL))
        tasks = st.get("tasks", [])
        await self._process_questions(tasks)
        # sid/cwd/questions 只在本机用于 ACP 订阅与提醒，不上设备：
        # 它们会让每 2s 一次的状态帧超过设备 512B 上限而被整帧拒收。
        for t in tasks:
            t.pop("questions", None)
            t.pop("sid", None)
            t.pop("cwd", None)
        # ts 必须在自检之前写入：它是实发帧的一部分。早期版本 ts 写在自检之后，
        # 缩水闸门量的帧比实发帧少 11~13B，470~480B 的"合规"帧实发 481~493B
        # 越线，日志每 2s 刷一条「帧超长」而缩水逻辑形同虚设（2026-09-30 修复）。
        st["ts"] = int(time.time() - self._start_time)
        # 待机页大号时间的数据源。设备没有 RTC 电池、纯 BLE 也拿不到 NTP，
        # 只能用主机墙钟：time.timezone 是"本地时间相对 UTC 的秒差（西为正）"，
        # epoch - timezone 就是"把 UTC 秒数平移到本地墙钟"的秒数，设备拿到后
        # 用 %86400 / 日历换算即可，不需要知道时区规则。
        # 单独用 "t" 而不复写 ts：ts 是主机 uptime，别的工具可能在看；
        # 老固件对未知键直接忽略，因此这条是向后兼容的新增字段（约 15B）。
        st["t"] = int(time.time() - time.timezone)
        oversize = self._shrink_status(st)
        if oversize > BLE_FRAME_MAX:
            # 压到极限仍超长：整帧跳过，避免每 2s 一次注定被设备拒收的写。
            # 尺寸不变时只记一条，防止刷爆 LOG_MAX 环形日志。
            if oversize != self._last_oversize:
                self._log(f"[daemon] 状态帧压到极限仍超长 {oversize}B，本帧跳过"
                          f"（{len(tasks)} 条会话）")
                self._last_oversize = oversize
            return
        self._last_oversize = 0
        if await self._write(st):
            # 会话→worker 端口映射随会话级 ACP 挂载一并移除（2026-09-30），
            # 无法再精确到"某会话跑在哪个端口"；改为展示当前发现的 worker
            # 端口总数（前端显示 "worker ×N"），用于排查 worker 是否异常增减。
            n_workers = len(self._worker_ports)
            for t in tasks:
                t["worker"] = n_workers
            self._st.set(sessions=tasks, sess_total=st["count"])
            states = "/".join(t["state"] for t in tasks) or "none"
            # 降噪：状态串不变时不打印，只在变化时留一条（RUNNING/NEED_ANSWER
            # 出现或切换、会话数增减等关键节点仍会记录）。
            if states != self._last_states:
                # 两种 count 语义下数字含义不同，日志要能区分，否则排查时
                # 会把"设备上显示 N 条"和"总共 N 个会话"混作一谈。
                if st["count"] != len(tasks):
                    self._log(f"[daemon] 会话共 {st['count']} 个（本次下发 "
                              f"{len(tasks)} 条）: {states}")
                else:
                    self._log(f"[daemon] 会话 {st['count']} 条: {states}")
                self._last_states = states

    @staticmethod
    def _shrink_status(payload: dict) -> int:
        """把状态帧压进设备接收缓冲，返回压缩后的实发字节数。

        自检对象必须是**整帧**（含 ts）：只量 {"type","count","tasks"} 会漏算
        ", \"ts\": N" 的 11~13B，闸门放行后实发越线（见 _push_status 注释）。
        返回值 > BLE_FRAME_MAX 表示压到极限仍超长，调用方应跳过发送。

        ---- 降级顺序（2026-10-31 重排）----
        设备第 2 屏是固定三行版式：
            行1 序号 + 状态 + 额度
            行2 任务名
            行3 msg（橙色，独立成行，空也占位）
        msg 是整张卡里唯一"需要用户动手"的信息（等审批 / 等回答），
        比任务名更该优先保住。原实现第一步就把 msg 全清空、之后才降级 name，
        等于先砍最贵的、后砍最便宜的，顺序是倒的。

        现在改成：先一起收窄（三张卡同步，避免卡与卡之间不一致），
        msg 留到倒数第二步，name 最后再牺牲。
        """
        tasks = payload.get("tasks") or []

        def size() -> int:
            return len(json.dumps(payload, ensure_ascii=False).encode())

        def trim(text: str, limit: int) -> str:
            # 按 UTF-8 字节边界截断：中文 3B/字，按字符截会超出约 3 倍。
            return (text or "").encode()[:limit].decode("utf-8", "ignore")

        def trim_all(limit: int) -> None:
            for t in tasks:
                t["name"] = trim(t.get("name"), limit)
                t["model"] = trim(t.get("model"), limit)
                t["msg"] = trim(t.get("msg"), limit)

        n = size()
        if n <= BLE_FRAME_MAX:
            return n

        # 51B = 17 汉字，正好铺满 214px 的卡片宽度；更短的名字不必动。
        # 这一档只用一次，目的是把"超出屏幕、本来就会画省略号"的部分先削掉。
        trim_all(51)
        n = size()
        if n <= BLE_FRAME_MAX:
            return n

        # 逐档收窄，三张卡同步降，保持版式一致。
        for limit in (30, 21, 15):
            trim_all(limit)
            n = size()
            if n <= BLE_FRAME_MAX:
                return n

        # 到这里 msg 已经只剩 15B（5 个汉字），信息量所剩无几，才轮到它让位。
        for t in tasks:
            t["msg"] = ""
        n = size()
        if n <= BLE_FRAME_MAX:
            return n

        # 最后才动任务名。状态和额度永远不砍 —— 那两项决定"这条会话要不要管"。
        for t in tasks:
            t["name"] = trim(t.get("name"), 9)
        return size()

    # ---- 会话级 ACP 订阅已于 2026-09-30 删除（writer 抢占问题，见
    # _process_questions 内注释）；作答投递统一走端口哨兵。 ----

    def _ensure_sentry_bg(self, port: int) -> None:
        """后台建立/重建端口哨兵连接（只 connect + initialize + 订阅）。"""
        if port in self._sentry_flight:
            return
        self._sentry_flight.add(port)

        async def run() -> None:
            try:
                # 哨兵复用 _on_acp_event：设备作答 resolveInterruption 的响应
                # 以 _acp_post 回来，必须走 _note_ans_result 销账，否则应答
                # 会一直挂账、会话持续显示 NEED_ANSWER。哨兵 sid 为空，_mount
                # 里不会 session/load（不抢写者），故不会污染会话状态。
                conn = AcpConnection(port, "", "", self._on_acp_event,
                                     asyncio.get_running_loop())
                try:
                    ok = await asyncio.wait_for(
                        asyncio.to_thread(conn.start), timeout=22.0)
                except Exception:
                    ok = False
                if ok:
                    self._acp_sentries[port] = conn
                    self._log(f"[daemon] 端口哨兵已建立 :{port}")
                else:
                    conn.stop()
                    self._log(f"[daemon] 端口哨兵建立失败 :{port}（下轮重试）")
            finally:
                self._sentry_flight.discard(port)

        asyncio.create_task(run())

    async def _process_questions(self, tasks: list[dict]) -> None:
        """把「等待用户回答的问题」推给设备的专用提问页（ask / ask_end）。

        提问信息合并两个来源：转录 pending + ACP 扩展请求（后者带 answerable
        判定与 rpc_id）。满足「单问题+单选+1~6 项」时 ask 帧置 ans=1，设备可
        OK 直接作答；否则页面维持只展示。一次只显示一个提问。
        设备离线时直接跳过，不影响其它通道。
        """
        current: dict[str, dict] = {}
        for t in tasks:
            sid = str(t.get("sid") or "")
            for q in (t.get("questions") or []):
                cid = str(q.get("cid") or "")
                if cid:
                    current[cid] = dict(q)   # 拷贝，补字段不污染共享状态
                    current[cid]["sid"] = sid

        # 不再按会话挂载 ACP 订阅（2026-09-30 15:30 复盘删除）。两个实测依据：
        # ① _codebuddy.ai/question 从不推给侧挂连接——全天日志零命中，连发起
        #    prompt 的连接也收不到，提问检测实际全部依赖转录扫描；
        # ② session/load 会把该会话的 agent connection（写者角色）抢到本
        #    daemon：设备作答后模型的续跑事件流入本 daemon 的盲订阅，桌面 UI
        #    收不到，用户看到的就是"作答没返回 WorkBuddy、提问卡片一直等待"
        #    （worker 日志证据：answer consumed → ToolManager OK → 模型恢复，
        #    但转录无 result、UI 无续跑）。作答投递由端口哨兵全端口覆盖
        #    （_answer_targets），哨兵从未 load 会话、不抢写者。
        # 合并 ACP 通道的 answerable 等完整信息
        for cid, pend in self._acp_questions.items():
            if cid in current:
                current[cid]["answerable"] = pend["answerable"]
            else:
                # 转录 pending 缺失（尾扫窗口/时序差）：ACP 信息也能驱动提问页
                current[cid] = {**pend, "answerable": pend["answerable"]}
        # 转录已闭合的提问：移出待答表。
        closed_acp = [c for c in self._acp_questions if c not in current]
        for c in closed_acp:
            self._acp_questions.pop(c, None)
            self._mark_answered(c)
        # 电脑端已作答（转录闭合）的提问：清掉其跳过标记，避免长期累积。
        self._ask_skipped &= set(current)
        # 确认窗口清理：已不在待答列表（转录闭合）的不必再等响应。
        for c in list(self._ans_watch):
            if c not in current:
                self._ans_watch.pop(c, None)

        if self._ble is None or not self._ble.is_connected:
            return

        # 1) 显示中的提问已被回答（转录里 function_call 闭合）→ 收回提问页。
        if self._ask_shown_cid is not None and self._ask_shown_cid not in current:
            closed_id = self._ask_shown_id
            self._clear_ask_shown()
            await self._write({"type": "ask_end", "id": closed_id})
            self._log(f"[daemon] 提问已作答，下发 ask_end #{closed_id}")
        # 1b) 排队的多题序列所属提问已闭合/被跳过 → 整队丢弃，设备上残留的
        #     那一页也要收回，否则会一直停在旧问题上（转录闭合不会逐题通知）。
        if (self._ask_queue and
                self._ask_queue_cid not in (None,) + tuple(current)):
            gone_cid = self._ask_queue_cid
            self._ask_queue.clear()
            self._ask_queue_cid = None
            self._ask_accum.pop(gone_cid, None)
            if self._ask_shown_cid is not None:
                stale_id = self._ask_shown_id
                self._clear_ask_shown()
                await self._write({"type": "ask_end", "id": stale_id})
                self._log(f"[daemon] 多题队列随提问闭合而丢弃，下发 ask_end #{stale_id}")

        # 2a) 显示中：设备端 90 秒无操作会自动退出提问页且不通知 PC（固件
        #     fail-open），按间隔原样重发一次，让提问页在设备上保持可见；
        #     顺带覆盖设备掉线重连后的恢复。重发会重置设备端光标与计时器。
        if self._ask_shown_cid is not None:
            now = time.monotonic()
            if (self._ask_shown_payload is not None
                    and now - self._ask_shown_at >= ASK_KEEPALIVE_SEC):
                ok = await self._write(self._ask_shown_payload)
                self._ask_shown_at = now
                self._log(f"[daemon] 提问页 #{self._ask_shown_id} 保活重推"
                          f"（{'成功' if ok else '写帧失败'}）")
            return
        # 2b) 设备空闲 → 先消耗已排队的多题序列（多题串行化）。
        if self._ask_queue:
            await self._push_ask_item(self._ask_queue[0], current,
                                      queued=True)
            return
        # 2c) 设备空闲且有新的待答问题 → 建队列并推第一题。
        for cid, q in current.items():
            # 已跳过（长按）或已处理过（设备作答/转录已闭合）的提问不再重推：
            # 失效提问的转录永远不闭合，缺这层拦截会每轮重推、无限循环。
            if cid in self._ask_skipped or cid in self._answered_cids:
                continue
            # 多题 → 拆成队列逐题下发；单题 → 队列长度 1，行为与原来一致。
            items = self._build_ask_queue(cid, q)
            if not items:
                continue
            self._ask_queue = items
            self._ask_queue_cid = cid
            await self._push_ask_item(items[0], current, queued=True)
            return

    # ---- 多题串行化 ----

    def _build_ask_queue(self, cid: str, q: dict) -> list[dict]:
        """把一个待答提问拆成"每题一项"的推送队列。

        设备一次只能显示一题（ask 帧只有一个 text + opts），故N 题的
        AskUserQuestion 必须拆成 N 次 ask。队列元素按**可作答优先**排序：
        单选且选项数合规的排前面，多选/超限的排到 _ask_deferred（只读展示），
        这样"能答的题"不会被"只能去电脑答的题"挡在后面。

        每一项都带上 cid/sid/qid：作答投递与闭合判定都依赖它们。
        """
        sid = str(q.get("sid") or "")
        tool = str(q.get("tool") or "AskUserQuestion")
        qlist = q.get("qlist") or []
        if not qlist:
            # 无 qlist（老结构/异常数据）：退回第 0 题的单题视图。
            qlist = [{"qid": str(q.get("qid") or "q_0"),
                      "hint": q.get("hint") or "",
                      "options": list(q.get("options") or []),
                      "multi": bool(q.get("multi")),
                      "answerable": bool(q.get("answerable"))}]
        ready, deferred = [], []
        for one in qlist:
            item = {"cid": cid, "sid": sid, "tool": tool,
                    "qid": str(one.get("qid") or "q_0"),
                    "hint": one.get("hint") or "",
                    "options": list(one.get("options") or []),
                    "multi": bool(one.get("multi")),
                    "answerable": bool(one.get("answerable"))}
            (ready if item["answerable"] else deferred).append(item)
        # 只读题排末尾：它们ans=0 只能去电脑答，不该挡住前面能答的题。
        items = ready + deferred
        if len(items) > 1:
            self._log(f"[daemon] 提问 {cid[:24]}… 拆为 {len(ready)} 可答 / "
                      f"{len(deferred)} 只读，逐题下发")
        return items

    async def _push_ask_item(self, item: dict, current: dict,
                             queued: bool = False) -> None:
        """下发一题到设备提问页，并把显示态登记到 _ask_shown*。

        item 来自 _build_ask_queue。queued=True 表示它属于多题序列，登记时
        额外记下 qid 与剩余题数，作答投递与日志要靠它定位。
        """
        cid = item["cid"]
        ask_id = self._next_id
        self._next_id += 1
        opts = [trunc_utf8(o, ASK_OPT_LEN)
                for o in (item.get("options") or [])][:ASK_OPT_MAX]
        answerable = bool(item.get("answerable")) and self._device_answer_enabled
        # 多选降级：设备只有一颗键，短按/长按分别是"移动光标/提交"，
        # 没有第三种手势去勾选。固件因此**不实现**多选态（见 wb_view.c
        # refresh_ask 的注释：不画 [ ]/[x] 勾选框）——它收到 multi=1
        # 会进多选分支，但 mask 永远是 0，submit_ask 直接拒绝提交。
        # 结果就是"设备停在提问页、用户怎么按都没反应"。
        #
        # 所以必须用 caps 位（CAP_ASK_MULTI）来协商：设备不声明多选能力
        # 时，把 multi 降级成 0，让用户按单选方式选一项，总比卡死好。
        # 主机侧以前定义了这个能力位却从没用过（只写在常量表里），
        # 于是每次都硬发 multi=1 —— 这就是多选问答没反应的根因。
        want_multi = bool(item.get("multi"))
        multi = want_multi and self._caps_has(CAP_ASK_MULTI)
        if want_multi and not multi:
            self._log(f"[daemon] 提问 #{ask_id} 原为多选，但设备未声明多选能力"
                      f"（caps={getattr(self._st, 'device_caps', None)}），降级为单选（选一项）")
        payload = {"type": "ask", "id": ask_id,
                   "tool": trunc_utf8(str(item.get("tool") or ""), ASK_TOOL_MAX),
                   "text": trunc_utf8(item.get("hint") or "", ASK_TEXT_MAX),
                   "opts": opts, "ans": 1 if answerable else 0,
                   # multi=1 → 设备进勾选态，回传 mask 位图。
                   # 只有设备声明 CAP_ASK_MULTI 时才发 1，否则发 0 走单选。
                   "multi": 1 if multi else 0}
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        # 设备接收缓冲 512 字节，超了会被整帧丢弃（ERR_LEN），故提前告警。
        warn = "（帧偏大，注意设备 512 字节上限）" if len(raw) > 480 else ""
        ok = await self._write(payload)
        self._ask_shown_cid = cid
        self._ask_shown_id = ask_id
        self._ask_shown = {"cid": cid, "sid": str(item.get("sid") or ""),
                           "qid": str(item.get("qid") or "q_0"),
                           "options": list(item.get("options") or []),
                           "multi": multi}
        self._ask_shown_at = time.monotonic()
        self._ask_shown_payload = payload
        remain = max(0, len(self._ask_queue) - 1) if queued else 0
        pos = f" 第 {len(self._ask_queue) - remain}/{len(self._ask_queue)} 题" if queued else ""
        self._log(f"[daemon] 提问页 #{ask_id} 已下发{pos} "
                  f"tool={payload['tool']} qid={item.get('qid')} "
                  f"选项={len(opts)} 设备可答={answerable} "
                  f"多选={multi} "
                  f"({len(raw)}B)（{'成功' if ok else '写帧失败'}）{warn}")

    def _caps_has(self, bit: int) -> bool:
        """设备是否声明了某个能力位。

        caps 存在 AppState 上（daemon 只是它的写入方），取值走 self._st。
        caps 为 None = 设备还没发过 hello（老固件或尚未握手），按"不支持"
        处理 —— 最保守，宁可少发新特性也不发设备不认的帧。
        """
        caps = getattr(self._st, "device_caps", None)
        if caps is None:
            return False
        try:
            return bool(int(caps) & bit)
        except (TypeError, ValueError):
            return False

    def _advance_ask_queue(self) -> list[dict] | None:
        """弹出当前题，返回下一题（没有则 None 并清空队列归属）。

        由 _handle_ask_answer（设备答完当前题）与 skip（用户长按跳过整题）调用。
        只弹不推——推送交给下一轮 _process_questions，避免在 BLE 回调栈里
        再次写帧（Windows 下写调用会静默挂起，见 _write 注释）。
        """
        if not self._ask_queue:
            self._ask_queue_cid = None
            return None
        self._ask_queue.pop(0)
        if not self._ask_queue:
            self._ask_queue_cid = None
            return None
        return self._ask_queue[0]

    # ---- 设备作答（ask_answer 帧）----

    def _answer_targets(self, sid: str) -> list:
        """作答投递目标：每个已知 worker 端口的哨兵连接各发一条（按端口去重）。

        会话级连接已删除（writer 抢占问题），唯一来源是端口哨兵——
        哨兵从未 session/load，不抢会话写者，模型续跑事件流向桌面 UI。
        多 worker 场景仍全端口覆盖：发错端口只回 resolved=false，无副作用。
        """
        by_port: dict[int, AcpConnection] = {
            c.port: c for c in self._acp_sentries.values() if c.alive}
        return list(by_port.values())

    def _handle_ask_answer(self, obj: dict) -> None:
        """设备作答回传：发 _codebuddy.ai/resolveInterruption 提交答案。

        这是产品内 API 文档公开的扩展方法（POST /api/v1/acp 的 method 枚举里
        就有它），也是桌面端自己的作答通道：会话在等 AskUserQuestion 时，
        params={sessionId, toolCallId, decision:"allow", answers:{...}} 即可把
        选项回给 Agent。answers 的键写问题 id（"q_0"/"q_1"…，服务端会映射回
        问题原文）。

        **多题必须累积后一次投递**：resolveInterruption 一旦发出，整个提问中断
        即视为已解决，剩余问题不会再有机会回传。所以只有队列排空（本题是最后
        一题）才真正投递；中间题只累积到 _ask_accum。
        """
        aid = obj.get("id")
        index = obj.get("index")
        mask = obj.get("mask")
        if not (isinstance(aid, int) and aid == self._ask_shown_id):
            return
        shown = dict(self._ask_shown or {})
        cid = self._ask_shown_cid or shown.get("cid")
        sid = str(shown.get("sid") or "")
        qid = str(shown.get("qid") or "q_0")
        is_multi = bool(shown.get("multi"))
        options = list(shown.get("options") or [])
        pend = self._acp_questions.get(cid) if cid else None
        if not options and pend:
            options = list(pend.get("options") or [])
            sid = sid or str(pend.get("sid") or "")

        # 先解码再改状态：答案无效时必须原样保留当前显示态与队列，
        # 否则会"队列前进了但答案没投出去"——用户平白丢一题，且没有
        # 任何路径能重答（设备已乐观退出提问页）。
        #
        # ⚠ 这里只判"答案本身无效"（picks 为空），**不能把 cid 为空一起
        # 判成无效**：cid 为空只说明这次作答没有归属会话（测试路径 /
        # 会话已失联），答案本身是有效的。设备提交后已乐观退出提问页，
        # 把它当无效丢掉 = 用户白答且无法重答。两种状态必须分开处理。
        picks = self._decode_ask_picks(mask, index, len(options), is_multi)
        if not picks:
            self._log(f"[daemon] 设备作答 #{aid} 未给出有效选项"
                      f"（cid={cid} picks={picks}），保留当前提问页")
            return
        labels = [options[i] for i in picks]
        # 至此答案有效，才推进状态：设备已乐观退出提问页，先清显示态
        # （停掉保活重推，防止提交后被再次推回）。
        self._clear_ask_shown()
        # 答案有效但无归属会话 → 记清楚"收到但没投出去"，不静默丢弃。
        # 这正是 send_test_ask 想验证的路径：证明帧确实到达了主机。
        # 放在 _advance_ask_queue 之前：无归属的题不该弹队列（否则会误
        # 弹掉后面真正待答的题）。
        if cid is None:
            self._log(f"[daemon] 设备作答 #{aid} 有效但无归属会话"
                      f"（选项：{'、'.join(l[:16] for l in labels)}），未投递")
            return
        # 队列里还有后面的题 → 只累积，不投递。
        nxt = self._advance_ask_queue()
        # 中间题只累积，不需要 sid / ACP 通道：即使此刻通道不可用也能先把
        # 答案存住，等最后一题再一起投（避免白白丢掉用户已做的选择）。
        if nxt is not None:
            self._ask_accum[cid] = self._ask_accum.get(cid, {})
            self._ask_accum[cid][qid] = labels
            self._log(f"[daemon] 设备作答 #{aid} 第 {qid} 题已记："
                      f"{'、'.join(l[:16] for l in labels)}"
                      f"（待答剩余 {len(self._ask_queue)} 题，暂不投递）")
            return
        # 最后一题：合并此前累积的答案，一次性投递。
        answers = dict(self._ask_accum.pop(cid, {}))
        answers[qid] = labels
        if not sid:
            self._log(f"[daemon] 设备作答 #{aid} 缺会话 id，未投递")
            return
        targets = self._answer_targets(sid)
        if not targets:
            self._log(f"[daemon] 设备作答 #{aid} 时 ACP 通道不在，未投递")
            return
        # 建确认账：请求 id 用 "ans-<cid>-<seq>"，响应回来时能对上号；
        # 全部连接都未受理 → 该提问失效、不再重推（见 _note_ans_result）。
        self._ans_watch[cid] = {"expect": len(targets), "fail": 0}
        for conn in targets:
            conn.post_request(
                "_codebuddy.ai/resolveInterruption",
                {"sessionId": sid, "toolCallId": cid, "decision": "allow",
                 "answers": self._encode_answers(answers)},
                tag=f"ans-{cid}")
        # 不再回 JSON-RPC result（走 callAskUserQuestion 原生应答）：我们
        # 已放弃成为会话 owner（不 session/load），resolveInterruption 是
        # 唯一官方通道；回 result 需要持有该提问的 rpc_id，而会话级连接已删。
        self._mark_answered(cid)
        self._log(f"[daemon] 设备作答 #{aid} 已投递："
                  f"{'；'.join(q + '=' + '、'.join(l[:16] for l in ls) for q, ls in answers.items())}"
                  f"（{len(targets)} 条连接）")

    @staticmethod
    def _decode_ask_picks(mask, index, n_opts: int, is_multi: bool) -> list[int]:
        """把设备回传的作答解码成选项序号列表（升序、去重、越界丢弃）。

        · 多选：新固件回 "mask"（位 i 对应第 i 个选项），可多位置位
        · 单选：回 "index"（单个序号）
        · 兼容：标了 multi 但设备只回了 index → 按单选处理（退化为选一项）
        """
        if isinstance(mask, int) and mask > 0:
            return [i for i in range(min(n_opts, 32)) if mask & (1 << i)]
        if isinstance(index, int) and 0 <= index < n_opts:
            return [index]
        return []

    @staticmethod
    def _encode_answers(answers: dict) -> dict:
        """把 {qid: [label, ...]} 规整成 resolveInterruption 的 answers 值。

        单选题直接给字符串（最常见的形态，服务端一定认）；多选题给
        逗号连接的字符串——服务端按 label 文本回填问题原文，逗号连接能让
        一次回传多个选项。若后续实测发现服务端接受数组，改这里即可。
        """
        out = {}
        for qid, labels in answers.items():
            out[qid] = labels[0] if len(labels) == 1 else ",".join(labels)
        return out

    def _mark_answered(self, cid: str) -> None:
        """记住已处理的提问（重连重放时用来跳过）。"""
        if not cid:
            return
        self._answered_cids.add(cid)
        if len(self._answered_cids) > 200:
            for k in list(self._answered_cids)[:100]:
                self._answered_cids.discard(k)

    def _note_ans_result(self, cid: str, ok: bool) -> None:
        """设备作答的响应到达时入账；全部未受理 → 标记提问失效、不再重推。

        投递时按目标连接数建账（expect=N）；任一条回 resolved=true 即销账，
        并写入"不再重推"名单（防转录不落 result 时的重启回潮）。
        累计 N 条都未受理且无一成功，说明服务端已无该提问的活跃中断（多见
        于转录里未闭合的残留提问）：记入跳过名单并留日志——否则提问页会
        每隔约 2s 重推一次，无限循环。
        """
        w = self._ans_watch.get(cid)
        if w is None:
            return
        if ok:
            self._ans_watch.pop(cid, None)
            # 成功受理 → 持久化销账：服务端转录偶发不落 result（实测两次：
            # 14:09、14:34），若不记盘，重启后该提问会被当待答重推（幽灵）。
            # 与"用户跳过"共用同一名单（语义：不再重推的提问）。
            skip_list_add(cid)
            # 已确认送达的提问立即记为已答：转录迟迟不落 result 期间，
            # 状态推流不再把该会话显示成 NEED_ANSWER（否则用户会误以为
            # 作答没送达、"WorkBuddy 一直在等待"）。
            self._mark_answered(cid)
            return
        w["fail"] += 1
        if w["fail"] >= w["expect"]:
            self._ans_watch.pop(cid, None)
            self._ask_skipped.add(cid)
            # 提问已失效：正在排队的后续题一并作废，否则会继续往设备上推
            # 残页（每轮 2s 一次，永远答不完）。
            if self._ask_queue_cid == cid:
                self._ask_queue.clear()
                self._ask_queue_cid = None
            self._ask_accum.pop(cid, None)
            self._log(f"[daemon] 提问 {cid[:24]}… 所有连接均无活跃中断，"
                      f"标记失效、不再重推")

    def _clear_ask_shown(self) -> None:
        """收起提问页显示状态（作答/跳过/闭合共用，顺带停掉保活重推）。"""
        self._ask_shown_cid = None
        self._ask_shown_id = -1
        self._ask_shown = None
        self._ask_shown_at = 0.0
        self._ask_shown_payload = None

    # ---- ACP 事件（来自 AcpConnection 线程）----

    async def _on_acp_event(self, ev: dict) -> None:
        if "_acp_info" in ev:
            self._log(f"[daemon] ACP: {ev['_acp_info']}")
            return
        if "_acp_error" in ev:
            self._log(f"[daemon] ACP 通道异常: {ev['_acp_error']}")
            return
        if "_acp_post" in ev:
            # 短 POST 的响应：id 为 "ans-<cid>-<seq>" 的是设备作答
            # （resolveInterruption）的结果。响应体是 SSE 形状，须逐行解析
            # （":ok" + "event: message" + "data: {...}"），不能整段 json.loads。
            post = ev["_acp_post"] or {}
            rid = post.get("id")
            port = ev.get("_src_port")
            resp = post.get("resp") or ""
            objs = _sse_json_objects(resp)
            if isinstance(rid, str) and rid.startswith("ans-"):
                cid = rid[4:].rsplit("-", 1)[0]
                obj = next((o for o in objs if o.get("id") == rid), None)
                if obj is None and len(objs) == 1:
                    obj = objs[0]   # 兜底：单条响应当作本请求的结果
                if not isinstance(obj, dict):
                    self._log(f"[daemon] :{port} 作答响应无法解析：{resp[:120]}")
                    self._note_ans_result(cid, ok=False)
                elif obj.get("error"):
                    self._log(f"[daemon] :{port} 作答未被受理：{obj['error']}")
                    self._note_ans_result(cid, ok=False)
                else:
                    res = obj.get("result") or {}
                    if res.get("resolved"):
                        self._log(f"[daemon] 作答已送达 worker :{port}"
                                  f"（resolved=true）")
                        self._note_ans_result(cid, ok=True)
                    else:
                        self._log(f"[daemon] :{port} 无匹配中断，未受理"
                                  f"（resolved=false）")
                        self._note_ans_result(cid, ok=False)
                return
            if any(o.get("error") for o in objs):
                err = next(o["error"] for o in objs if o.get("error"))
                self._log(f"[daemon] 设备作答未被接受（可能已在别处作答）：{err}")
            return
        # 诊断：服务端→客户端的请求（带 id 且有 method）全部留痕，
        # 用于确认 AskUserQuestion 是否真的会被推给我们。
        if ev.get("method") and "id" in ev:
            self._log(f"[daemon] ACP 请求 {ev.get('method')} id={ev.get('id')}")
        if ev.get("method") == "_codebuddy.ai/question":
            pend = self._build_acp_pending(ev.get("id"), ev.get("params") or {})
            if pend and pend["cid"] in self._answered_cids:
                return   # 重连后重放的旧提问：已处理过，不再上屏
            if pend:
                pend["port"] = ev.get("_src_port")
                pend["sid"] = ev.get("_src_sid") or pend.get("sid") or ""
                self._acp_questions[pend["cid"]] = pend
                self._log(f"[daemon] 收到 ACP 提问 {pend['cid'][:24]} "
                          f"设备可答={pend['answerable']}")

    @staticmethod
    def _build_acp_pending(rpc_id, params: dict) -> dict | None:
        """把 _codebuddy.ai/question 请求归一化为内部 pending 结构。"""
        try:
            schema = params.get("schema") or {}
            qlist = schema.get("questions") or []
            if not isinstance(qlist, list) or not qlist:
                return None
            tool_call_id = str(params.get("toolCallId") or "")
            if not tool_call_id:
                return None
            # 逐题归一化，复用转录通道的同一套规则（qid/label/单选取舍一致）。
            ones = []
            for i, q in enumerate(qlist):
                one = _one_question(q, i)
                if one and one["options"]:
                    ones.append(one)
            if not ones:
                return None
            first = ones[0]
            return {
                "cid": tool_call_id,
                "tool": "AskUserQuestion",
                "hint": first["hint"],
                "options": first["options"],
                # 设备作答门槛：单题 + 单选 + 1~6 个选项（多题走队列逐题下发）。
                "answerable": (len(ones) == 1 and first["answerable"]),
                "rpc_id": rpc_id,
                "port": None,   # 由 _on_acp_event 用 _src_port 覆盖
                "sid": "",      # 由 _on_acp_event 用 _src_sid 覆盖
                "qid": first["qid"],
                "qlist": ones,
                "session_id": str(params.get("sessionId") or ""),
            }
        except (AttributeError, TypeError):
            return None

    # ---- ACP worker 发现循环 ----

    async def _acp_discover_loop(self) -> None:
        """周期发现本地 WorkBuddy worker（只维护名单；会话连接按需建立）。

        connect 返回 200 的端口为 worker；其它 WorkBuddy 内部端口（404）
        加入忽略名单，端口消失后自动清除。
        """
        while not self._stop.is_set():
            try:
                ports = await asyncio.to_thread(_list_workbuddy_ports)
                port_set = set(ports)
                # 端口消失：清理 worker/忽略名单及其端口哨兵
                for p in (self._worker_ports | self._nonworker_ports) - port_set:
                    self._worker_ports.discard(p)
                    self._nonworker_ports.discard(p)
                    sent = self._acp_sentries.pop(p, None)
                    if sent:
                        sent.stop()
                for port in ports:
                    if port in self._worker_ports or port in self._nonworker_ports:
                        continue
                    cid = await asyncio.to_thread(probe_worker, port)
                    if cid:
                        self._worker_ports.add(port)
                        self._log(f"[daemon] 发现 ACP worker :{port}")
                        self._ensure_sentry_bg(port)
                    else:
                        self._nonworker_ports.add(port)
                # 哨兵例行维护：每个已知 worker 至少一条连接（作答投递的兜底
                # 覆盖；建失败或断线时每轮自动重建）。
                for port in list(self._worker_ports):
                    sent = self._acp_sentries.get(port)
                    if sent is not None and sent.alive:
                        continue
                    if sent is not None:
                        self._acp_sentries.pop(port, None)
                        sent.stop()
                    self._ensure_sentry_bg(port)
            except Exception as exc:
                self._log(f"[daemon] ACP 发现出错: {exc}")
            await asyncio.sleep(ACP_DISCOVER_INTERVAL)


    async def _status_loop(self) -> None:
        await self._push_status()
        while True:
            await asyncio.sleep(self._status_interval)
            try:
                await self._push_status()
            except Exception as exc:
                self._log(f"[daemon] 状态推流出错: {exc}")

    # ---- 周期积分 ----

    async def _push_credit(self) -> None:
        if not self._auth:
            return
        remain, total = fetch_balance(self._auth)
        ok = await self._write({"remain": remain, "total": total})
        # 积分已取到：清空错误态，并把「一键登录」状态复位为 idle ——
        # 否则 done 会一直挂着，将来凭据失效时前端按钮被隐藏、提示区被旧文案占住。
        self._st.set(credit={"remain": remain, "total": total}, credit_reason="",
                     login_state="idle", login_msg="")
        # 成功即清空错误态，让下一次失败能重新完整打印。
        self._credit_err, self._credit_err_count = "", 0
        if ok:
            self._log(f"[daemon] 积分已推 remain={remain:g} total={total:g}")
        else:
            self._log("[daemon] 积分已查询，设备未连接（下一周期再推）")

    def _reload_credit_auth(self) -> bool:
        """重新解析凭据，返回是否可用；仅在状态变化时打日志。"""
        auth, reason = _load_credit_auth()
        changed = (reason or "") != self._auth_reason
        self._auth, self._auth_reason = auth, (reason or "")
        self._st.set(credit_reason=self._auth_reason if not auth else "")
        if changed:
            if auth:
                self._log("[daemon] 已检测到 AIWK 凭据，周期积分恢复")
            else:
                self._log(f"[daemon] 周期积分不可用：{self._auth_reason}；"
                          "如需启用请执行 python wb_credential.py login")
        return bool(auth)

    def _note_credit_error(self, exc: Exception) -> None:
        """积分错误降噪：同一错误按 CREDIT_ERR_REPEAT 合并，避免每周期刷屏。"""
        text = str(exc)
        now = time.monotonic()
        if text != self._credit_err:
            self._credit_err, self._credit_err_at, self._credit_err_count = text, now, 1
            self._log(f"[daemon] 积分查询失败（下一周期再试）: {text}")
            self._st.set(credit_reason=text)
            return
        self._credit_err_count += 1
        if now - self._credit_err_at >= CREDIT_ERR_REPEAT:
            self._log(f"[daemon] 积分查询仍失败（已重试 {self._credit_err_count} 次）: {text}")
            self._credit_err_at, self._credit_err_count = now, 0

    async def _credit_loop(self) -> None:
        if self._credit_interval <= 0:
            self._log(f"[daemon] 周期积分禁用（interval={self._credit_interval}）")
            return
        while not self._stop.is_set():
            if not self._auth and not self._reload_credit_auth():
                # 无凭据时按较长间隔重试：用户补一次 wb_credential.py login 即可
                # 自愈，不必重启 daemon。
                await asyncio.sleep(CREDIT_AUTH_RETRY)
                continue
            try:
                await self._push_credit()
            except Exception as exc:
                self._note_credit_error(exc)
                # token 失效时丢掉缓存凭据，下一轮重新解析会触发刷新。
                if "401" in str(exc) or "403" in str(exc):
                    self._auth = None
            await asyncio.sleep(self._credit_interval)

    # ---- 今日 token 消耗（设备第 7 屏）----

    async def _usage_loop(self) -> None:
        """周期把「今日 token 消耗」推给设备。

        为什么独立成一个循环而不是塞进 _push_status：
          1. status 是 2s 一跳，扫 142MB 日志跑不起（15 倍频率）。
          2. status 帧实测最坏 488B / 480B 上限，本来就满的，塞不下
             models 数组。
        所以走独立的 usage 帧，30s 一跳。设备断线时 _write 返回 false，
        不补发 —— 重连后下一轮自然带最新数字，补旧值只会让屏幕闪一下。

        扫描放 to_thread：0.42s 的同步 IO 直接在事件循环里跑会卡住状态
        推流（2s 一跳，卡 0.4s 就是一次肉眼可见的抖动）。
        """
        while not self._stop.is_set():
            if self._st.caps() is None:
                # 设备还没发 hello，不知道它要不要这一帧。等下一轮。
                await asyncio.sleep(USAGE_INTERVAL)
                continue
            try:
                payload = await asyncio.to_thread(self._build_usage_frame)
            except Exception as exc:
                self._log(f"[daemon] 今日消耗统计失败：{exc}")
                await asyncio.sleep(USAGE_INTERVAL)
                continue
            if payload:
                ok = await self._write(payload)
                if ok and payload.get("today") != self._last_usage_today:
                    self._last_usage_today = payload.get("today")
                    self._log(f"[daemon] 今日消耗已推 today={payload['today']:,} "
                              f"req={payload['req']} 模型 {len(payload['models'])} 个")

            # 主机大盘：Web 控制台的网关/连接信息走独立小帧（status_list 已满）。
            # caps 协商是必须的：老固件不认识无 type 的 host 帧会回 ERR_JSON。
            if self._caps_has(CAP_HOST_DASH):
                try:
                    host = await asyncio.to_thread(self._build_host_frame)
                    ok_host = await self._write(host)
                    # 30s 一跳，内容不变时别刷屏；首次/变化时留一条证据。
                    sig = (host["gw_ports"], host["gw_sessions"],
                           host["secret"], host["cli"])
                    if sig != getattr(self, "_last_host_sig", None):
                        self._last_host_sig = sig
                        self._log(
                            f"[daemon] 主机大盘帧已推（{'成功' if ok_host else '写帧失败'}）"
                            f"网关 {sig[0]} 端口 / 活会话 {sig[1]} / "
                            f"密钥 {'有' if sig[2] else '无'}")
                except Exception as exc:
                    self._log(f"[daemon] 主机大盘帧组装失败：{exc}")

            await asyncio.sleep(USAGE_INTERVAL)

    def _build_usage_frame(self) -> dict | None:
        """算好今日用量，拼成 usage 帧。无数据时返回 None（不发空帧）。

        为什么"无数据就不发"而不是发 0：设备首次连上时 jsonl 里可能还
        没有今天的记录，发 0 会让屏幕显示"今日 0"并一直挂着，用户看到
        会以为统计坏了。不发则保持 "--"，语义是"还没有数据"。
        """
        now = datetime.now()
        day0 = int(datetime(now.year, now.month, now.day).timestamp() * 1000)
        u = usage_scan_today(day0)
        if not u["req"]:
            return None
        return {"usage": 1,
                "today": u["today"],
                "req": u["req"],
                "cache": u["cache"],
                "models": u["models"]}

    def _build_host_frame(self) -> dict:
        """把 Web 大盘里「主机侧独有」的指标拼成独立小帧推给设备。

        status_list 实测最坏 488B（设备 512B 上限、主机按 480B 告警），
        塞不下网关信息；独立帧约 100B，与 usage 帧同频（30s）。

        对应控制台：网关卡片的端口数 / 活会话数 / 密钥 / CLI，
        以及顶栏「已连接」时长。设备固件用 cJSON_IsTrue 解析 secret/cli，
        所以这里必须发 JSON bool，不能发 0/1。
        """
        try:
            gw = wb_gateway_state()
        except Exception:
            gw = {"count": 0, "secret": False, "cli": False}
        try:
            live = wb_live_sessions(force=True) if gw.get("secret") else []
        except Exception:
            live = []
        since = getattr(self, "_ble_since", 0.0) or getattr(self, "_start_time", time.time())
        return {"host": 1,
                "gw_ports": int(gw.get("count") or 0),
                "gw_sessions": len(live),
                "secret": bool(gw.get("secret")),
                "cli": bool(gw.get("cli")),
                "uptime": max(0, int(time.time() - since))}

    # ---- 审批遥控 ----

    async def _on_approval(self, tool: str, hint: str, timeout: float,
                          session_id: str = "") -> str | None:
        # AskUserQuestion 由专用提问页呈现，不走设备审批闸门：走审批会切到审批页
        # 并强制关掉提问页。hook 侧已过滤，这里是配置未更新时的兜底（fail-open）。
        if (tool or "").strip().lower() == "askuserquestion":
            self._log("[daemon] 提问不走设备审批，交由提问页（fail-open）")
            return None
        if not await self._ensure_ble():
            return None
        approval_id = self._next_id
        self._next_id += 1
        loop = asyncio.get_running_loop()
        fut: asyncio.Future = loop.create_future()
        self._notify_events[approval_id] = fut
        active_key = f"{session_id}|permission|{tool}"
        self._active_approvals.add(active_key)
        try:
            ok = await self._write(
                {"type": "approval", "id": approval_id, "tool": tool, "hint": hint})
            if not ok:
                self._notify_events.pop(approval_id, None)
                return None
        except Exception:
            self._notify_events.pop(approval_id, None)
            return None
        self._log(f"[daemon] 审批 #{approval_id} 已下发 tool={tool} hint=…{hint[:24]}，等待按键")
        try:
            key = await asyncio.wait_for(fut, timeout=timeout)
        except asyncio.TimeoutError:
            self._log(f"[daemon] 审批 #{approval_id} 超时，fail-open")
            return None
        finally:
            self._notify_events.pop(approval_id, None)
            self._active_approvals.discard(active_key)
        self._log(f"[daemon] 审批 #{approval_id} 按键={key}")
        if key == "ok":
            return "allow"
        if key == "down":
            return "deny"
        return None

    @staticmethod
    def _session_waiting_question(p: dict) -> bool:
        """该会话此刻是否已有未作答的 AskUserQuestion。

        status_hook 的 Notification 载荷带 session_id / cwd，直接读该会话
        转录尾部即可判断，无需等 2s 的状态轮询。
        """
        sid = str(p.get("session_id") or "")
        cwd = str(p.get("cwd") or "")
        if not sid or not cwd:
            return False
        try:
            tr = read_transcript(cwd, sid)
        except Exception:
            return False
        return bool((tr or {}).get("pending"))

    async def _on_event(self, ev: str, p: dict) -> None:
        """处理 status_hook 上报的生命周期事件，向设备推送提醒（不阻塞、不回决）。

        覆盖两类现有阻塞式审批之外的场景：
          * Notification / permission_prompt：WorkBuddy 弹出权限确认框
          * Stop / ends_with_question：助手以问题收尾、等待用户回答
        status_hook 仅给 2s 超时，故设备不在线时直接跳过、绝不触发连接尝试。
        """
        sid = str(p.get("session_id") or "")
        tool = str(p.get("tool_name") or "")
        ntype = str(p.get("notification_type") or "")
        kind = ""
        if ev == "Notification" and ntype == "permission_prompt":
            kind = "permission"
        elif ev == "Stop" and bool(p.get("ends_with_question")):
            kind = "question"
        if not kind:
            return
        if self._ble is None or not self._ble.is_connected:
            return
        # 提问页正在显示、或该会话已有未作答的提问时，都不再推审批提醒：
        # 提醒会切到审批页并把提问页顶掉。
        # 现场读转录是必要的——提问写入转录（10:51:24）早于 WorkBuddy 的
        # permission_prompt 通知（10:51:25），而状态循环每 2s 才轮询一次，
        # 只靠 _ask_shown_cid 会晚一步、先闪出审批页。
        if self._ask_shown_cid is not None or self._session_waiting_question(p):
            self._log(f"[daemon] 该会话正在等待提问作答，跳过 {kind} 提醒（避免顶掉提问页）")
            return
        key = f"{sid}|{kind}|{tool}"
        # 同一审批项已在阻塞等待按键（PreToolUse 路径），不再重复提醒。
        if key in self._active_approvals:
            return
        now = time.monotonic()
        for k in [k for k, exp in self._event_dedup.items() if exp <= now]:
            self._event_dedup.pop(k, None)
        if self._event_dedup.get(key, 0.0) > now:
            return
        self._event_dedup[key] = now + EVENT_DEDUP_TTL
        approval_id = self._next_id
        self._next_id += 1
        ok = await self._write({
            "type": "approval", "id": approval_id,
            "tool": tool or kind, "hint": ntype or kind})
        self._log(f"[daemon] {kind} 提醒已推送 tool={tool or kind}"
                  f"（{'成功' if ok else '写帧失败'}）")

    # ---- TCP server（审批 IPC） ----

    async def _dispatch(self, request: dict) -> dict:
        cmd = request.get("cmd")
        if cmd == "ping":
            return {"ok": True}
        if cmd == "approval":
            tool = str(request.get("tool") or "")
            hint = str(request.get("hint") or "")
            timeout = float(request.get("timeout") or 90.0)
            session_id = str(request.get("session_id") or "")
            decision = await self._on_approval(tool, hint, timeout, session_id)
            return {"ok": True, "decision": decision}
        if cmd == "event":
            ev = str(request.get("event") or "")
            payload = request.get("payload")
            payload = payload if isinstance(payload, dict) else {}
            await self._on_event(ev, payload)
            return {"ok": True}
        if cmd == "status":
            st = {
                "type": "status_list",
                "count": 1,
                "tasks": [{
                    "state": str(request.get("state") or "RUNNING"),
                    "model": str(request.get("model") or "test-model"),
                    "name": str(request.get("msg") or "manual status push"),
                    "credit": float(request.get("credit") or 0.0),
                }],
                "ts": int(time.time() - self._start_time),
            }
            ok = await self._write(st)
            self._log(f"[daemon] 手动会话已推 {st['tasks'][0]['state']} {st['tasks'][0]['name'][:30]}")
            return {"ok": ok}
        return {"ok": False, "error": f"unknown cmd: {cmd}"}

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            raw = await asyncio.wait_for(reader.readline(), timeout=10.0)
            if not raw or len(raw) > MAX_LINE_BYTES:
                return
            try:
                request = json.loads(raw.decode("utf-8", errors="replace"))
            except json.JSONDecodeError:
                await self._reply(writer, {"ok": False, "error": "malformed json"})
                return
            if not isinstance(request, dict):
                await self._reply(writer, {"ok": False, "error": "expected object"})
                return
            await self._reply(writer, await self._dispatch(request))
        except (asyncio.TimeoutError, ConnectionResetError):
            pass
        except Exception as exc:
            try:
                await self._reply(writer, {"ok": False, "error": str(exc)[:200]})
            except Exception:
                pass
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    @staticmethod
    async def _reply(writer: asyncio.StreamWriter, payload: dict) -> None:
        writer.write((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))
        await writer.drain()

    # ---- 生命周期 ----

    async def run(self) -> None:
        self._stop = asyncio.Event()
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", self._port)
        await self._server.start_serving()
        target = self._address or "待扫描"
        self._log(f"[daemon] IPC 已监听 127.0.0.1:{self._port}，设备={target}")
        tasks = [
            asyncio.create_task(self._status_loop(), name="status"),
            asyncio.create_task(self._credit_loop(), name="credit"),
            asyncio.create_task(self._usage_loop(), name="usage"),
            asyncio.create_task(self._device_watch_loop(), name="watch"),
            asyncio.create_task(self._acp_discover_loop(), name="acp"),
            asyncio.create_task(self._voice_loop(), name="voice"),
        ]
        try:
            await self._stop.wait()
        finally:
            for t in tasks:
                t.cancel()
                try:
                    await t
                except (asyncio.CancelledError, Exception):
                    pass
            for conn in list(self._acp_sentries.values()):
                conn.stop()
            self._acp_sentries.clear()
            await self._shutdown_ble()
            if self._server is not None:
                self._server.close()
                try:
                    await self._server.wait_closed()
                except Exception:
                    pass
                self._server = None
            self._st.set(connected=False)
            self._log("[daemon] 已停止")

    async def _shutdown_ble(self) -> None:
        if self._ble is not None:
            try:
                await self._ble.disconnect()
            except Exception:
                pass
            self._ble = None

# ===================== daemon 生命周期控制 =====================

class DaemonController:
    """在独立线程跑 daemon 的 asyncio loop，支持 Web 线程启停。"""

    def __init__(self, st: AppState, address: str | None, port: int = IPC_PORT,
                 credit_interval: float = CREDIT_INTERVAL) -> None:
        self._st = st
        self._address = address
        self._port = port
        self._credit_interval = credit_interval
        self._thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._daemon: UnifiedDaemon | None = None
        self._login_thread: threading.Thread | None = None

    def is_alive(self) -> bool:
        return bool(self._thread and self._thread.is_alive())

    def start(self) -> bool:
        if self.is_alive():
            return False
        if self._address:
            self._st.set(address=self._address)  # 启动即暴露目标地址，供前端首次配对判定
        self._thread = threading.Thread(target=self._run, daemon=True, name="wb-daemon")
        self._st.set(running=True)
        self._thread.start()
        return True

    def _run(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._amain())
        except Exception as exc:
            self._st.log(f"[daemon] 异常退出: {exc}")
        finally:
            self._st.set(running=False)
            self._loop.close()
            self._loop = None
            self._daemon = None

    async def _amain(self) -> None:
        # IPC 与 BLE 解耦：无论此刻能否发现设备，daemon 都立即启动并监听 IPC；
        # 设备扫描/重连由 UnifiedDaemon 的看护循环在后台持续进行。
        self._daemon = UnifiedDaemon(
            self._st, self._address or "", port=self._port,
            status_interval=STATUS_INTERVAL, credit_interval=self._credit_interval)
        try:
            await self._daemon.run()
        except asyncio.CancelledError:
            pass

    def stop(self) -> bool:
        if not self.is_alive():
            self._st.set(running=False)
            return True
        loop = self._loop
        daemon = self._daemon
        if loop is not None and daemon is not None:
            loop.call_soon_threadsafe(daemon.request_stop)
        return True

    # 调试用：通过当前 BLE 连接发一条伪造审批帧（长命令 + 多类关键词）。
    # 在 daemon 事件循环线程内执行写帧，等待结果，超时即视为失败。
    def send_test_approval(self) -> bool:
        loop = self._loop
        daemon = self._daemon
        if loop is None or daemon is None:
            return False
        payload = {
            "type": "approval",
            "id": 9001,
            "tool": "bash",
            "hint": ("sudo rm -rf /tmp/wb-test/cache-dir --verbose "
                     "--preserve-permissions # cleanup old build artifacts"),
        }
        fut = asyncio.run_coroutine_threadsafe(daemon._write(payload), loop)
        try:
            return bool(fut.result(timeout=3))
        except Exception:
            return False

    # 调试用：发一条伪造提问帧，验证"设备作答 → 回传 → 主机收到"整条链路。
    #
    # ⚠ 关键点：**必须同时登记 _ask_shown_***。
    # 设备的 ask_answer 帧回来时，_handle_ask_answer 第一件事就是
    #     if (isinstance(aid, int) and aid == self._ask_shown_id)
    # 不登记的话 id 对不上，作答被静默丢弃 —— 表现为"设备显示提交成功、
    # 但主机日志什么都没有"，极难排查（因为设备侧看起来完全正常）。
    #
    # 区别对待 cid/sid：真实提问的 cid/sid 来自 ACP 会话，这里没有会话，
    # 所以都留空 —— 这样 _handle_ask_answer 会走到
    # 「答案有效但无归属会话，未投递」这条分支并**打日志**，正好能证明
    # 「设备答案确实送达了主机」而无副作用（不会真的投进某个会话）。
    # 这正是测试想要的：验证协议链路，不污染真实会话。
    def send_test_ask(self, multi: bool = False) -> bool:
        loop = self._loop
        daemon = self._daemon
        if loop is None or daemon is None:
            return False
        # 用 daemon 的自增 id 序列，不要写死：写死的 id 可能和真实提问
        # 撞上，届时设备回传会被误认成真实题的作答。
        ask_id = daemon._next_id
        daemon._next_id += 1
        options = (["staging 测试环境", "production 生产环境", "本地 docker",
                    "跳过，先不改"] if not multi
                   else ["单元测试", "集成测试", "E2E 测试", "性能测试"])
        # 与 _push_ask_item 同一套降级规则：设备没声明 CAP_ASK_MULTI 时
        # 不能发 multi=1，否则设备会进多选态却无法勾选（mask 恒 0），
        # 提交被拒 → 用户按了没反应。
        real_multi = multi and daemon._caps_has(CAP_ASK_MULTI)
        if multi and not real_multi:
            daemon._log(f"[daemon] 测试提问 #{ask_id} 请求多选，但设备未声明"
                        f"多选能力（caps={getattr(daemon._st, 'device_caps', None)}），降级为单选")
        payload = {"type": "ask", "id": ask_id,
                   "tool": "AskUserQuestion",
                   "text": "选哪个环境部署？" if multi else "选哪个环境部署？",
                   "opts": options,
                   "ans": 1,
                   "multi": 1 if real_multi else 0}
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        warn = "（帧偏大）" if len(raw) > BLE_FRAME_MAX else ""

        async def _go() -> bool:
            ok = await daemon._write(payload)
            # 登记显示态，让设备回来的 ask_answer 能被认领。
            # 这几行与 _push_ask_item 的登记保持一致，缺一个都会让
            # 作答被视为"不是当前题"而丢弃。
            daemon._ask_shown_id = ask_id
            daemon._ask_shown_cid = None      # 无真实会话 → 走"未投递"日志分支
            daemon._ask_shown = {"cid": None, "sid": "", "qid": "q_0",
                                 "options": list(options), "multi": real_multi}
            daemon._ask_shown_at = time.monotonic()
            daemon._ask_shown_payload = payload
            daemon._log(f"[daemon] 测试提问 #{ask_id} 已下发 "
                        f"多选={real_multi} 选项={len(options)} ({len(raw)}B)"
                        f"（{'成功' if ok else '写帧失败'}）{warn}")
            return ok

        fut = asyncio.run_coroutine_threadsafe(_go(), loop)
        try:
            return bool(fut.result(timeout=3))
        except Exception:
            return False

    # 设置提示音音量（0..100，0 = 静音）：持久化到本地配置并立即下发设备。
    # test=True 时顺带让设备试听一声（调音量不用等真事件）。
    # 返回 False 表示帧没写出去（设备未连接/写超时），配置仍已保存，
    # 连上后由看护循环补推。
    def set_beep_vol(self, vol: int, test: bool = False) -> bool:
        vol = beep_vol_clamp(vol)
        cfg = console_cfg_load()
        cfg["beep_vol"] = vol
        console_cfg_save(cfg)
        return self._send_beep_cfg(vol, test)

    # 试听：只发一帧带 beep_test 的 cfg，不改音量。
    def test_beep(self) -> bool:
        return self._send_beep_cfg(beep_vol_get(), True)

    def _send_beep_cfg(self, vol: int, test: bool) -> bool:
        """在 daemon 事件循环线程内写一帧 cfg，超时即视为失败。"""
        loop = self._loop
        daemon = self._daemon
        if loop is None or daemon is None:
            return False
        payload: dict = {"type": "cfg", "beep_vol": vol}
        if test:
            # 必须发 JSON bool，不能发 1。passport-fw 的 wb_proto.c 用
            # cJSON_IsTrue 解 beep_test，而 cJSON 的 IsTrue 对 cJSON_Number
            # 恒返回 false —— 发 1 会让试听标志被静默吞掉，按钮点了没声音。
            # 与 _build_host_frame 的 secret/cli 是同一条约定。
            # 另一条固件线的 wb_json_bool 同时兼容 bool 与非 0 数字，
            # 所以老设备收到 true 也不会退化。
            payload["beep_test"] = True
        fut = asyncio.run_coroutine_threadsafe(daemon._write(payload), loop)
        try:
            return bool(fut.result(timeout=3))
        except Exception:
            return False

    # 切换「允许设备端作答」：持久化到本地配置，并同步运行中的 daemon。
    def set_device_answer(self, on: bool) -> bool:
        cfg = console_cfg_load()
        cfg["device_answer"] = bool(on)
        console_cfg_save(cfg)
        daemon = self._daemon
        loop = self._loop
        if daemon is not None and loop is not None:
            def apply():
                daemon._device_answer_enabled = bool(on)
            loop.call_soon_threadsafe(apply)
        return True

    def trigger_login(self) -> bool:
        """后台执行 wb_credential.login()（自动打开浏览器授权）。

        完成后把结果写回 AppState，并通知 daemon 重新加载凭据 + 立即推一次积分。
        登录进行中重复调用返回 False（防重入）。
        """
        if self._login_thread is not None and self._login_thread.is_alive():
            return False
        self._st.set(login_state="running",
                     login_msg="已打开浏览器，请在浏览器中完成登录授权…")

        def _worker():
            try:
                import wb_credential
                auth = wb_credential.login(open_browser=True)
                who = (auth.get("email") or auth.get("nickname")
                       or auth.get("uid") or "已登录")
                self._st.set(login_state="done",
                             login_msg=f"登录成功：{who}，积分即将刷新")
                self._st.log(f"[app] 一键登录成功：{who}")
                self._notify_login_done()
            except Exception as exc:
                self._st.set(login_state="error", login_msg=f"登录失败：{exc}")
                self._st.log(f"[app] 一键登录失败：{exc}")

        self._login_thread = threading.Thread(
            target=_worker, daemon=True, name="wb-login")
        self._login_thread.start()
        return True

    def _notify_login_done(self) -> None:
        """登录成功后，让 daemon 重新解析凭据并立即推一次积分。"""
        loop, daemon = self._loop, self._daemon
        if loop is None or daemon is None:
            return

        def _apply():
            async def _do():
                try:
                    if daemon._reload_credit_auth():
                        await daemon._push_credit()
                except Exception as exc:
                    daemon._note_credit_error(exc)
            asyncio.ensure_future(_do())

        try:
            loop.call_soon_threadsafe(_apply)
        except Exception:
            pass

async def find_device() -> str:
    """按名字扫描设备，返回地址。

    ⚠ 必须用 return_adv=True：
    `BleakScanner.discover()`（不带 adv）拿到的是 **macOS 缓存的设备表**，
    里面的 name 是上次连接/扫描时记下的旧值。改了固件里的广播名之后，
    缓存里还叫旧名字，于是"设备明明在广播、名字也对，主机就是说找不到"。
    带上 adv 才能读到本次广播里的 local_name。
    """
    from bleak import BleakScanner
    print(f"扫描 {' / '.join(DEVICE_NAMES)} ...", flush=True)
    found = await BleakScanner.discover(timeout=SCAN_TIMEOUT, return_adv=True)
    # bleak 的返回形状随版本变：老版本是 [(BLEDevice, AdvertisementData), …]，
    # 新版本是 {地址: (BLEDevice, AdvertisementData)}。两种都摊平成对。
    pairs = found.values() if isinstance(found, dict) else found
    seen: set[str] = set()
    for d, adv in pairs:
        # 优先用广播里的名字：d.name 是缓存值，adv.local_name 才是本次实测。
        name = ((getattr(adv, "local_name", "") if adv else "") or d.name or "").strip()
        if name:
            seen.add(name)
        # 任一候选名精确相等或作为子串出现即命中（两套固件的广播名都认）。
        if any(name == want or (name and want in name) for want in DEVICE_NAMES):
            print(f"  找到 {name} @ {d.address}", flush=True)
            return d.address
    # 扫不到时把看到的设备名打出来，便于判断"是名字不对还是设备没广播"。
    if seen:
        print(f"  本轮未命中，看到的设备: {', '.join(sorted(seen)[:8])}", flush=True)
    raise RuntimeError(f"未找到设备 {' / '.join(DEVICE_NAMES)}（请先在设备上进入 WorkBuddy 页面）")

# ===================== Web UI 前端 =====================

PAGE_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN" data-theme="graphite">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WORKBUDDY 控制台 · AIWK 本地服务</title>
<style>
/* ============================================================
   设计令牌
   主题通过 html[data-theme] 切换，全部为本地 CSS 变量，
   不依赖任何外部 CDN；断网时页面与图标照常可用。
   ============================================================ */
:root{
  --bg:#0b0e13; --bg2:#0e1116; --panel:#11151c; --panel2:#0b0d11;
  --line:#1e2734; --line2:#2a3546;
  --txt:#e8eef7; --muted:#93a2b8; --dim:#64748b;
  --accent:#22d3ee; --accent2:#67e8f9; --accent-ink:#04222a;
  --ok:#34d399; --warn:#fbbf24; --err:#fb7185; --info:#818cf8;
  --ok-bg:rgba(52,211,153,.12); --warn-bg:rgba(251,191,36,.12);
  --err-bg:rgba(251,113,133,.12); --accent-bg:rgba(34,211,238,.12);
  --info-bg:rgba(129,140,248,.12);
  --shadow:0 18px 40px -24px rgba(0,0,0,.9);
  --mono:"JetBrains Mono","Fira Code",ui-monospace,SFMono-Regular,Consolas,"Courier New",monospace;
  --sans:Inter,system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  --r:14px; --r-sm:10px;
}
html[data-theme="nord"]{
  --bg:#0d1218; --bg2:#101820; --panel:#141d27; --panel2:#0a0f15;
  --line:#1e2b38; --line2:#2b3c4d;
  --txt:#e5eef8; --muted:#93a7bd; --dim:#6b7f95;
  --accent:#7dd3fc; --accent2:#bae6fd; --accent-ink:#04202e;
  --ok:#5eead4; --warn:#fcd34d; --err:#fda4af; --info:#a5b4fc;
  --ok-bg:rgba(94,234,212,.12); --warn-bg:rgba(252,211,77,.12);
  --err-bg:rgba(253,164,175,.12); --accent-bg:rgba(125,211,252,.12);
  --info-bg:rgba(165,180,252,.12);
}
html[data-theme="warm"]{
  --bg:#12100d; --bg2:#17130f; --panel:#1c1712; --panel2:#100d0a;
  --line:#2b241c; --line2:#3a3026;
  --txt:#f4ece0; --muted:#b9a892; --dim:#8a7b68;
  --accent:#f0b862; --accent2:#fbd38d; --accent-ink:#241703;
  --ok:#a3c98a; --warn:#e8b25a; --err:#e08a7a; --info:#c2a7e0;
  --ok-bg:rgba(163,201,138,.12); --warn-bg:rgba(232,178,90,.12);
  --err-bg:rgba(224,138,122,.12); --accent-bg:rgba(240,184,98,.13);
  --info-bg:rgba(194,167,224,.12);
}
html[data-theme="sage"]{
  --bg:#0c1210; --bg2:#101714; --panel:#141d19; --panel2:#0a100e;
  --line:#1e2b25; --line2:#2a3b33;
  --txt:#e6f1ea; --muted:#93ab9c; --dim:#6b8274;
  --accent:#4ade80; --accent2:#86efac; --accent-ink:#03210f;
  --ok:#4ade80; --warn:#d9c46b; --err:#f08c8c; --info:#8fd3c7;
  --ok-bg:rgba(74,222,128,.12); --warn-bg:rgba(217,196,107,.12);
  --err-bg:rgba(240,140,140,.12); --accent-bg:rgba(74,222,128,.12);
  --info-bg:rgba(143,211,199,.12);
}
html[data-theme="light"]{
  --bg:#f4f6fa; --bg2:#ffffff; --panel:#ffffff; --panel2:#f0f3f8;
  --line:#e2e8f0; --line2:#cbd5e1;
  --txt:#0f1a2b; --muted:#526176; --dim:#7b8a9e;
  --accent:#0891b2; --accent2:#0e7490; --accent-ink:#ffffff;
  --ok:#059669; --warn:#b45309; --err:#e11d48; --info:#4f46e5;
  --ok-bg:rgba(5,150,105,.10); --warn-bg:rgba(180,83,9,.10);
  --err-bg:rgba(225,29,72,.10); --accent-bg:rgba(8,145,178,.10);
  --info-bg:rgba(79,70,229,.10);
  --shadow:0 10px 26px -20px rgba(15,26,43,.5);
}

*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%}
body{
  background:var(--bg) fixed; color:var(--txt);
  font-family:var(--sans); font-size:14px; line-height:1.5;
  -webkit-font-smoothing:antialiased; min-height:100vh;
  display:flex; flex-direction:column;
  background-image:
    radial-gradient(900px 420px at 12% -8%, color-mix(in srgb, var(--accent) 12%, transparent), transparent 70%),
    radial-gradient(760px 380px at 92% 0%, color-mix(in srgb, var(--info) 10%, transparent), transparent 72%),
    repeating-linear-gradient(0deg, transparent 0 31px, color-mix(in srgb, var(--line) 34%, transparent) 31px 32px),
    repeating-linear-gradient(90deg, transparent 0 31px, color-mix(in srgb, var(--line) 26%, transparent) 31px 32px);
  background-attachment:fixed;
}
::selection{background:color-mix(in srgb, var(--accent) 34%, transparent); color:var(--txt)}
.i{width:15px;height:15px;flex:0 0 auto;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}
.mono{font-family:var(--mono);font-variant-numeric:tabular-nums}
.sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0)}
a{color:inherit}

/* ---------------- 头部 ---------------- */
header{
  position:sticky; top:0; z-index:40;
  background:color-mix(in srgb, var(--bg2) 92%, transparent);
  backdrop-filter:blur(14px) saturate(140%);
  border-bottom:1px solid var(--line);
}
.hd{max-width:1440px;margin:0 auto;padding:9px 20px 8px;display:flex;flex-direction:column;gap:8px}
.hd-top{display:flex;align-items:center;gap:16px;flex-wrap:wrap;width:100%}
.hd-bot{display:flex;align-items:center;gap:8px;flex-wrap:wrap;width:100%;padding-top:8px;border-top:1px solid color-mix(in srgb,var(--line) 80%, transparent)}
.hd-bot .grow{flex:1 1 auto}
.brand{display:flex;align-items:center;gap:11px;min-width:0}
.mark{
  width:36px;height:36px;border-radius:11px;display:grid;place-items:center;cursor:pointer;
  background:linear-gradient(160deg, color-mix(in srgb,var(--accent) 22%, var(--panel)), var(--panel));
  border:1px solid var(--line2); color:var(--accent); position:relative; transition:.18s;
}
.mark:hover{border-color:color-mix(in srgb,var(--accent) 55%, var(--line2)); transform:translateY(-1px)}
.mark .dotlive{position:absolute;right:-2px;bottom:-2px;width:9px;height:9px;border-radius:50%;background:var(--dim);box-shadow:0 0 0 2.5px var(--bg2);transition:.2s}
.mark .dotlive.on{background:var(--ok);box-shadow:0 0 0 2.5px var(--bg2),0 0 10px var(--ok)}
.btitle{display:flex;align-items:center;gap:8px}
.btitle h1{font-size:15px;letter-spacing:.06em;font-weight:800}
.tag-mini{font-family:var(--mono);font-size:10px;letter-spacing:.1em;padding:2px 6px;border-radius:6px;background:var(--panel);border:1px solid var(--line2);color:var(--muted)}
.bmeta{display:flex;align-items:center;gap:7px;font-family:var(--mono);font-size:11px;color:var(--muted)}
.bmeta .sep{color:var(--line2)}
.bmeta strong{color:var(--txt);font-weight:600}
.copyable{cursor:pointer;transition:.15s;display:inline-flex;align-items:center;gap:4px}
.copyable:hover{color:var(--accent2)}

.telemetry{display:flex;align-items:center;gap:6px;padding-left:14px;border-left:1px solid var(--line)}
.tm{display:flex;align-items:center;gap:6px;padding:4px 9px;border-radius:9px;font-size:11px;font-family:var(--mono);
    background:color-mix(in srgb,var(--panel) 86%, transparent);border:1px solid var(--line);color:var(--muted);white-space:nowrap}
.tm strong{color:var(--txt);font-weight:600}
.tm.ok{color:var(--ok)} .tm.ac{color:var(--accent)} .tm.iv{color:var(--info)}

.hd-right{margin-left:auto;display:flex;align-items:center;gap:8px;flex-wrap:wrap;justify-content:flex-end}
.hd-bot .seg{margin-left:0}
.kbd{font-family:var(--mono);font-size:10px;padding:1px 5px;border-radius:5px;background:var(--panel);border:1px solid var(--line2);color:var(--muted)}

.seg{display:flex;align-items:center;gap:2px;padding:3px;border-radius:11px;background:color-mix(in srgb,var(--panel) 80%, transparent);border:1px solid var(--line)}
.seg button{
  border:0;background:transparent;color:var(--muted);font:inherit;font-size:11px;font-weight:600;
  padding:3px 9px;border-radius:8px;cursor:pointer;transition:.16s;white-space:nowrap;
}
.seg button:hover{color:var(--txt)}
.seg button.act{background:color-mix(in srgb,var(--accent) 18%, var(--panel));color:var(--accent2);
  box-shadow:inset 0 0 0 1px color-mix(in srgb,var(--accent) 40%, transparent)}

.badge{
  display:inline-flex;align-items:center;gap:7px;padding:6px 12px;border-radius:999px;
  font-size:12px;font-weight:600;border:1px solid var(--line2);background:var(--panel);color:var(--muted);
  transition:.2s;white-space:nowrap;
}
.badge .bd{width:7px;height:7px;border-radius:50%;background:var(--dim)}
.badge.on{color:var(--ok);background:var(--ok-bg);border-color:color-mix(in srgb,var(--ok) 38%, transparent)}
.badge.on .bd{background:var(--ok);box-shadow:0 0 0 0 var(--ok);animation:pulse 2.4s infinite}
.badge.mid{color:var(--warn);background:var(--warn-bg);border-color:color-mix(in srgb,var(--warn) 38%, transparent)}
.badge.mid .bd{background:var(--warn)}
.badge.off{color:var(--err);background:var(--err-bg);border-color:color-mix(in srgb,var(--err) 34%, transparent)}
.badge.off .bd{background:var(--err)}
@keyframes pulse{0%{box-shadow:0 0 0 0 color-mix(in srgb,var(--ok) 60%, transparent)}70%{box-shadow:0 0 0 7px transparent}100%{box-shadow:0 0 0 0 transparent}}

.sw{display:inline-flex;align-items:center;gap:7px;padding:5px 10px;border-radius:10px;cursor:pointer;
    background:color-mix(in srgb,var(--panel) 86%, transparent);border:1px solid var(--line);transition:.16s;user-select:none}
.sw:hover{border-color:color-mix(in srgb,var(--accent) 42%, var(--line2))}
.sw span.lbl{font-size:11.5px;color:var(--muted);white-space:nowrap}
.sw input{position:absolute;opacity:0;pointer-events:none}
.sw .track{width:30px;height:16px;border-radius:99px;background:var(--line2);position:relative;transition:.2s;flex:0 0 auto}
.sw .track::after{content:"";position:absolute;top:2px;left:2px;width:12px;height:12px;border-radius:50%;background:#fff;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.4)}
.sw input:checked + .track{background:var(--accent)}
.sw input:checked + .track::after{transform:translateX(14px)}
.sw input:focus-visible + .track{outline:2px solid color-mix(in srgb,var(--accent) 60%, transparent);outline-offset:2px}

.btn{
  display:inline-flex;align-items:center;gap:6px;padding:7px 13px;border-radius:10px;border:1px solid var(--line2);
  background:var(--panel);color:var(--txt);font:inherit;font-size:12.5px;font-weight:600;cursor:pointer;
  transition:.16s;white-space:nowrap;
}
.btn:hover{background:color-mix(in srgb,var(--panel) 70%, var(--line));transform:translateY(-1px)}
.btn:active{transform:translateY(0)}
.btn:disabled{opacity:.5;cursor:not-allowed;transform:none}
.btn.pri{background:linear-gradient(180deg, color-mix(in srgb,var(--accent) 88%, #fff 6%), var(--accent));color:var(--accent-ink);border-color:transparent}
.btn.pri:hover{filter:brightness(1.07)}
.btn.stop{background:var(--err-bg);color:var(--err);border-color:color-mix(in srgb,var(--err) 34%, transparent)}
.btn.start{background:var(--ok-bg);color:var(--ok);border-color:color-mix(in srgb,var(--ok) 34%, transparent)}
.btn.ghost{background:transparent;color:var(--muted)}
.btn.ghost:hover{color:var(--txt);background:color-mix(in srgb,var(--panel) 80%, transparent)}
.btn.ico{padding:6px}
.btn.sm{padding:4px 9px;font-size:11.5px;border-radius:8px}

/* ---------------- 主体 ---------------- */
main{max-width:1440px;width:100%;margin:0 auto;padding:18px 20px 26px;flex:1;display:flex;flex-direction:column;gap:16px}
.card{
  background:linear-gradient(180deg, color-mix(in srgb,var(--panel) 96%, transparent), var(--panel));
  border:1px solid var(--line); border-radius:var(--r); box-shadow:var(--shadow);
  position:relative; overflow:hidden;
}
.card::before{content:"";position:absolute;inset:0 0 auto;height:1px;background:linear-gradient(90deg,transparent,color-mix(in srgb,var(--accent) 34%, transparent),transparent);opacity:.75}
.card-hd{display:flex;align-items:center;gap:10px;padding:12px 15px;border-bottom:1px solid var(--line);flex-wrap:wrap}
.card-hd h2{font-size:13px;font-weight:700;letter-spacing:.02em}
.card-hd .spacer{margin-left:auto}
.ibox{width:28px;height:28px;border-radius:9px;display:grid;place-items:center;flex:0 0 auto;
      background:var(--accent-bg);border:1px solid color-mix(in srgb,var(--accent) 32%, transparent);color:var(--accent)}
.ibox.iv{background:var(--info-bg);border-color:color-mix(in srgb,var(--info) 32%, transparent);color:var(--info)}
.ibox.ok{background:var(--ok-bg);border-color:color-mix(in srgb,var(--ok) 32%, transparent);color:var(--ok)}
.ibox.wn{background:var(--warn-bg);border-color:color-mix(in srgb,var(--warn) 32%, transparent);color:var(--warn)}

/* 指标卡 */
.metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}
.metric{padding:13px 14px 12px;display:flex;flex-direction:column;gap:8px;min-height:150px}
.metric-hd{display:flex;align-items:center;gap:9px}
.metric-hd .t{font-size:12.5px;font-weight:700}
.metric-hd .r{margin-left:auto;display:flex;align-items:center;gap:5px}
.metric .big{font-size:24px;font-weight:800;letter-spacing:-.02em;font-family:var(--mono);line-height:1.1}
.metric .unit{font-size:12px;color:var(--muted);font-weight:600;margin-left:2px}
.kv{display:flex;align-items:center;justify-content:space-between;gap:8px;font-size:12px;color:var(--muted)}
.kv b{color:var(--txt);font-weight:600;font-family:var(--mono);font-size:12px;max-width:62%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.metric-ft{margin-top:auto;padding-top:9px;border-top:1px solid var(--line);display:flex;align-items:center;justify-content:space-between;gap:8px;font-size:11.5px;color:var(--muted)}
.chipst{display:inline-flex;align-items:center;gap:5px;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600;
        background:var(--panel2);border:1px solid var(--line2);color:var(--muted);font-family:var(--mono)}
.chipst.ok{color:var(--ok);background:var(--ok-bg);border-color:color-mix(in srgb,var(--ok) 34%, transparent)}
.chipst.wn{color:var(--warn);background:var(--warn-bg);border-color:color-mix(in srgb,var(--warn) 34%, transparent)}
.chipst.er{color:var(--err);background:var(--err-bg);border-color:color-mix(in srgb,var(--err) 34%, transparent)}
.chipst .d{width:6px;height:6px;border-radius:50%;background:currentColor}

.bar{height:6px;border-radius:99px;background:var(--panel2);border:1px solid var(--line);overflow:hidden}
.bar > i{display:block;height:100%;width:0;border-radius:99px;background:linear-gradient(90deg,var(--accent),var(--accent2));transition:width .5s ease}
.bar.wn > i{background:linear-gradient(90deg,var(--warn),#fcd34d)}
.bar.er > i{background:linear-gradient(90deg,var(--err),#fda4af)}

input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:5px;border-radius:99px;background:var(--panel2);border:1px solid var(--line);outline:none;cursor:pointer}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:15px;height:15px;border-radius:50%;background:var(--accent);border:2px solid var(--bg2);box-shadow:0 0 0 1px var(--accent),0 2px 6px rgba(0,0,0,.4)}
input[type=range]::-moz-range-thumb{width:13px;height:13px;border-radius:50%;background:var(--accent);border:2px solid var(--bg2)}

.ports{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:7px}
.port{padding:6px 8px;border-radius:9px;background:var(--panel2);border:1px solid var(--line);display:flex;align-items:center;justify-content:space-between;gap:6px}
.port .p{font-family:var(--mono);font-size:12px;font-weight:700;color:var(--accent2)}
.port .s{width:6px;height:6px;border-radius:50%;background:var(--ok);box-shadow:0 0 6px var(--ok)}
.port.empty{grid-column:1/-1;justify-content:center;color:var(--dim);font-size:11px;font-family:var(--mono);
              background:transparent;border:0;padding:0;height:14px}
.port .s.off{background:var(--dim);box-shadow:none}

/* 工作区双栏 */
.workspace{display:grid;grid-template-columns:minmax(0,7fr) minmax(0,5fr);gap:16px;align-items:start}
.col{display:flex;flex-direction:column;gap:16px;min-width:0}

/* 会话表 */
.tw{overflow-x:auto}
table{width:100%;border-collapse:collapse;text-align:left}
thead th{font-size:10.5px;letter-spacing:.06em;color:var(--dim);font-weight:700;
         padding:8px 10px;background:color-mix(in srgb,var(--panel2) 70%, transparent);border-bottom:1px solid var(--line);white-space:nowrap}
tbody td{padding:9px 10px;border-bottom:1px solid color-mix(in srgb,var(--line) 65%, transparent);font-size:12.5px;vertical-align:middle}
tbody tr{transition:.14s}
tbody tr:hover{background:color-mix(in srgb,var(--accent) 5%, transparent)}
tbody tr:last-child td{border-bottom:0}
.idc{display:flex;align-items:center;gap:8px;min-width:170px}
.led{width:7px;height:7px;border-radius:50%;flex:0 0 auto;box-shadow:0 0 8px currentColor}
.sid{font-family:var(--mono);font-size:10px;color:var(--dim)}
.sname{font-size:12.5px;font-weight:600;color:var(--txt);max-width:210px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.chip{display:inline-block;padding:2px 8px;border-radius:7px;background:var(--panel2);border:1px solid var(--line2);
      font-family:var(--mono);font-size:11px;color:var(--muted);max-width:145px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.st{display:inline-flex;align-items:center;gap:6px;padding:3px 9px;border-radius:999px;font-size:11px;font-weight:700;
    border:1px solid transparent;white-space:nowrap}
.st .d{width:5px;height:5px;border-radius:50%;background:currentColor}
.st.live .d{animation:pulse 1.8s infinite}
.mono-cell{font-family:var(--mono);font-size:12px;color:var(--muted);white-space:nowrap}
.empty-state{padding:34px 20px;text-align:center;color:var(--muted)}
.empty-state .ico2{width:42px;height:42px;border-radius:12px;margin:0 auto 10px;display:grid;place-items:center;background:var(--panel2);border:1px solid var(--line);color:var(--dim)}
.empty-state h4{font-size:13px;color:var(--txt);margin-bottom:4px}
.empty-state p{font-size:12px;color:var(--muted);max-width:340px;margin:0 auto}

/* 投递卡 */
.deliver{padding:14px 15px 15px;display:flex;flex-direction:column;gap:11px}
.tpl-row{display:flex;align-items:center;gap:6px;flex-wrap:wrap}
.tpl{font-size:11px;padding:4px 9px;border-radius:8px;background:var(--panel2);border:1px solid var(--line);
     color:var(--muted);cursor:pointer;transition:.15s;font:inherit;font-size:11px}
.tpl:hover{color:var(--accent2);border-color:color-mix(in srgb,var(--accent) 42%, var(--line))}
.ta-wrap{position:relative}
textarea#wbText{
  width:100%;min-height:92px;resize:vertical;padding:12px 13px 26px;border-radius:11px;
  background:var(--panel2);border:1px solid var(--line);color:var(--txt);font:inherit;font-size:13px;
  line-height:1.6;outline:none;transition:.16s;
}
textarea#wbText::placeholder{color:var(--dim)}
textarea#wbText:focus{border-color:color-mix(in srgb,var(--accent) 55%, var(--line));box-shadow:0 0 0 3px color-mix(in srgb,var(--accent) 13%, transparent)}
.ta-foot{position:absolute;right:10px;bottom:8px;display:flex;align-items:center;gap:7px;font-family:var(--mono);font-size:10.5px;color:var(--dim);pointer-events:none}
.deliver-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.field{display:flex;align-items:center;gap:8px;padding:0 10px;border-radius:10px;background:var(--panel2);border:1px solid var(--line);transition:.16s}
.field:focus-within{border-color:color-mix(in srgb,var(--accent) 55%, var(--line))}
.field label{font-size:11.5px;color:var(--muted);white-space:nowrap}
.field select,.field input{flex:1;min-width:0;background:transparent;border:0;outline:none;color:var(--txt);
  font:inherit;font-size:12.5px;padding:8px 0;font-family:var(--mono)}
.field select option{background:var(--panel);color:var(--txt)}
.hint{font-size:12px;color:var(--muted);min-height:18px}
.hint.warn{color:var(--warn)} .hint.err{color:var(--err)} .hint.ok{color:var(--ok)}
.deliver-actions{display:grid;grid-template-columns:1fr 1fr auto;gap:10px;align-items:center}

/* 日志终端 */
.term{background:var(--panel2);border-radius:var(--r);border:1px solid var(--line);overflow:hidden;display:flex;flex-direction:column}
.term-hd{padding:10px 12px;background:color-mix(in srgb,var(--panel) 92%, transparent);border-bottom:1px solid var(--line);display:flex;flex-direction:column;gap:9px}
.term-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.dots{display:flex;gap:5px}
.dots i{width:9px;height:9px;border-radius:50%;display:block}
.dots i:nth-child(1){background:color-mix(in srgb,var(--err) 85%, transparent)}
.dots i:nth-child(2){background:color-mix(in srgb,var(--warn) 85%, transparent)}
.dots i:nth-child(3){background:color-mix(in srgb,var(--ok) 85%, transparent)}
.term-title{display:flex;align-items:center;gap:6px;font-size:12px;font-weight:700;color:var(--txt)}
.vr{width:1px;height:14px;background:var(--line2)}
.cnt-badge{font-family:var(--mono);font-size:10.5px;padding:1px 7px;border-radius:6px;background:var(--panel);border:1px solid var(--line2);color:var(--muted)}
.fpills{display:flex;gap:2px;padding:2.5px;border-radius:8px;background:var(--panel);border:1px solid var(--line)}
.fpill{border:0;background:transparent;color:var(--muted);font:inherit;font-family:var(--mono);font-size:10.5px;
  padding:2px 7px;border-radius:6px;cursor:pointer;transition:.15s}
.fpill:hover{color:var(--txt)}
.fpill.act{background:color-mix(in srgb,var(--accent) 20%, var(--panel));color:var(--accent2);font-weight:700}
.search{display:flex;align-items:center;gap:6px;padding:4px 8px;border-radius:8px;background:var(--panel);border:1px solid var(--line);transition:.15s}
.search:focus-within{border-color:color-mix(in srgb,var(--accent) 50%, var(--line))}
.search input{background:transparent;border:0;outline:none;color:var(--txt);font:inherit;font-family:var(--mono);font-size:11.5px;width:100%}
.search input::placeholder{color:var(--dim)}
select.mini{background:var(--panel);border:1px solid var(--line);color:var(--muted);border-radius:8px;
  font:inherit;font-family:var(--mono);font-size:11px;padding:4px 6px;outline:none;cursor:pointer}
select.mini option{background:var(--panel);color:var(--txt)}
.term-body{height:430px;overflow-y:auto;padding:10px 12px;font-family:var(--mono);font-size:11.5px;line-height:1.75;scrollbar-width:thin}
.term-body::-webkit-scrollbar{width:9px}
.term-body::-webkit-scrollbar-thumb{background:var(--line2);border-radius:9px;border:2px solid var(--panel2)}
.lg{display:flex;gap:7px;align-items:flex-start;padding:1px 0;border-radius:4px}
.lg:hover{background:color-mix(in srgb,var(--txt) 3%, transparent)}
.lg-t{color:var(--dim);flex:0 0 auto}
.lg-tag{flex:0 0 auto;padding:0 5px;border-radius:4px;font-size:10px;line-height:1.6;
  background:var(--panel);border:1px solid var(--line2);color:var(--muted)}
.lg-tag.web{color:var(--accent2);border-color:color-mix(in srgb,var(--accent) 34%, transparent);background:var(--accent-bg)}
.lg-tag.daemon{color:var(--info);border-color:color-mix(in srgb,var(--info) 34%, transparent);background:var(--info-bg)}
.lg-tag.app{color:var(--ok);border-color:color-mix(in srgb,var(--ok) 34%, transparent);background:var(--ok-bg)}
.lg-tag.tray{color:var(--warn);border-color:color-mix(in srgb,var(--warn) 34%, transparent);background:var(--warn-bg)}
.lg-m{color:var(--muted);word-break:break-word;flex:1 1 auto}
.lg-m.err{color:var(--err)} .lg-m.warn{color:var(--warn)}
.term-empty{padding:26px 12px;text-align:center;color:var(--dim);font-size:12px}
.term-ft{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:7px 12px;
  border-top:1px solid var(--line);background:color-mix(in srgb,var(--panel) 92%, transparent);
  font-family:var(--mono);font-size:10.5px;color:var(--dim);flex-wrap:wrap}
.term-ft .g{display:flex;align-items:center;gap:8px}
.live-dot{width:6px;height:6px;border-radius:50%;background:var(--ok);animation:pulse 2.2s infinite}

/* 扫描提示 */
.scan{display:none;padding:8px 14px 10px;border-bottom:1px solid var(--line);background:color-mix(in srgb,var(--warn) 5%, transparent)}
.scan.show{display:block}
.scan-top{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--warn);margin-bottom:6px}
.scan-top .t{margin-left:auto;font-family:var(--mono);font-size:11px;color:var(--muted)}
.scan .bar{height:4px}
.scan .sub{font-size:11px;color:var(--dim);margin-top:5px}

/* 页脚 */
footer{max-width:1440px;width:100%;margin:0 auto;padding:12px 20px 18px;border-top:1px solid var(--line);
  display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap;font-size:11.5px;color:var(--dim)}
footer .g{display:flex;align-items:center;gap:9px;flex-wrap:wrap}
footer .sep{color:var(--line2)}

/* Toast */
.toasts{position:fixed;right:18px;bottom:18px;z-index:90;display:flex;flex-direction:column;gap:8px;max-width:340px}
.toast{display:flex;align-items:flex-start;gap:8px;padding:10px 12px;border-radius:11px;font-size:12.5px;
  background:color-mix(in srgb,var(--panel) 96%, transparent);border:1px solid var(--line2);color:var(--txt);
  box-shadow:0 12px 30px -14px rgba(0,0,0,.8);opacity:0;transform:translateY(8px);transition:.22s}
.toast.show{opacity:1;transform:none}
.toast.ok{border-color:color-mix(in srgb,var(--ok) 42%, transparent)} .toast.ok .ti{color:var(--ok)}
.toast.err{border-color:color-mix(in srgb,var(--err) 42%, transparent)} .toast.err .ti{color:var(--err)}
.toast.warn{border-color:color-mix(in srgb,var(--warn) 42%, transparent)} .toast.warn .ti{color:var(--warn)}
.toast .ti{color:var(--accent);flex:0 0 auto}

/* Modal */
.modal{position:fixed;inset:0;z-index:80;display:none;align-items:center;justify-content:center;padding:20px;
  background:color-mix(in srgb,#000 62%, transparent);backdrop-filter:blur(3px)}
.modal.show{display:flex}
.modal-card{width:min(430px,100%);border-radius:16px;background:var(--panel);border:1px solid var(--line2);
  box-shadow:0 30px 70px -30px #000;padding:20px;display:flex;flex-direction:column;gap:12px}
.modal-ico{width:42px;height:42px;border-radius:12px;display:grid;place-items:center;background:var(--accent-bg);
  border:1px solid color-mix(in srgb,var(--accent) 34%, transparent);color:var(--accent)}
.modal-card h3{font-size:15px}
.modal-card p{font-size:12.5px;color:var(--muted);line-height:1.65}
.modal-addr{font-family:var(--mono);font-size:12px;padding:8px 10px;border-radius:9px;background:var(--panel2);
  border:1px solid var(--line);color:var(--accent2)}
.modal-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:4px}

/* 命令面板 */
.palette{position:fixed;inset:0;z-index:85;display:none;padding:12vh 16px 16px;justify-content:center;
  background:color-mix(in srgb,#000 55%, transparent);backdrop-filter:blur(3px)}
.palette.show{display:flex}
.palette-card{width:min(560px,100%);max-height:70vh;background:var(--panel);border:1px solid var(--line2);
  border-radius:14px;box-shadow:0 30px 70px -30px #000;display:flex;flex-direction:column;overflow:hidden}
.palette-in{display:flex;align-items:center;gap:9px;padding:12px 14px;border-bottom:1px solid var(--line)}
.palette-in input{flex:1;background:transparent;border:0;outline:none;color:var(--txt);font:inherit;font-size:14px}
.palette-in input::placeholder{color:var(--dim)}
.palette-list{overflow-y:auto;padding:6px}
.pitem{display:flex;align-items:center;gap:10px;padding:9px 11px;border-radius:9px;cursor:pointer;font-size:13px}
.pitem:hover,.pitem.act{background:color-mix(in srgb,var(--accent) 12%, transparent);color:var(--accent2)}
.pitem .hint-k{margin-left:auto;font-family:var(--mono);font-size:10.5px;color:var(--dim)}
.palette-ft{padding:8px 14px;border-top:1px solid var(--line);font-size:11px;color:var(--dim);display:flex;gap:12px;flex-wrap:wrap}

/* 响应式 */
@media (max-width:1180px){
  .metrics{grid-template-columns:repeat(2,minmax(0,1fr))}
  .workspace{grid-template-columns:minmax(0,1fr)}
  .telemetry{display:none}
}
@media (max-width:720px){
  .hd{padding:8px 12px;gap:10px}
  main{padding:12px}
  .metrics{grid-template-columns:minmax(0,1fr)}
  .deliver-row,.deliver-actions{grid-template-columns:1fr}
  .term-body{height:340px}
  .bmeta .hide-sm{display:none}
  /* 窄屏只保留「会话 / 状态 / 待办」，模型与积分靠点开桌面端看，避免表格横向滚动 */
  table th:nth-child(2),table td:nth-child(2),
  table th:nth-child(3),table td:nth-child(3){display:none}
  .sname{max-width:230px}
  .hd-right{gap:6px}
}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style>
</head>
<body>
<!-- 内联图标精灵：全部本地绘制，不引图标字体 -->
<svg width="0" height="0" style="position:absolute" aria-hidden="true">
  <defs>
    <symbol id="i-cpu" viewBox="0 0 24 24"><rect x="5" y="5" width="14" height="14" rx="2"/><rect x="9" y="9" width="6" height="6" rx="1"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/></symbol>
    <symbol id="i-globe" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a15 15 0 0 1 0 18M12 3a15 15 0 0 0 0 18"/></symbol>
    <symbol id="i-pulse" viewBox="0 0 24 24"><path d="M3 12h4l2-7 4 14 2-7h6"/></symbol>
    <symbol id="i-batt" viewBox="0 0 24 24"><rect x="2" y="7" width="17" height="10" rx="2.5"/><path d="M22 11v2"/><rect x="5" y="10" width="8" height="4" rx="1" fill="currentColor" stroke="none"/></symbol>
    <symbol id="i-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></symbol>
    <symbol id="i-bt" viewBox="0 0 24 24"><path d="m7 7 10 10-5 4V3l5 4L7 17"/></symbol>
    <symbol id="i-net" viewBox="0 0 24 24"><rect x="9" y="2" width="6" height="6" rx="1.5"/><rect x="2" y="16" width="6" height="6" rx="1.5"/><rect x="16" y="16" width="6" height="6" rx="1.5"/><path d="M12 8v4M5 16v-4h14v4"/></symbol>
    <symbol id="i-spk" viewBox="0 0 24 24"><path d="M11 5 6 9H3v6h3l5 4z"/><path d="M15 9a4 4 0 0 1 0 6M18 6a8 8 0 0 1 0 12"/></symbol>
    <symbol id="i-coin" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v10M9.5 9.5h4a1.8 1.8 0 0 1 0 3.6h-3a1.8 1.8 0 0 0 0 3.6h4"/></symbol>
    <symbol id="i-chat" viewBox="0 0 24 24"><path d="M21 12a8 8 0 0 1-8 8H7l-4 3V12a8 8 0 0 1 8-8h2a8 8 0 0 1 8 8z"/><path d="M9 11h6M9 15h4"/></symbol>
    <symbol id="i-send" viewBox="0 0 24 24"><path d="M21 3 3 10.5l6 2.5L21 3z"/><path d="M9 13v6l3-4 4-3"/><path d="M21 3 9 19"/></symbol>
    <symbol id="i-term" viewBox="0 0 24 24"><rect x="2.5" y="4" width="19" height="16" rx="2.5"/><path d="m7 10 3 2.5L7 15M13 15h4"/></symbol>
    <symbol id="i-search" viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><path d="m20 20-4.2-4.2"/></symbol>
    <symbol id="i-copy" viewBox="0 0 24 24"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></symbol>
    <symbol id="i-refresh" viewBox="0 0 24 24"><path d="M3 12a9 9 0 0 1 15.5-6.2L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-15.5 6.2L3 16"/><path d="M3 21v-5h5"/></symbol>
    <symbol id="i-power" viewBox="0 0 24 24"><path d="M12 3v9"/><path d="M7 6a8 8 0 1 0 10 0"/></symbol>
    <symbol id="i-dl" viewBox="0 0 24 24"><path d="M12 3v11"/><path d="m7 10 5 5 5-5"/><path d="M4 20h16"/></symbol>
    <symbol id="i-check" viewBox="0 0 24 24"><path d="M20 6 9 17l-5-5"/></symbol>
    <symbol id="i-alert" viewBox="0 0 24 24"><path d="M12 3 2.5 20h19z"/><path d="M12 9v5M12 17h.01"/></symbol>
    <symbol id="i-info" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></symbol>
    <symbol id="i-folder" viewBox="0 0 24 24"><path d="M3 7a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></symbol>
    <symbol id="i-cmd" viewBox="0 0 24 24"><rect x="9" y="9" width="6" height="6" rx="1"/><path d="M9 9H7a2 2 0 1 1 2-2zM15 9h2a2 2 0 1 0-2-2zM9 15H7a2 2 0 1 0 2 2zM15 15h2a2 2 0 1 1-2 2z"/></symbol>
    <symbol id="i-palette" viewBox="0 0 24 24"><path d="M12 3a9 9 0 1 0 0 18h1.5a2 2 0 0 0 0-4H13a2 2 0 0 1 0-4h5a3 3 0 0 0 3-3c0-4-4-7-9-7z"/><circle cx="7.5" cy="10.5" r="1"/><circle cx="10" cy="6.8" r="1"/><circle cx="15" cy="7.5" r="1"/></symbol>
    <symbol id="i-wifi-off" viewBox="0 0 24 24"><path d="M2 8.8a15 15 0 0 1 6-3.4M14 5.2a15 15 0 0 1 8 3.6"/><path d="M5 12.4a11 11 0 0 1 4-2.2M15 10.2a11 11 0 0 1 4 2.2"/><path d="M8.5 16a6 6 0 0 1 7 0"/><path d="M12 19.5h.01"/><path d="m2 3 19 18"/></symbol>
    <symbol id="i-trash" viewBox="0 0 24 24"><path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/></symbol>
    <symbol id="i-login" viewBox="0 0 24 24"><path d="M14 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/><path d="M10 8l4 4-4 4"/><path d="M14 12H3"/></symbol>
  </defs>
</svg>

<header>
  <div class="hd">
   <div class="hd-top">
    <div class="brand">
      <div class="mark" onclick="openPalette()" title="打开命令面板 (Ctrl+K)">
        <svg class="i" style="width:19px;height:19px"><use href="#i-cpu"/></svg>
        <span class="dotlive" id="markDot"></span>
      </div>
      <div>
        <div class="btitle">
          <h1>WORKBUDDY</h1>
          <span class="tag-mini">CONSOLE</span>
        </div>
        <div class="bmeta">
          <span class="copyable" onclick="copyText(location.host || '127.0.0.1', '控制台地址已复制')">
            <svg class="i" style="width:12px;height:12px"><use href="#i-globe"/></svg>
            <span id="hostLine">127.0.0.1</span>
          </span>
          <span class="sep hide-sm">|</span>
          <span class="hide-sm">IPC <strong>__IPC_PORT__</strong></span>
        </div>
      </div>
    </div>

    <div class="telemetry">
      <div class="tm ok"><svg class="i" style="width:12px;height:12px"><use href="#i-pulse"/></svg>轮询 <strong id="pingVal">--</strong></div>
      <div class="tm ac"><svg class="i" style="width:12px;height:12px"><use href="#i-batt"/></svg>电量 <strong id="battTop">--</strong></div>
      <div class="tm iv"><svg class="i" style="width:12px;height:12px"><use href="#i-clock"/></svg>已连接 <strong id="uptimeVal">00:00:00</strong></div>
    </div>

    <div class="hd-right">
      <div class="badge off" id="badge"><span class="bd" id="dot"></span><span id="btext">服务未启动</span></div>
      <button class="btn stop" id="btnToggle" onclick="toggle()">
        <svg class="i" style="width:13px;height:13px"><use href="#i-power"/></svg>
        <span id="btnToggleText">停止服务</span>
      </button>
    </div>
   </div>

   <div class="hd-bot">
      <button class="btn ghost sm" onclick="openPalette()" title="命令面板 Ctrl+K">
        <svg class="i" style="width:13px;height:13px"><use href="#i-cmd"/></svg>
        <span>快捷指令</span>
        <kbd class="kbd">Ctrl K</kbd>
      </button>

      <div class="seg" id="themeSeg" role="group" aria-label="主题切换">
        <button data-theme="graphite" class="act" onclick="setTheme('graphite')" title="石墨深灰">石墨</button>
        <button data-theme="nord" onclick="setTheme('nord')" title="极地冰蓝">冰蓝</button>
        <button data-theme="warm" onclick="setTheme('warm')" title="暖砂低蓝光">暖砂</button>
        <button data-theme="sage" onclick="setTheme('sage')" title="松林护眼">松林</button>
        <button data-theme="light" onclick="setTheme('light')" title="素雅浅色">浅色</button>
      </div>

      <span class="grow"></span>

      <label class="sw" title="开机自启">
        <span class="lbl">开机自启</span>
        <input type="checkbox" id="autostart" onchange="toggleAutostart()">
        <span class="track"></span>
      </label>
      <label class="sw" title="允许在设备上回答 WorkBuddy 的提问">
        <span class="lbl">设备作答</span>
        <input type="checkbox" id="devAnswer" onchange="toggleDeviceAnswer()">
        <span class="track"></span>
      </label>
      <label class="sw" title="暂停自动搜索/自动连接设备">
        <span class="lbl">暂停搜索</span>
        <input type="checkbox" id="searchPause" onchange="toggleSearchPause()">
        <span class="track"></span>
      </label>
   </div>
  </div>
</header>

<main>
  <!-- 第一层：四张指标卡 -->
  <section class="metrics">
    <!-- 1 设备终端 -->
    <div class="card metric">
      <div class="metric-hd">
        <div class="ibox"><svg class="i"><use href="#i-bt"/></svg></div>
        <span class="t">BLE 设备终端</span>
        <span class="r"><span class="chipst" id="conn"><span class="d"></span>--</span></span>
      </div>
      <div class="scan" id="scanBox">
        <div class="scan-top">
          <svg class="i" style="width:13px;height:13px"><use href="#i-wifi-off"/></svg>
          <span id="scanLabel">正在搜索设备…</span>
          <span class="t" id="scanTime"></span>
        </div>
        <div class="bar"><i id="scanBar"></i></div>
        <div class="sub" id="scanSub"></div>
      </div>
      <div class="kv"><span>电量</span><b id="batt">--%</b></div>
      <div class="kv"><span>设备地址</span><b id="addr">--</b></div>
      <div class="kv"><span>固件</span><b id="devFw">--</b></div>
      <div class="kv"><span>能力</span><b id="devCaps" title="设备在 hello 帧里声明的能力位">--</b></div>
      <div class="kv"><span>扫描</span><b id="scanState">--</b></div>
      <div class="metric-ft">
        <span class="mono" style="color:var(--dim)">AI Passport · ESP32-S3</span>
        <span class="chipst" id="connFoot">--</span>
      </div>
    </div>

    <!-- 2 网关 / 端口 -->
    <div class="card metric">
      <div class="metric-hd">
        <div class="ibox iv"><svg class="i"><use href="#i-net"/></svg></div>
        <span class="t">ACP 端口集群</span>
        <span class="r"><span class="chipst" id="gwState">--</span></span>
      </div>
      <div class="kv"><span>活会话</span><b id="liveCount">--</b></div>
      <div class="ports" id="portList"><div class="port empty">检测中…</div></div>
      <div class="metric-ft">
        <span class="mono" id="secretState">密钥 --</span>
        <span class="mono" id="cliState">CLI --</span>
      </div>
    </div>

    <!-- 3 提示音 -->
    <div class="card metric">
      <div class="metric-hd">
        <div class="ibox ok"><svg class="i"><use href="#i-spk"/></svg></div>
        <span class="t">提示音音量</span>
        <span class="r">
          <button class="btn sm ghost" onclick="setQuickVolume(0)">静音</button>
          <button class="btn sm" onclick="saveBeepVol()">应用</button>
        </span>
      </div>
      <div class="kv"><span>增益</span><b><span id="beepVolVal">--</span>%</b></div>
      <input id="beepVol" type="range" min="0" max="100" value="100" oninput="onBeepVolInput()" aria-label="提示音音量">
      <div class="metric-ft">
        <button class="btn sm" id="btnBeepTest" onclick="testBeep()">
          <svg class="i" style="width:12px;height:12px"><use href="#i-spk"/></svg>试听
        </button>
        <span class="mono" style="font-size:10.5px">存本机 · 重连自动下发</span>
      </div>
    </div>

    <!-- 4 积分 -->
    <div class="card metric">
      <div class="metric-hd">
        <div class="ibox"><svg class="i"><use href="#i-coin"/></svg></div>
        <span class="t">WorkBuddy 积分</span>
        <span class="r"><span class="chipst" id="creditState">--</span></span>
      </div>
      <div class="kv" style="align-items:flex-end">
        <span>剩余 / 总额</span>
        <b style="font-size:15px;color:var(--txt)"><span id="remain">--</span> <span style="color:var(--dim)">/</span> <span id="total" style="color:var(--muted)">--</span></b>
      </div>
      <div class="bar" id="usageWrap"><i id="usagebar"></i></div>
      <div class="kv" style="font-size:11px"><span id="usedPct">已消耗 --</span><span id="creditHint" class="hint" style="min-height:0"></span></div>
      <div class="metric-ft">
        <span class="mono" style="font-size:10.5px">每 2s 刷新</span>
        <button class="btn sm" id="btnLogin" onclick="doLogin()" style="display:none">
          <svg class="i" style="width:12px;height:12px"><use href="#i-login"/></svg>
          <span id="btnLoginText">一键登录</span>
        </button>
      </div>
    </div>
  </section>

  <!-- 第二层：双栏工作区 -->
  <div class="workspace">
    <div class="col">
      <!-- 会话列表 -->
      <section class="card">
        <div class="card-hd">
          <div class="ibox"><svg class="i"><use href="#i-chat"/></svg></div>
          <h2>会话列表</h2>
          <span class="cnt-badge" id="sessBadge">共 0 项</span>
          <div class="spacer"></div>
          <div class="seg" id="sessSeg" role="group" aria-label="会话筛选">
            <button data-f="ALL" class="act" onclick="filterSessions('ALL')">全部</button>
            <button data-f="RUNNING" onclick="filterSessions('RUNNING')">运行中</button>
            <button data-f="COMPLETED" onclick="filterSessions('COMPLETED')">已完成</button>
          </div>
        </div>
        <div id="sessions"></div>
        <div class="term-ft">
          <div class="g">
            <span>会话 <strong class="mono" id="cnt" style="color:var(--txt)">0</strong></span>
            <span class="vr"></span>
            <span>待答 <strong class="mono" id="ansCount" style="color:var(--accent2)">0</strong></span>
          </div>
          <div class="g"><span>服务进程 <strong class="mono" id="workerSum" style="color:var(--accent2)">--</strong></span></div>
        </div>
      </section>

      <!-- 设备发消息 -->
      <section class="card">
        <div class="card-hd">
          <div class="ibox"><svg class="i"><use href="#i-send"/></svg></div>
          <h2>设备发消息 · 投递</h2>
          <span class="chipst" id="wbLiveBadge">检测中…</span>
          <div class="spacer"></div>
          <button class="btn ghost sm" onclick="refreshWbLive()">
            <svg class="i" style="width:12px;height:12px"><use href="#i-refresh"/></svg>刷新焦点
          </button>
        </div>
        <div class="deliver">
          <div class="tpl-row">
            <span style="font-size:11px;color:var(--muted)">预设：</span>
            <button class="tpl" onclick="fillQuickPrompt('检查当前仓库未提交的代码改动并生成总结报告')">代码改动审查</button>
            <button class="tpl" onclick="fillQuickPrompt('分析最近一次构建日志中的异常堆栈并给出修复建议')">排查异常堆栈</button>
            <button class="tpl" onclick="fillQuickPrompt('为当前改动补充单元测试并跑通')">补单元测试</button>
            <span style="margin-left:auto;font-size:11px;color:var(--dim)" class="mono" id="wbCliTip"></span>
          </div>
          <div class="ta-wrap">
            <textarea id="wbText" placeholder="要投递给 WorkBuddy 的话（设备语音录入后会自动填入，这里可手工试验）" aria-label="投递内容"></textarea>
            <div class="ta-foot"><span id="charCount">0 字</span><span>·</span><span>Ctrl + Enter 投递</span></div>
          </div>
          <div class="deliver-actions">
            <div class="field">
              <label for="wbTarget">投递到</label>
              <select id="wbTarget" aria-label="投递目标">
                <option value="reply">回复当前活会话</option>
                <option value="job">新建任务</option>
              </select>
            </div>
            <div class="field">
              <svg class="i" style="width:13px;height:13px;color:var(--dim)"><use href="#i-folder"/></svg>
              <input type="text" id="wbJobCwd" placeholder="新任务目录（留空用默认）" onchange="onWbCwdChange()" aria-label="新任务工作目录">
            </div>
            <button class="btn pri" id="btnWbSend" onclick="wbSend()">
              <svg class="i" style="width:13px;height:13px"><use href="#i-send"/></svg>
              <span id="btnWbSendText">投递</span>
            </button>
          </div>
          <div id="wbResult" class="hint"></div>
        </div>
      </section>
    </div>

    <div class="col">
      <!-- 日志终端 -->
      <section class="card">
        <div class="term-hd">
          <div class="term-row">
            <div class="dots"><i></i><i></i><i></i></div>
            <div class="vr"></div>
            <div class="term-title">
              <svg class="i" style="width:13px;height:13px;color:var(--accent)"><use href="#i-term"/></svg>
              运行日志
              <span class="cnt-badge" id="logCount">--</span>
            </div>
            <div style="margin-left:auto;display:flex;align-items:center;gap:6px">
              <label class="sw" style="padding:3px 8px" title="新日志到达时自动滚动到底部">
                <span class="lbl">自动滚动</span>
                <input type="checkbox" id="autoScroll" checked onchange="renderLog()">
                <span class="track"></span>
              </label>
              <button class="btn ico ghost" onclick="copyLog()" title="复制全部日志" aria-label="复制全部日志">
                <svg class="i" style="width:13px;height:13px"><use href="#i-copy"/></svg>
              </button>
              <button class="btn ico ghost" onclick="exportLog()" title="导出日志文件" aria-label="导出日志">
                <svg class="i" style="width:13px;height:13px"><use href="#i-dl"/></svg>
              </button>
            </div>
          </div>
          <div class="term-row">
            <div class="fpills" role="group" aria-label="日志分类筛选">
              <button class="fpill act" data-filter="ALL" onclick="setLogFilter('ALL')">ALL</button>
              <button class="fpill" data-filter="web" onclick="setLogFilter('web')">WEB</button>
              <button class="fpill" data-filter="daemon" onclick="setLogFilter('daemon')">DAEMON</button>
              <button class="fpill" data-filter="app" onclick="setLogFilter('app')">APP</button>
            </div>
            <select class="mini" id="logLevel" onchange="renderLog()" aria-label="日志级别筛选">
              <option value="ALL">全部级别</option>
              <option value="warn">警告以上</option>
              <option value="err">仅错误</option>
            </select>
            <span class="search" style="flex:1;min-width:110px">
              <svg class="i" style="width:12px;height:12px;color:var(--dim)"><use href="#i-search"/></svg>
              <input type="text" id="logSearch" oninput="onLogSearch(this.value)" placeholder="过滤关键词…" aria-label="搜索日志内容">
            </span>
          </div>
        </div>
        <div class="term-body" id="log" tabindex="0" role="log" aria-live="off" aria-label="运行日志"></div>
        <div class="term-ft">
          <div class="g"><span class="live-dot"></span><span>IPC 通道 127.0.0.1:__IPC_PORT__</span></div>
          <div class="g"><span id="logTotal">缓冲 0 行</span><span class="sep">|</span><span>UTF-8</span></div>
        </div>
      </section>
    </div>
  </div>
</main>

<footer>
  <div class="g">
    <svg class="i" style="width:12px;height:12px"><use href="#i-cpu"/></svg>
    <span>WorkBuddy 控制台 · AIWK 本地服务</span>
    <span class="sep">|</span>
    <span class="mono">本地 / 无外网依赖</span>
  </div>
  <div class="g"><span id="footState">等待服务启动</span></div>
</footer>

<div id="toasts" class="toasts" role="status" aria-live="polite"></div>

<!-- 首次配对/首次连接引导 -->
<div id="modal" class="modal" role="dialog" aria-modal="true" aria-labelledby="fpTitle">
  <div class="modal-card">
    <div class="modal-ico"><svg class="i" style="width:20px;height:20px"><use href="#i-bt"/></svg></div>
    <h3 id="fpTitle">首次连接这台设备</h3>
    <p>控制台会在后台扫描并连接 WorkBuddy 硬件终端。请先确认设备已开机，并停留在 WorkBuddy 页面。</p>
    <div class="modal-addr" id="fpAddr">未识别地址</div>
    <p style="font-size:11.5px;color:var(--dim)">连接成功后本提示不再出现；换新设备时会重新询问。</p>
    <div class="modal-actions">
      <button class="btn ghost" onclick="closeModal()">稍后再说</button>
      <button class="btn pri" onclick="confirmConnect()">立即连接</button>
    </div>
  </div>
</div>

<!-- 命令面板 -->
<div id="palette" class="palette" role="dialog" aria-modal="true" aria-label="命令面板">
  <div class="palette-card">
    <div class="palette-in">
      <svg class="i" style="width:15px;height:15px;color:var(--accent)"><use href="#i-search"/></svg>
      <input id="paletteInput" type="text" placeholder="输入指令或关键词…" oninput="renderPalette(this.value)" autocomplete="off">
      <kbd class="kbd">ESC</kbd>
    </div>
    <div class="palette-list" id="paletteList"></div>
    <div class="palette-ft"><span>↑↓ 选择</span><span>Enter 执行</span><span>Ctrl K 开关面板</span></div>
  </div>
</div>
<script>
"use strict";
const $ = id => document.getElementById(id);
const $$ = sel => Array.from(document.querySelectorAll(sel));

// 日志增量判据：后端 /api/log 返回 {seq, lines}，seq 单调递增。
// 不能用 lines.length —— 环形缓冲(deque maxlen)写满后长度恒定不变，会永久冻结。
let lastLogSeq = -1;
let lastLogLines = [];
let daemonRunning = false;
let firstPairDismissed = false;
const SCAN_TIMEOUT_SEC = 20;
const RESCAN_INTERVAL_SEC = 25;

const ST_COLOR = {RUNNING:'#22d3ee', COMPLETED:'#34d399', IDLE:'#94a3b8',
                  ERROR:'#fb7185', NEED_APPROVAL:'#fbbf24', NEED_ANSWER:'#818cf8'};
const ST_LABEL = {RUNNING:'正在执行', COMPLETED:'已完成', IDLE:'空闲',
                  ERROR:'出错', NEED_APPROVAL:'待审批', NEED_ANSWER:'待回答'};
const TAG_CLASS = {web:'web', daemon:'daemon', app:'app', tray:'tray'};

let lastSessions = [];
// 会话总数（status_list.count 的新语义）。设备未声明 caps bit3 时恒等于
// 下发条数，前端据此决定要不要显示「共 N 项 · 显示 M」。
let lastSessCount = 0;
let sessionFilter = 'ALL';
let connectedSince = 0;      // 本次「已连接」起始时刻（ms），断开清零
let pingMs = null;

function fmtCredit(v){ const x = Number(v||0); return x.toLocaleString('zh-CN',{maximumFractionDigits:2}); }
function esc(s){
  const m={'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'};
  return String(s==null?'':s).replace(/[&<>"']/g, c=>m[c]||c);
}
function svg(id, size){
  const s = size || 15;
  return '<svg class="i" style="width:'+s+'px;height:'+s+'px"><use href="#'+id+'"/></svg>';
}

/* ---------------- 主题 ---------------- */
const THEMES = ['graphite','nord','warm','sage','light'];
function setTheme(name){
  if(THEMES.indexOf(name) < 0) name = 'graphite';
  document.documentElement.setAttribute('data-theme', name);
  try{ localStorage.setItem('aiwk-theme', name); }catch(e){}
  $$('#themeSeg button').forEach(b=>b.classList.toggle('act', b.getAttribute('data-theme') === name));
}
function initTheme(){
  let name = 'graphite';
  try{ name = localStorage.getItem('aiwk-theme') || 'graphite'; }catch(e){}
  setTheme(name);
}

/* ---------------- Toast ---------------- */
function toast(msg, type){
  const box = $('toasts');
  if(!box) return;
  while(box.children.length >= 3) box.removeChild(box.firstChild);
  const el = document.createElement('div');
  el.className = 'toast' + (type ? ' ' + type : '');
  const icon = type === 'ok' ? 'i-check' : (type === 'err' ? 'i-alert' : (type === 'warn' ? 'i-alert' : 'i-info'));
  el.innerHTML = '<span class="ti">' + svg(icon, 15) + '</span><span>' + esc(msg) + '</span>';
  box.appendChild(el);
  requestAnimationFrame(()=>el.classList.add('show'));
  setTimeout(()=>{
    el.classList.remove('show');
    setTimeout(()=>{ if(el.parentNode) el.parentNode.removeChild(el); }, 300);
  }, 3000);
}

/* ---------------- 剪贴板（局域网 http 下 clipboard API 不可用，需回退） ---------------- */
async function copyText(text, okMsg){
  try{
    if(navigator.clipboard && navigator.clipboard.writeText && window.isSecureContext){
      await navigator.clipboard.writeText(text);
    }else{
      const ta = document.createElement('textarea');
      ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
      document.body.appendChild(ta); ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    }
    toast(okMsg || '已复制', 'ok');
  }catch(e){ toast('复制失败，请手动选中复制', 'err'); }
}

/* ---------------- 日志 ---------------- */
let logFilter = 'ALL';
let logQuery = '';

function parseLog(line){
  const s = String(line == null ? '' : line);
  let m = s.match(/^\[(\d{2}:\d{2}:\d{2})\]\s*\[([A-Za-z]+)\]\s*([\s\S]*)$/);
  if(m) return {time:m[1], tag:m[2].toLowerCase(), text:m[3]};
  m = s.match(/^\[(\d{2}:\d{2}:\d{2})\]\s*([\s\S]*)$/);
  if(m) return {time:m[1], tag:'', text:m[2]};
  return {time:'', tag:'', text:s};
}
function levelOf(text){
  if(/失败|错误|异常|超时|断开|error|failed|timeout|exception/i.test(text)) return 'err';
  if(/警告|重试|未找到|未配置|暂停|warn/i.test(text)) return 'warn';
  return '';
}

function renderLog(){
  const box = $('log');
  if(!box) return;
  const levelSel = $('logLevel') ? $('logLevel').value : 'ALL';
  const all = lastLogLines.map(parseLog);
  const items = all.filter(x=>{
    const okTag = (logFilter === 'ALL') || (x.tag === logFilter.toLowerCase());
    const okQ = !logQuery || x.text.toLowerCase().includes(logQuery) || x.tag.includes(logQuery);
    const lv = levelOf(x.text);
    const okLv = levelSel === 'ALL'
      || (levelSel === 'warn' && (lv === 'warn' || lv === 'err'))
      || (levelSel === 'err' && lv === 'err');
    return okTag && okQ && okLv;
  });
  if($('logCount')) $('logCount').textContent = items.length + ' 条';
  if($('logTotal')) $('logTotal').textContent = '缓冲 ' + all.length + ' 行 · 显示 ' + items.length;

  if(items.length === 0){
    box.innerHTML = '<div class="term-empty">'
      + (all.length === 0 ? '暂无日志输出' : '暂无符合筛选条件的日志')
      + '</div>';
    return;
  }
  box.innerHTML = items.map(x=>{
    const cls = levelOf(x.text);
    const tc = TAG_CLASS[x.tag] || '';
    const tagHtml = x.tag ? '<span class="lg-tag ' + tc + '">' + esc(x.tag) + '</span>' : '';
    return '<div class="lg">'
      + '<span class="lg-t">' + (x.time ? '[' + esc(x.time) + ']' : '') + '</span>'
      + tagHtml
      + '<span class="lg-m' + (cls ? ' ' + cls : '') + '">' + esc(x.text) + '</span>'
      + '</div>';
  }).join('');
  const auto = $('autoScroll');
  if(!auto || auto.checked) box.scrollTop = box.scrollHeight;
}

function setLogFilter(f){
  logFilter = f;
  $$('.fpill').forEach(b=>b.classList.toggle('act', b.getAttribute('data-filter') === f));
  renderLog();
}
function onLogSearch(v){ logQuery = String(v||'').trim().toLowerCase(); renderLog(); }

async function copyLog(){
  if(!lastLogLines.length){ toast('暂无日志可复制'); return; }
  await copyText(lastLogLines.join('\n'), '已复制 ' + lastLogLines.length + ' 行日志');
}

function exportLog(){
  if(!lastLogLines.length){ toast('暂无日志可导出', 'warn'); return; }
  const d = new Date();
  const pad = n => String(n).padStart(2,'0');
  const name = 'workbuddy-log-' + d.getFullYear() + pad(d.getMonth()+1) + pad(d.getDate())
             + '-' + pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds()) + '.txt';
  const blob = new Blob([lastLogLines.join('\n')], {type:'text/plain;charset=utf-8'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a); a.click();
  setTimeout(()=>{ URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  toast('已导出 ' + name, 'ok');
}

/* ---------------- 会话 ---------------- */
function filterSessions(f){
  sessionFilter = f;
  $$('#sessSeg button').forEach(b=>b.classList.toggle('act', b.getAttribute('data-f') === f));
  renderSessions(lastSessions);
}

function renderSessions(list){
  lastSessions = Array.isArray(list) ? list : [];
  const box = $('sessions');
  if(!box) return;
  const n = lastSessions.length;
  // 这两个数字含义不同，界面上必须说清：
  //   n        = 正在推给设备的条数（设备最多显示 3 条）
  //   sessTotal = 会话总数（来自 status_list.count 的新语义）
  // 老固件不改语义时 sessTotal 恒等于 n，此时不显示总数，免得写成「共 3 / 共 3」。
  const total = Number(lastSessCount) || 0;
  if($('sessBadge')) $('sessBadge').textContent = (total > n)
    ? ('共 ' + total + ' 项 · 显示 ' + n)
    : ('共 ' + n + ' 项');
  if($('cnt')) $('cnt').textContent = n;
  if($('ansCount')) $('ansCount').textContent = lastSessions.filter(t=>t.msg).length;
  // daemon 给每条会话写的 worker 都是「当前发现的 worker 端口总数」（同一个值），
  // 相加会把 2 个端口 ×3 条会话算成 ×6，取最大值才是真实端口数。
  if($('workerSum')) $('workerSum').textContent = n
    ? '×' + lastSessions.reduce((s,t)=>Math.max(s, Number(t.worker)||0), 0)
    : '--';

  const shown = lastSessions.filter(t=> sessionFilter === 'ALL' || t.state === sessionFilter);

  if(n === 0){
    box.innerHTML = '<div class="empty-state">'
      + '<div class="ico2">' + svg('i-chat', 22) + '</div>'
      + '<h4>暂无活动会话</h4>'
      + '<p>当前没有正由 WorkBuddy 调度的任务。在桌面端发起任务后会在这里自动出现。</p>'
      + '</div>';
    return;
  }
  if(shown.length === 0){
    box.innerHTML = '<div class="empty-state"><h4>没有符合条件的会话</h4>'
      + '<p>换个筛选条件试试，或选「全部」。</p></div>';
    return;
  }

  let rows = '';
  shown.forEach((t,i)=>{
    // 颜色只从白名单取，其余字段一律转义 —— 它们来自转录文本，不可信。
    const c = ST_COLOR[t.state] || '#22d3ee';
    const label = ST_LABEL[t.state] || t.state || '--';
    const w = Number(t.worker) || 0;
    rows += '<tr>'
      + '<td><div class="idc">'
      +   '<span class="led" style="background:' + c + ';color:' + c + '"></span>'
      +   '<div><div class="sid">#' + String(i+1).padStart(2,'0') + '</div>'
      +   '<div class="sname" title="' + esc(t.name || 'task') + '">' + esc(t.name || 'task') + '</div></div>'
      + '</div></td>'
      + '<td><span class="chip">' + esc(t.model || '--') + '</span></td>'
      + '<td class="mono-cell">' + fmtCredit(t.credit) + '</td>'
      + '<td><span class="st' + (t.state === 'RUNNING' ? ' live' : '') + '" style="color:' + c
      +   ';background:' + c + '1f;border-color:' + c + '4d"><span class="d"></span>' + esc(label) + '</span></td>'
      + '<td class="mono-cell" style="color:' + (t.msg ? '#22d3ee' : 'var(--dim)') + '">'
      +   '<span title="' + (t.msg ? esc(t.msg) : '') + '">' + (t.msg ? '待答' : '—') + '</span></td>'
      + '</tr>';
  });
  box.innerHTML = '<div class="tw"><table><thead><tr>'
    + '<th>会话 / 任务</th><th>模型</th><th>积分</th><th>状态</th><th>待办</th>'
    + '</tr></thead><tbody>' + rows + '</tbody></table></div>';
}

/* ---------------- 顶部状态 ---------------- */
function renderTopState(d){
  const on = !!d.running;
  const conn = on && d.connected === true;
  daemonRunning = on;

  const badge = $('badge');
  badge.className = 'badge ' + (on ? (conn ? 'on' : 'mid') : 'off');
  $('btext').textContent = on
    ? (conn ? '设备已连接' : '服务运行中 · 设备未连接')
    : '服务未启动';
  const md = $('markDot');
  if(md) md.classList.toggle('on', conn);

  const btn = $('btnToggle');
  btn.className = 'btn ' + (on ? 'stop' : 'start');
  $('btnToggleText').textContent = on ? '停止服务' : '启动服务';

  const cs = $('conn');
  cs.className = 'chipst ' + (conn ? 'ok' : 'er');
  cs.innerHTML = '<span class="d"></span>' + (conn ? '在线' : '离线');

  const cf = $('connFoot');
  if(cf){
    cf.className = 'chipst ' + (conn ? 'ok' : (on ? 'wn' : ''));
    cf.textContent = conn ? '链路正常' : (on ? '等待设备' : '服务未启动');
  }
  if($('footState')) $('footState').textContent = on
    ? (conn ? '设备已连接 · 运行中' : '服务运行中 · 设备未连接')
    : '等待服务启动';
  if($('liveTag')){
    $('liveTag').textContent = conn ? '链路正常' : (on ? '等待设备' : '未启动');
    $('liveTag').style.color = conn ? 'var(--ok)' : (on ? 'var(--warn)' : 'var(--dim)');
  }

  // 已连接时长：连接态开始计时，断开清零
  if(conn){
    if(!connectedSince) connectedSince = Date.now();
  }else{
    connectedSince = 0;
    if($('uptimeVal')) $('uptimeVal').textContent = '00:00:00';
  }
}

function tickUptime(){
  if(!connectedSince || !$('uptimeVal')) return;
  const s = Math.max(0, Math.floor((Date.now() - connectedSince)/1000));
  const pad = n => String(n).padStart(2,'0');
  $('uptimeVal').textContent = pad(Math.floor(s/3600)) + ':' + pad(Math.floor(s%3600/60)) + ':' + pad(s%60);
}

function renderScan(d){
  const box = $('scanBox');
  if(!box) return;
  const scanning = d.scanning === true;
  const rescanRemain = Number(d.rescan_remaining) || 0;
  const paused = d.search_paused === true;
  const show = !!d.running && !d.connected && (scanning || rescanRemain > 0 || paused);
  box.classList.toggle('show', show);
  // 卡片里的「扫描」行始终反映真实状态，不必展开提示区才能看到
  if($('scanState')){
    $('scanState').textContent = d.connected ? '已连接'
      : (paused ? '已暂停' : (scanning ? '搜索中 ' + Math.round(Number(d.scan_elapsed)||0) + 's'
      : (rescanRemain > 0 ? '等待重试 ' + Math.ceil(rescanRemain) + 's' : '空闲')));
  }
  if(!show) return;
  const label = $('scanLabel'), timeEl = $('scanTime'), bar = $('scanBar'), sub = $('scanSub');
  if(paused){
    label.textContent = '已暂停自动连接';
    timeEl.textContent = '';
    bar.style.width = '0%';
    sub.textContent = '设备不会被自动连接；关闭顶部「暂停搜索」可恢复。';
  }else if(scanning){
    const el = Number(d.scan_elapsed) || 0;
    label.textContent = '正在搜索设备…';
    timeEl.textContent = Math.round(el) + 's';
    bar.style.width = Math.min(100, Math.round(el/SCAN_TIMEOUT_SEC*100)) + '%';
    sub.textContent = '请确认设备已开机并停在 WorkBuddy 页面；最长约 45 秒。';
  }else if(rescanRemain > 0){
    const wait = Math.ceil(rescanRemain);
    label.textContent = '未找到设备，准备重试';
    timeEl.textContent = wait + 's';
    bar.style.width = Math.max(0, Math.min(100, Math.round(rescanRemain/RESCAN_INTERVAL_SEC*100))) + '%';
    sub.textContent = '将自动重试；确认设备已开机、蓝牙可用。';
  }
}

// 弱网下一轮 2s 可能还没回来，重入会让早发出的旧响应覆盖新状态。
let statusBusy = false;
let logBusy = false;

async function refreshStatus(){
  if(statusBusy) return;
  statusBusy = true;
  const t0 = performance.now();
  try{
    const r = await fetch('/api/status');
    const d = await r.json();
    pingMs = Math.max(0, Math.round(performance.now() - t0));
    if($('pingVal')) $('pingVal').textContent = pingMs + 'ms';

    renderTopState(d);
    if($('addr')) $('addr').textContent = d.address || '--';
    // 固件版本 / 能力位来自设备的 hello 握手帧。caps 为 null 表示还没握手
    // （老固件不发 hello，或刚连上还没收到），要和"握手了但没有任何能力"
    // 区分开——后者才说明固件功能不全。
    if($('devFw')) $('devFw').textContent = d.device_fw
      ? (d.device_fw + (d.device_proto ? ' / p' + d.device_proto : ''))
      : '--';
    if($('devCaps')){
      const caps = d.device_caps;
      $('devCaps').textContent = (caps === null || caps === undefined)
        ? '未握手' : d.device_caps_text;
      $('devCaps').style.color = (caps === null || caps === undefined)
        ? 'var(--dim)' : 'var(--ok)';
    }
    if($('batt')) $('batt').textContent = (d.battery != null) ? d.battery + '%' : '--%';
    if($('battTop')) $('battTop').textContent = (d.battery != null) ? d.battery + '%' : '--';

    // ---- 积分 ----
    const c = d.credit;
    const hint = $('creditHint');
    if(c && c.total){
      $('remain').textContent = Math.round(c.remain).toLocaleString();
      $('total').textContent = Math.round(c.total).toLocaleString();
      const used = Math.max(0, c.total - c.remain);
      const pct = c.total > 0 ? Math.round(used/c.total*100) : 0;
      $('usagebar').style.width = Math.min(100, pct) + '%';
      if($('usageWrap')) $('usageWrap').className = 'bar' + (pct >= 80 ? ' er' : (pct >= 55 ? ' wn' : ''));
      if($('usedPct')) $('usedPct').textContent = '已消耗 ' + pct + '%';
      const cst = $('creditState');
      if(cst){
        const left = 100 - pct;
        cst.className = 'chipst ' + (left >= 50 ? 'ok' : (left >= 20 ? 'wn' : 'er'));
        cst.textContent = left >= 50 ? '储备充足' : (left >= 20 ? '注意余额' : '余额偏低');
      }
      if(hint){ hint.className = 'hint'; hint.textContent = ''; }
    }else{
      $('remain').textContent='--'; $('total').textContent='--'; $('usagebar').style.width='0%';
      if($('usageWrap')) $('usageWrap').className = 'bar';
      if($('usedPct')) $('usedPct').textContent = '已消耗 --';
      if($('creditState')){ $('creditState').className = 'chipst'; $('creditState').textContent = '未同步'; }
      if(hint){
        if(d.login_state === 'running' && d.login_msg){
          hint.textContent = d.login_msg; hint.className = 'hint warn';
        }else if(d.login_state === 'error' && d.login_msg){
          hint.textContent = d.login_msg; hint.className = 'hint err';
        }else{
          hint.textContent = d.credit_reason || '';
          hint.className = 'hint' + (d.credit_reason ? ' warn' : '');
        }
      }
    }
    updateLoginBtn(d);

    // ---- 开关与音量 ----
    $('autostart').checked = !!d.autostart;
    $('devAnswer').checked = d.device_answer !== false;
    $('searchPause').checked = !!d.search_paused;
    // 滑块：拖动/保存后 2.5s 内不用轮询值回写，否则拖到一半会被打回原值。
    if(Date.now() - beepVolHold > 2500){
      const bv = (d.beep_vol == null) ? 100 : d.beep_vol;
      $('beepVol').value = bv;
      $('beepVolVal').textContent = bv;
    }
    $('btnBeepTest').disabled = !d.connected;

    lastSessCount = Number(d.sess_total) || 0;
    renderSessions(d.sessions);
    renderScan(d);

    // 首次连接引导：服务在跑、设备没连、且这台设备没记录过 → 自动弹一次
    if(d.connected){
      $('modal').classList.remove('show');
    }else if(!firstPairDismissed && d.running && d.first_pair === true){
      $('fpAddr').textContent = d.address || '未识别地址';
      $('modal').classList.add('show');
    }
  }catch(e){
    if($('pingVal')) $('pingVal').textContent = '超时';
  }finally{
    statusBusy = false;
  }
}

async function refreshLog(){
  if(logBusy) return;
  logBusy = true;
  try{
    const r = await fetch('/api/log');
    const d = await r.json();
    if(d.seq === lastLogSeq) return;     // 环形缓冲满载后长度恒定，只能按 seq 判增量
    lastLogSeq = d.seq;
    lastLogLines = Array.isArray(d.lines) ? d.lines : [];
    renderLog();
  }catch(e){}finally{
    logBusy = false;
  }
}

/* ---------------- 服务控制 ---------------- */
function doStartStop(running){
  const btn = $('btnToggle');
  btn.disabled = true;
  fetch('/api/' + (running ? 'stop' : 'start'), {method:'POST'})
    .then(r=>r.json()).catch(()=>{})
    .finally(()=>{ btn.disabled = false; setTimeout(refreshStatus, 300); });
}

async function toggle(){
  const running = daemonRunning;      // 用状态变量，不用按钮文案反推
  if(!running){
    try{
      const d = await (await fetch('/api/status')).json();
      if(!firstPairDismissed && d.first_pair === true){
        $('fpAddr').textContent = d.address || '未识别地址';
        $('modal').classList.add('show');
        return;
      }
    }catch(e){}
  }
  doStartStop(running);
}

function closeModal(){ $('modal').classList.remove('show'); }

async function confirmConnect(){
  firstPairDismissed = true;
  closeModal();
  try{
    const d = await (await fetch('/api/status')).json();
    if(d.running){ return; }           // 默认自动启动时服务已在跑，不重复 start
  }catch(e){}
  doStartStop(false);
}

async function toggleAutostart(){
  const on = $('autostart').checked;
  try{
    await fetch('/api/autostart', {method:'POST', body:JSON.stringify({on:on})});
    toast(on ? '已开启开机自启' : '已关闭开机自启', 'ok');
  }catch(e){ toast('设置失败，请稍后重试', 'err'); }
}

function updateLoginBtn(d){
  const btn = $('btnLogin');
  if(!btn) return;
  const txt = $('btnLoginText');
  const needLogin = !(d.credit && d.credit.total);
  if(d.login_state === 'running'){
    btn.style.display = ''; btn.disabled = true;
    if(txt) txt.textContent = '登录中…请在浏览器完成授权';
  }else if(needLogin){
    btn.style.display = ''; btn.disabled = false;
    if(txt) txt.textContent = d.login_state === 'done' ? '重新登录' : '一键登录';
  }else{
    btn.style.display = 'none';
  }
}

async function doLogin(){
  const btn = $('btnLogin');
  btn.disabled = true;
  const txt = $('btnLoginText'); if(txt) txt.textContent = '登录中…';
  try{
    const r = await fetch('/api/login', {method:'POST'});
    const d = await r.json();
    if(d.ok === false) toast('登录已在进行中，请先完成浏览器授权', 'warn');
  }catch(e){
    toast('无法发起登录，请稍后重试', 'err');
  }
}

async function toggleDeviceAnswer(){
  const on = $('devAnswer').checked;
  try{
    await fetch('/api/device-answer', {method:'POST', body:JSON.stringify({on:on})});
    toast(on ? '已允许设备端作答' : '已关闭设备端作答', 'ok');
  }catch(e){ toast('设置失败，请稍后重试', 'err'); }
}

/* ---------------- 提示音 ---------------- */
let beepVolHold = 0;

function onBeepVolInput(){
  beepVolHold = Date.now();
  $('beepVolVal').textContent = $('beepVol').value;
}

function setQuickVolume(v){
  $('beepVol').value = v;
  beepVolHold = Date.now();
  $('beepVolVal').textContent = v;
  saveBeepVol();
}

async function saveBeepVol(){
  const vol = parseInt($('beepVol').value, 10) || 0;
  beepVolHold = Date.now();
  $('beepVolVal').textContent = vol;
  try{
    const r = await fetch('/api/beep-volume', {method:'POST', body:JSON.stringify({vol:vol})});
    const d = await r.json();
    if(!d.ok) toast('设备未连接，音量已保存，连上后自动生效', 'warn');
    else toast(vol === 0 ? '提示音已静音' : '提示音音量 ' + d.vol, 'ok');
  }catch(e){ toast('设置失败，请稍后重试', 'err'); }
}

async function testBeep(){
  if((parseInt($('beepVol').value, 10) || 0) === 0){
    toast('当前音量为 0（静音），试听不会出声', 'warn'); return;
  }
  try{
    const r = await fetch('/api/test-beep', {method:'POST'});
    const d = await r.json();
    toast(d.ok ? '已发送试听，请留意设备' : '设备未连接，无法试听', d.ok ? 'ok' : 'err');
  }catch(e){ toast('设置失败，请稍后重试', 'err'); }
}

async function toggleSearchPause(){
  const on = $('searchPause').checked;
  try{
    await fetch('/api/search-pause', {method:'POST', body:JSON.stringify({on:on})});
    toast(on ? '已暂停自动连接' : '已恢复自动连接', 'ok');
  }catch(e){ toast('设置失败，请稍后重试', 'err'); }
  setTimeout(refreshStatus, 300);
}

/* ---------------- 投递 ---------------- */
let wbLive = [];

function wbLiveText(){
  if(!wbLive.length) return '网关在线 · 无活会话';
  const s = wbLive[0];
  return '活会话 ' + String(s.sessionId||'').slice(0,8) + ' · 端口 ' + s.port;
}

function renderPorts(gw, liveCount){
  const box = $('portList');
  if(!box) return;
  if($('liveCount')) $('liveCount').textContent = (typeof liveCount === 'number') ? (liveCount + ' 个') : '--';
  const ports = (gw && gw.ports) || [];
  if(!ports.length){
    box.innerHTML = '<div class="port empty">未发现 WorkBuddy 端口</div>';
  }else{
    const shown = ports.slice(0, 4);
    const online = !!(gw && gw.online);
    let html = shown.map(p=>'<div class="port"><span class="p">:' + esc(p) + '</span>'
      + '<span class="s' + (online ? '' : ' off') + '" title="' + (online ? '监听中' : '网关未运行') + '"></span></div>').join('');
    if(ports.length > shown.length){
      html += '<div class="port empty">另有 ' + (ports.length - shown.length) + ' 个端口未列出</div>';
    }
    box.innerHTML = html;
  }
  const st = $('gwState');
  if(st){
    const online = !!(gw && gw.online);
    st.className = 'chipst ' + (online ? 'ok' : 'er');
    st.textContent = online ? '网关在线' : '网关未运行';
  }
  if($('secretState')) $('secretState').textContent = (gw && gw.secret) ? '密钥 ✓' : '密钥 ✗';
  if($('cliState')) $('cliState').textContent = (gw && gw.cli) ? 'CLI ✓' : 'CLI ✗';
}

async function refreshWbLive(){
  try{
    const d = await (await fetch('/api/wb-live')).json();
    wbLive = d.sessions || [];
    const gw = d.gateway || {};
    renderPorts(gw, wbLive.length);

    const el = $('wbLiveBadge');
    if(!gw.online){
      el.className = 'chipst er';
      el.textContent = '网关未运行' + (gw.count ? '（' + gw.count + ' 个端口无密钥）' : '') + ' · 投不出去';
    }else if(wbLive.length){
      el.className = 'chipst ok';
      el.textContent = wbLiveText();
    }else{
      el.className = 'chipst wn';
      el.textContent = '网关在线 · 无活会话';
    }
    const cwd = $('wbJobCwd');
    if(cwd && !cwd.value){
      let local = '';
      try{ local = localStorage.getItem('aiwk-job-cwd') || ''; }catch(e){}
      const v = local || (d.job && d.job.cwd) || '';
      if(v) cwd.value = v;
    }
    if($('wbCliTip')) $('wbCliTip').textContent = !d.cli ? '未找到 WorkBuddy CLI'
      : (gw.online ? '网关在线' : 'CLI 兜底需登录');
  }catch(e){
    // 接口失败必须显式回写，否则占位文案「检测中…」会一直挂在那儿。
    const st = $('gwState');
    if(st){ st.className = 'chipst er'; st.textContent = '读取失败'; }
    const box = $('portList');
    if(box) box.innerHTML = '<div class="port empty">状态读取失败 · 请点刷新重试</div>';
    const el = $('wbLiveBadge');
    if(el){ el.className = 'chipst er'; el.textContent = '网关状态读取失败'; }
  }
}

function wbShow(res){
  const el = $('wbResult');
  el.className = 'hint' + (res.ok ? ' ok' : ' err');
  const bits = [res.kind === 'job' ? '新建任务' : '回复活会话', res.ok ? '成功' : '失败'];
  if(res.ok){
    if(res.sessionId) bits.push('会话 ' + String(res.sessionId).slice(0,8));
    if(res.jobId) bits.push('任务 ' + res.jobId);
    if(res.port) bits.push('端口 ' + res.port);
  }else{
    bits.push(res.err || '未知错误');
  }
  el.textContent = bits.join(' · ');
}

// 目录只在本地记住，不单独发请求：/api/wb-deliver 的 target=job 分支会先存目录再派发，
// 为了「存目录」去打一次会把占位文字真的当任务提交（review 修复）。
// 真正投递时 cwd 随 body 一起走，后端 target=job 时顺带把它记进配置。
function onWbCwdChange(){
  const v = $('wbJobCwd').value.trim();
  try{
    if(v) localStorage.setItem('aiwk-job-cwd', v);
    else localStorage.removeItem('aiwk-job-cwd');
    toast(v ? '新任务目录已记住，下次投递生效' : '已改为使用后端默认目录', 'ok');
  }catch(e){}
}

async function wbSend(){
  const text = $('wbText').value.trim();
  if(!text){ toast('先输入要投递的内容', 'err'); return; }
  const btn = $('btnWbSend');
  btn.disabled = true; $('btnWbSendText').textContent = '投递中…';
  try{
    const r = await fetch('/api/wb-deliver', {method:'POST', body:JSON.stringify({
      text:text, target:$('wbTarget').value, cwd:$('wbJobCwd').value.trim()})});
    const d = await r.json();
    wbShow(d);
    toast(d.ok ? '投递成功' : ('投递失败：' + (d.err || '')), d.ok ? 'ok' : 'err');
    refreshWbLive();
  }catch(e){
    wbShow({ok:false, err:'请求失败，请确认控制台服务在运行'});
    toast('投递失败', 'err');
  }finally{
    btn.disabled = false; $('btnWbSendText').textContent = '投递';
  }
}

function fillQuickPrompt(text){
  $('wbText').value = text;
  updateCharCount();
  $('wbText').focus();
}

function updateCharCount(){
  const n = ($('wbText').value || '').length;
  if($('charCount')) $('charCount').textContent = n + ' 字';
}

/* ---------------- 命令面板 ---------------- */
let paletteItems = [];
let paletteActive = 0;

function consoleCommands(){
  return [
    {title: daemonRunning ? '停止服务' : '启动服务', tag:'服务', run: ()=>toggle()},
    {title:'一键登录 WorkBuddy', tag:'账号', run: ()=>doLogin()},
    {title:'刷新活会话与端口', tag:'设备', run: ()=>refreshWbLive()},
    {title:'立即刷新状态', tag:'数据', run: ()=>{ refreshStatus(); refreshLog(); }},
    {title:( $('autostart').checked ? '关闭' : '开启') + '开机自启', tag:'设备', run: ()=>{ $('autostart').checked = !$('autostart').checked; toggleAutostart(); }},
    {title:'提示音静音', tag:'设备', run: ()=>setQuickVolume(0)},
    {title:'试听提示音', tag:'设备', run: ()=>testBeep()},
    {title:'跳到日志搜索', tag:'日志', run: ()=>{ closePalette(); $('logSearch').focus(); }},
    {title:'复制全部日志', tag:'日志', run: ()=>copyLog()},
    {title:'导出日志文件', tag:'日志', run: ()=>exportLog()},
    {title:'重置日志筛选', tag:'日志', run: ()=>{ setLogFilter('ALL'); $('logSearch').value=''; onLogSearch(''); $('logLevel').value='ALL'; renderLog(); }},
    {title:'主题：石墨深灰', tag:'外观', run: ()=>setTheme('graphite')},
    {title:'主题：极地冰蓝', tag:'外观', run: ()=>setTheme('nord')},
    {title:'主题：暖砂低蓝光', tag:'外观', run: ()=>setTheme('warm')},
    {title:'主题：松林护眼', tag:'外观', run: ()=>setTheme('sage')},
    {title:'主题：素雅浅色', tag:'外观', run: ()=>setTheme('light')}
  ];
}

function openPalette(){
  $('palette').classList.add('show');
  const inp = $('paletteInput');
  inp.value = ''; paletteActive = 0;
  renderPalette('');
  setTimeout(()=>inp.focus(), 10);
}
function closePalette(){ $('palette').classList.remove('show'); }

function renderPalette(q){
  const kw = String(q||'').trim().toLowerCase();
  paletteItems = consoleCommands().filter(c=>
    !kw || c.title.toLowerCase().includes(kw) || c.tag.toLowerCase().includes(kw));
  if(paletteActive >= paletteItems.length) paletteActive = 0;
  const box = $('paletteList');
  if(!paletteItems.length){
    box.innerHTML = '<div class="term-empty">没有匹配的指令</div>';
    return;
  }
  box.innerHTML = paletteItems.map((c,i)=>
    '<div class="pitem' + (i === paletteActive ? ' act' : '') + '" onclick="runPalette(' + i + ')"'
    + ' onmouseenter="paletteActive=' + i + '">'
    + '<span style="font-size:10px;color:var(--dim);font-family:var(--mono);width:34px">' + esc(c.tag) + '</span>'
    + '<span>' + esc(c.title) + '</span>'
    + '<span class="hint-k">Enter</span></div>').join('');
}

function runPalette(i){
  const c = paletteItems[i];
  closePalette();
  if(c) setTimeout(()=>c.run(), 30);
}

/* ---------------- 快捷键 ---------------- */
document.addEventListener('keydown', e=>{
  const paletteOpen = $('palette').classList.contains('show');
  if((e.ctrlKey || e.metaKey) && (e.key === 'k' || e.key === 'K')){
    e.preventDefault();
    if(paletteOpen) closePalette(); else openPalette();
    return;
  }
  if(paletteOpen){
    if(e.key === 'Escape'){ e.preventDefault(); closePalette(); }
    else if(e.key === 'ArrowDown'){
      e.preventDefault();
      paletteActive = Math.min(paletteItems.length - 1, paletteActive + 1);
      renderPalette($('paletteInput').value);
    }else if(e.key === 'ArrowUp'){
      e.preventDefault();
      paletteActive = Math.max(0, paletteActive - 1);
      renderPalette($('paletteInput').value);
    }else if(e.key === 'Enter'){
      e.preventDefault();
      runPalette(paletteActive);
    }
    return;
  }
  if(e.key === 'Escape' && $('modal').classList.contains('show')) closeModal();
  if((e.ctrlKey || e.metaKey) && e.key === 'Enter' && document.activeElement === $('wbText')){
    e.preventDefault(); wbSend();
  }
});

/* ---------------- 初始化 ---------------- */
initTheme();
$('modal').addEventListener('click', e=>{ if(e.target === $('modal')) closeModal(); });
$('palette').addEventListener('click', e=>{ if(e.target === $('palette')) closePalette(); });
$('wbText').addEventListener('input', updateCharCount);
if($('hostLine')) $('hostLine').textContent = location.host || '127.0.0.1';

refreshStatus(); refreshLog(); refreshWbLive();
renderLog();
setInterval(refreshStatus, 2000);
setInterval(refreshLog, 2000);
setInterval(tickUptime, 1000);
// 网关/端口状态每 10s 自刷新：后端异常时由 catch 写成「读取失败」而不是停在「检测中…」。
setInterval(refreshWbLive, 10000);
</script>
</body>
</html>
"""

# ===================== Web Server =====================

class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # 关闭默认请求日志
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path in ("/", "/index.html"):
            # 页面里的 IPC 端口占位在发送时替换，避免把端口写死在静态字符串里。
            page = PAGE_HTML.replace("__IPC_PORT__", str(IPC_PORT))
            self._send(200, page.encode("utf-8"), "text/html; charset=utf-8")
            return
        if self.path == "/api/status":
            self._send(200, json.dumps(self.server.app_state.snapshot(),
                                       ensure_ascii=False).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/log":
            self._send(200, json.dumps(self.server.app_state.log_bundle(),
                                       ensure_ascii=False).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/autostart":
            self._send(200, json.dumps({"on": autostart_status() == "开启"}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/device-answer":
            on = bool(self.server.app_state.snapshot().get("device_answer", True))
            self._send(200, json.dumps({"on": on}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/wb-live":
            # 活会话概览：前端「设备发消息」卡片据此显示当前能投递到哪。
            self._send(200, json.dumps(wb_live_overview(), ensure_ascii=False).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self) -> None:
        if self.path == "/api/login":
            ok = self.server.controller.trigger_login()
            self._send(200, json.dumps({"ok": ok}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/start":
            ok = self.server.controller.start()
            self._send(200, json.dumps({"ok": ok}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/stop":
            ok = self.server.controller.stop()
            self._send(200, json.dumps({"ok": ok}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/test-approval":
            ok = self.server.controller.send_test_approval()
            self._send(200, json.dumps({"ok": ok}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/test-ask":
            # body 可选：{"multi": true} 发多选题（勾选态，回传 mask）
            # 缺省单选（回传 index），对应设备上的"短按换选项/长按提交"。
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    body = {}
            except Exception:
                body = {}
            ok = self.server.controller.send_test_ask(multi=bool(body.get("multi")))
            self._send(200, json.dumps({"ok": ok,
                                        "multi": bool(body.get("multi"))}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/beep-volume":
            # body: {"vol": 0..100, "test": 0/1}。vol 缺失时按缺省处理，
            # 越界由 beep_vol_clamp 统一夹取，前端不必自己夹。
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise ValueError("body 不是对象")
            except Exception:
                self._send(400, json.dumps({"ok": False, "err": "bad body"}).encode("utf-8"),
                           "application/json; charset=utf-8")
                return
            vol = beep_vol_clamp(body.get("vol", BEEP_VOL_DEFAULT))
            ok = self.server.controller.set_beep_vol(vol, test=bool(body.get("test")))
            self._send(200, json.dumps({"ok": ok, "vol": vol}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/test-beep":
            ok = self.server.controller.test_beep()
            self._send(200, json.dumps({"ok": ok}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/autostart":
            try:
                length = int(self.headers.get("Content-Length", 0))
                on = json.loads(self.rfile.read(length) or b"{}").get("on", False)
            except Exception:
                on = False
            ok = autostart_enable() if on else autostart_disable()
            self._send(200, json.dumps({"ok": ok, "on": on}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/device-answer":
            try:
                length = int(self.headers.get("Content-Length", 0))
                on = bool(json.loads(self.rfile.read(length) or b"{}").get("on", False))
            except Exception:
                on = False
            ok = self.server.controller.set_device_answer(on)
            self._send(200, json.dumps({"ok": ok, "on": on}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/wb-deliver":
            # body: {"text": "...", "target": "reply"|"job", "cwd": "..."(可选)}
            # 阶段 1 供人工手工触发；阶段 2 起由设备语音链路直接调用同一函数。
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise ValueError("body 不是对象")
            except Exception:
                self._send(400, json.dumps({"ok": False, "err": "bad body"}).encode("utf-8"),
                           "application/json; charset=utf-8")
                return
            target = "job" if str(body.get("target")) == "job" else "reply"
            if target == "job" and isinstance(body.get("cwd"), str):
                wb_job_cfg_set({"cwd": body["cwd"]})   # 顺带记住工作目录
            res = wb_deliver(str(body.get("text") or ""), target)
            self.server.app_state.log(f"[deliver] {target} -> {res}")
            self._send(200, json.dumps(res, ensure_ascii=False).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        if self.path == "/api/search-pause":
            try:
                length = int(self.headers.get("Content-Length", 0))
                on = bool(json.loads(self.rfile.read(length) or b"{}").get("on", False))
            except Exception:
                on = False
            # 纯运行时状态，watch loop 每轮读 _st.search_paused，无需通知 daemon。
            self.server.app_state.set(search_paused=on)
            self._send(200, json.dumps({"ok": True, "on": on}).encode("utf-8"),
                       "application/json; charset=utf-8")
            return
        self._send(404, b"not found", "text/plain")


class WebServer:
    def __init__(self, st: AppState, controller: DaemonController,
                 host: str = "127.0.0.1", port: int = 8080) -> None:
        self._st = st
        self._controller = controller
        self._host = host
        self._port = port
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        class S(ThreadingHTTPServer):
            app_state = None
            controller = None
        S.app_state = self._st
        S.controller = self._controller
        self._httpd = S((self._host, self._port), _Handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever,
                                        daemon=True, name="wb-web")
        self._thread.start()
        self._st.log(f"[web] UI 已启动: http://{self._host}:{self._port}")

    def stop(self) -> None:
        if self._httpd:
            self._httpd.shutdown()
            self._httpd = None


# ===================== 开机自启 =====================

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_NAME = "WorkBuddy"


def _pythonw_path() -> str:
    """返回 pythonw.exe 绝对路径（无控制台窗口的 Python 解释器）。"""
    return os.path.join(os.path.dirname(sys.executable), "pythonw.exe")


def autostart_command() -> str:
    """开机自启时运行的完整命令行（pythonw + 本脚本 --tray）。"""
    script = os.path.abspath(__file__)
    return f'"{_pythonw_path()}" "{script}" --tray'


def autostart_enable() -> bool:
    if winreg is None:
        print("当前环境不支持 Windows 注册表开机自启")
        return False
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.SetValueEx(key, RUN_NAME, 0, winreg.REG_SZ, autostart_command())
        return True
    except OSError as exc:
        print(f"写入开机自启注册表失败: {exc}")
        return False


def autostart_disable() -> bool:
    if winreg is None:
        return False
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                            winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key, RUN_NAME)
        return True
    except FileNotFoundError:
        return True  # 原本就没开
    except OSError as exc:
        print(f"删除开机自启注册表失败: {exc}")
        return False


def autostart_status() -> str:
    if winreg is None:
        return "不支持"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.QueryValueEx(key, RUN_NAME)
        return "开启"
    except (FileNotFoundError, OSError):
        return "关闭"


# ===================== 已知设备（首次配对判定） =====================

KNOWN_FILE = os.path.join(os.path.expanduser("~"), ".workbuddy", "known_devices.json")


def _known_load() -> set:
    try:
        with open(KNOWN_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return set(data.get("devices", []))
    except Exception:
        return set()


def is_known(addr: str) -> bool:
    """该地址是否曾成功连接过（非首次）。"""
    return addr.upper() in _known_load()


def mark_known(addr: str) -> None:
    """记录该地址已成功连接过，之后不再视为首次。"""
    devs = _known_load()
    devs.add(addr.upper())
    try:
        os.makedirs(os.path.dirname(KNOWN_FILE), exist_ok=True)
        with open(KNOWN_FILE, "w", encoding="utf-8") as f:
            json.dump({"devices": sorted(devs)}, f, ensure_ascii=False)
    except OSError:
        pass


# ===================== 托盘 =====================

def _ensure_pystray() -> bool:
    """需要托盘但缺 pystray 时尝试自动安装（首次运行）。"""
    global HAS_TRAY, pystray, Image, ImageDraw, ImageFont
    if HAS_TRAY:
        return True
    print("缺少 pystray，正在自动安装（仅首次需要）...")
    try:
        subprocess.run([sys.executable, "-m", "pip", "install", "-q", "pystray"],
                       check=True, capture_output=True,
                       creationflags=SUB_NO_WINDOW)
    except Exception as exc:
        print(f"自动安装 pystray 失败，请手动执行 pip install pystray（{exc}）")
        return False
    try:
        import pystray
        from PIL import Image, ImageDraw, ImageFont
        HAS_TRAY = True
        return True
    except ImportError:
        print("pystray 已安装但导入失败，请重启程序")
        return False


def _make_tray_icon():
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([4, 4, 60, 60], radius=14, fill=(9, 13, 20, 255),
                        outline=(34, 211, 238, 255), width=3)
    try:
        font = ImageFont.truetype("seguisym.ttf", 30)
    except Exception:
        font = ImageFont.load_default()
    d.text((20, 12), "W", fill=(34, 211, 238, 255), font=font)
    return img


def run_tray(st, controller, web_port: int, no_daemon: bool) -> None:
    """在主线程运行系统托盘图标（阻塞直到用户选择退出）。"""
    def _open_web():
        webbrowser.open(f"http://127.0.0.1:{web_port}")

    def _start_daemon(_icon, _item):
        if controller.start():
            st.log("[tray] 启动 daemon")
        else:
            st.log("[tray] daemon 已在运行")

    def _stop_daemon(_icon, _item):
        controller.stop()
        st.log("[tray] 停止 daemon")

    def _toggle_autostart(_icon, _item):
        if autostart_status() == "开启":
            autostart_disable()
        else:
            autostart_enable()
        _icon.update_menu()

    def _quit(_icon, _item):
        _icon.stop()
        controller.stop()
        st.log("[app] 已从托盘退出")

    menu = pystray.Menu(
        pystray.MenuItem("打开控制台", _open_web, default=True),
        pystray.MenuItem("启动 daemon", _start_daemon,
                         enabled=lambda _i: not no_daemon and not controller.is_alive()),
        pystray.MenuItem("停止 daemon", _stop_daemon,
                         enabled=lambda _i: not no_daemon and controller.is_alive()),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("开机自启", _toggle_autostart,
                         checked=lambda _i: autostart_status() == "开启"),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("退出", _quit),
    )
    icon = pystray.Icon("WorkBuddy", _make_tray_icon(), "WorkBuddy 控制台", menu)
    icon.run()


# ===================== 入口 =====================

def _cb_authorization() -> int | None:
    """查 CoreBluetooth 的授权状态（0=未决定 1=受限 2=拒绝 3=已授权）。

    只读状态，不建 central、不扫描，因此本身不会触发 TCC 拦截。
    非 macOS 或查询失败时返回 None（调用方据此跳过自检）。
    """
    if sys.platform != "darwin":
        return None
    try:
        import ctypes
        import ctypes.util
        objc = ctypes.CDLL(ctypes.util.find_library("objc"))
        ctypes.CDLL("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth")
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.objc_getClass.argtypes = [ctypes.c_char_p]
        objc.sel_registerName.restype = ctypes.c_void_p
        objc.sel_registerName.argtypes = [ctypes.c_char_p]
        cls = objc.objc_getClass(b"CBCentralManager")
        if not cls:
            return None
        sel = objc.sel_registerName(b"authorization")
        objc.objc_msgSend.restype = ctypes.c_long
        objc.objc_msgSend.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        return int(objc.objc_msgSend(ctypes.c_void_p(cls), ctypes.c_void_p(sel)))
    except Exception:
        return None


# 子进程探测脚本：只建 CBCentralManager 并开始扫 0.6 秒。
# 有蓝牙授权时它正常返回；没有时内核直接 SIGABRT（退出码 -6 / 134）。
# 放在子进程里做，是因为这个崩溃**不可捕获**——在本进程里试一次就把主程序
# 带走了，只能拿一个"试验品"去承受。
_BLE_PROBE_SRC = r"""
import asyncio, sys
async def main():
    try:
        from bleak import BleakScanner
    except Exception:
        print("NO_BLEAK"); return 0
    try:
        await BleakScanner.discover(timeout=0.6, return_adv=False)
    except Exception as exc:
        print("ERR:%s" % exc); return 0
    print("OK"); return 0
sys.exit(asyncio.run(main()))
"""


def _ble_probe_subprocess(timeout: float = 12.0) -> str:
    """在子进程里真扫一次蓝牙，返回 'ok' / 'killed' / 'no_bleak' / 'error'。

    这是唯一可靠的办法：TCC 拦截发生在 CoreBluetooth 第一次真正访问硬件时，
    而它不能被 try/except 捕获（是内核发来的 SIGABRT）。

    注意：有 GUI 会话的宿主（Terminal）第一次会弹授权窗，子进程会一直等到
    用户点「允许」；所以超时给得宽一些，并且超时按 'ok' 处理——宁可放行让
    主流程自己撞上，也不要因为用户点得慢而误判成"没权限"。
    """
    try:
        p = subprocess.run(
            [sys.executable, "-c", _BLE_PROBE_SRC],
            capture_output=True, text=True, timeout=timeout,
            creationflags=SUB_NO_WINDOW)
    except subprocess.TimeoutExpired:
        # 多半是授权弹窗还开着没人点。放行，交给主流程。
        return "ok"
    except Exception:
        return "error"
    out = (p.stdout or "").strip()
    if p.returncode < 0 or p.returncode in (134, -6, 133, -5):
        # 负 returncode = 被信号杀死；-6 = SIGABRT，正是 TCC 拦截的形态。
        return "killed"
    if out == "OK":
        return "ok"
    if out == "NO_BLEAK":
        return "no_bleak"
    return "error"


def _macos_ble_precheck() -> str:
    """B-04：macOS 上后台进程碰 CoreBluetooth 会被 TCC 直接 SIGABRT。

    现象：日志停在「扫描 Xiaozhi BOX ...」，进程消失，退出码 134，
    崩溃报告里是 TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION。

    根因不是代码：蓝牙权限绑定在「有 GUI 会话的应用」上，从 launchd / IDE /
    后台 shell 启动的进程拿不到弹窗机会——未授权时系统宁可杀掉也不静默降级。

    判定方式（两步，缺一不可）：
      1. authorization == 2（用户明确拒绝）→ 直接给拒绝指引，不必试。
      2. 其余情况在**子进程**里真扫一次：被杀 = 当前宿主没权限。
         为什么不能只看 authorization == 0：从 Terminal 第一次运行时它也是 0，
         但那恰恰是**应该弹窗并被允许**的正常路径；只看状态会把首次启动挡掉。

    返回空串 = 可以继续；非空 = 阻断原因（调用方打印后退出）。
    """
    if sys.platform != "darwin":
        return ""
    auth = _cb_authorization()
    if auth == 2:
        return ("蓝牙权限已被拒绝。请到「系统设置 → 隐私与安全性 → 蓝牙」中"
                "勾选你用来启动本程序的终端 App（Terminal / iTerm），然后重试。")
    if auth == 1:
        return "蓝牙权限被系统策略限制（restricted），当前账号无法使用蓝牙。"
    verdict = _ble_probe_subprocess()
    if verdict in ("ok", "no_bleak", "error"):
        return ""
    # verdict == "killed"：子进程被 TCC 杀了，主进程也一定会被杀，先拦住。
    return ("当前进程没有蓝牙权限，一旦开始扫描，macOS 会直接终止进程"
            "（表现为进程无声消失、退出码 134）。\n"
            "   蓝牙权限绑定在「有图形界面的应用」上，后台/IDE 启动的进程"
            "拿不到授权弹窗。请从终端手动启动一次：\n"
            "     双击 host/启动主机端.command\n"
            "   或在终端里执行：./启动主机端.command\n"
            "   首次会弹出「Terminal 想要使用蓝牙」，点「允许」即可。")


def _lan_ip() -> str:
    """取本机局域网 IP，用来提示手机端该填哪个地址。"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("223.5.5.5", 80))
            return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"


def main() -> int:
    parser = argparse.ArgumentParser(description="WorkBuddy 控制台（daemon + 本地 Web UI）")
    parser.add_argument("--mac", help="设备 BLE 地址（否则扫描）")
    parser.add_argument("--port", type=int, default=IPC_PORT, help="IPC 审批端口（默认 48765）")
    parser.add_argument("--web-port", type=int, default=8080, help="Web UI 端口（默认 8080）")
    parser.add_argument("--web-host", default="127.0.0.1",
                        help="Web UI 监听地址（默认 127.0.0.1 仅本机；手机访问填 0.0.0.0）")
    parser.add_argument("--credit-interval", type=float, default=CREDIT_INTERVAL,
                        help="周期积分推送秒数（0=禁用）")
    parser.add_argument("--no-daemon", action="store_true", help="只起 Web UI，不自动启动 daemon")
    parser.add_argument("--tray", action="store_true", help="最小化到系统托盘运行")
    parser.add_argument("--open-browser", action="store_true", help="启动后自动打开 Web UI 浏览器")
    parser.add_argument("--autostart-on", action="store_true", help="开启开机自启（写入注册表）")
    parser.add_argument("--autostart-off", action="store_true", help="关闭开机自启（删除注册表）")
    parser.add_argument("--autostart-status", action="store_true", help="查询开机自启状态")
    parser.add_argument("--voice-selftest", metavar="WAV",
                        help="文字链路自检：把 WAV 灌进虚拟声卡并取回听写文字（不接设备）")
    parser.add_argument("--voice-deliver", default="none",
                        choices=("none", "reply", "job", "manual"),
                        help="自检取到字后是否顺手投递（默认 none，只看字）")
    parser.add_argument("--voice-hotkey", default="",
                        help=f"自检用的语音快捷键（默认 {VOICE_HOTKEY_DEFAULT}）")
    parser.add_argument("--voice-manual", action="store_true",
                        help="自检改为手动模式：只循环灌音，热键由你手按（用来分离注入与输入法）")
    parser.add_argument("--voice-manual-seconds", type=float, default=40.0,
                        help="手动模式的灌音时长（默认 40 秒）")
    parser.add_argument("--wb-status", action="store_true",
                        help="打印 WorkBuddy 投递层现状（worker 端口/密钥/活会话）后退出")
    args = parser.parse_args()

    if args.wb_status:
        wb_status_report()
        return 0

    if args.voice_selftest:
        return voice_selftest(args.voice_selftest, args.voice_hotkey, args.voice_deliver,
                              manual=args.voice_manual,
                              manual_seconds=args.voice_manual_seconds)

    if args.autostart_on or args.autostart_off or args.autostart_status:
        if args.autostart_on:
            print("开机自启：", "已开启" if autostart_enable() else "开启失败")
        elif args.autostart_off:
            print("开机自启：", "已关闭" if autostart_disable() else "关闭失败")
        else:
            print("开机自启状态：", autostart_status())
        return 0

    st = AppState()
    controller = DaemonController(st, address=args.mac, port=args.port,
                                  credit_interval=args.credit_interval)

    st = AppState()
    controller = DaemonController(st, address=args.mac, port=args.port,
                                  credit_interval=args.credit_interval)

    # B-04：daemon 一启动就会扫描 BLE。macOS 上若当前进程没有蓝牙授权，
    # CoreBluetooth 会被 TCC 直接 SIGABRT（退出码 134、进程无声消失）。
    # 自检必须放在**起 Web 服务之前**：否则会出现"服务起了、daemon 没起、
    # 浏览器已经打开一个连不上设备的页面"这种半残状态，比直接报错更难懂。
    if not args.no_daemon:
        block = _macos_ble_precheck()
        if block:
            print("\n[蓝牙自检未通过，未启动]\n" + block, file=sys.stderr)
            print("\n（Web UI 也一并没有启动，避免留下一个连不上设备的页面）",
                  file=sys.stderr)
            return 2

    try:
        web = WebServer(st, controller, host=args.web_host, port=args.web_port)
        web.start()
    except OSError as exc:
        print(f"Web UI 启动失败（端口 {args.web_port} 可能被占用）: {exc}", file=sys.stderr)
        return 1

    if args.open_browser:
        # 等 Web 服务端口就绪后自动打开浏览器
        def _open_later():
            try:
                webbrowser.open(f"http://127.0.0.1:{args.web_port}")
                st.log(f"[app] 已在浏览器打开 http://127.0.0.1:{args.web_port}")
            except Exception:
                pass
        threading.Timer(0.6, _open_later).start()

    if not args.no_daemon:
        if controller.start():
            st.log("[app] daemon 已启动")
        else:
            st.log("[app] daemon 已在运行")

    st.log(f"[app] 浏览器打开 http://127.0.0.1:{args.web_port} 查看控制台")
    if args.web_host in ("0.0.0.0", "::", ""):
        st.log(f"[app] 手机端填 http://{_lan_ip()}:{args.web_port}（需同一局域网）")

    if args.tray:
        if not _ensure_pystray():
            print("托盘不可用，退回控制台模式运行")
            return 1
        try:
            run_tray(st, controller, args.web_port, args.no_daemon)
        finally:
            controller.stop()
        return 0

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[app] 退出中...")
    finally:
        controller.stop()
        web.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
