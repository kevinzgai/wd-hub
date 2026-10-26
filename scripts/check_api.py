#!/usr/bin/env python3
"""看板服务回归自检。

跑法（服务得已经在跑）：
    python scripts/check_api.py
    BOARD_PORT=9000 python scripts/check_api.py

检查项：
  1. 全部端点返回 200
  2. /status 满足固件契约：< 4096 字节、含 "ok":true
  3. /api/v1/health 各数据源状态
  4. /screen/1..N-1 结构合法（标题 + 行数 + 行字段）
  5. 越界屏号被正确拒绝

退出码：0 全过，1 有失败。
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request

HOST = os.environ.get("BOARD_HOST", "127.0.0.1")
PORT = os.environ.get("BOARD_PORT", "27777")
BASE = f"http://{HOST}:{PORT}"

FIRMWARE_MAX_BYTES = 4096

V1_ENDPOINTS = [
    "/api/v1/board", "/api/v1/summary", "/api/v1/sessions", "/api/v1/tasks",
    "/api/v1/automations", "/api/v1/activity", "/api/v1/artifacts",
    "/api/v1/system", "/api/v1/health", "/api/ticker",
]

fails: list[str] = []


def get(path: str) -> tuple[int, bytes]:
    try:
        with urllib.request.urlopen(f"{BASE}{path}", timeout=5) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return 0, str(e).encode()


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}{('  ' + detail) if detail else ''}")
    if not ok:
        fails.append(label)


def main() -> int:
    print(f"看板服务回归自检 -> {BASE}\n")

    # ---- 1. 固件契约 ----
    print("固件契约 /status")
    code, body = get("/status")
    check("HTTP 200", code == 200, f"got {code}")
    size = len(body)
    check(f"响应体 < {FIRMWARE_MAX_BYTES} 字节", size < FIRMWARE_MAX_BYTES, f"{size} 字节")
    check('含 "ok":true', b'"ok":true' in body)
    try:
        d = json.loads(body)
        check("JSON 可解析且含必需字段",
              all(k in d for k in ("ok", "clock", "date", "working", "today_done",
                                   "automations", "sessions", "nscr")),
              f"nscr={d.get('nscr')}")
        check("sessions 为列表", isinstance(d.get("sessions"), list),
              f"{len(d.get('sessions') or [])} 条")
    except Exception as e:
        check("JSON 可解析", False, str(e))

    # ---- 2. v1 接口 ----
    print("\nv1 接口")
    for ep in V1_ENDPOINTS:
        code, body = get(ep)
        check(f"{ep}", code == 200, f"{code}")

    # ---- 3. 数据源自检 ----
    print("\n数据源自检")
    code, body = get("/api/v1/health")
    if code == 200:
        h = json.loads(body)
        print(f"       版本 v{h.get('version')}  缓存年龄 {h.get('cache_age_ms')}ms"
              f"  stale={h.get('stale')}")
        for name, s in (h.get("sources") or {}).items():
            check(f"数据源 {name}", bool(s.get("ok")),
                  f"{s.get('count')} 条 {s.get('detail', '')}")
    else:
        check("/api/v1/health", False, f"{code}")

    # ---- 4. 详情屏 ----
    print("\n详情屏")
    code, body = get("/status")
    nscr = json.loads(body).get("nscr", 1) if code == 200 else 1
    for i in range(1, nscr):
        code, body = get(f"/screen/{i}")
        ok = code == 200
        detail = f"{code}"
        if ok:
            s = json.loads(body)
            rows = s.get("rows") or []
            ok = bool(s.get("t")) and len(rows) <= 6 and all(
                "t" in r for r in rows)
            detail = f"标题={s.get('t')} 行数={len(rows)}"
        check(f"/screen/{i}", ok, detail)

    # ---- 5. 越界保护 ----
    # 越界返回 HTTP 200 + {"ok":false,...} 是**故意**的：固件按 `"ok":true`
    # 判断成败（firmware/main/main.cpp 的 strstr 判定），不看 HTTP 状态码。
    # 所以这里断言 ok 字段为 false，而不是状态码为 4xx。
    print("\n越界保护")
    for bad in (nscr, 0, 99):
        code, body = get(f"/screen/{bad}")
        try:
            d = json.loads(body)
            rejected = d.get("ok") is False and bool(d.get("error"))
        except Exception:
            rejected = False
        check(f"/screen/{bad} 被拒绝", code == 200 and rejected,
              f"{code} {body[:40].decode(errors='ignore')}")

    # ---- 汇总 ----
    print("\n" + "=" * 52)
    if fails:
        print(f"失败 {len(fails)} 项：")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
