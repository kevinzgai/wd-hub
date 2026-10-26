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
  python configure.py -s MyWiFi -p 12345678 -b 192.168.1.100:8080
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
    url = f"http://{ip}:{port}/health"
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            data = json.loads(r.read().decode("utf-8"))
            return bool(data.get("ok")), f"db exists={data.get('exists')}"
    except Exception as e:
        return False, str(e)


def main() -> int:
    ap = argparse.ArgumentParser(description="配置 WorkBuddy 看板固件")
    ap.add_argument("-s", "--ssid", help="WiFi 名称")
    ap.add_argument("-p", "--password", help="WiFi 密码")
    ap.add_argument("-b", "--bridge", help="bridge 地址，如 192.168.1.100:8080")
    args = ap.parse_args()

    ssid = args.ssid or input("WiFi 名称 (SSID): ").strip()
    if not ssid:
        print("[!] SSID 不能为空", file=sys.stderr)
        return 1

    password = args.password
    if password is None:
        import getpass
        password = getpass.getpass("WiFi 密码 (留空表示开放网络): ")

    # bridge 地址：没给就自动探测
    bridge = args.bridge
    if not bridge:
        ip = detect_lan_ip()
        if ip:
            print(f"[i] 检测到本机局域网 IP: {ip}")
            bridge = f"{ip}:8080"
        else:
            bridge = input("bridge 地址 (IP:端口): ").strip()

    host, _, port_s = bridge.partition(":")
    port = int(port_s or 8080)

    # 校验 bridge 是否真的在跑，避免刷完固件才发现连不上
    ok, detail = check_bridge(host, port)
    if ok:
        print(f"[+] bridge 可达: http://{host}:{port}/status  ({detail})")
    else:
        print(f"[!] bridge 不可达: {detail}")
        print(f"    请先启动: py -m server.api_server --port {port}")
        if input("    仍然写入配置? [y/N] ").strip().lower() != "y":
            return 1

    content = f"""// 自动生成，请勿手工编辑（运行 configure.py 重新生成）
// 本文件不进版本库，避免 WiFi 密码泄漏
#pragma once

#define WIFI_SSID      "{ssid}"
#define WIFI_PASSWORD  "{password}"
#define BRIDGE_URL     "http://{host}:{port}/status"
"""
    LOCAL_H.write_text(content, encoding="utf-8")
    print(f"[+] 已写入 {LOCAL_H}")
    print(f"    SSID   : {ssid}")
    print(f"    Bridge : http://{host}:{port}/status")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
