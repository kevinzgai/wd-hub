# -*- coding: utf-8 -*-
"""AIWK 自有 WorkBuddy 凭据：官方 OAuth 登录 + token 刷新 + 统一读取。

为什么要这个模块
----------------
WorkBuddy 5.6 起把 ``workbuddy-desktop.info`` 里的 ``accessToken`` /
``refreshToken`` 改成加密信封形态::

    {"$wbEncrypted": 1, "envelope": "eyJzdWl0ZSI6MSwia2V5SWQiOiI..."}

信封是 AES-256-GCM（``suite=1`` + ``keyId`` + ``nonce`` + ``authTag`` +
``ciphertext``），**密钥不在磁盘上**：``~/.workbuddy/keyblob`` 只存被包裹的
密文（protector ``static-v1``），真密钥运行时才由 WorkBuddy 原生模块下发。
所以 AIWK 无法再从该文件直读 token。

官方桌面端自己走的 OAuth 流程是公开接口，AIWK 直接复刻它、自己持有一份明文
凭据即可，且不随客户端升级失效::

    python wb_credential.py login     # 扫码/浏览器登录一次
    python wb_credential.py status    # 查看凭据状态与到期时间
    python wb_credential.py refresh   # 手动刷新

凭据落地在脚本同目录的 ``.aiwk_auth.json``（仅本机，含明文 token，不要外传）。
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
import webbrowser

# ---- 官方接口常量（取自 WorkBuddy 客户端 / 公开 OAuth 流程）----
API_ENDPOINT = "https://www.codebuddy.cn"
API_PREFIX = "/v2/plugin"
OAUTH_PLATFORM = "workbuddy"

# 懒刷新阈值：剩余有效期低于该秒数即自动刷新（官方 refresh token 寿命 60 天）。
LAZY_REFRESH_SECS = 24 * 3600
# 扫码登录轮询节奏与总超时。
POLL_INTERVAL = 2.0
LOGIN_TIMEOUT = 300.0

AUTH_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".aiwk_auth.json")


# ===================== 基础工具 =====================

def _origin_of(url: str) -> str:
    """从 URL 取 scheme://host，用于拼与请求 host 一致的 Origin/Referer。"""
    try:
        scheme, rest = url.split("://", 1)
        host = rest.split("/", 1)[0]
        return f"{scheme}://{host}"
    except ValueError:
        return API_ENDPOINT


def _system_proxy() -> str | None:
    try:
        import winreg
    except ImportError:
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
    return server if (enable and server) else None


