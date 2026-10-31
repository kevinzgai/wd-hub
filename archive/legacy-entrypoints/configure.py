#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
写入 WiFi 与 bridge 地址到固件配置。

为什么需要这个脚本：
  直接在 main.cpp 里改宏容易改错、也容易把改好的一提交就漏掉。
  这里统一定义在 config.local.h（不进版本库），main.cpp 优先包含它。

用法：
  python configure.py                      # 交互式填写
  python configure.py -i 192.168.1.100     # 端口留空 → 默认 27777
  python configure.py -i 192.168.1.100 --port 8080
  python configure.py -b 192.168.1.100:27777   # 老写法，仍然支持
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


def parse_bridge(raw: str) -> tuple[str | None, int]:
    """把用户输入的 bridge 地址解析成 (host, port)。

    ⚠ 必须容错 —— 这是用户手打的第一个参数，几乎所有人第一次都会写成
    "http://192.168.1.5:27777"。原来的写法 partition(":") 拿到
    port_s = "//192.168.1.5"，int() 直接抛 ValueError 崩掉，
    而且崩在 argparse 之后、配置写入之前，错误信息还是个天书。

    接受这些写法：
        192.168.1.5:27777
        192.168.1.5
        http://192.168.1.5:27777
        https://192.168.1.5/
    """
    s = (raw or "").strip()
    s = re.sub(r"^[a-zA-Z][a-zA-Z0-9+.-]*://", "", s)   # 去协议前缀
    s = s.rstrip("/")                                     # 去尾部斜杠
    if not s:
        return None, DEFAULT_PORT

    if ":" in s:
        host, _, port_s = s.rpartition(":")
        try:
            port = int(port_s)
        except ValueError:
            return None, DEFAULT_PORT
    else:
        host, port = s, DEFAULT_PORT

    host = host.strip()
    if not host:
        return None, DEFAULT_PORT
    return host, port


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
    ap.add_argument("-b", "--bridge", help="bridge 地址（老写法），如 192.168.1.100:27777")
    ap.add_argument("-i", "--ip", help="bridge IP，如 192.168.1.100（与 --port 分开填）")
    ap.add_argument("--port", type=int, default=None,
                    help=f"bridge 端口，留空默认 {DEFAULT_PORT}")
    args = ap.parse_args()

    if args.ssid or args.password:
        print("[!] -s / -p 已废弃，本脚本不再写入 WiFi 凭据。")
        print("    换 WiFi 请在设备上长按 BOOT 3 秒，用手机连热点配置。")
        print("    （固件里不再保留任何默认 WiFi，避免换环境重烧和密码泄漏。）")
        print()

    # 服务地址：支持 IP / 端口分开给（-i + --port），也兼容老的 -b 整串。
    # 端口不填就用默认 27777 —— 绝大部分场景只换 IP，端口不用管。
    if args.port is not None and not (0 < args.port < 65536):
        print("[!] 端口必须在 1..65535", file=sys.stderr)
        return 1

    bridge = args.bridge
    if args.ip:
        # 走 parse_bridge 而不是直接拼：用户把端口也粘进 -i 时（"-i 1.2.3.4:8080"）
        # 直接拼会得到 "1.2.3.4:8080:27777"，静默写进一个错地址。
        host, port = parse_bridge(args.ip)
        if host is None:
            print(f"[!] 无法解析 IP: {args.ip}", file=sys.stderr)
            return 1
        if args.port is not None:
            port = args.port          # 显式 --port 优先
        bridge = f"{host}:{port}"
    elif args.port is not None:
        print("[!] --port 要配合 -i 一起用（或用 -b IP:端口）", file=sys.stderr)
        return 1

    # 都没给就自动探测
    if not bridge:
        ip = detect_lan_ip()
        if ip:
            print(f"[i] 检测到本机局域网 IP: {ip}")
            bridge = f"{ip}:{DEFAULT_PORT}"
        else:
            ip_in = input("服务地址 IP（电脑局域网 IP）: ").strip()
            port_in = input(f"端口（留空默认 {DEFAULT_PORT}）: ").strip()
            bridge = f"{ip_in}:{port_in or DEFAULT_PORT}" if ip_in else ""

    if not bridge:
        print("[!] 必须提供 bridge 地址", file=sys.stderr)
        return 1

    host, port = parse_bridge(bridge)
    if host is None:
        print(f"[!] 无法解析 bridge 地址: {bridge}", file=sys.stderr)
        print("    正确格式: 192.168.1.100:27777", file=sys.stderr)
        return 1

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
//
// ⚠ BRIDGE_BASE 只是**出厂兜底值**：服务地址也存进 NVS 了，板子配过一次
//   就以 NVS 为准。电脑换 IP 在配置页改一下即可，不用改这里、不用重烧。
#pragma once

// 固件读的是 BRIDGE_BASE（只给 base，第 0 屏走 /status，其余屏走 /screen/<i>），
// 路径由固件自己拼。以前这里写的是 BRIDGE_URL，固件根本不读那个宏 —— 配置静默失效。
#define BRIDGE_BASE    "http://{host}:{port}"
"""
    LOCAL_H.write_text(content, encoding="utf-8")
    print(f"[+] 已写入 {LOCAL_H}")
    print(f"    Bridge : http://{host}:{port}/status（出厂兜底值）")
    print(f"    WiFi   : 设备端配网（长按 BOOT 3 秒）")
    print()
    print("[i] 以后电脑换 IP 不用再跑本脚本、也不用重烧：")
    print("    板子连不上服务时会在屏幕上给出配置页地址，打开改服务地址即可。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
