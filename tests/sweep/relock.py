#!/usr/bin/env python3
"""底座重算（封板器）—— 参考输出 vs 原生输出逐字节比对，全一致才封板。

本脚本**架构中立**（这是它能入库的前提）
--------------------------------------
它只读两种东西：

1. 本仓库自己编译的原生构件（``build/built/``，路径由脚本自身位置推出）；
2. 一份「参考输出目录」（``--refdir``）：每个参数组合一个文件，内容是那个组合
   下的**原始输出字节流**。

参考输出可用 ``selfref.py`` 在纯 loong64 上自产（见下），也可从**任何**地方取得
—— 用哪台机器、哪份实现、怎么运行，本仓库
**不规定、不要求、也不依赖**。仓库里既没有产生它的代码，也没有任何指向外部
工具链 / 模拟器 / 翻译层的参数、环境变量或路径。日常回归（``regress.py``、
``mnt-regress.py``）更是完全用不到它。

⭐ **在纯 loong64 上，参考输出可以完全自产**（`selfref.py`）
----------------------------------------------------------
本协议虽是私有协议，但产出命令流的那几个构件**源码是公开的**（GPL-2），且
与官方源码包逐字节相同、不依赖任何闭源库。因此：

    /usr/bin/python3 tests/sweep/selfref.py --outdir <目录> --emit-refdir <参照目录>

它用 loong64 原生 gcc 现编 `rastertocanonij` / `tocnpwg` / `cmdtocanonij3`，
跑出全量参考输出。**实测该参照封板得到的底座与「用官方二进制取参照」得到的
逐字节相同** —— 于是「取得参照」这一步在纯 loong64 上不需要任何翻译层即可完成，
`--refdir` 只是保留「你也可以从别处取参照」这条通用通路。

为什么参照仍是必须的
--------------------
私有协议没有公开规格 ——「什么才算对」只能由**官方实现**给出。区别只在于
「官方实现」可以是我们用官方源码现编的那一份（这就是 `selfref.py`），
而不必是官方发布的某个架构的二进制。

它做四件事
----------
1. 预检：``--refdir`` 里该有的文件是否齐全（缺就一次性列全，不跑一半才停）；
2. 逐组合比对「参考输出」与「原生输出」（归一化 ``datetime`` 后比长度 + SHA-256），
   有任何一组不一致就**拒绝写入**；
3. 全部一致时写出底座文件，封 ``# integrity sha256:`` 指纹，追加台账一条记录；
4. 同步回归脚本内的 ``BASELINE_DIGEST`` 常量，并回读三道锁自证。

用法
----
    # ① 打印"需要哪些参考输出"（可自产：见 selfref.py；也可从别处取）
    /usr/bin/python3 tests/sweep/relock.py --plan

    # ② 参考输出就绪后封板（必须写明理由）
    /usr/bin/python3 tests/sweep/relock.py --refdir <目录> --reason "底座为什么要变"

    --target print|mnt|all    只重算打印链 / 维护链 / 两者（默认 all）
    --quick                   只跑代表性组合（试跑用）
    --log <文件>              另存一份运行日志

参考输出目录布局
----------------
    <refdir>/print/<kind>__<value>.bin     内容是原始输出字节流
    <refdir>/mnt/<job>.bin
路径与文件名直接取自 ``--plan`` 的输出（非字母数字字符会替换成 ``_``）。
"""
import argparse
import hashlib
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # tests/sweep
TROOT = os.path.dirname(HERE)                              # tests
PROJ = os.path.dirname(TROOT)                              # 项目根
sys.path.insert(0, HERE)
sys.path.insert(0, TROOT)

REQ_HEAD = b"<?xml"
DT_RE = re.compile(rb"<ivec:datetime>(\d{14})</ivec:datetime>")


def load_module(path, name):
    """按文件路径加载模块（同目录脚本名带连字符，不能直接 import）。"""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def safe(s: str) -> str:
    """文件名安全化：非 [A-Za-z0-9._-] 一律换成 _。"""
    return re.sub(r"[^A-Za-z0-9._-]+", "_", s)


