#!/bin/bash
# ==================================================================
#  make-deb.sh —— 打 cnijfilter2 的 loong64「全原生」deb
#
#  组成：
#    * 二进制（全部 LoongArch，自编译）：filters / backend / lgmon3 /
#      tocanonij / tocnpwg / 4 个私有共享库
#    * 纯资源（PPD、.utl 维护命令、cnnet.ini、keytext .res）：
#      PPD 取自 ppd/（6.90 全量 179 个，GPL-2）；其余取自源码树
#
#  本脚本完全自包含，**不依赖任何原厂 deb**：官方包里只有 /opt/canonij2
#  下的预编译二进制是我们要替换的，其余资源与架构无关，仓库已自带
#  （实测 .utl/.res/cnnet.ini 在 6.20 与 6.90 逐字节相同）。
#
#  用法： ./make-deb.sh [--keep]      --keep 保留 stage 便于检查
# ==================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"            # build/
PROJ="$(cd "$ROOT/.." && pwd)"                   # 仓库根（= 源码树，已上移不再套娃）
PKGVER="${PKGVER:-6.90-6loong64}"
REV="$(printf %s "$PKGVER" | sed 's/-*loong64$//')"   # 6.90-6loong64 -> 6.90-6
SRCROOT="${SRCROOT:-${CNIJ_WORK:-$PROJ}}"                # 源码树位置（= 仓库根，源码树已上移）
PPDROOT="${PPDROOT:-$PROJ/ppd}"                  # 6.90 全量 PPD 就在原厂的 ppd/（GPL-2）
PKGPOOL="$PROJ/packages"                         # 产物汇总处（唯一出包位置）
OUT="${OUT:-$PKGPOOL/cnijfilter2_${REV}_loong64.deb}"
mkdir -p "$(dirname "$OUT")"                     # 产物目录可能尚不存在（首次克隆/刚清过）

DIST="$ROOT/dist"
STAGE="$DIST/stage"

say() { printf '\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31m!! %s\033[0m\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------
say "0) 校验所有原生构件存在且是 LoongArch"
need_bin() {
    [ -f "$1" ] || die "缺少构件：$1（请先跑 build-one.sh / native-net/build-net.sh）"
    case "$(file -b "$1")" in
        *LoongArch*) : ;;
        *) die "构件不是 LoongArch：$1 -> $(file -b "$1")" ;;
    esac
}
# 编译产物取法：优先归档自带的 built/（本树自包含，可直接打包），
#              否则回退到源码树 $SRCROOT/ 下的原位产物。
pick() {   # pick <built 内文件名> <构建树内相对路径>
    if [ -f "$ROOT/built/$1" ]; then printf '%s' "$ROOT/built/$1"
    else printf '%s' "$SRCROOT/$2"; fi
}
B_LGMON3="$(pick cnijlgmon3      lgmon3/src/cnijlgmon3)"
B_CNIJBE2="$(pick cnijbe2        cnijbe2/src/cnijbe2)"
B_RASTER="$(pick rastertocanonij rastertocanonij/src/rastertocanonij)"
B_CMDTO2="$(pick cmdtocanonij2   cmdtocanonij2/filter/cmdtocanonij2)"
B_CMDTO3="$(pick cmdtocanonij3   cmdtocanonij3/filter/cmdtocanonij3)"
B_TOCANONIJ="$(pick tocanonij    tocanonij/src/tocanonij)"
B_TOCNPWG="$(pick tocnpwg        tocnpwg/src/tocnpwg)"
B_CLSLIB="$(pick libcnbpcnclapicom2.so.5.0.0 cncl/libcnbpcnclapicom2.so.5.0.0)"
B_CNNET2="$(pick libcnnet2.so.1.2.5      cnnet/libcnnet2.so.1.2.5)"
B_CNBP20="$(pick libcnbpnet20.so.1.0.0   cnnet/libcnbpnet20.so.1.0.0)"
B_CNBP30="$(pick libcnbpnet30.so.1.0.0   cnnet/libcnbpnet30.so.1.0.0)"
for f in "$B_LGMON3" "$B_CNIJBE2" "$B_RASTER" "$B_CMDTO2" "$B_CMDTO3" \
         "$B_TOCANONIJ" "$B_TOCNPWG" "$B_CLSLIB" \
         "$B_CNNET2" "$B_CNBP20" "$B_CNBP30"; do
    need_bin "$f"
done

# ------------------------------------------------------------------
say "1) 校验资源（PPD 与本地化，全部随仓库分发）"
# PPD：从源码树的 ppd/ 取（6.90 全量 179 个，已用官方修订版覆盖 6.20 的 112 个
#      同名文件 —— 同名文件内容被官方改过，混用会导致装入旧描述）。
#      PPD 为 GPL-2，可随仓库分发。PPD 只此一处，不再另存副本。
# 其余资源（.utl / .res / cnnet.ini）：实测 6.20 与 6.90 逐字节相同，
#      直接从源码树取，不另存副本。
[ -d "$PPDROOT" ] || die "找不到 PPD 目录：$PPDROOT"
[ "$(ls "$PPDROOT"/*.ppd 2>/dev/null | wc -l)" -gt 0 ] || die "PPD 目录为空：$PPDROOT"

