#!/usr/bin/env python3
"""补齐 ESP-IDF 的 git submodule。

## 为什么需要这个脚本

ESP-IDF 的 GitHub tarball（codeload 下载的源码包）**不含 submodule 内容**，
`components/mbedtls/mbedtls/` 之类全是空目录，cmake 一跑就报
"Include directory ... is not a directory"。

正常做法是 `git submodule update --init --recursive`，但本机 github.com
的 git/https 都不通（只有 codeload.github.com 和 api.github.com 能连）。
所以改为：

  api.github.com    → 查每个 submodule 指针的 commit SHA
  codeload.github.com → 按 SHA 下 tarball
  解压到对应路径

## 用法

    python tools/fetch_idf_submodules.py                     # 只补构建必需的
    python tools/fetch_idf_submodules.py --all                # 补全部（含蓝牙）
    python tools/fetch_idf_submodules.py --idf ~/esp/esp-idf-6.1
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

API = "https://api.github.com/repos/espressif/esp-idf/contents"
CODELOAD = "https://codeload.github.com"
REF = "v6.1"

# 构建 esp32s3 WiFi 固件必需的 submodule。蓝牙/OpenThread 的 lib 体积大且
# 这套固件没启用（sdkconfig 里没有 CONFIG_BT_ENABLED），默认跳过。
ESSENTIAL = [
    "components/mbedtls/mbedtls",
    "components/lwip/lwip",
    "components/esp_phy/lib",
    "components/esp_wifi/lib",
    "components/heap/tlsf",
    "components/bootloader/subproject/components/micro-ecc/micro-ecc",
    "components/spiffs/spiffs",
    "components/esp_coex/lib",
    "components/protobuf-c/protobuf-c",
]

SKIP_MARKERS = ("components/bt/", "components/openthread/",
                "components/unity/", "components/cmock/")


def http_json(url: str, retries: int = 3) -> object:
    last = None
    for i in range(retries):
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": "idf-submodule-fetcher"})
            with urllib.request.urlopen(req, timeout=45) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:                      # noqa: BLE001
            last = e
            time.sleep(2 * (i + 1))
    raise RuntimeError(f"请求失败 {url}: {last}")


def parse_gitmodules(idf: Path) -> dict[str, str]:
    """path → 仓库（espressif/xxx）。url 形如 ../../espressif/mbedtls.git"""
    text = (idf / ".gitmodules").read_text(encoding="utf-8")
    out: dict[str, str] = {}
    cur_path = cur_url = None
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r"path\s*=\s*(.+)", line)
        if m:
            cur_path = m.group(1).strip()
            continue
        m = re.match(r"url\s*=\s*(.+)", line)
        if m:
            cur_url = m.group(1).strip()
            if cur_path and cur_url:
                repo = cur_url
                repo = re.sub(r"^\.\./\.\./", "", repo)
                repo = re.sub(r"\.git$", "", repo)
                # 相对路径 ../../espressif/x → espressif/x；绝对 URL 取末两段
                if "/" in repo:
                    parts = [p for p in repo.split("/") if p]
                    repo = "/".join(parts[-2:])
                if "/" in repo:
                    out[cur_path] = repo
            cur_path = cur_url = None
    return out


def submodule_sha(idf: Path, path: str) -> str | None:
    """查某个 submodule 的 commit SHA。

    path 形如 components/mbedtls/mbedtls → 查 components/mbedtls 目录，
    找名为 mbedtls 的条目。GitHub 对 submodule 有时返 type=file，
    sha 就是 gitlink 指向的 commit。
    """
    parent, _, name = path.rpartition("/")
    try:
        entries = http_json(f"{API}/{parent}?ref={REF}")
    except Exception as e:                          # noqa: BLE001
        print(f"    ! 查目录失败: {e}")
        return None
    if not isinstance(entries, list):
        return None
    for e in entries:
        if e.get("name") == name:
            sha = e.get("sha")
            if sha:
                return sha
    return None


def download_extract(repo: str, sha: str, dest: Path) -> bool:
    url = f"{CODELOAD}/{repo}/tar.gz/{sha}"
    tmp = Path("/tmp") / f"sm-{sha[:12]}.tgz"
    try:
        subprocess.run(["curl", "-sL", "--max-time", "300", "--retry", "2",
                        "-o", str(tmp), url], check=True)
    except subprocess.CalledProcessError as e:
        print(f"    ! 下载失败: {e}")
        return False

    if not tmp.is_file() or tmp.stat().st_size < 1024:
        print(f"    ! 下载文件异常: {tmp.stat().st_size if tmp.is_file() else 0} B")
        return False

    # tarball 顶层是 <repo>-<sha>/，用 --strip-components=1 摊平到目标目录
    dest.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(["tar", "-xzf", str(tmp), "-C", str(dest),
                        "--strip-components=1"],
                       capture_output=True, text=True)
    try:
        tmp.unlink()
    except OSError:
        pass
    if r.returncode != 0:
        print(f"    ! 解压失败: {r.stderr[:120]}")
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="补齐 ESP-IDF submodule")
    ap.add_argument("--idf", default=os.path.expanduser("~/esp/esp-idf-6.1"))
    ap.add_argument("--all", action="store_true", help="补全部，含蓝牙/OpenThread")
    args = ap.parse_args()

    idf = Path(args.idf).expanduser()
    if not (idf / ".gitmodules").is_file():
        print(f"[ERROR] {idf} 不像 ESP-IDF 目录（缺 .gitmodules）")
        return 1

    repos = parse_gitmodules(idf)
    if args.all:
        targets = [p for p in repos if p not in
                   ("components/unity/unity", "components/cmock/CMock")]
    else:
        targets = [p for p in ESSENTIAL if p in repos]

    def _empty(p: str) -> bool:
        """目录不存在或为空 = 待补。目录不存在时 iterdir() 会抛异常，必须判 is_dir。"""
        d = idf / p
        if not d.is_dir():
            return True
        try:
            return not any(d.iterdir())
        except OSError:
            return True

    missing = [p for p in targets if _empty(p)]

    print(f"ESP-IDF: {idf}")
    print(f"待补 submodule: {len(missing)} / 候选 {len(targets)}\n")

    ok = fail = skip = 0
    for path in missing:
        repo = repos.get(path)
        print(f"  [{path}]")
        print(f"    repo = {repo}")
        if not repo:
            print("    ! .gitmodules 里没解析出仓库名，跳过")
            skip += 1
            continue

        sha = submodule_sha(idf, path)
        if not sha:
            print("    ! 没拿到 SHA，跳过")
            skip += 1
            continue
        print(f"    sha  = {sha}")

        if download_extract(repo, sha, idf / path):
            n = len(list((idf / path).iterdir()))
            print(f"    OK   -> {n} 个条目")
            ok += 1
        else:
            fail += 1

    print(f"\n完成: 成功 {ok} / 失败 {fail} / 跳过 {skip}")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