# ── 底座表头 ────────────────────────────────────────────────────────────────
# ⚠️ 表头只承载**不随时间变化的事实**（来源、口径、去哪看历史）。
#    别写"重算工具在哪"这类会搬家的事实 —— 工具一挪位就得重封一次指纹。
HDR_PRINT = [
    "# cnijfilter2 回归基线 —— 组合 → 归一化输出长度 + SHA-256 前 16 位",
    "# 来源：上游 6.90 官方实现与原生实现对同一批输入的逐字节比对（全部一致）。",
    "# 此处只固化**派生数据**（归一化后的输出长度与摘要），不含任何上游内容。",
    "# 内容指纹见末行 `# integrity sha256:...`（不含该行本身）；变更历史与重算",
    "# 要求见同目录 BASELINE-LOG.md（append-only，改基线必须登记理由）。",
    "#",
    "# kind\tvalue\toutlen\tsha16\topts",
]

HDR_MNT = [
    "# cnijfilter2 维护路径回归基线 —— 作业 → 归一化输出长度 + SHA-256 前 16 位",
    "# 来源：上游 6.90 官方实现与原生实现对同一批输入的逐字节比对（全部一致）。",
    "# 此处只固化**派生数据**（归一化后的输出长度与摘要），不含任何上游内容。",
    "# 内容指纹见末行 `# integrity sha256:...`（不含该行本身）；变更历史与重算",
    "# 要求见同目录 BASELINE-LOG.md（append-only，改基线必须登记理由）。",
    "#",
    "# job\toutlen\tsha16\tutl",
]


def render(rows, header, fmt):
    return "\n".join(header + [fmt(*r) for r in rows]) + "\n"


# ── 参考输出清单 ────────────────────────────────────────────────────────────
def print_items(a, prm):
    """打印链需要哪些参考输出 → [(相对路径, kind, value, opts)]"""
    combos = prm.build_combos()
    if a.quick:
        combos = [c for c in combos if c[1] in prm.QUICK_KEEP.get(c[0], [])]
    return [("print/%s__%s.bin" % (safe(k), safe(v)), k, v, o) for k, v, o in combos]


def mnt_items(a, prm):
    """维护链需要哪些参考输出 → [(相对路径, 作业, utl 文件名)]"""
    return [("mnt/%s.bin" % safe(n), n, u) for n, u in prm.MNT_CASES]


def read_ref(refdir, rel):
    """读一份参考输出并做结构合法性检查 → (bytes, 错误信息)"""
    p = os.path.join(refdir, rel)
    if not os.path.exists(p):
        return None, "缺少文件：%s" % p
    with open(p, "rb") as f:
        d = f.read()
    if not d:
        return None, "空文件（0 字节）：%s" % p
    if not d.startswith(REQ_HEAD):
        return None, "不是合法的命令流（不以 XML 声明开头）：%s" % p
    if not DT_RE.search(d):
        return None, "命令流里找不到 14 位 `<ivec:datetime>`：%s" % p
    return d, None


def do_plan(a, prm, refdir, say):
    for title, items in (("打印链路", print_items(a, prm)),
                         ("维护链路", mnt_items(a, prm))):
        if not items:
            continue
        if refdir:
            have = sum(os.path.exists(os.path.join(refdir, rel)) for rel, *_ in items)
            say("══ %s：需要 %d 份参考输出（%s 已有 %d 份）══"
                % (title, len(items), refdir, have))
        else:
            say("══ %s：需要 %d 份参考输出 ══" % (title, len(items)))
        for rel, *rest in items:
            label = ("%s=%s" % (rest[0], rest[1])) if title == "打印链路" else rest[1]
            mark = "✓" if refdir and os.path.exists(os.path.join(refdir, rel)) else " "
            say("  [%s] %-42s %s" % (mark, rel, label))
        say()
    say("参照可用 selfref.py 在纯 loong64 上自产，也可从别处取得；就绪后用 --reason 封板。")
    return 0