def http_json(url: str, method: str = "GET", body: dict | None = None,
              headers: dict | None = None, timeout: float = 20.0) -> dict:
    """发一个 JSON 请求并返回解析后的 payload（非 dict 时返回 {}）。"""
    data = json.dumps(body).encode("utf-8") if body is not None else None
    hdrs = {"Accept": "application/json", "Content-Type": "application/json"}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    proxy = _system_proxy()
    if proxy:
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": proxy, "https": proxy}))
    else:
        opener = urllib.request.build_opener()
    with opener.open(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8")
    try:
        out = json.loads(raw)
    except ValueError:
        raise RuntimeError(f"接口返回非 JSON: {raw[:200]}")
    return out if isinstance(out, dict) else {}


def is_envelope(value) -> bool:
    """判断是否为 WorkBuddy 加密信封字段。"""
    return isinstance(value, dict) and bool(value.get("$wbEncrypted"))


def _now_ms() -> int:
    return int(time.time() * 1000)


def _norm_ms(value) -> int | None:
    """把官方返回的 absolutetime 规整成毫秒时间戳（兼容秒 / 毫秒 / 字符串）。"""
    if value is None:
        return None
    try:
        n = int(value)
    except (TypeError, ValueError):
        return None
    return n if n > 10 ** 12 else n * 1000


# ===================== 凭据读写 =====================

def load() -> dict | None:
    """读取 AIWK 自有凭据；文件缺失或损坏时返回 None。"""
    try:
        with open(AUTH_FILE, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not data.get("access_token"):
        return None
    if is_envelope(data.get("access_token")):
        return None
    return data


def save(auth: dict) -> None:
    """原子写入凭据文件（先写临时文件再替换，避免半截 JSON）。"""
    tmp = AUTH_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(auth, fh, ensure_ascii=False, indent=2)
    os.replace(tmp, AUTH_FILE)


def clear() -> None:
    try:
        os.remove(AUTH_FILE)
    except OSError:
        pass


# ===================== OAuth 登录 =====================

def oauth_start() -> tuple[str, str]:
    """申请登录 state，返回 (state, 授权页 URL)。"""
    url = f"{API_ENDPOINT}{API_PREFIX}/auth/state?platform={OAUTH_PLATFORM}"
    resp = http_json(url, "POST", body={})
    data = resp.get("data") or {}
    state = (data.get("state") or "").strip()
    if not state:
        raise RuntimeError(f"auth/state 响应缺少 state: {json.dumps(resp)[:300]}")
    auth_url = (data.get("authUrl") or data.get("auth_url")
                or data.get("url") or "").strip()
    if not auth_url:
        auth_url = f"{API_ENDPOINT}/login?state={state}"
    return state, auth_url


def oauth_poll(state: str) -> dict | None:
    """轮询一次 token 接口；用户尚未授权时返回 None。"""
    url = f"{API_ENDPOINT}{API_PREFIX}/auth/token?state={state}"
    resp = http_json(url, "GET")
    if resp.get("code") not in (0, 200):
        return None
    data = resp.get("data") or {}
    token = data.get("accessToken") or data.get("access_token")
    if not token or is_envelope(token):
        return None
    return data


def oauth_fetch_account(state: str, access_token: str, domain: str) -> dict:
    """用刚拿到的 token 拉账号信息（uid / nickname / enterpriseId）。"""
    url = f"{API_ENDPOINT}{API_PREFIX}/login/account?state={state}"
    headers = {"Authorization": f"Bearer {access_token}"}
    if domain:
        headers["X-Domain"] = domain
    try:
        resp = http_json(url, "GET", headers=headers)
    except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError):
        return {}
    data = resp.get("data")
    return data if isinstance(data, dict) else {}


def _auth_from_token_data(data: dict, profile: dict) -> dict:
    """把官方 token 响应 + 账号信息组装成 AIWK 凭据结构。"""
    expires_at = _norm_ms(data.get("expiresAt") or data.get("expires_at"))
    if expires_at is None and data.get("expiresIn"):
        expires_at = _now_ms() + int(data["expiresIn"]) * 1000
    refresh_expires_at = _norm_ms(
        data.get("refreshExpiresAt") or data.get("refresh_expires_at"))
    if refresh_expires_at is None and data.get("refreshExpiresIn"):
        refresh_expires_at = _now_ms() + int(data["refreshExpiresIn"]) * 1000
    return {
        "access_token": data.get("accessToken") or data.get("access_token"),
        "refresh_token": data.get("refreshToken") or data.get("refresh_token"),
        "token_type": data.get("tokenType") or data.get("token_type") or "Bearer",
        "domain": (data.get("domain") or profile.get("domain") or "").strip(),
        "uid": profile.get("uid"),
        "nickname": profile.get("nickname"),
        "email": profile.get("email"),
        "enterprise_id": profile.get("enterpriseId") or profile.get("enterprise_id"),
        "expires_at": expires_at,
        "refresh_expires_at": refresh_expires_at,
        "obtained_at": _now_ms(),
        "source": "aiwk-oauth",
    }


def login(open_browser: bool = True) -> dict:
    """完整登录流程：申请 state → 引导用户授权 → 轮询取 token → 落盘。"""
    state, auth_url = oauth_start()
    print("=" * 68)
    print("请在浏览器中完成 WorkBuddy 登录授权（已尝试自动打开）：")
    print(f"\n  {auth_url}\n")
    print(f"授权完成后本脚本会自动接管，最长等待 {LOGIN_TIMEOUT:.0f} 秒。")
    print("=" * 68)
    if open_browser:
        try:
            webbrowser.open(auth_url)
        except Exception:
            pass

    deadline = time.time() + LOGIN_TIMEOUT
    while time.time() < deadline:
        data = oauth_poll(state)
        if data:
            profile = oauth_fetch_account(
                state, data.get("accessToken") or data.get("access_token") or "",
                (data.get("domain") or "").strip())
            auth = _auth_from_token_data(data, profile)
            save(auth)
            print(f"登录成功：{auth.get('email') or auth.get('nickname') or auth.get('uid')}")
            print(f"凭据已保存到 {AUTH_FILE}")
            return auth
        time.sleep(POLL_INTERVAL)
    raise RuntimeError("登录超时：未在限定时间内检测到授权完成")


# ===================== token 刷新 =====================

def refresh(auth: dict) -> dict | None:
    """用 refresh token 换新 token；失败返回 None（调用方应提示重新登录）。"""
    rt = (auth or {}).get("refresh_token")
    if not rt:
        return None
    # 刷新必须打账号自身档位的域名，并带 X-Auth-Refresh-Source: plugin，
    # 缺该头时网关会判定成其它 client 来源并返回 invalid_grant。
    endpoint = API_ENDPOINT
    domain = (auth.get("domain") or "").strip()
    if domain.endswith(".workbuddy.ai"):
        endpoint = "https://www.workbuddy.ai"
    url = f"{endpoint}{API_PREFIX}/auth/token/refresh"
    headers = {
        "Authorization": f"Bearer {auth.get('access_token') or ''}",
        "X-Refresh-Token": rt,
        "X-Auth-Refresh-Source": "plugin",
    }
    try:
        resp = http_json(url, "POST", body={}, headers=headers)
    except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError):
        return None
    if resp.get("code") not in (0, 200):
        return None
    data = resp.get("data") or {}
    if not (data.get("accessToken") or data.get("access_token")):
        return None

    merged = dict(auth)
    merged["access_token"] = data.get("accessToken") or data.get("access_token")
    if data.get("refreshToken") or data.get("refresh_token"):
        merged["refresh_token"] = data.get("refreshToken") or data.get("refresh_token")
    expires_at = _norm_ms(data.get("expiresAt") or data.get("expires_at"))
    if expires_at is None and data.get("expiresIn"):
        expires_at = _now_ms() + int(data["expiresIn"]) * 1000
    if expires_at:
        merged["expires_at"] = expires_at
    refresh_expires_at = _norm_ms(
        data.get("refreshExpiresAt") or data.get("refresh_expires_at"))
    if refresh_expires_at:
        merged["refresh_expires_at"] = refresh_expires_at
    merged["refreshed_at"] = _now_ms()
    if (data.get("domain") or "").strip():
        merged["domain"] = data["domain"].strip()
    merged.pop("needs_relogin", None)
    save(merged)
    return merged


