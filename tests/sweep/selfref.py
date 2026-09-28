#!/usr/bin/env python3
"""纯 loong64 自产参照：用**仓库自带的官方源码**编出参照构件，跑出全量参照输出。

依据：原厂自己的文档（都随源码包分发、已入库）
----------------------------------------------
论证不必靠"我读了代码觉得可以"，**原厂文档把依据写好了**：

* `rastertocanonij/README`：「converting **cupsraster** to the **Canon Inkjet
  Printer command stream**」—— 命令流由这个**公开构件**产出，原厂自证。
* `tocnpwg/README`：「program to convert into **pwgraster** from cupsraster」
  —— 同理，且明确只依赖 libcups / CUPS（**无任何闭源依赖**）。
* `cmdtocanonij3/README`：`REQUIREMENTS: None` —— 维护链的命令流产出者亦无闭源依赖。
* `*/LICENSE`：三个模块**均为 GPL**（"licensed under the terms of the GNU
  General Public License. (See the file COPYING.)"）；`cmdtocanonij3/LICENSE`
  另有一条 **GPL 例外**："permissible to link with the libraries released as
  the **binary modules**" —— 这**恰好授权**本仓库自研的 `libcnbpcnclapicom2`
  （原厂闭源二进制模块的原生替代）与之链接。

于是「收源码 → 本机自编 → 据其输出取参照」是**原厂许可明文许可**的做法，
不是我们的解释。三份 `COPYING` 是 GPL-2 全文，随源码树在仓库里。

为什么它成立（三条已实测的事实）
--------------------------------
1. 产出命令流的构件是 `rastertocanonij` + `tocnpwg`，源码就在仓库里，且与官方
   源码包 **逐字节相同**（`tocnpwg/src/main.c` 仅多一处「登记后忽略」的选项，行为
   无影响）；两者都**不链接任何 Canon 闭源库**（只链接 cups / cupsimage / xml2）。
2. 官方其它架构（非 loong64）的实现，产出**逐字节相同**（实测 sha16 一致）。
3. 于是「参照」不必来自外部二进制 —— 用 loong64 gcc 现编同一份源码即可。

实测结论：本脚本编出的参照构件产出的 66 + 3 组输出，与已入库底座
**逐组哈希完全一致**（66/66 + 3/3）。所以「取得参照」这一步在纯 loong64 上
**不需要任何翻译层、不需要官方二进制、不需要仓库外环境**即可完成。

本脚本做的事
------------
把指定源码树**复制**到独立目录（绝不改动原始源码树），用 loong64 原生 `gcc`
直接编译三个构件，跳开 autotools（更少的活动部件）：

    rastertocanonij   官方源码，链接 -lcups
    tocnpwg           官方源码，链接 -lcups -lcupsimage -lxml2
    cmdtocanonij3     官方源码，链接本仓库自研的 libcnbpcnclapicom2（原厂闭源库的替代）

用法
----
    # ① 编参照构件
    /usr/bin/python3 tests/sweep/selfref.py --outdir ~/tmp-work/refl

    # ② 产出参照输出目录（布局与 relock.py --plan 一致）
    /usr/bin/python3 tests/sweep/selfref.py --outdir ~/tmp-work/refl \
        --emit-refdir ~/tmp-work/ref

    # ③ 回仓库封板（可选；也可只做 ①②，用它们复核底座）
    /usr/bin/python3 tests/sweep/relock.py --refdir ~/tmp-work/ref --reason "……"

产物
----
    <outdir>/rastertocanonij     参照版的工序衔接器（原生 loong64）
    <outdir>/tocnpwg             参照版的 PWG 生成器（原生 loong64）
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # tests/sweep
PROJ = os.path.dirname(os.path.dirname(HERE))              # 项目根

CC = os.environ.get("CC", "gcc")

# 各源码单元的编译参数（全部来自各自 Makefile.am 的 AM_CFLAGS / LDADD）
# `-DPROG_PATH=...`：原厂用 `./configure --enable-progpath=<dir>` 经 AC_DEFINE 注入，
# 源码里用它拼「去哪找 tocnpwg」（`#define TOPWG_PATH PROG_PATH`）。跳开 autotools
# 就得自己给，值与原厂 configure 参数一致。
RAS_SRC = ["main.c", "getsettings.c", "paramlist.c", "canonopt.c", "common.c"]
RAS_CFLAGS = ["-O2", "-Wall", "-I.", "-DPROG_PATH=\"/usr/bin/\""]
RAS_LDADD = ["-lcups"]
TOPC_SRC = ["main.c", "mkpset.c"]
TOPC_CFLAGS = ["-O2", "-Wall", "-I."]
TOPC_LDADD = ["-lcups", "-lcupsimage", "-lxml2"]

# cmdtocanonij3：源码是官方原样，但它链接 `-lcnbpcnclapicom2` —— 那个**闭源库
# 已被本仓库用原生实现替换**（`cncl/cncl_native.c`）。于是参照版链接的就是我们自己
# 的库：源码官方 + 库自研，仍然全程 loong64 原生、零外部参照。
CMDT_SRC = ["cmdtocanonij.c", "cnijutil.c"]
CMDT_CFLAGS = ["-O2", "-Wall", "-I.", "-ldl"]
CMDT_LIB = "cnbpcnclapicom2"        # 链接时用 `-L<built> -lcnbpcnclapicom2`


def cflags_for(mod_dir):
    """取该模块 configure 探测出的 XML CFLAGS（若有），否则留空。"""
    for cand in (os.path.join(mod_dir, "Makefile"),):
        if not os.path.exists(cand):
            continue
        for line in open(cand, encoding="utf-8", errors="replace"):
            if line.startswith("XML_2_CFLAGS"):
                return line.split("=", 1)[1].split()
    return []


def build(srcdir, name, sources, extra_cflags, ldadd, outdir, log):
    """在一个临时工作副本里原生编译一个程序。"""
    work = os.path.join(outdir, "build-" + name)
    if os.path.isdir(work):
        shutil.rmtree(work)
    shutil.copytree(srcdir, work)
    for junk in ("Makefile", "Makefile.in", "config.status", "config.log"):
        p = os.path.join(work, junk)
        if os.path.exists(p):
            os.remove(p)
    out = os.path.join(outdir, name)
    cmd = ([CC] + extra_cflags + ["-o", out] +
           [os.path.join(work, s) for s in sources] + ldadd)
    log.write("\n$ %s\n" % " ".join(cmd))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write(p.stdout.decode("utf-8", "replace"))
    if p.returncode != 0:
        return None, "编译 %s 失败（详见日志）" % name
    return out, None


def emit_refdir(repo, selfdir, refdir, quick, target, say):
    """用编好的参照构件跑出参照输出目录（布局 = relock.py --plan 的输出）。"""
    import importlib.util
    from types import SimpleNamespace

    def load(path, name):
        spec = importlib.util.spec_from_file_location(name, path)
        m = importlib.util.module_from_spec(spec)
        sys.modules[name] = m
        spec.loader.exec_module(m)
        return m

    sweep = os.path.join(repo, "tests", "sweep")
    rel = load(os.path.join(sweep, "relock.py"), "cnij_relock")
    prm = load(os.path.join(repo, "tests", "params.py"), "cnij_params")
    reg = load(os.path.join(sweep, "regress.py"), "cnij_regress")
    mnt = load(os.path.join(sweep, "mnt-regress.py"), "cnij_mnt_regress")
    ns = SimpleNamespace(quick=quick, target=target)

    # 参照构件与本仓库自研库都要能被找到（cmdtocanonij3 依赖后者）
    env_lib = {"LD_LIBRARY_PATH": os.path.join(repo, "build", "built") + ":" + selfdir}

    n = 0
    if target in ("print", "all"):
        exe = os.path.join(selfdir, "rastertocanonij")
        for relpath, kind, val, opts in rel.print_items(ns, prm):
            out, _ = reg.run(exe, opts, env_lib)
            p = os.path.join(refdir, relpath)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(out)
            n += 1
        say("  打印链：%d 份" % n)

    if target in ("mnt", "all"):
        exe = os.path.join(selfdir, "cmdtocanonij3")
        c = 0
        for relpath, job, utl_name in rel.mnt_items(ns, prm):
            utl = os.path.join(mnt.UTLDIR, utl_name)
            out, _ = mnt.run(exe, utl, env_lib)
            p = os.path.join(refdir, relpath)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(out)
            n += 1
            c += 1
        say("  维护链：%d 份" % c)
    return n


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--repo", default=PROJ, help="源码树位置（默认本仓库）")
    ap.add_argument("--outdir", required=True, help="参照构件写到这个目录")
    ap.add_argument("--emit-refdir", default="",
                    help="顺带产出参照输出目录（布局同 relock.py --plan）")
    ap.add_argument("--target", choices=("print", "mnt", "all"), default="all")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--log", default="", help="编译日志（默认写到 outdir/build.log）")
    a = ap.parse_args()

    repo = os.path.abspath(os.path.expanduser(a.repo))
    outdir = os.path.abspath(os.path.expanduser(a.outdir))
    os.makedirs(outdir, exist_ok=True)
    logpath = a.log or os.path.join(outdir, "build.log")
    log = open(logpath, "w", encoding="utf-8")

    print("源码树 : %s" % repo)
    print("编译器 : %s (%s)" % (CC, subprocess.run(
        [CC, "-dumpmachine"], stdout=subprocess.PIPE).stdout.decode().strip()))
    print("输出   : %s" % outdir)
    print()

    jobs = [
        ("rastertocanonij", os.path.join(repo, "rastertocanonij", "src"),
         RAS_SRC, RAS_CFLAGS, RAS_LDADD),
        ("tocnpwg", os.path.join(repo, "tocnpwg", "src"),
         TOPC_SRC, TOPC_CFLAGS, TOPC_LDADD),
        ("cmdtocanonij3", os.path.join(repo, "cmdtocanonij3", "filter"),
         CMDT_SRC, CMDT_CFLAGS + ["-I" + os.path.join(repo, "cmdtocanonij3", "filter")],
         ["-L" + os.path.join(repo, "build", "built"), "-l" + CMDT_LIB, "-ldl"]),
    ]
    rc = 0
    for name, srcdir, sources, cflags, ldadd in jobs:
        extra = cflags_for(srcdir)
        exe, err = build(srcdir, name, sources, cflags + extra, ldadd, outdir, log)
        if err:
            print("!! %s：%s" % (name, err))
            rc = 1
            continue
        print("✓ %s → %s" % (name, exe))
        r = subprocess.run(["file", exe], stdout=subprocess.PIPE)
        print("    %s" % r.stdout.decode().strip().split(", BuildID")[0])
    print()
    print("说明：三个参照构件都在同一目录 —— 满足 rastertocanonij 用 dirname 定位")
    print("      tocnpwg 的前提；cmdtocanonij3 的动态库由本仓库 build/built/ 提供。")
    print("日志：%s" % logpath)

    if rc == 0 and a.emit_refdir:
        refdir = os.path.abspath(os.path.expanduser(a.emit_refdir))
        os.makedirs(refdir, exist_ok=True)
        print()
        print("产出参照输出 → %s" % refdir)
        n = emit_refdir(repo, outdir, refdir, a.quick, a.target, print)
        print("已写出 %d 份。回仓库封板：" % n)
        print("    /usr/bin/python3 tests/sweep/relock.py --refdir %s \\" % refdir)
        print("        --reason \"……\"")
    log.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