def missing(refdir, rels, total, say):
    """缺文件就一次性列全（不跑一半才停）。返回缺失数。"""
    miss = [r for r in rels if not os.path.exists(os.path.join(refdir, r))]
    if not miss:
        return 0
    say("!! 参考目录缺 %d / %d 份输出，拒绝开始（先补齐再跑）：" % (len(miss), total))
    for r in miss[:20]:
        say("     %s" % r)
    if len(miss) > 20:
        say("     …… 另有 %d 份" % (len(miss) - 20))
    say("   生成完整清单：/usr/bin/python3 tests/sweep/relock.py --plan")
    return len(miss)


# ── 打印链封板 ──────────────────────────────────────────────────────────────
def do_print(a, reg, bg, prm, refdir, say):
    items = print_items(a, prm)
    natf = os.path.join(reg.NATD, "rastertocanonij")
    if not os.path.exists(natf):
        say("!! 找不到原生构件：%s（先跑 build/build-all.sh）" % natf)
        return 1
    if missing(refdir, [r for r, *_ in items], len(items), say):
        return 1

    say("══════ 打印链路 ══════")
    say("%-11s %-16s %9s %9s %-18s %s"
        % ("kind", "value", "参考长度", "原生长度", "sha16", "结果"))
    say("-" * 74)
    rows, bad = [], 0
    for rel, kind, val, opts in items:
        ref, err = read_ref(refdir, rel)
        if err:
            say("!! %s" % err)
            return 1
        no, _ = reg.run(natf, opts, {"LD_LIBRARY_PATH": reg.NATD})
        rn, nn = reg.norm(ref), reg.norm(no)
        sha = hashlib.sha256(rn).hexdigest()[:16]
        ok = (rn == nn)
        bad += 0 if ok else 1
        say("%-11s %-16s %9d %9d %-18s %s"
            % (kind, val, len(rn), len(nn), sha, "OK" if ok else "不一致!"))
        rows.append((kind, val, len(rn), sha, opts))
    if bad:
        say("\n有 %d 个组合不一致，**拒绝写入底座**：参考侧与原生侧输出不同，"
            "要么原生实现有回归，要么参考输出来路不对。" % bad)
        return 1

    txt = render(rows, HDR_PRINT,
                 lambda k, v, l, s, o: "%s\t%s\t%d\t%s\t%s" % (k, v, l, s, o))
    dg = bg.commit(reg.BASELINE, txt)
    bg.record(reg.LOG, os.path.basename(reg.BASELINE), dg,
              "%d/%d 一致" % (len(rows), len(rows)), a.reason)
    if not bg.sync_constant(reg.__file__, dg):
        say('!! 未能同步 %s 内的 BASELINE_DIGEST，请手工填 "%s"' % (reg.__file__, dg))
        return 1
    bg.load(reg.BASELINE, dg, reg.LOG)
    say("\n打印链底座已更新：%s" % reg.BASELINE)
    say("  指纹 %s   组合 %d   三道锁回读通过" % (dg, len(rows)))
    say()
    return 0


