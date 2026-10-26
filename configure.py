#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
写入 WiFi 与 bridge 地址到固件配置。

为什么需要这个脚本：
  直接在 main.cpp 里改宏容易改错、也容易把改好的一提交就漏掉。
  这里统一定义在 config.local.h（不进版本库），main.cpp 优先包含它。

用法：
  python configure.py                      # 交互式填写
  python configure.py -s MyWiFi -p 12345678
  python configure.py -s MyWiFi -p 12345678 -b 192.168.1.100:27777
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FW = ROOT / "firmware"
LOCAL_H = FW / "main" / "config.local.h"


def detect_lan_ip() -> str | None:
    """取本机对外网卡 IP，优先返回能连 bridge 的那个。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("223.5.5.5", 80))   # 阿里 DNS，不发包只查路由
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return None


def check_bridge(ip: str, port: int) -> tuple[bool, str]:
    """探活服务端。

    ⚠ 端点必须是 `/api/v1/health` —— 服务端没有裸 `/health` 路由（那是 404）。
    响应里也没有 `exists` 字段，别按老格式解析。
    """
    url = f"http://{ip}:{port}/api/v1/health"
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            data = json.loads(r.read().decode("utf-8"))
            sources = data.get("sources") or {}
            bad = [k for k, v in sources.items() if not v.get("ok")]
            detail = (f"v{data.get('version', '?')} · {len(sources)} 个数据源"
                      + (f"，降级: {','.join(bad)}" if bad else "，全部正常"))
            return bool(data.get("ok")), detail
    except Exception as e:
        return False, str(e)


# 与服务端 server/api_server.py 的 DEFAULT_PORT、固件 BRIDGE_BASE 保持一致
DEFAULT_PORT = 27777


def main() -> int:
    ap = argparse.ArgumentParser(description="配置 WorkBuddy 看板固件")
    # WiFi 参数保留但已废弃：2026-10-27 起固件不再读编译期凭据，
    # 改成手机配网（长按 BOOT 3 秒）。留着是为了不打断老脚本，传了只提示。
    ap.add_argument("-s", "--ssid", help="[已废弃] WiFi 名称，现在用手机配网")
    ap.add_argument("-p", "--password", help="[已废弃] WiFi 密码，现在用手机配网")
    ap.add_argument("-b", "--bridge", help="bridge 地址，如 192.168.1.100:27777")
    args = ap.parse_args()

    if args.ssid or args.password:
        print("[!] -s / -p 已废弃，本脚本不再写入 WiFi 凭据。")
        print("    换 WiFi 请在设备上长按 BOOT 3 秒，用手机连热点配置。")
        print("    （固件里不再保留任何默认 WiFi，避免换环境重烧和密码泄漏。）")
        print()

    # bridge 地址：没给就自动探测
    bridge = args.bridge
    if not bridge:
        ip = detect_lan_ip()
        if ip:
            print(f"[i] 检测到本机局域网 IP: {ip}")
            bridge = f"{ip}:{DEFAULT_PORT}"
        else:
            bridge = input("bridge 地址 (IP:端口): ").strip()

    if not bridge:
        print("[!] 必须提供 bridge 地址", file=sys.stderr)
        return 1

    host, _, port_s = bridge.partition(":")
    port = int(port_s or DEFAULT_PORT)

    # 校验 bridge 是否真的在跑，避免刷完固件才发现连不上
    ok, detail = check_bridge(host, port)
    if ok:
        print(f"[+] bridge 可达: http://{host}:{port}/status  ({detail})")
    else:
        print(f"[!] bridge 不可达: {detail}")
        print(f"    请先启动: python -m server.api_server --port {port}")
        print(f"    或双击 start-board.bat（Windows）/ ./start-board.sh（macOS/Linux）")
        if input("    仍然写入配置? [y/N] ").strip().lower() != "y":
            return 1

    content = f"""// 自动生成，请勿手工编辑（运行 configure.py 重新生成）
// 本文件不进版本库。
//
// ⚠ 这里**不再**包含 WiFi 凭据。2026-10-27 起固件只从 NVS 读 WiFi，
//   没有就开机进配网模式（长按 BOOT 3 秒，手机连热点配置）。
//   固件里不留默认 WiFi，换环境不用重烧，也不会把密码带进版本库。
#pragma once

// 固件读的是 BRIDGE_BASE（只给 base，第 0 屏走 /status，其余屏走 /screen/<i>），
// 路径由固件自己拼。以前这里写的是 BRIDGE_URL，固件根本不读那个宏 —— 配置静默失效。
#define BRIDGE_BASE    "http://{host}:{port}"
"""
    LOCAL_H.write_text(content, encoding="utf-8")
    print(f"[+] 已写入 {LOCAL_H}")
    print(f"    Bridge : http://{host}:{port}/status")
    print(f"    WiFi   : 设备端配网（长按 BOOT 3 秒）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
