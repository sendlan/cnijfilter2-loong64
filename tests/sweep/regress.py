#!/usr/bin/env python3
"""全参数矩阵回归 —— 原生构件 vs 已入库的基线清单（哈希自证）。

本脚本**只做一件事**：跑本仓库自己编译出的构件，对 ``baseline.tsv`` 里的
「输出长度 + 归一化 SHA-256」自证。

零依赖承诺
----------
* 只用仓库自带的 ``build/built/``、``ppd/`` 与 ``../small.raster``（共享测试输入）；
* **不需要联网、不需要安装系统包、不需要任何上游二进制或模拟器**；
* 不引用仓库外任何目录（路径全部由脚本自身位置推出，可用环境变量覆盖）。

基线是**只读物证**，不是构建依赖：它把「本实现与上游官方实现对本批输入
逐字节等价」这个事实冻结下来。重算属维护操作、入口在 ``relock.py``；
本脚本日常运行**完全不碰它，也不碰任何外部参照物**（见同目录 BASELINE-LOG.md）。

基线防篡改
----------
基线是一串不可读的哈希，若能被随手重算，「回归」就退化成「红了就重置」的仪式。
因此 ``baseline_guard.py`` 上了三道锁（详见该模块文档）：

* **L1** 表末内嵌 ``# integrity sha256:...``，加载时自校验；
* **L2** 本脚本顶部硬编码 ``BASELINE_DIGEST``，与表内指纹交叉验证 —— 只改 .tsv 改不动；
* **L3** ``BASELINE-LOG.md`` 为 append-only 指纹链，每次重算**必须登记理由**。

真正的信任锚是 git 提交历史：这三道锁的作用是让「改基线」必然留下 3 个文件的显式 diff。

比对口径
--------
把 ``<ivec:datetime>`` 归一化成全 0（时间戳天然不可复现），再比
「长度 + SHA-256 前 16 位」。两项都一致才算 PASS。

用法
----
    /usr/bin/python3 tests/sweep/regress.py                 # 对基线自证（默认）
    /usr/bin/python3 tests/sweep/regress.py --quick         # 只跑代表性组合

环境变量
--------
    CNIJ_BUILT   构件目录（默认 <项目根>/build/built）
    CNIJ_PPD     PPD 路径（默认 <项目根>/ppd/canong3010.ppd）
"""
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # tests/sweep
TROOT = os.path.dirname(HERE)                              # tests
PROJ = os.path.dirname(TROOT)                              # 项目根

sys.path.insert(0, HERE)
sys.path.insert(0, TROOT)
import baseline_guard as bg                                # noqa: E402  同目录的基线守卫
import params                                              # noqa: E402  参数矩阵（单一来源）

NATD = os.environ.get("CNIJ_BUILT", os.path.join(PROJ, "build", "built"))
# PPD 取自仓库自带的 ppd/（Canon 以 GPL-2 发布），避免依赖系统安装的包
PPD = os.environ.get("CNIJ_PPD", os.path.join(PROJ, "ppd", "canong3010.ppd"))
RASTER = os.path.join(TROOT, "small.raster")   # 共享测试输入（在 tests/ 根，两层共用）
BASELINE = os.path.join(HERE, "baseline.tsv")
LOG = os.path.join(HERE, bg.LOG_NAME)
OUT = os.path.join(HERE, "regress")

# ── L2：源码内指纹 ──────────────────────────────────────────────────────────
# 与 baseline.tsv 末尾的 integrity 行互为交叉校验，两处必须一致。
# **不要手工填**：改基线一律走同目录的 relock.py —— 它在写表的同时同步这一行、
# 登记 BASELINE-LOG.md、并回读三道锁。手工填会让 L2 失去意义。
BASELINE_DIGEST = "a0101be22d1aad396444c8380ad01873dc0598199c16201eab5fae4f14c7c2a5"

NATF = os.path.join(NATD, "rastertocanonij")

DT_RE = re.compile(rb'<ivec:datetime>\d{14}</ivec:datetime>')


def norm(d: bytes) -> bytes:
    """把 datetime 打成固定值，消除时间戳噪声。"""
    return DT_RE.sub(b'<ivec:datetime>00000000000000</ivec:datetime>', d)


def digest(d: bytes) -> str:
    return hashlib.sha256(norm(d)).hexdigest()[:16]


