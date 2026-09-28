#!/bin/bash
# 原生编译 cnijfilter2 单个模块（loong64）
# 用法: ./build-one.sh <模块名> [configure 参数...]
# 老工程（2000 年代 autotools）需要手动跑标准序列，跳过其 autogen.sh 的 libtool 检查
#
# ⚠️ 不要再给这些步骤加 `2>&1 | tail -N`：老 autotools 在输出走管道时会提前退出，
#    导致 Makefile.in / Makefile 生成不全（现象是
#    `config.status: error: cannot find input file: 'Makefile.in'`）。
#    这里让输出完整落盘，日志在 build/logs/build-<模块>.log。
set -u
MOD="$1"; shift || true
SELF="$(cd "$(dirname "$0")" && pwd)"                                   # build/
W="${CNIJ_WORK:-$(cd "$SELF/.." && pwd)}"                               # 源码树（= 仓库根）
D="$W/$MOD"
LOG="${LOG:-$SELF/logs/build-$MOD.log}"
mkdir -p "$(dirname "$LOG")"

if [ ! -d "$D" ]; then echo "找不到模块目录: $D"; exit 1; fi
cd "$D" || exit 1

{
echo "=================== $MOD ==================="
echo "工作目录: $D"
echo "实际 pwd: $(pwd)"
echo "参数: $*"
echo

# 清掉可能残留的自动生成物，保证可重复
rm -f configure aclocal.m4 ltmain.sh config.h.in stamp-h.in config.log config.status
rm -rf autom4te.cache
find . -name 'Makefile.in' -delete 2>/dev/null
find . -name 'Makefile'    -delete 2>/dev/null
find . -name 'config.h'    -delete 2>/dev/null
# autopoint 会把 gettext 宏放进 m4/、把 po/Makefile.in.in 放进 po/；
# 连跑时旧宏会让下一步 libtoolize 报
#   "error: 'm4/libtool.m4' exists: use '--force' to overwrite"（libtool 2.5.x）。
# 一并清掉，让最终状态只取决于源码。
rm -f m4/libtool.m4 m4/ltoptions.m4 m4/ltsugar.m4 m4/ltversion.m4 m4/lt~obsolete.m4
rm -f po/Makefile.in po/Makefile.in.in
# autoconf 的探测中间物（confNNNNNN.dir / confNNNNNN.file / confNNNNNNsubs.awk
# 等，以及 conftest* / confdefs.h / confcache）。正常退出时 configure 会自清理，
# 被 kill 或中断才残留 —— 现象是模块目录里积一堆 confNNNNNN.dir。
# ⚠️ 模式必须用 `conf[0-9]*`（后接数字）而非 `conf??????*`（六字符通配）：
#    后者会把 `config.sub` / `config.status` / `config.h.in` 这类**正常生成物**
#    一起匹配上（.gitignore 第 50 行即有此误伤）。autoconf 的临时名固定是
#    conf 后接纯数字，按数字收窄即可精确命中。
rm -rf conf[0-9]* conftest conftest.c conftest.o conftest.i confdefs.h confcache 2>/dev/null
# 同样的中间物也可能落到「源码树根」（$W = CNIJ_WORK）与它下面的 out/：
# 在带安全删除垫片的沙箱里跑构建时，外层会先做一遍删除重放，个别情况下
# 把 configure 的中间物写在工作区根而不是模块目录。基准必须取 $W（源码树
# 根），不能用 "$SELF/.." —— 后者只是 build/ 的上级，在仓库内恰好等于
# $W，一旦用 CNIJ_WORK 指向外部源码副本就错位了。
# ⚠️ 只删残留文件本身，不 `rm -rf out`：out/ 可能承载真实产物（且 `/*/out/`
#    已在 .gitignore 里），整目录删有风险。
W_ROOT="$(cd "$W" && pwd)"
if [ "$W_ROOT" != "$(pwd)" ]; then
    ( cd "$W_ROOT" && rm -rf conf[0-9]* conftest* confdefs.h confcache 2>/dev/null
      rm -f out/conftest* */out/conftest* 2>/dev/null )
fi

echo "--- 1) autopoint（生成 po/Makefile.in.in）---"
# 原厂源码包不带 po/Makefile.in.in（该文件本应由 gettext 的 autopoint 生成）。
# configure.in 里已补 AM_GNU_GETTEXT_VERSION，否则 autopoint 会报 "Missing version"。
# 没有这一步，config.status 会报 cannot find input file: 'po/Makefile.in.in'。
# 放在 aclocal 之前：autopoint 会往 m4/ 放 gettext 宏，需要被 aclocal 采到。
if [ -f po/Makevars ] || [ -n "$(ls po/*.po 2>/dev/null)" ]; then
  autopoint --force || echo "!! autopoint 失败（po 国际化将不可用，不影响主体）"
fi
# autopoint（/usr/bin/autopoint 第 481 行 work_dir=tmpwrk$$）在**当前目录**建
# tmpwrk<PID>/ 工作区，正常退出自删，被中断则残留；其 archive/ 下是 gettext
# 模板副本，与 po/ 内容重复。清掉以免被 `git add -A` 连带提交。
rm -rf tmpwrk[0-9]* 2>/dev/null
echo "--- 2) aclocal ---"
aclocal -I .
echo "--- 3) libtoolize ---"
libtoolize --force --copy
echo "--- 4) autoheader ---"
autoheader
echo "--- 5) automake ---"
automake --add-missing --foreign --copy
echo "--- 6) autoconf ---"
autoconf

if [ ! -f configure ]; then echo "!! configure 未生成，中止"; exit 2; fi

echo "--- 7) configure $* ---"
./configure "$@"
if [ ! -f Makefile ]; then echo "!! Makefile 未生成，configure 失败"; exit 3; fi

echo "--- 8) make -j6 ---"
make -j6
rc=$?
echo "--- make 退出码: $rc ---"
echo
echo "--- 9) 产物 ---"
for f in $(find . -maxdepth 3 -type f -perm -u+x ! -name '*.sh' ! -name '*.in' ! -name 'configure' ! -name 'config.guess' ! -name 'config.sub' ! -name 'install-sh' ! -name 'missing' ! -name 'depcomp' ! -name 'compile' ! -name 'ltmain.sh' 2>/dev/null); do
  printf '  %-46s %s\n' "$f" "$(file -b "$f" | cut -c1-56)"
done
find . -maxdepth 3 -name '*.so*' -type f -printf '  %-46s %s\n' -exec sh -c 'file -b "$1" | cut -c1-56' _ {} \; 2>/dev/null
exit $rc
} > "$LOG" 2>&1
rc=$?
echo "== $MOD 日志尾部 =="
tail -25 "$LOG"
echo
echo "== 完整日志: $LOG =="
exit $rc
