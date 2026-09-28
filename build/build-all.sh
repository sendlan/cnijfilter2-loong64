#!/bin/bash
# ==================================================================
#  build-all.sh —— 从源码一键编译全部 11 个原生构件 → build/built/
#
#  编译顺序（有依赖关系，不能乱）：
#    1) cncl   -> libcnbpcnclapicom2.so.5.0.0
#    2) cnnet  -> libcnnet2 / libcnbpnet30 / libcnbpnet20
#    3) 原厂 7 个模块（lgmon3 与 cmdtocanonij{2,3} 要链接上面两份自研库）
#
#  用法:
#    bash build/build-all.sh              # 全编 + 收集到 build/built/
#    bash build/build-all.sh lgmon3       # 只编指定模块（自研库仍会先编）
#    bash build/build-all.sh --deb        # 编完接着跑 make-deb.sh 出包
#
#  编完可直接出包:  bash build/make-deb.sh
# ==================================================================
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BUILT="$HERE/built"
mkdir -p "$BUILT"

ONLY=""
DO_DEB=0
for a in "$@"; do
    case "$a" in
        --deb) DO_DEB=1 ;;
        -*)    echo "未知选项: $a" >&2; exit 2 ;;
        *)     ONLY="$a" ;;
    esac
done

MODULES="lgmon3 tocanonij tocnpwg cnijbe2 rastertocanonij cmdtocanonij2 cmdtocanonij3"

# 各模块的 configure 参数。
#   链接我们替代库的模块：LDFLAGS 指向 build/built/（自研库 + 开发链接都在那）
#   其余沿用原厂需要的 prefix / progpath
mod_params() {
    case "$1" in
        lgmon3|cmdtocanonij2|cmdtocanonij3) printf '%s' "LDFLAGS=-L$BUILT" ;;
        *)                                  printf '%s' "--prefix=/usr --enable-progpath=/usr/bin" ;;
    esac
}

# 模块 -> "<源码树内相对路径> <build/built 内文件名>"
artifact_of() {
    case "$1" in
        lgmon3)          printf '%s' "lgmon3/src/cnijlgmon3 cnijlgmon3" ;;
        cnijbe2)         printf '%s' "cnijbe2/src/cnijbe2 cnijbe2" ;;
        rastertocanonij) printf '%s' "rastertocanonij/src/rastertocanonij rastertocanonij" ;;
        cmdtocanonij2)   printf '%s' "cmdtocanonij2/filter/cmdtocanonij2 cmdtocanonij2" ;;
        cmdtocanonij3)   printf '%s' "cmdtocanonij3/filter/cmdtocanonij3 cmdtocanonij3" ;;
        tocanonij)       printf '%s' "tocanonij/src/tocanonij tocanonij" ;;
        tocnpwg)         printf '%s' "tocnpwg/src/tocnpwg tocnpwg" ;;
    esac
}

# ---------------------------------------------------------------- 1) 自研库
echo "########## 1/3  自研库（cncl / cnnet） ##########"
bash "$REPO/cncl/build-lib.sh"
bash "$REPO/cnnet/build-net.sh"

# ---------------------------------------------------------------- 2) 原厂模块
echo
echo "########## 2/3  原厂模块 ##########"
for m in $MODULES; do
    if [ -n "$ONLY" ] && [ "$ONLY" != "$m" ]; then continue; fi
    echo
    echo "---- $m ----"
    # shellcheck disable=SC2086
    bash "$HERE/build-one.sh" "$m" $(mod_params "$m")
done

# ---------------------------------------------------------------- 3) 收集
echo
echo "########## 3/3  收集产物 → build/built/ ##########"
rc=0
for m in $MODULES; do
    if [ -n "$ONLY" ] && [ "$ONLY" != "$m" ]; then continue; fi
    set -- $(artifact_of "$m")
    src="$REPO/$1"; dst="$BUILT/$2"
    if [ ! -f "$src" ]; then
        echo "  !! 缺产物：$1（该模块编译失败？看 build/logs/build-$m.log）" >&2
        rc=1; continue
    fi
    install -m 0755 "$src" "$dst"
    printf '  %-20s <- %s\n' "$2" "$1"
done

# 架构自检：build/built/ 里不允许出现非 LoongArch 实体
echo
echo "########## 架构自检 ##########"
bad=0
for f in "$BUILT"/*; do
    [ -f "$f" ] || continue
    [ -L "$f" ] && continue                       # 跳过开发链接
    head -c4 "$f" 2>/dev/null | grep -q ELF || continue
    case "$(file -b "$f")" in
        *LoongArch*) printf '  OK   %s\n' "$(basename "$f")" ;;
        *)           printf '  FAIL %s -> %s\n' "$(basename "$f")" "$(file -b "$f")" >&2; bad=1 ;;
    esac
done
[ "$bad" -eq 0 ] || { echo "!! build/built/ 存在非 LoongArch 构件" >&2; exit 1; }

echo
echo "== 全部完成，构件在 $BUILT =="
if [ "$DO_DEB" -eq 1 ]; then
    echo
    bash "$HERE/make-deb.sh"
fi
