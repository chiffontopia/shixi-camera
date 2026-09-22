#!/bin/sh
# ============================================================
#  build_curl_arm.sh — 交叉编译一个「静态、纯 HTTP」的 curl 给开发板用
#
#  为什么不能用现成的：
#    板上是 glibc 2.23，既没有 libssl.so.1.1 也没有 libcurl.so.4，
#    任何动态链接的 ARM curl 都跑不起来（/mnt/e/embd/curl-arm.tar.gz 正是这种：
#    NEEDED libcurl.so.4 + libssl.so.1.1）。
#  做法：只编 HTTP（连宿主机的转发服务，不需要 TLS），静态链接 → 单文件无依赖，
#        拷到板上任意位置都能跑。
#
#  产出：bin/curl-arm  （约 1.4 MB，`file` 显示 statically linked）
#  用法：./tools/build_curl_arm.sh [版本号]        默认 8.11.1
#  依赖：能上网（下载源码）、arm-linux-gcc
# ============================================================
set -e

VER="${1:-8.11.1}"
CROSS="${CROSS:-arm-linux-}"
CC="${CC:-${CROSS}gcc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
BUILD="${BUILD:-/tmp/shixi-curl-build}"

echo "==> curl $VER，编译器 $CC"
command -v "$CC" >/dev/null 2>&1 || { echo "找不到 $CC（试试 make CROSS=arm-linux- 或装好工具链）"; exit 1; }

mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f "curl-$VER.tar.gz" ]; then
    echo "==> 下载源码"
    curl -sSLO "https://curl.se/download/curl-$VER.tar.gz"
fi
[ -d "curl-$VER" ] || tar xzf "curl-$VER.tar.gz"
cd "curl-$VER"

if [ ! -f Makefile ]; then
    # arm-linux- → arm-linux（configure 要的是去掉末尾横杠的三元组）
    HOST_TRIPLE="${CROSS%gcc}"
    HOST_TRIPLE="${HOST_TRIPLE%-}"
    echo "==> configure (--host=$HOST_TRIPLE)"
    # shellcheck disable=SC2086
    CC="$CC" ./configure --host="$HOST_TRIPLE" --prefix="$BUILD/out" \
        --enable-static --disable-shared \
        --without-ssl --without-zlib --without-brotli --without-zstd \
        --without-libpsl --without-libidn2 --without-librtmp --without-libssh2 --without-nghttp2 \
        --disable-ares --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
        --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
        --disable-gopher --disable-mqtt --disable-manual --disable-ipv6 \
        > "$BUILD/configure.log" 2>&1 || { tail -20 "$BUILD/configure.log"; exit 1; }
fi

echo "==> make"
make -j"$(nproc)" > "$BUILD/make.log" 2>&1 || { tail -30 "$BUILD/make.log"; exit 1; }

# 注意：configure 里的 LDFLAGS=-static 会被 libtool 当成「只用静态库」的模式开关吃掉，
# 链接出来的仍是动态可执行文件。要完全静态必须用 libtool 的 -all-static 重新链接一次。
echo "==> 静态重新链接（libtool 的 -static 不够，要用 -all-static）"
cd src
rm -f curl
make LDFLAGS="-all-static" curl > "$BUILD/relink.log" 2>&1 || { tail -20 "$BUILD/relink.log"; exit 1; }

mkdir -p "$ROOT/bin"
cp curl "$ROOT/bin/curl-arm"
echo "==> 产出 $ROOT/bin/curl-arm"
file "$ROOT/bin/curl-arm"
ls -l "$ROOT/bin/curl-arm"
