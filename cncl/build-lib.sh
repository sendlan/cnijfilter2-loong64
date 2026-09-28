#!/bin/bash
# ==================================================================
#  build-lib.sh —— 构建 Canon IVEC 命令层的 loong64 原生库
#                  libcnbpcnclapicom2.so.5.0.0
#
#  源码 cncl_native.c 是闭源 libcnbpcnclapicom2 的替代实现，
#  契约来源与合规说明见该文件头部注释。
#
#  用法:
#    ./build-lib.sh              # release 构建，产物 → ../build/built/
#    ./build-lib.sh debug        # -g -O0，带调试信息
#    OUT=/path ./build-lib.sh    # 自定义输出目录
# ==================================================================
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="${OUT:-$REPO/build/built}"          # 与其他构件同放 build/built/
SONAME="libcnbpcnclapicom2.so.5"
FILE="libcnbpcnclapicom2.so.5.0.0"

EXTRA="-O2"
if [ "${1:-}" = "debug" ]; then
    EXTRA="-g -O0"
    echo "== DEBUG 构建（-g -O0） =="
else
    echo "== RELEASE 构建（-O2） =="
fi

mkdir -p "$OUT"
gcc $EXTRA -fPIC -shared -o "$OUT/$FILE" "$HERE/cncl_native.c" \
    -I"$HERE" \
    -Wl,-soname,"$SONAME" \
    -Wall -Wextra -Wno-unused-parameter

# 开发链接：让其他模块能用 -L"$OUT" -lcnbpcnclapicom2 链接（autotools 找 libX.so）
ln -sfn "$FILE" "$OUT/$SONAME"                          # libcnbpcnclapicom2.so.5
ln -sfn "$FILE" "$OUT/${SONAME%.so.5}.so"               # libcnbpcnclapicom2.so

# 架构自检：必须是 LoongArch
case "$(file -b "$OUT/$FILE")" in
    *LoongArch*)
        echo "  OK   $FILE"
        echo "       架构: $(file -b "$OUT/$FILE" | cut -d, -f1-2)"
        echo "       大小: $(stat -c %s "$OUT/$FILE") 字节"
        echo "       导出: $(nm -D --defined-only "$OUT/$FILE" | wc -l) 个符号"
        ;;
    *)
        echo "  FAIL 非 LoongArch: $(file -b "$OUT/$FILE")" >&2
        exit 1
        ;;
esac

echo "  -> $OUT/$FILE"