def run(prog, opts, extra_env):
    raw = open(RASTER, "rb").read()
    env = dict(os.environ)
    env.update({"PPD": PPD, "CONTENT_TYPE": "application/vnd.cups-raster"})
    env.update(extra_env)
    for k in ("ELECTRON_RUN_AS_NODE", "NODE_OPTIONS", "CHROME_DESKTOP"):
        env.pop(k, None)
    p = subprocess.run([prog, "1", "atree", "test", "1", opts],
                       input=raw, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       env=env, timeout=180)
    return p.stdout, p.stderr


def read_baseline(path):
    """读基线清单 → ({(kind,value): (outlen, sha16, opts)}, 指纹)；先过三道完整性锁。"""
    text = bg.load(path, BASELINE_DIGEST, LOG)          # L1 + L2 + L3，不过即退出
    tbl = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        kind, val, ln, sha, opts = parts[0], parts[1], int(parts[2]), parts[3], parts[4]
        tbl[(kind, val)] = (ln, sha, opts)
    return tbl, bg.table_digest(text)


def verify(combos):
    """默认模式：只跑原生链，对基线清单自证。"""
    if not os.path.exists(NATF):
        sys.exit(f"找不到原生构件：{NATF}\n先跑 bash build/build-all.sh")
    if not os.path.exists(PPD):
        sys.exit(f"找不到 PPD：{PPD}")
    base, bdig = read_baseline(BASELINE)
    print("基线完整性：L1 表自校验 ✓  L2 源码内指纹 ✓  L3 台账指纹链 ✓")
    print("  指纹 %s…  变更历史见 BASELINE-LOG.md" % bdig[:16])
    print()

    npass = nfail = nmiss = 0
    fails = []
    lines = []
    hdr = f"{'kind':<11} {'value':<16} {'outlen':>7} {'sha16':<18} 结果"
    print(hdr); print("-" * len(hdr))
    for kind, val, opts in combos:
        exp = base.get((kind, val))
        if exp is None:
            nmiss += 1
            fails.append((kind, val, "基线缺该组合"))
            print(f"{kind:<11} {val:<16} {'-':>7} {'-':<18} MISSING")
            continue
        try:
            no, ne = run(NATF, opts, {"LD_LIBRARY_PATH": NATD})
        except subprocess.TimeoutExpired:
            nfail += 1; fails.append((kind, val, "TIMEOUT"))
            print(f"{kind:<11} {val:<16} {'-':>7} {'-':<18} TIMEOUT")
            continue
        got_len, got_sha = len(no), digest(no)
        exp_len, exp_sha = exp[0], exp[1]
        ok = (got_len == exp_len) and (got_sha == exp_sha)
        res = "PASS" if ok else "FAIL"
        if ok:
            npass += 1
        else:
            nfail += 1
            why = (f"长度 {got_len}≠{exp_len}" if got_len != exp_len
                   else f"哈希 {got_sha}≠{exp_sha}")
            fails.append((kind, val, why))
            os.makedirs(OUT, exist_ok=True)
            tag = val.replace('/', '_')
            open(os.path.join(OUT, f"FAIL-{kind}-{tag}.nat.bin"), "wb").write(no)
            if ne.strip():
                print(f"    stderr: {ne.decode('utf-8', 'replace').strip()[:200]}")
        print(f"{kind:<11} {val:<16} {got_len:>7} {got_sha:<18} {res}")
        lines.append(f"{kind}\t{val}\t{got_len}\t{got_sha}\t{exp_len}\t{exp_sha}\t{res}\t{opts}")

    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "result.tsv"), "w", encoding="utf-8") as f:
        f.write("kind\tvalue\tgot_len\tgot_sha\texp_len\texp_sha\tresult\topts\n")
        f.write("\n".join(lines) + "\n")

    print("-" * len(hdr))
    print(f"合计 PASS={npass}  FAIL={nfail}  MISSING={nmiss}")
    if fails:
        print("失败项:")
        for k, v, why in fails:
            print(f"  {k:<11} {v:<16} {why}")
    return 0 if (nfail == 0 and nmiss == 0) else 1


def main():
    if "--gen-baseline" in sys.argv:
        sys.exit("重算入口已迁到同目录的 relock.py（本脚本只做日常回归，\n"
                 "  除本仓库自带构件外不跑任何别的东西）。先看需要哪些参考输出：\n"
                 "      /usr/bin/python3 tests/sweep/relock.py --plan\n"
                 "  日常回归直接用：\n"
                 "      /usr/bin/python3 tests/sweep/regress.py\n")
    combos = params.build_combos()
    if "--quick" in sys.argv:
        combos = [c for c in combos if c[1] in params.QUICK_KEEP.get(c[0], [])]
    return verify(combos)


if __name__ == "__main__":
    sys.exit(main())
