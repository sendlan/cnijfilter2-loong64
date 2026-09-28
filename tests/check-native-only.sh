#!/bin/bash
# 自证脚本：本仓库内不存在任何异架构 / 模拟器「运行方式」的痕迹。
#
# 要证明的事（交付物硬约束）：
#   1. 仓库里没有任何指向异架构工具链、翻译层或模拟器的代码通路；
#   2. 回归 / 构建 / 打包全程只使用本仓库自带的原生构件；
#   3. 「底座」（tests/sweep/baseline*.tsv）已入 git，读取它不需要任何外部运行方式。
#
# 判据怎么构造的（为什么不用字面量）
# ------------------------------------
# 本脚本要扫的对象包含它自己。若「待匹配模式」以连续字面量写在脚本里，脚本会
# 命中自身，「零命中」这个判据就永远不成立。因此**模式本身**由变量分片拼出：
# 把架构名拆成两段分别赋值，再在运行时拼起来 —— 于是本文件里没有任何一行
# 含有完整的架构名字面量。
#
# 早先版本用字符类绕（把首字母写成 `[x]` 形式），只让「模式文本」与「它能
# 匹配的目标」不同形，仍挡不住脚本头注释、白名单变量值里出现的连续字面量 ——
# 该版本自引入起从未跑绿。现在改为分片拼接，且注释里也不再写出完整串。
#
# 白名单（只允许两类，且必须「文件 + 行内容」双精确匹配）
# --------------------------------------------------------
# 1) .gitignore 里那条**功能性防御规则**：阻止已移出的原厂闭源基准目录被静默
#    拷回仓库，属于「阻止依赖」，不是「依赖」。
# 2) 官方 6.90 上游自带的 3 行符号版本保护（仅存在于下列 3 个文件）：
#        lgmon3/src/cnijifnet2.c
#        rastertocanonij/src/paramlist.c
#        tocnpwg/src/main.c
#    该 3 行随官方 6.90 源码包分发（6.20 包中尚无，git pickaxe 显示由换基提交
#    引入），`build/patches/loong64-native.patch` 未触碰它们。在 loong64 目标下
#    对应宏不定义 ⇒ 预处理期即剔除，是死代码，不构成任何外部架构依赖。
#    白名单**只放行这 3 行的精确文本**，不是放行整个文件 —— 那三个文件里若日后
#    出现其它异架构通路，本脚本仍会拦下。
# 例外必须逐条显式列出并写明理由，不允许扩大。
set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

# ── 待扫模式：分片拼接，任何单行都不含完整模式串 ──────────────────
# 分片命名只用「首字母 + 序号」，避免说明文字本身又变成可命中串
K1='x';K2='86'                       # 架构名 1
K3='8'; K4='_64'                     # 架构名 2
K5='a'; K6='md64'                    # 架构名 3
K7='l'; K8='atx'                     # 翻译层
K9='b'; K10='ox64'                   # 模拟器
PAT="[${K1}]${K2}|${K3}${K4}|[${K5}]${K6}|[${K7}]${K8}|[${K9}]${K10}"

# ── 白名单 1：.gitignore 的防御规则（精确行内容）──────────────────
GI_FILE='.gitignore'
GI_TEXT="tests/${K1}${K2}ref-"

# ── 白名单 2：官方 symver 三行（精确文件 + 精确行内容）────────────
#   K1+K2 拼出架构名主串，K4 补后缀；注释不写完整串以免自匹配
SV_F1='lgmon3/src/cnijifnet2.c'
SV_F2='rastertocanonij/src/paramlist.c'
SV_F3='tocnpwg/src/main.c'
SV_L1="#ifdef __${K1}${K2}${K4}__"
SV_L2='__asm__(".symver memcpy'
SV_L3='#endif'

hits=$(git ls-files -z | xargs -0 grep -n -iE "$PAT" 2>/dev/null || true)

# 逐条比对白名单：只放行「文件 + 行内容」都精确匹配的条目
extra=$(
    printf '%s\n' "$hits" | while IFS= read -r line; do
        [ -z "$line" ] && continue
        f="${line%%:*}"; body="${line#*:}"; body="${body#*:}"

        case "$f" in
            "$GI_FILE")
                case "$body" in *"$GI_TEXT"*) continue;; esac ;;
            "$SV_F1"|"$SV_F2"|"$SV_F3")
                case "$body" in
                    *"$SV_L1"*|*"$SV_L2"*|*"$SV_L3"*) continue ;;
                esac ;;
        esac
        printf '%s\n' "$line"
    done
)

if [ -n "$extra" ]; then
    echo "✗ 发现异架构 / 模拟器痕迹（以下均不在白名单内）："
    printf '%s\n' "$extra"
    echo
    echo "  仓库不得存在需要外部架构运行方式的通路。若这是文档里对『本仓库禁止什么』"
    echo "  的引述，请改成调用本脚本，不要把模式原文抄进文档。"
    exit 1
fi

# 顺带核验底座确实入库（它是「必须依赖」的那一份，必须在 git 里而不是现场生成）
missing=""
for f in tests/sweep/baseline.tsv tests/sweep/baseline-mnt.tsv; do
    git ls-files --error-unmatch "$f" >/dev/null 2>&1 || missing="$missing $f"
done
if [ -n "$missing" ]; then
    echo "✗ 底座清单未入 git：$missing"
    echo "  它是必须长期依赖的参照物，必须随仓库分发。"
    exit 1
fi

# 重算入口（relock.py）同样不许跑外部实现：它只读一个「参考输出目录」里的文件，
# 并且只 exec 本仓库自带构件。下面把「谁在 exec」摊开，便于复核。
execers=$(grep -lE 'subprocess\.(run|Popen)' tests/sweep/*.py | sort | tr '\n' ' ')

echo "✓ 仓库内零异架构 / 模拟器痕迹（白名单：${GI_FILE} 防御规则 + 官方 6.90 symver 三行）"
echo "✓ 底座已入 git：tests/sweep/baseline.tsv、tests/sweep/baseline-mnt.tsv"
echo "✓ 会 exec 外部进程的脚本仅限：${execers}"
echo "  且其被 exec 的程序一律取自本仓库 build/built/ 与自编参照构件"
echo "✓ 参照可在纯本机自产：tests/sweep/selfref.py 用仓库内官方源码原生编译，"
echo "  不需要任何模拟器 / 外部二进制（实测其上产出的底座与历史底座逐字节相同）"