# ------------------------------------------------------------------
say "2) 组装 stage"
rm -rf "$STAGE"

put() {   # put <源文件> <包内路径> <权限>
    install -D -m "$3" "$1" "$STAGE$2"
}

# ---- 2a) 资源（与架构无关） ----
for p in "$PPDROOT"/*.ppd; do
    put "$p" "/usr/share/cups/model/$(basename "$p")" 644
done
# .utl 维护命令（6.20 与 6.90 相同）
for lvl in 2 3; do
    for u in "$SRCROOT"/cmdtocanonij$lvl/utilfiles/*.utl; do
        [ -e "$u" ] || continue
        put "$u" "/usr/share/cmdtocanonij$lvl/$(basename "$u")" 644
    done
done
# keytext 资源（6.20 与 6.90 相同）
put "$SRCROOT/lgmon3/keytext/cnb_cnijlgmon2.res" /usr/share/cnijlgmon3/cnb_cnijlgmon2.res 644
# 网络配置（6.20 与 6.90 相同；优先取自研 built/ 里的覆盖版）
if [ -f "$ROOT/built/cnnet.ini" ]; then
    put "$ROOT/built/cnnet.ini" /usr/lib/bjlib2/cnnet.ini 644
else
    put "$SRCROOT/com/ini/cnnet.ini" /usr/lib/bjlib2/cnnet.ini 644
fi

# ---- 2b) 原生二进制 ----
put "$B_CNIJBE2"  /usr/lib/cups/backend/cnijbe2      700
put "$B_RASTER"   /usr/lib/cups/filter/rastertocanonij 755
put "$B_CMDTO2"   /usr/lib/cups/filter/cmdtocanonij2   755
put "$B_CMDTO3"   /usr/lib/cups/filter/cmdtocanonij3   755
put "$B_LGMON3"   /usr/bin/cnijlgmon3                755
put "$B_TOCANONIJ" /usr/bin/tocanonij                755
put "$B_TOCNPWG"  /usr/bin/tocnpwg                   755

# ---- 2c) 私有共享库 ----
# 装到 Debian 多架构三元组目录（dpkg 体系下各发行版通用）。
LIBDIR=/usr/lib/loongarch64-linux-gnu
put "$B_CLSLIB" "$LIBDIR/libcnbpcnclapicom2.so.5.0.0" 644
put "$B_CNNET2" "$LIBDIR/libcnnet2.so.1.2.5"          644
put "$B_CNBP20" "$LIBDIR/libcnbpnet20.so.1.0.0"       644
put "$B_CNBP30" "$LIBDIR/libcnbpnet30.so.1.0.0"       644

liblink() {   # liblink <链接名> <目标名>
    mkdir -p "$STAGE$LIBDIR"
    ln -sfn "$2" "$STAGE$LIBDIR/$1"
}
liblink libcnbpcnclapicom2.so   libcnbpcnclapicom2.so.5.0.0
liblink libcnbpcnclapicom2.so.5 libcnbpcnclapicom2.so.5.0.0
liblink libcnnet2.so            libcnnet2.so.1.2.5
liblink libcnnet2.so.1          libcnnet2.so.1.2.5
liblink libcnbpnet20.so         libcnbpnet20.so.1.0.0
liblink libcnbpnet20.so.1       libcnbpnet20.so.1.0.0
liblink libcnbpnet30.so         libcnbpnet30.so.1.0.0
liblink libcnbpnet30.so.1       libcnbpnet30.so.1.0.0

# ---- 2d) DEBIAN 控制区 ----
say "3) DEBIAN 控制区"
install -D -m 644 "$ROOT/pkg/control"      "$STAGE/DEBIAN/control"
install -D -m 755 "$ROOT/pkg/postinst"     "$STAGE/DEBIAN/postinst"
install -D -m 755 "$ROOT/pkg/postrm"       "$STAGE/DEBIAN/postrm"
sed -i "s/^Version:.*/Version: $PKGVER/" "$STAGE/DEBIAN/control"

# ------------------------------------------------------------------
say "4) 全量架构自检（包内不允许出现任何非 LoongArch 可执行文件）"
bad=0; n=0
while IFS= read -r -d '' f; do
    [ -L "$f" ] && continue
    case "$(file -b "$f")" in
        *ELF*) n=$((n+1))
               case "$(file -b "$f")" in
                   *LoongArch*) : ;;
                   *) echo "  非 LoongArch: ${f#$STAGE}"; bad=$((bad+1)) ;;
               esac ;;
    esac
done < <(find "$STAGE" -type f -print0)
echo "  ELF 文件 $n 个，非 LoongArch $bad 个"
[ "$bad" -eq 0 ] || die "包内存在非 LoongArch 可执行文件"

# ------------------------------------------------------------------
say "5) 打 deb"
rm -f "$OUT"
dpkg-deb --root-owner-group -Zxz -b "$STAGE" "$OUT" >/dev/null
echo "  产物: $OUT"
ls -la "$OUT"
dpkg-deb -I "$OUT" | sed -n '1,20p'

# 中间物（stage 组装树）默认清掉，保持仓库干净；想留现场排查就加 --keep。
if [ "${1:-}" = "--keep" ]; then
    echo "  --keep：保留 $DIST 供检查"
else
    rm -rf "$DIST"
fi
say "完成 version=$PKGVER"
