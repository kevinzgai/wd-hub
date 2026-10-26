#!/usr/bin/env python3
"""
小智设备数据服务 - 最小 Demo
零框架依赖（标准库 + psutil），一个文件跑起来。

启动:
    python api_server.py
    python api_server.py --port 8080

接口:
    GET /api/status   -> 设备要显示的 JSON 数据
    GET /api/ticker   -> 纯文本滚动消息
    GET /             -> 浏览器调试页（240x240 预览）

设备端访问: http://<本机局域网IP>:8080/api/status
"""

import argparse
import json
import os
import socket
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import psutil
except ImportError:
    psutil = None

# ---------------------------------------------------------------- 数据源

WEEKDAY_CN = ["一", "二", "三", "四", "五", "六", "日"]
WEEKDAY_EN = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

# 想改屏幕显示的内容，改这里
MESSAGE_CN = "你好，小智"
MESSAGE_EN = "Hello XiaoZhi"
TICKER_CN = "小智设备数据看板 · 正在从本机服务拉取数据 · 数据每两秒刷新一次"


def collect_status() -> dict:
    """采集一次状态快照。字段名压到最短 —— 设备端 JSON 解析省内存。"""
    now = datetime.now()
    wd = now.weekday()

    if psutil:
        cpu = round(psutil.cpu_percent(interval=None))
        mem = round(psutil.virtual_memory().percent)
        try:
            temp = round(psutil.sensors_temperatures().get("coretemp", [{}])[0].get("current", 0))
        except Exception:
            temp = 0
    else:
        cpu = mem = temp = 0

    return {
        "t": now.strftime("%H:%M:%S"),          # 时间
        "d": now.strftime("%Y-%m-%d"),          # 日期
        "wd": WEEKDAY_EN[wd],                   # 星期(英文, 内置字体能渲染)
        "wd_cn": WEEKDAY_CN[wd],                # 星期(中文, LVGL 阶段用)
        "cpu": cpu,                             # CPU 占用 %
        "mem": mem,                             # 内存占用 %
        "temp": temp,                           # 温度 °C (拿不到就是 0)
        "host": socket.gethostname()[:16],
        "msg": MESSAGE_EN,
        "msg_cn": MESSAGE_CN,
        "up": int(time.time() - START_TS),      # 服务已运行秒数
    }


START_TS = time.time()

# ---------------------------------------------------------------- 设备看板数据
#
# 固件（firmware/main/main.cpp）的字段约束，改这里必须同步：
#   clock   char[8]  最多 7 字节，ASCII
#   date    char[8]  最多 7 字节 —— 中文一个占 3 字节，所以只能用 ASCII
#   title   char[40] 约 13 个中文
#   project char[20] 约 6 个中文
#   elapsed char[12]
#   ok 必须精确输出 "ok":true —— 固件用 strstr 匹配，不能有空格
#
# 三格统计的语义：
#   working    = 正在跑的开发进程数
#   today_done = 今天改动过的文件数
#   automations= 自动化任务数（读 WorkBuddy 的自动化配置）

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {".git", "node_modules", "build", "dist", "__pycache__", ".workbuddy",
             ".vscode", "managed_components", "venv", ".venv"}
DEV_PROCS = {"python.exe", "python3.exe", "node.exe", "code.exe", "git.exe",
             "cargo.exe", "esptool.exe"}


def utf8_trunc(s: str, max_bytes: int) -> str:
    """按字节截断，不切碎 UTF-8 字符。"""
    b = s.encode("utf-8")
    if len(b) <= max_bytes:
        return s
    return b[:max_bytes].decode("utf-8", errors="ignore")


def human_ago(sec: float) -> str:
    if sec < 60:
        return f"{int(sec)}s"
    if sec < 3600:
        return f"{int(sec // 60)}m"
    if sec < 86400:
        return f"{int(sec // 3600)}h"
    return f"{int(sec // 86400)}d"


def scan_files(root: str, max_depth: int = 3):
    """遍历项目文件，跳过噪音目录。返回 [(mtime, path)]。"""
    out = []
    root_len = len(root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and not d.startswith(".")]
        if dirpath[root_len:].count(os.sep) >= max_depth:
            dirnames[:] = []
        for fn in filenames:
            if fn.startswith("."):
                continue
            fp = os.path.join(dirpath, fn)
            try:
                out.append((os.stat(fp).st_mtime, fp))
            except OSError:
                pass
    return out


def count_automations() -> int:
    """数一下 WorkBuddy 里配了多少自动化任务。读不到就 0。"""
    home = os.path.expanduser("~")
    for rel in (r".workbuddy\automations", r".workbuddy\user\automations"):
        d = os.path.join(home, rel)
        if os.path.isdir(d):
            n = len([f for f in os.listdir(d) if f.endswith((".json", ".yaml", ".yml"))])
            if n:
                return n
    return 0


def collect_board() -> dict:
    """生成固件要的 /status 数据。"""
    now = datetime.now()
    files = scan_files(PROJECT_ROOT)
    now_ts = time.time()
    today_start = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()

    today_done = sum(1 for m, _ in files if m >= today_start)

    working = 0
    if psutil:
        for p in psutil.process_iter(["name"]):
            try:
                if (p.info.get("name") or "").lower() in DEV_PROCS:
                    working += 1
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass

    # 最近改动的 3 个文件当"会话"展示
    files.sort(reverse=True)
    sessions = []
    for mtime, path in files[:3]:
        rel = os.path.relpath(path, PROJECT_ROOT)
        parts = rel.split(os.sep)
        sessions.append({
            "title": utf8_trunc(os.path.basename(path), 40),
            "project": utf8_trunc(parts[0] if len(parts) > 1 else "root", 20),
            "elapsed": human_ago(now_ts - mtime),
        })

    return {
        "ok": True,
        "error": "",
        "clock": now.strftime("%H:%M"),          # 5 字节
        "date": now.strftime("%m-%d"),           # 5 字节，纯 ASCII
        "working": working,
        "today_done": today_done,
        "automations": count_automations(),
        "sessions": sessions,
    }

