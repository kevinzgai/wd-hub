#!/usr/bin/env python3
"""
ESP-IDF 脚本启动器 —— 绕开 Windows + Git Bash 下的三个坑。

坑 1: Git Bash 设了 MSYSTEM=MINGW64，idf_tools.py / idf.py 见到就拒绝运行。
      MSYS 的 `env -u` 对此无效（对大小写不敏感处理有 bug），只能进程内 pop。
坑 2: 环境里同时有 HTTP_PROXY / http_proxy 等重复变量，PowerShell 的 Env: 驱动器
      会抛 "已添加了具有相同键的项"。
坑 3: PowerShell 把 python.exe 当文档打开（文件关联被改坏），& $py 直接失败。

用法:
    <idf-python> tools/idfwrap.py <idf脚本> [参数...]

例:
    python_env/Scripts/python.exe tools/idfwrap.py "C:/esp/v6.1/esp-idf/tools/idf.py" build -p COM5
"""

import os
import runpy
import subprocess
import sys

# 默认环境，可用环境变量覆盖
IDF_PATH = os.environ.get("IDF_PATH", r"C:\esp\v6.1\esp-idf").rstrip("\\")
IDF_TOOLS_PATH = os.environ.get("IDF_TOOLS_PATH", os.path.expanduser(r"~\.espressif"))

# GitHub 直连在国内下不动，工具链走乐鑫镜像
GITHUB_ASSETS_MIRROR = "dl.espressif.com/github_assets"


def _fill_missing_env():
    """Git Bash 里有些 Windows 变量是空的，ccache 之类会因此罢工。"""
    home = os.path.expanduser("~")
    defaults = {
        "LOCALAPPDATA": os.path.join(home, "AppData", "Local"),
        "APPDATA": os.path.join(home, "AppData", "Roaming"),
        "USERPROFILE": home,
        "SystemRoot": r"C:\Windows",
        "COMSPEC": r"C:\Windows\System32\cmd.exe",
    }
    for k, v in defaults.items():
        if not os.environ.get(k):
            os.environ[k] = v


def _apply_toolchain_path():
    """把 xtensa 工具链 / cmake / ninja 塞进 PATH。

    不调 `idf_tools.py export` —— 它见到没装的 riscv-gdb 会直接报错退出，
    而 esp32s3 根本用不到 riscv。自己扫目录更稳。
    """
    tools_root = os.path.join(IDF_TOOLS_PATH, "tools")
    # (工具名, 可执行文件所在子路径)
    layout = [
        ("xtensa-esp-elf", r"xtensa-esp-elf\bin"),
        ("cmake", r"bin"),
        ("ninja", ""),
        ("idf-exe", ""),
        ("ccache", ""),
    ]
    added = []
    for tool, sub in layout:
        base = os.path.join(tools_root, tool)
        if not os.path.isdir(base):
            continue
        for ver in sorted(os.listdir(base), reverse=True):
            p = os.path.join(base, ver, sub) if sub else os.path.join(base, ver)
            if os.path.isdir(p):
                added.append(p)
                break  # 每样工具只用最新版本

    if added:
        os.environ["PATH"] = os.pathsep.join(added + [os.environ.get("PATH", "")])

    # ESP_ROM_ELF_DIR 不设的话 gen_gdbinit.py 会抛异常，而 CMake 只当 warning 吞掉，
    # 随后 bootloader 子项目 configure 阶段直接死锁（obj 不再增长，日志静止）。
    rom = os.path.join(tools_root, "esp-rom-elfs")
    if os.path.isdir(rom):
        versions = sorted(os.listdir(rom), reverse=True)
        if versions:
            os.environ["ESP_ROM_ELF_DIR"] = os.path.join(rom, versions[0])


def main():
    if len(sys.argv) < 2:
        sys.exit("用法: idfwrap.py <idf脚本> [参数...]")

    # 坑 1
    os.environ.pop("MSYSTEM", None)
    os.environ.pop("MSYSCON", None)

    # 坑 2 —— 同时避免 Clash 对 github release 返回 502
    for k in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
              "ALL_PROXY", "all_proxy"):
        os.environ.pop(k, None)

    _fill_missing_env()

    os.environ["IDF_PATH"] = IDF_PATH
    os.environ.setdefault("IDF_TOOLS_PATH", IDF_TOOLS_PATH)
    os.environ.setdefault("IDF_GITHUB_ASSETS", GITHUB_ASSETS_MIRROR)
    os.environ.setdefault("IDF_PYTHON_ENV_PATH", sys.prefix)
    os.environ.setdefault("ESP_IDF_VERSION", _read_idf_version())

    _apply_toolchain_path()

    script = sys.argv[1].replace("\\", "/")
    sys.argv = [script] + sys.argv[2:]

    # 脚本同目录往往有自己的模块，要进 sys.path
    script_dir = os.path.dirname(os.path.abspath(script))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)

    # ⚠️ 不要 chdir 到 IDF_PATH。
    # idf.py 把**当前工作目录**当作项目根，切到 IDF_PATH 后会报
    # "Current directory is not buildable"。保持调用者的 cwd，
    # 也就是在 firmware/ 下运行即可正确找到 CMakeLists.txt。
    # （旧版本这里 chdir 过，导致 build/flash 全部失败。）
    runpy.run_path(script, run_name="__main__")


def _read_idf_version() -> str:
    try:
        with open(os.path.join(IDF_PATH, "version.txt"), encoding="utf-8") as f:
            return f.read().strip() or "6.1"
    except OSError:
        return "6.1"


if __name__ == "__main__":
    main()
