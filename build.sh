#!/bin/bash
# build.sh — 编译 daemon
#
# 用法:
#   chmod +x build.sh && ./build.sh
#
# 依赖:
#   - Android NDK (推荐)，或
#   - aarch64-linux-gnu-gcc (Ubuntu 交叉编译)，或
#   - Termux clang (手机上直接编译)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/redirect_daemon.c"
OUT_DIR="$SCRIPT_DIR/bin"
OUT="$OUT_DIR/daemon"

echo "============================================"
echo "  易の重定向 — Daemon 编译"
echo "============================================"

# 检查源文件
if [ ! -f "$SRC" ]; then
    echo "错误: 找不到 $SRC"
    exit 1
fi

mkdir -p "$OUT_DIR"

# 检测编译器
CC=""
EXTRA_FLAGS=""

if [ -n "$ANDROID_NDK_HOME" ] || [ -n "$NDK_HOME" ]; then
    NDK="${ANDROID_NDK_HOME:-$NDK_HOME}"
    TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

    # 尝试不同 API level
    for API in 34 33 32 31 30 29 28 26 24; do
        CANDIDATE="$TOOLCHAIN/aarch64-linux-android${API}-clang"
        if [ -f "$CANDIDATE" ]; then
            CC="$CANDIDATE"
            break
        fi
    done

    if [ -z "$CC" ]; then
        echo "错误: NDK 中找不到 aarch64 clang"
        echo "检查: ls $TOOLCHAIN/aarch64-linux-android*-clang"
        exit 1
    fi
    echo "编译器: NDK ($CC)"

elif command -v aarch64-linux-gnu-gcc &>/dev/null; then
    CC="aarch64-linux-gnu-gcc"
    echo "编译器: aarch64-linux-gnu-gcc"

elif command -v aarch64-linux-android-clang &>/dev/null; then
    CC="aarch64-linux-android-clang"
    echo "编译器: Termux clang"

elif command -v gcc &>/dev/null && [ "$(uname -m)" = "aarch64" ]; then
    CC="gcc"
    echo "编译器: 本机 gcc (aarch64)"

else
    echo "============================================"
    echo "找不到编译器！请安装以下之一："
    echo ""
    echo "1. Android NDK:"
    echo "   https://developer.android.com/ndk/downloads"
    echo "   然后: export ANDROID_NDK_HOME=/path/to/ndk"
    echo ""
    echo "2. Ubuntu/Debian 交叉编译:"
    echo "   apt install gcc-aarch64-linux-gnu"
    echo ""
    echo "3. Termux (手机上):"
    echo "   pkg install clang"
    echo "============================================"
    exit 1
fi

echo "源文件: $SRC"
echo "输出:   $OUT"
echo ""

# 编译
$CC -static -O2 -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" \
    "$SRC" \
    -lpthread \
    $EXTRA_FLAGS

echo ""
echo "============================================"
echo "  编译成功!"
echo "============================================"
ls -lh "$OUT"
file "$OUT"
echo ""
echo "将 bin/daemon 放入模块目录即可使用"
