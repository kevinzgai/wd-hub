#!/bin/bash
# 一键跑全部测试。
#
#   ./tests/run_all.sh
#
# 主机侧（Python，直接 import 真实的 host/wb_app.py）：
#   test_host_ask       —— _decode_ask_picks 解码（index / mask / 越界）
#   test_ask_flow       —— _handle_ask_answer 全分支（含 2026-10-31 修的丢答案 bug）
#   test_multi_negotiate—— CAP_ASK_MULTI 能力协商与多选降级
#
# 固件侧（C，直接编译真实的 wb_core.c / wb_proto.c / wb_json.c）：
#   test_ask            —— 提问页协议 + 状态机
#   test_usage_frame    —— usage 帧解码
#   test_standby        —— 待机页
#   test_multi_downgrade—— 固件侧多选降级（WB_CAPS 口径）
#
# 注意：固件侧测试只链接核心层，不含 wb_view.c —— 后者依赖 LVGL，
# 需要 ESP-IDF 环境才能编。

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
FW="$ROOT/firmware/main"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

report_ok() {
    echo "  [PASS] $1"
    PASS=$((PASS+1))
}

report_fail() {
    echo "  [FAIL] $1"
    FAIL=$((FAIL+1))
    if [ -f "$2" ]; then
        sed 's/^/         /' "$2" | tail -20
    fi
}

run_py() {
    name=$1
    log="$TMP/$name.log"
    if python3 "$HERE/$name.py" > "$log" 2>&1; then
        report_ok "$name"
    else
        report_fail "$name" "$log"
    fi
}

run_c() {
    name=$1
    blog="$TMP/$name.build.log"
    clog="$TMP/$name.log"
    if gcc -I"$FW" -o "$TMP/$name" "$HERE/$name.c" \
        "$FW/wb_core.c" "$FW/wb_proto.c" "$FW/wb_json.c" > "$blog" 2>&1
    then
        if "$TMP/$name" > "$clog" 2>&1; then
            report_ok "$name"
        else
            report_fail "$name" "$clog"
        fi
    else
        report_fail "$name (编译失败)" "$blog"
    fi
}

echo "主机侧："
run_py test_encode_cwd
run_py test_host_ask
run_py test_ask_flow
run_py test_multi_negotiate
run_py test_transcript_scan

echo "固件侧："
run_c test_ask
run_c test_usage_frame
run_c test_standby
run_c test_multi_downgrade

echo ""
echo "通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
exit 0
