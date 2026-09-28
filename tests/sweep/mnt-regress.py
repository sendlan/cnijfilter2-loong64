#!/usr/bin/env python3
"""维护路径回归 —— 原生 cmdtocanonij3 vs 已入库的基线清单（哈希自证）。

覆盖三种维护作业：清洗(Clean all) / 喷嘴检查(PrintSelfTestPage) / 自动对齐。
维护命令纯离线生成，不需要打印机在线。

与 ``regress.py`` 同一套设计：只跑本仓库自己的构件，对 ``baseline-mnt.tsv``
里的「长度 + 归一化 SHA-256」自证 —— **不依赖任何闭源件、不需联网、不需装
系统包、不需要任何上游二进制或模拟器，也不引用仓库外任何目录**。
基线是**只读物证**；重算属维护操作、入口在同目录 ``relock.py``，本脚本日常运行
完全不碰它（见 BASELINE-LOG.md）。

基线防篡改
----------
与 ``regress.py`` 共用 ``baseline_guard.py``：L1 表内嵌 integrity 自校验、
L2 本脚本硬编码 ``BASELINE_DIGEST`` 交叉验证、L3 ``BASELINE-LOG.md`` append-only
指纹链（重算必须登记理由）。详见该模块文档。

用法
----
    /usr/bin/python3 tests/sweep/mnt-regress.py
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
PPD = os.environ.get("CNIJ_PPD", os.path.join(PROJ, "ppd", "canong3010.ppd"))
SRC = os.environ.get("CNIJ_SRC", PROJ)                     # 源码树 = 仓库根
BASELINE = os.path.join(HERE, "baseline-mnt.tsv")
LOG = os.path.join(HERE, bg.LOG_NAME)
OUT = os.path.join(HERE, "regress")

# ── L2：源码内指纹 ──────────────────────────────────────────────────────────
# 与 baseline-mnt.tsv 末尾的 integrity 行互为交叉校验，两处必须一致。
# **不要手工填**：改基线一律走同目录的 relock.py —— 它在写表的同时同步这一行、
# 登记 BASELINE-LOG.md、并回读三道锁。手工填会让 L2 失去意义。
BASELINE_DIGEST = "16ccdc72f387ef92aefae8416a4af43e0b06f4f164c8d0288c97d312d7a5be3c"

NATF = os.path.join(NATD, "cmdtocanonij3")
UTLDIR = os.path.join(SRC, "cmdtocanonij3", "utilfiles")
UUID = "job-uuid=urn:uuid:12345678-1234-1234-1234-123456789abc"

DT_RE = re.compile(rb'<ivec:datetime>\d{14}</ivec:datetime>')


def norm(d: bytes) -> bytes:
    return DT_RE.sub(b'<ivec:datetime>00000000000000</ivec:datetime>', d)


def digest(d: bytes) -> str:
    return hashlib.sha256(norm(d)).hexdigest()[:16]


def run(prog, utl, extra_env):
    env = dict(os.environ)
    env.update({"PPD": PPD})
    env.update(extra_env)
    for k in ("ELECTRON_RUN_AS_NODE", "NODE_OPTIONS", "CHROME_DESKTOP"):
        env.pop(k, None)
    p = subprocess.run([prog, "1", "atree", "test", "1", UUID, utl],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       env=env, timeout=120)
    return p.stdout, p.stderr


def read_baseline(path):
    """读基线清单 → ({作业: (outlen, sha16, utl)}, 指纹)；先过三道完整性锁。"""
    text = bg.load(path, BASELINE_DIGEST, LOG)          # L1 + L2 + L3，不过即退出
    tbl = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        tbl[parts[0]] = (int(parts[1]), parts[2], parts[3])
    return tbl, bg.table_digest(text)


def verify():
    if not os.path.exists(NATF):
        sys.exit(f"找不到原生构件：{NATF}\n先跑 bash build/build-all.sh")
    if not os.path.exists(PPD):
        sys.exit(f"找不到 PPD：{PPD}")
    base, bdig = read_baseline(BASELINE)
    print("基线完整性：L1 表自校验 ✓  L2 源码内指纹 ✓  L3 台账指纹链 ✓")
    print("  指纹 %s…  变更历史见 BASELINE-LOG.md" % bdig[:16])
    print()

    npass = nfail = nmiss = 0
    print(f"{'作业':<14} {'outlen':>7} {'sha16':<18} 结果")
    print("-" * 58)
    for name, utl_name in params.MNT_CASES:
        exp = base.get(name)
        if exp is None:
            nmiss += 1
            print(f"{name:<14} {'-':>7} {'-':<18} MISSING")
            continue
        utl = os.path.join(UTLDIR, utl_name)
        no, ne = run(NATF, utl, {"LD_LIBRARY_PATH": NATD})
        got_len, got_sha = len(no), digest(no)
        exp_len, exp_sha = exp[0], exp[1]
        ok = (got_len == exp_len) and (got_sha == exp_sha)
        print(f"{name:<14} {got_len:>7} {got_sha:<18} {'PASS' if ok else 'FAIL'}")
        if ok:
            npass += 1
            continue
        nfail += 1
        os.makedirs(OUT, exist_ok=True)
        open(os.path.join(OUT, f"FAIL-mnt-{name}.nat.bin"), "wb").write(no)
        if got_len != exp_len:
            print(f"    长度不符：原生 {got_len} vs 基线 {exp_len}")
        # 哈希不符时给出与原厂基线可比的定位信息（只看长度前缀内的差异位置）
        if ne.strip():
            print(f"    stderr: {ne.decode('utf-8', 'replace').strip()[:300]}")
    print("-" * 58)
    print(f"合计 PASS={npass}  FAIL={nfail}  MISSING={nmiss}")
    return 0 if (nfail == 0 and nmiss == 0) else 1


def main():
    if "--gen-baseline" in sys.argv:
        sys.exit("重算入口已迁到同目录的 relock.py（本脚本只做日常回归，\n"
                 "  除本仓库自带构件外不跑任何别的东西）。先看需要哪些参考输出：\n"
                 "      /usr/bin/python3 tests/sweep/relock.py --plan\n"
                 "  日常回归直接用：\n"
                 "      /usr/bin/python3 tests/sweep/mnt-regress.py\n")
    return verify()


if __name__ == "__main__":
    sys.exit(main())
