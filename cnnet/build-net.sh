#!/bin/bash
# ==================================================================
#  build-net.sh —— 构建 Canon 网络通信层的三份 loong64 原生库
#
#  同一个 cnnet_native.c 编三次，靠 SONAME + 符号版本脚本(.map) 区分：
#    libcnnet2.so.1.2.5     -> CNNL_*   （旧通信接口）
#    libcnbpnet30.so.1.0.0  -> CNNET3_* （传输层：9100 RAW + CHMP over HTTP）
#    libcnbpnet20.so.1.0.0  -> CNNET2_* （SNMP v1 发现）
#
#  .map 是链接期符号版本脚本：每个库只导出自己那一套前缀，其余一律隐藏，
#  与原厂三个库各司其职的行为一致。不接 .map 的话三套符号会全部导出。
#
#  已真机验证：
#    * 发现 : SNMP 单播
#    * 打印 : TCP 9100 RAW，灌 rastertocanonij 输出 → 出纸
#    * 状态 : CHMP POST+GET（chunked）
#
#  用法:
#    ./build-net.sh              # release 构建，产物 → ../build/built/
#    ./build-net.sh debug        # -g -O0，带 CHMP 交互日志
#    OUT=/path ./build-net.sh    # 自定义输出目录
# ==================================================================
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="${OUT:-$REPO/build/built}"          # 与其他构件同放 build/built/

EXTRA="-O2"
if [ "${1:-}" = "debug" ]; then
    EXTRA="-D_DEBUG_MODE_ -g -O0"
    echo "== DEBUG 构建（含 CHMP 交互日志） =="
else
    echo "== RELEASE 构建 =="
fi

mkdir -p "$OUT"

build() {   # build <输出文件名> <soname> <符号版本脚本>
    local out="$1" soname="$2" map="$3"
    gcc $EXTRA -fPIC -shared -o "$OUT/$out" "$HERE/cnnet_native.c" \
        -I"$HERE/../lgmon3/src/common" \
        -Wl,-soname,"$soname" \
        -Wl,--version-script="$HERE/$map" \
        -Wall -Wextra -Wno-unused-parameter
}

build libcnnet2.so.1.2.5    libcnnet2.so    cnnet2.map
build libcnbpnet30.so.1.0.0 libcnbpnet30.so cnbpnet30.map
build libcnbpnet20.so.1.0.0 libcnbpnet20.so cnbpnet20.map

# 开发链接：让 lgmon3 能用 -L"$OUT" -lcnnet2 / -lcnbpnet30 链接（autotools 找 libX.so）
ln -sfn libcnnet2.so.1.2.5    "$OUT/libcnnet2.so"
ln -sfn libcnbpnet30.so.1.0.0 "$OUT/libcnbpnet30.so"
ln -sfn libcnbpnet20.so.1.0.0 "$OUT/libcnbpnet20.so"

# ---------------------------------------------------------------- 自检
echo "== 架构与导出符号自检 =="
check() {   # check <文件> <期望前缀>
    local f="$1" pfx="$2" bad
    case "$(file -b "$OUT/$f")" in
        *LoongArch*) : ;;
        *) echo "  FAIL 非 LoongArch: $f -> $(file -b "$OUT/$f")" >&2; exit 1 ;;
    esac
    bad=$(nm -D --defined-only "$OUT/$f" | awk '{print $3}' | grep -vE "^${pfx}" | wc -l)
    if [ "$bad" -ne 0 ]; then
        echo "  FAIL $f 导出了 $bad 个非 ${pfx}* 符号：" >&2
        nm -D --defined-only "$OUT/$f" | awk '{print $3}' | grep -vE "^${pfx}" | head -5 >&2
        exit 1
    fi
    printf '  OK   %-24s 仅导出 %-9s（%s 个）\n' \
        "$f" "${pfx}*" "$(nm -D --defined-only "$OUT/$f" | wc -l)"
}

check libcnnet2.so.1.2.5    CNNL_
check libcnbpnet30.so.1.0.0 CNNET3_
check libcnbpnet20.so.1.0.0 CNNET2_

echo "  -> $OUT/"