def ensure_fresh(auth: dict | None) -> dict | None:
    """惰性刷新：有效期缺失或剩余不足 LAZY_REFRESH_SECS 时先刷新再返回。"""
    if not auth:
        return None
    exp = auth.get("expires_at")
    stale = (not exp) or (_now_ms() >= exp) or (exp - _now_ms() < LAZY_REFRESH_SECS * 1000)
    if not stale:
        return auth
    fresh = refresh(auth)
    if fresh:
        return fresh
    # 刷新失败但旧 token 还没真过期：先用着，下轮再试。
    if exp and _now_ms() < exp:
        return auth
    return None


# ===================== 统一读取入口 =====================

def legacy_auth_status(path: str | None) -> str | None:
    """检查 WorkBuddy 自己的 auth 文件；返回不可用原因，可用时返回 None。"""
    if not path or not os.path.isfile(path):
        return "未找到 WorkBuddy 登录信息，请执行 python wb_credential.py login 登录一次"
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return "WorkBuddy 登录信息解析失败，请执行 python wb_credential.py login 重新登录"
    auth = data.get("auth") or {}
    token = auth.get("accessToken")
    if is_envelope(token):
        return ("WorkBuddy 登录态已加密，无法直接读取积分；"
                "请执行 python wb_credential.py login 登录一次（会自动打开浏览器）")
    if not token or not (data.get("account") or {}).get("uid"):
        return "WorkBuddy 登录信息不完整，请执行 python wb_credential.py login 登录一次"
    return None


