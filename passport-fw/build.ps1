# 在 Windows / PowerShell 下编译 passport-fw（ESP32-C3）。
#
# 底座原本只提供 WSL 的 build.sh；本工程新增这个入口。使用仓库
# toolchain/ 下的 ESP-IDF v5.5.3（底座 README 指定的版本），并复用
# 本机已安装的 riscv32-esp-elf 14.2.0_20251107 与 idf5.5 Python 环境。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\build.ps1              # 增量编译
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 clean        # 清 build 后全量
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 menuconfig   # 配置界面
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 flash        # 烧到 COM6
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 -p COM6 flash
#
# 可用环境变量覆盖：
#   PASSPORT_IDF_PATH   指定其它 ESP-IDF 路径
#   PASSPORT_IDF_TOOLS  指定 IDF_TOOLS_PATH（默认 ~/.espressif）

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs = @("build")
)

$ErrorActionPreference = "Stop"
$Project = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo    = Split-Path -Parent $Project

$IdfPath = Join-Path $Repo "toolchain\esp-idf-v5.5.3"
if ($env:PASSPORT_IDF_PATH) { $IdfPath = $env:PASSPORT_IDF_PATH }
$Tools = Join-Path $env:USERPROFILE ".espressif"
if ($env:PASSPORT_IDF_TOOLS) { $Tools = $env:PASSPORT_IDF_TOOLS }
$PyEnv    = Join-Path $Tools "python_env\idf5.5_py3.13_env"
$Py       = Join-Path $PyEnv "Scripts\python.exe"
$IdfPy    = Join-Path $IdfPath "tools\idf.py"
$Export   = Join-Path $IdfPath "export.ps1"

if (-not (Test-Path $IdfPy))   { throw "找不到 ESP-IDF v5.5.3: $IdfPath" }
if (-not (Test-Path $Py))      { throw "找不到 idf5.5 Python 环境: $PyEnv" }
if (-not (Test-Path $Export))  { throw "找不到 export.ps1: $Export" }

if ($IdfArgs.Count -gt 0 -and $IdfArgs[0] -eq "clean") {
    Remove-Item -Recurse -Force (Join-Path $Project "build") -ErrorAction SilentlyContinue
    $IdfArgs = @("build")
}
# flash 未指定端口时默认 COM6（AI Passport 的 USB-Serial/JTAG）
if ($IdfArgs.Count -gt 0 -and $IdfArgs -contains "flash" -and $IdfArgs -notcontains "-p") {
    $IdfArgs = @("-p", "COM6") + $IdfArgs
}

$env:IDF_PATH             = $IdfPath
$env:IDF_TOOLS_PATH       = $Tools
$env:IDF_PYTHON_ENV_PATH  = $PyEnv
$env:ESP_IDF_VERSION      = "5.5.3"
$env:ESP_ROM_ELF_DIR      = Join-Path $Tools "tools\esp-rom-elfs\20241011\"

# 注意：不要 source export.ps1 —— 它默认检查 tools.json 里全部 target 的
# 工具链（含 xtensa），而本机只装了 esp32c3 所需的 riscv 工具。这里按
# tools.json 锁定的版本显式拼 PATH，只覆盖 C3 构建实际用到的工具。
$ToolPaths = @(
    (Join-Path $Tools "tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin"),
    (Join-Path $Tools "tools\cmake\3.30.2\bin"),
    (Join-Path $Tools "tools\ninja\1.12.1"),
    (Join-Path $Tools "tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"),
    (Join-Path $PyEnv "Scripts")
)
foreach ($p in $ToolPaths) {
    if (-not (Test-Path $p)) { throw "缺少工具路径: $p" }
}
$env:PATH = ($ToolPaths -join ";") + ";" + $env:PATH

& $Py $IdfPy @IdfArgs
exit $LASTEXITCODE
