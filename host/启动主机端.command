#!/bin/bash
# WorkBuddy 主机端启动（工程版）
#
# ⚠ 为什么必须双击这个脚本、不能在别处后台启动：
#   macOS 的 TCC 隐私沙盒要求「蓝牙」权限绑定在**有 GUI 会话的前台应用**上。
#   从 IDE / 后台 shell / launchd 直接跑 python，CoreBluetooth 一扫描设备
#   就被系统 SIGABRT 掉（崩溃报告里是 TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION）。
#   Terminal.app 已经有了蓝牙授权，所以经由它启动的子进程才合法。
#
# 用法：
#   双击运行
#   WEB_PORT=9090 ./启动主机端.command     改 Web UI 端口（默认 8080）
#   WB_HOST=0.0.0.0 ./启动主机端.command    放开监听，同一局域网手机可访问（默认 127.0.0.1）
#   ./启动主机端.command --stop            停止已在运行的服务

cd "$(dirname "$0")" || exit 1

WEB_PORT="${WEB_PORT:-8080}"
LOG_FILE="$HOME/.workbuddy/wb-app.log"
PID_FILE="$HOME/.workbuddy/wb-app.pid"

# 优先用系统安装的 Python：WorkBuddy 自带那个缺 pystray/Pillow。
PY=""
for c in \
    /Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
    /Library/Frameworks/Python.framework/Versions/3.12/bin/python3 \
    /usr/bin/python3
do
    if [ -x "$c" ] && "$c" -c "import bleak, certifi" 2>/dev/null; then
        PY="$c"; break
    fi
done
if [ -z "$PY" ]; then
    echo "[错误] 没找到装了 bleak 的 Python 3.9+。"
    echo "       试：/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -m pip install --user bleak pystray Pillow certifi"
    exit 1
fi
echo "使用 Python：$PY"

running_pid() {
    if [ -f "$PID_FILE" ]; then
        p="$(cat "$PID_FILE" 2>/dev/null)"
        if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then echo "$p"; return 0; fi
    fi
    lsof -nP -iTCP:"$WEB_PORT" -sTCP:LISTEN 2>/dev/null | awk 'NR==2{print $2}'
}

if [ "$1" = "--stop" ]; then
    pid="$(running_pid)"
    if [ -z "$pid" ]; then echo "没有正在运行的 WorkBuddy 服务。"; exit 0; fi
    kill "$pid" 2>/dev/null
    sleep 2
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
    rm -f "$PID_FILE"
    echo "已停止（PID $pid）。"
    exit 0
fi

mkdir -p "$HOME/.workbuddy"

# CA 证书：Python 默认不读 macOS 钥匙串。公司/校园网络常有 HTTPS 中继根证书，
# 只装在钥匙串里，缺失时积分查询会报 self-signed certificate in certificate chain。
CA_FILE="$HOME/.workbuddy/ca.pem"
{
    cat "$("$PY" -c 'import certifi; print(certifi.where())')"
    security find-certificate -a -p /Library/Keychains/System.keychain 2>/dev/null
    security find-certificate -a -p "$HOME/Library/Keychains/login.keychain-db" 2>/dev/null
} > "$CA_FILE" 2>/dev/null
export SSL_CERT_FILE="$CA_FILE"

pid="$(running_pid)"
if [ -n "$pid" ]; then
    echo "已在运行（PID $pid），Web UI: http://127.0.0.1:$WEB_PORT"
    exit 0
fi
if lsof -nP -iTCP:"$WEB_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "[错误] 端口 $WEB_PORT 已被占用，用 WEB_PORT=9090 ./启动主机端.command 换端口。"
    exit 1
fi

echo "正在启动（Web UI: http://127.0.0.1:$WEB_PORT）"
# 输出同时进日志文件和本窗口：出错时窗口里能直接看到原因，
# 而日志文件供事后排查（原来只写窗口，关掉就没了）。
#
# 注意不能用 `python ... | tee`：管道会把 $! 变成 tee 的 PID，
# 后面 kill -0 检测的就不是 python 了，进程死了也看不出来。
# 这里用进程替换让 python 本体保持为后台作业。
"$PY" -u wb_app.py --tray --web-host "${WB_HOST:-127.0.0.1}" --web-port "$WEB_PORT" --open-browser \
    > >(tee -a "$LOG_FILE") 2>&1 &
new_pid=$!
echo "$new_pid" > "$PID_FILE"

sleep 5
if ! kill -0 "$new_pid" 2>/dev/null; then
    # 程序自己的报错已经实时显示在上方（tee 到了本窗口），这里只补一句
    # "去日志里看"，不再重复贴同一段内容——否则同一个原因会连打三遍。
    echo
    echo "[错误] 启动失败（PID $new_pid 已退出）。原因见上方输出。"
    echo "       完整日志：$LOG_FILE"
    rm -f "$PID_FILE"
    exit 1
fi

echo "已启动（PID $new_pid）。这个窗口可以关掉。"
echo "日志：$LOG_FILE"
echo "停止：./启动主机端.command --stop"