def resolve(with_refresh: bool = True) -> tuple[dict | None, str]:
    """凭据统一解析：AIWK 自有凭据优先，其次环境变量注入的明文 token。

    返回 ``(auth, reason)``；``auth`` 为 None 时 ``reason`` 是可读原因。
    """
    auth = load()
    if auth:
        if with_refresh:
            auth = ensure_fresh(auth)
            if auth:
                return auth, ""
            return None, "AIWK 凭据已过期且刷新失败，请重新运行 wb_credential.py login"
        return auth, ""

    # 兜底：允许用环境变量注入明文 token（便于临时排查 / CI）。
    env_token = (os.environ.get("AIWK_ACCESS_TOKEN") or "").strip()
    env_uid = (os.environ.get("AIWK_UID") or "").strip()
    if env_token and env_uid:
        return {
            "access_token": env_token,
            "uid": env_uid,
            "domain": (os.environ.get("AIWK_DOMAIN") or "").strip(),
            "enterprise_id": (os.environ.get("AIWK_ENTERPRISE_ID") or "").strip(),
            "source": "env",
        }, ""

    reason = legacy_auth_status(find_legacy_auth_path())
    if reason is None:
        reason = "已检测到登录信息，请执行 python wb_credential.py login 让本程序持有凭据"
    return None, reason


def find_legacy_auth_path() -> str | None:
    """WorkBuddy 官方 auth 文件位置（与 wb_app.find_auth_path 同源）。"""
    user = os.environ.get("USERPROFILE", "")
    local = os.environ.get("LOCALAPPDATA", os.path.join(user, "AppData", "Local"))
    roamin = os.environ.get("APPDATA", os.path.join(user, "AppData", "Roaming"))
    for base in (local, roamin):
        p = os.path.join(base, "CodeBuddyExtension", "Data", "Public", "auth",
                         "workbuddy-desktop.info")
        if os.path.isfile(p):
            return p
    return None


# ===================== CLI =====================

def _cmd_status() -> int:
    auth = load()
    if not auth:
        reason = legacy_auth_status(find_legacy_auth_path())
        print(f"AIWK 自有凭据：无（{AUTH_FILE} 不存在）")
        print(f"WorkBuddy 官方 auth 文件：{reason or '可用'}")
        print("\n请执行：python wb_credential.py login")
        return 1
    exp = auth.get("expires_at")
    left = f"{(exp - _now_ms()) / 3600000:.1f} 小时" if exp else "未知"
    print(f"账号：{auth.get('email') or auth.get('nickname') or auth.get('uid')}")
    print(f"域名：{auth.get('domain')}")
    print(f"access_token 剩余：{left}")
    print(f"有 refresh_token：{'是' if auth.get('refresh_token') else '否'}")
    print(f"凭据文件：{AUTH_FILE}")
    return 0


def main(argv: list[str]) -> int:
    cmd = (argv[1] if len(argv) > 1 else "status").lower()
    if cmd == "login":
        try:
            login(open_browser="--no-browser" not in argv)
        except Exception as exc:
            print(f"登录失败：{exc}", file=sys.stderr)
            return 1
        return 0
    if cmd == "refresh":
        auth = load()
        if not auth:
            print("没有可刷新的凭据，请先 login", file=sys.stderr)
            return 1
        fresh = refresh(auth)
        if not fresh:
            print("刷新失败，请重新登录：python wb_credential.py login", file=sys.stderr)
            return 1
        print("刷新成功")
        return 0
    if cmd == "logout":
        clear()
        print("已删除 AIWK 凭据")
        return 0
    if cmd == "status":
        return _cmd_status()
    print(__doc__)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