# ── 维护链封板 ──────────────────────────────────────────────────────────────
def do_mnt(a, mnt, bg, prm, refdir, say):
    items = mnt_items(a, prm)
    natf = os.path.join(mnt.NATD, "cmdtocanonij3")
    if not os.path.exists(natf):
        say("!! 找不到原生构件：%s（先跑 build/build-all.sh）" % natf)
        return 1
    if missing(refdir, [r for r, *_ in items], len(items), say):
        return 1

    say("══════ 维护链路 ══════")
    say("%-14s %9s %9s %-18s %s"
        % ("作业", "参考长度", "原生长度", "sha16", "结果"))
    say("-" * 64)
    rows, bad = [], 0
    for rel, job, utl_name in items:
        ref, err = read_ref(refdir, rel)
        if err:
            say("!! %s" % err)
            return 1
        utl = os.path.join(mnt.UTLDIR, utl_name)
        no, _ = mnt.run(natf, utl, {"LD_LIBRARY_PATH": mnt.NATD})
        rn, nn = mnt.norm(ref), mnt.norm(no)
        sha = hashlib.sha256(rn).hexdigest()[:16]
        ok = (rn == nn)
        bad += 0 if ok else 1
        say("%-14s %9d %9d %-18s %s"
            % (job, len(rn), len(nn), sha, "OK" if ok else "不一致!"))
        rows.append((job, len(rn), sha, utl_name))
    if bad:
        say("\n有 %d 个维护作业不一致，**拒绝写入底座**。" % bad)
        return 1

    txt = render(rows, HDR_MNT, lambda n, l, s, u: "%s\t%d\t%s\t%s" % (n, l, s, u))
    dg = bg.commit(mnt.BASELINE, txt)
    bg.record(mnt.LOG, os.path.basename(mnt.BASELINE), dg,
              "%d/%d 一致" % (len(rows), len(rows)), a.reason)
    if not bg.sync_constant(mnt.__file__, dg):
        say('!! 未能同步 %s 内的 BASELINE_DIGEST，请手工填 "%s"' % (mnt.__file__, dg))
        return 1
    bg.load(mnt.BASELINE, dg, mnt.LOG)
    say("\n维护链底座已更新：%s" % mnt.BASELINE)
    say("  指纹 %s   作业 %d   三道锁回读通过" % (dg, len(rows)))
    say()
    return 0


def make_say(logpath):
    if not logpath:
        def say(s=""):
            print(s)
        return say
    logpath = os.path.abspath(os.path.expanduser(logpath))
    if os.path.dirname(logpath):
        os.makedirs(os.path.dirname(logpath), exist_ok=True)
    f = open(logpath, "w", encoding="utf-8")

    def say(s=""):
        print(s)
        f.write(s + "\n")
        f.flush()
    return say


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--refdir", default="", help="参考输出目录（--plan 时可选）")
    ap.add_argument("--reason", default="", help="底座为什么要变（封板必填，单行）")
    ap.add_argument("--target", choices=("print", "mnt", "all"), default="all")
    ap.add_argument("--quick", action="store_true", help="只跑代表性组合（试跑用）")
    ap.add_argument("--plan", action="store_true", help="只打印需要哪些参考输出")
    ap.add_argument("--log", default="", help="另存一份运行日志到该文件")
    a = ap.parse_args()

    say = make_say(a.log)
    bg = load_module(os.path.join(HERE, "baseline_guard.py"), "baseline_guard")
    prm = load_module(os.path.join(TROOT, "params.py"), "cnij_params")
    refdir = os.path.abspath(os.path.expanduser(a.refdir)) if a.refdir else ""

    if a.plan:
        return do_plan(a, prm, refdir, say)

    if not a.refdir:
        sys.exit("缺少 --refdir（参考输出目录）。\n"
                 "  先跑 `--plan` 看需要哪些文件（可用 selfref.py 自产），就绪后再回来封板。")
    if not os.path.isdir(refdir):
        sys.exit("参考输出目录不存在：%s" % refdir)
    if not a.reason.strip():
        sys.exit("缺少 --reason（一句话说明底座为什么要变）—— 台账强制登记理由。")

    reg = load_module(os.path.join(HERE, "regress.py"), "cnij_regress")
    mnt = load_module(os.path.join(HERE, "mnt-regress.py"), "cnij_mnt_regress")

    say("参考目录 : %s" % refdir)
    say("理由     : %s" % a.reason)
    say()

    rc = 0
    if a.target in ("print", "all"):
        rc |= do_print(a, reg, bg, prm, refdir, say)
    if a.target in ("mnt", "all"):
        rc |= do_mnt(a, mnt, bg, prm, refdir, say)
    return rc


if __name__ == "__main__":
    sys.exit(main())
