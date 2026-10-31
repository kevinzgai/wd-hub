#!/usr/bin/env bash
# macOS / Linux 启动看板服务。与 Windows 的 start-board.bat 等价。
#
#   ./start-board.sh              # 前台启动，Ctrl+C 停止
#   ./start-board.sh --port 9000  # 换端口
#
# 首次运行会自动建 .venv 并装依赖（国内镜像，PyPI 直连常常 SSL 断流）。

set -uo pipefail
cd "$(dirname "$0")"

PORT=27777
REFRESH=2.0
HOST="0.0.0.0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port|--refresh|--host)
      # ⚠ 缺值检查：原来 `$2` 为空时 shift 2 会报 "shift: count out of range"，
      # 而且 --port 后面不跟值时 PORT 被赋成空串，后面 lsof -iTCP: 出错，
      # 报错信息完全指不到是哪个参数缺值。
      opt="$1"
      if [[ $# -lt 2 || -z "$2" ]]; then
        echo "[ERROR] $opt 缺少参数值"
        echo "       用法: $0 [--port 端口] [--refresh 秒] [--host 地址]"
        exit 1
      fi
      case "$opt" in
        --port)    PORT="$2" ;;
        --refresh) REFRESH="$2" ;;
        --host)    HOST="$2" ;;
      esac
      shift 2
      ;;
    -h|--help)
      echo "用法: $0 [--port 端口] [--refresh 秒] [--host 地址]"
      echo "  --port     监听端口，默认 27777"
      echo "  --refresh  采集刷新间隔（秒），默认 2.0"
      echo "  --host     监听地址，默认 0.0.0.0"
      exit 0
      ;;
    *) echo "[ERROR] 未知参数: $1"; echo "       用 $0 --help 看用法"; exit 1 ;;
  esac
done

# ---- 参数合法性 ----
if ! [[ "$PORT" =~ ^[0-9]+$ ]] || [[ "$PORT" -lt 1 ]] || [[ "$PORT" -gt 65535 ]]; then
  echo "[ERROR] 端口必须是 1~65535 的整数，当前是: $PORT"
  exit 1
fi

# ---- 端口占用检查 ----
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "[INFO] 端口 $PORT 已被占用 —— 服务可能已经在跑了。"
  echo "       占用进程："
  lsof -nP -iTCP:"$PORT" -sTCP:LISTEN | tail -n +2
  echo "       要重启的话，先停掉上面那个进程。"
  exit 0
fi

# ---- 准备 Python 环境 ----
PY=""
for cand in python3.13 python3.12 python3 python; do
  if command -v "$cand" >/dev/null 2>&1; then PY="$cand"; break; fi
done
if [[ -z "$PY" ]]; then
  echo "[ERROR] 没找到 python3。装一个 3.11+ 的再试。"
  exit 1
fi
echo "[INFO] 使用 $PY ($("$PY" --version 2>&1))"

if [[ ! -d .venv ]]; then
  echo "[INFO] 创建虚拟环境 .venv ..."
  "$PY" -m venv .venv || exit 1
fi

VPY=".venv/bin/python"
if ! "$VPY" -c "import fastapi, uvicorn, psutil" >/dev/null 2>&1; then
  echo "[INFO] 安装依赖（阿里云镜像，首次约 1 分钟）..."
  "$VPY" -m pip install -q --upgrade pip
  "$VPY" -m pip install -q -i https://mirrors.aliyun.com/pypi/simple \
        --trusted-host mirrors.aliyun.com -r server/requirements.txt || {
    echo "[ERROR] 依赖安装失败。手动跑："
    echo "        $VPY -m pip install -r server/requirements.txt"
    exit 1
  }
fi

# ---- 启动 ----
exec "$VPY" -m server.api_server --port "$PORT" --host "$HOST" --refresh "$REFRESH"
