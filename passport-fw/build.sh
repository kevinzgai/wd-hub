#!/usr/bin/env bash
# 在 WSL 中编译 passport-os。
#
# 用法:
#   bash build.sh          编译本地官方 Jollybot 形象版
#   bash build.sh clean    先清掉 CMake 缓存再全量编译（换 IDF 版本或路径变了才需要）
#   bash build.sh release  用官网发布配置独立构建官方 Jollybot 版并校验
#   bash build.sh diag     内存诊断版(堆任务追踪 + 申请峰值),输出到 build-diag,不可发布
#
# 注意:ESP-IDF 装在 WSL 的 /opt/esp-idf,Windows 侧没有工具链,所以编译必须在 WSL 里跑。
# 烧录走 Windows 的 COM3,WSL 默认看不到,需要 usbipd 绑定,或直接在 Windows 上用 esptool。
set -u

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IDF_DIR="${IDF_DIR:-/opt/esp-idf}"

cd "$PROJECT_DIR" || { echo "找不到项目目录 $PROJECT_DIR"; exit 1; }

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "找不到 ESP-IDF: $IDF_DIR/export.sh"
    echo "如有其他位置,用 IDF_DIR=/path/to/esp-idf bash build.sh 指定"
    exit 1
fi

echo "==> 载入 ESP-IDF 环境 ($IDF_DIR)"
set +e
. "$IDF_DIR/export.sh" >/dev/null 2>&1
rc=$?
set -e
if [ $rc -ne 0 ]; then
    echo "export.sh 执行失败,请检查 IDF 安装"
    exit 1
fi

if [ "${1:-}" = "release" ]; then
    # Seed from tracked defaults every time. Local sdkconfig may silently retain
    # obsolete TLS/flash options even after sdkconfig.defaults has been updated.
    mkdir -p build-public
    cp -- sdkconfig.defaults build-public/sdkconfig
    idf.py -B build-public -DSDKCONFIG="$PROJECT_DIR/build-public/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=ON \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build \
        merge-bin -o FoloToy-AI-Passport-full.bin
    python tools/verify_firmware.py build-public --public
    exit 0
fi

if [ "${1:-}" = "diag" ]; then
    mkdir -p build-diag
    cat -- sdkconfig.defaults sdkconfig.diag > build-diag/sdkconfig
    idf.py -B build-diag -DSDKCONFIG="$PROJECT_DIR/build-diag/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
    exit 0
fi

if [ "${1:-}" = "clean" ]; then
    echo "==> 清理构建目录"
    rm -rf build
fi

echo "==> 开始编译（本地官方 Jollybot 形象版）"
# Never reuse the ignored root sdkconfig: it may preserve obsolete options even
# after defaults change. Keep the expanded generated config inside the build
# directory and seed it from the tracked memory budget on every invocation.
mkdir -p build
cp -- sdkconfig.defaults build/sdkconfig
idf.py -B build -DSDKCONFIG="$PROJECT_DIR/build/sdkconfig" \
    -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
    -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