# ---------------------------------------------------------------- HTTP


DEBUG_PAGE = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>小智数据服务</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#f6f7f9;font-family:-apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
       min-height:100vh;display:flex;flex-direction:column;align-items:center;
       justify-content:center;gap:28px;padding:32px}
  .screen{width:300px;height:300px;background:#0b0d10;border-radius:16px;
          box-shadow:0 12px 40px rgba(0,0,0,.18);padding:18px;color:#e8ecf1;
          display:flex;flex-direction:column;font-variant-numeric:tabular-nums}
  .clock{font-size:44px;font-weight:600;letter-spacing:1px;color:#7cd0ff}
  .date{font-size:12px;color:#7c8794;margin-top:2px}
  .bars{margin-top:auto;display:flex;flex-direction:column;gap:8px}
  .row{display:flex;align-items:center;gap:8px;font-size:11px;color:#9aa6b2}
  .row span:first-child{width:34px;flex:none}
  .track{flex:1;height:6px;background:#1e232a;border-radius:3px;overflow:hidden}
  .fill{height:100%;background:linear-gradient(90deg,#7170ff,#4fd1c5);
        border-radius:3px;transition:width .4s}
  .val{width:34px;text-align:right;color:#e8ecf1}
  .msg{margin-top:12px;font-size:12px;color:#c8d2dd;text-align:center;
       border-top:1px solid #1e232a;padding-top:10px}
  .meta{font-size:12px;color:#8a94a0;text-align:center;line-height:1.8}
  code{background:#eef0f3;padding:2px 6px;border-radius:4px;font-size:11px}
</style></head><body>
  <div class="screen">
    <div class="clock" id="t">--:--:--</div>
    <div class="date" id="d">&nbsp;</div>
    <div class="bars">
      <div class="row"><span>CPU</span><div class="track"><div class="fill" id="cpu" style="width:0"></div></div><span class="val" id="cpuv">0%</span></div>
      <div class="row"><span>MEM</span><div class="track"><div class="fill" id="mem" style="width:0"></div></div><span class="val" id="memv">0%</span></div>
    </div>
    <div class="msg" id="msg">&nbsp;</div>
  </div>
  <div class="meta">
    设备接口 <code id="ep">/api/status</code><br>
    <span id="host"></span>
  </div>
<script>
async function tick(){
  try{
    const r = await fetch('/api/status');
    const j = await r.json();
    t.textContent = j.t;
    d.textContent = j.d + ' 周' + j.wd_cn;
    cpu.style.width = j.cpu + '%'; cpuv.textContent = j.cpu + '%';
    mem.style.width = j.mem + '%'; memv.textContent = j.mem + '%';
    msg.textContent = j.msg_cn;
    host.textContent = 'host: ' + j.host + '  ·  uptime ' + j.up + 's';
  }catch(e){ console.warn(e); }
}
ep.textContent = location.origin + '/api/status';
tick(); setInterval(tick, 2000);
</script>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    server_version = "XiaoZhiDemo/1.0"

    def _send(self, code: int, body: bytes, ctype: str):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]

        # 固件用的端点。紧凑 JSON，固件靠 strstr 找 "\"ok\":true"
        if path == "/status":
            board = collect_board()
            body = json.dumps(board, ensure_ascii=False, separators=(",", ":"))
            self._send(200, body.encode("utf-8"), "application/json; charset=utf-8")

        elif path == "/api/status":
            body = json.dumps(collect_status(), ensure_ascii=False, separators=(",", ":"))
            self._send(200, body.encode("utf-8"), "application/json; charset=utf-8")

        elif path == "/api/ticker":
            # 屏幕下方滚动条用的纯文本，设备端不用解析 JSON
            self._send(200, TICKER_CN.encode("utf-8"), "text/plain; charset=utf-8")

        elif path in ("/", "/index.html"):
            self._send(200, DEBUG_PAGE.encode("utf-8"), "text/html; charset=utf-8")

        else:
            self._send(404, b'{"error":"not found"}', "application/json")

    def log_message(self, fmt, *args):
        # 只打 API 请求，别刷屏
        line = args[0] if args else ""
        if "/api/" in line or "/status" in line:
            print(f"  [{datetime.now():%H:%M:%S}] {line}")


def lan_ips() -> list[str]:
    out = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127.") and ip not in out:
                out.append(ip)
    except Exception:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        if ip not in out:
            out.insert(0, ip)
    except Exception:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()

    # 预热 psutil，第一次调用返回值无意义
    if psutil:
        psutil.cpu_percent(interval=None)
        time.sleep(0.1)

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    ips = lan_ips()

    print("=" * 60)
    print("  小智数据服务已启动")
    print("=" * 60)
    print(f"  本机调试页 : http://127.0.0.1:{args.port}/")
    for ip in ips:
        print(f"  固件接口   : http://{ip}:{args.port}/status")
    print(f"  通用接口   : http://127.0.0.1:{args.port}/api/status")
    print(f"  数据源     : {'psutil 真实数据' if psutil else '假数据(psutil 未安装)'}")
    print("=" * 60)
    print("  Ctrl+C 停止\n")

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
