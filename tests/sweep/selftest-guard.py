#!/usr/bin/env python3
"""基线守卫自测 —— 证明三道锁真的会拦，而不是装饰。

`baseline_guard.py` 声称能让「静默改基线」变成不可能。这个说法必须可证伪，
否则它只是一段好看的说辞。本脚本构造 10 种攻击/误操作，逐一验证守卫确实拦下，
并且验证**正常情况必须放行**（否则锁会把好人一起锁在门外）。

第 10 条是**结构一致性**而非攻击：`LOG_HEADER` 是「台账丢了就照它重建」的模板，
若它与真实 `BASELINE-LOG.md` 的骨架漂移，重建出来的台账会把关键说明丢掉，
而所有攻击用例仍会全绿 —— 这个盲区本仓库踩过一次，故固化成判据。

不依赖任何闭源件、不需联网、不碰真实基线 —— 全程在 `regress/selftest/`
（已 gitignore）里的副本上做。

用法：``/usr/bin/python3 tests/sweep/selftest-guard.py``
返回 0 = 全部符合预期。
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import baseline_guard as bg                                # noqa: E402

TMP = os.path.join(HERE, "regress", "selftest")
REAL_BASELINE = os.path.join(HERE, "baseline.tsv")
REAL_LOG = os.path.join(HERE, bg.LOG_NAME)


def wipe():
    if os.path.isdir(TMP):
        shutil.rmtree(TMP)
    os.makedirs(TMP)


def fresh():
    """每个用例一份全新的临时目录，返回 (基线路径, 台账路径)。"""
    d = os.path.join(TMP, "case%d" % fresh.n)
    fresh.n += 1
    os.makedirs(d)
    return os.path.join(d, "baseline.tsv"), os.path.join(d, "BASELINE-LOG.md")
fresh.n = 1


def try_load(path, digest, log):
    """跑一次守卫加载。返回 None=放行，否则返回拦截信息。"""
    try:
        bg.load(path, digest, log)
        return None
    except SystemExit as e:
        return str(e)


def try_record(log, name, digest, compare, reason):
    try:
        bg.record(log, name, digest, compare, reason)
        return None
    except SystemExit as e:
        return str(e)


def write_log(path, rows):
    with open(path, "w", encoding="utf-8") as f:
        f.write(bg.LOG_HEADER)
        for r in rows:
            f.write("| %s |\n" % " | ".join(r))


# ── 构造基线：真实 baseline.tsv 的副本 + 一条合法台账 ──────────────────────
REAL_TEXT = open(REAL_BASELINE, "r", encoding="utf-8").read()
REAL_DIGEST = bg.table_digest(REAL_TEXT)
NAME = os.path.basename(REAL_BASELINE)


def seed(bp, lp, tamper=False, reseal=False):
    """铺一份「已登记」的正常现场；tamper/reseal 用于构造攻击。"""
    text = REAL_TEXT
    if tamper:
        text = text.replace("\t12483\t", "\t12484\t", 1)
    if reseal:
        text = bg.seal(text)
    with open(bp, "w", encoding="utf-8") as f:
        f.write(text)
    d = bg.table_digest(text)
    write_log(lp, [["2026-09-27T20:44:43", NAME, "-", REAL_DIGEST, "66/66 一致", "初始登记"]])
    return d


def main():
    wipe()
    results = []

    # 0. 正常现场必须放行 —— 锁不能把好人锁在门外
    bp, lp = fresh()
    seed(bp, lp)
    got = try_load(bp, REAL_DIGEST, lp)
    results.append(("正常：表/脚本/台账三者一致", "放行", got is None, got))

    # 1. 改动数据行，不动 integrity 行 → L1 拦
    bp, lp = fresh()
    seed(bp, lp, tamper=True)
    got = try_load(bp, REAL_DIGEST, lp)
    results.append(("改一个哈希值（不动 integrity 行）", "拦截",
                    got is not None and "integrity" in got, got))

    # 2. 改数据行 + 重算 integrity 行 → L2 拦（脚本常量没跟着改）
    bp, lp = fresh()
    d = seed(bp, lp, tamper=True, reseal=True)
    got = try_load(bp, REAL_DIGEST, lp)          # 仍按脚本里的旧常量校验
    results.append(("连 integrity 行一起重算", "拦截",
                    got is not None and "BASELINE_DIGEST" in got, got))

    # 3. 上一步再改脚本常量 → L3 拦（台账末条对不上）
    bp, lp = fresh()
    d = seed(bp, lp, tamper=True, reseal=True)
    got = try_load(bp, d, lp)                    # 常量也同步了，L1/L2 都过
    results.append(("再同步脚本内常量（三层全改）", "拦截",
                    got is not None and "台账末条" in got, got))

    # 4. 完全没有台账 → L3 拦
    bp, lp = fresh()
    seed(bp, lp)
    os.remove(lp)
    got = try_load(bp, REAL_DIGEST, lp)
    results.append(("把台账整个删掉", "拦截",
                    got is not None and "没有" in got and "记录" in got, got))

    # 5. 台账里没有这个基线的记录 → L3 拦
    bp, lp = fresh()
    seed(bp, lp)
    write_log(lp, [["2026-09-27T20:44:43", "别的基线.tsv", "-", REAL_DIGEST, "66/66 一致", "无关记录"]])
    got = try_load(bp, REAL_DIGEST, lp)
    results.append(("台账里只有其它基线的记录", "拦截",
                    got is not None and "没有" in got, got))

    # 6. 篡改台账历史行的 prev → 指纹链断裂
    bp, lp = fresh()
    seed(bp, lp)
    a = "a" * 64
    write_log(lp, [["t1", NAME, "-", a, "1/1 一致", "第一次"],
                   ["t2", NAME, "b" * 64, REAL_DIGEST, "1/1 一致", "第二次"]])
    got = try_load(bp, REAL_DIGEST, lp)
    results.append(("台账历史行的 prev 被改（链断裂）", "拦截",
                    got is not None and "不衔接" in got, got))

    # 7. 台账末条 new 被回滚成旧值 → 与当前表不符
    bp, lp = fresh()
    seed(bp, lp)
    write_log(lp, [["t1", NAME, "-", "c" * 64, "1/1 一致", "旧值"]])
    got = try_load(bp, REAL_DIGEST, lp)
    results.append(("台账末条 new 被回滚成旧值", "拦截",
                    got is not None and "不符" in got, got))

    # 8. 重算时不写理由 → 拒绝登记
    bp, lp = fresh()
    seed(bp, lp)
    got = try_record(lp, NAME, REAL_DIGEST, "1/1 一致", "   ")
    results.append(("理由为空（拒绝登记）", "拦截", got is not None and "理由" in got, got))

    # 9. 脚本内常量未初始化 → L2 拦（不给「空串放行」的后门）
    bp, lp = fresh()
    seed(bp, lp)
    got = try_load(bp, "", lp)
    results.append(("脚本内常量留空（未初始化）", "拦截",
                    got is not None and "未初始化" in got, got))

    # 10. 结构一致性：LOG_HEADER 必须与真实台账的骨架对齐
    #     否则「台账丢了 → 照模板重建」会把关键说明丢掉，而上面 9 条仍全绿。
    real_log = open(REAL_LOG, "r", encoding="utf-8").read()
    hdr = bg.LOG_HEADER
    k = hdr.find("## 修订记录")
    m = real_log.find("## 修订记录")
    ok_struct = (k > 0 and m > 0 and hdr[:k] == real_log[:m])
    # 表头行也必须是同一份
    ok_struct = ok_struct and "| 时间 | 清单 | prev_digest | new_digest | 比对结论 | 理由 |" in hdr
    detail = "" if ok_struct else "LOG_HEADER 与真实台账的骨架不一致 —— 重建会丢说明"
    results.append(("LOG_HEADER 与真实台账骨架一致", "放行", ok_struct, detail))

    # ── 汇总 ────────────────────────────────────────────────────────────────
    print("%-34s %-6s %s" % ("攻击 / 误操作场景", "期望", "结果"))
    print("-" * 66)
    bad = 0
    for title, expect, ok, msg in results:
        mark = ("已拦截 ✓" if expect == "拦截" else "已放行 ✓") if ok else "!! 不符合预期"
        print("%-34s %-6s %s" % (title, expect, mark))
        if not ok:
            bad += 1
            print("    -> %s" % (msg or "(竟然放行了)"))
    print("-" * 66)
    print("合计 %d/%d 符合预期" % (len(results) - bad, len(results)))
    shutil.rmtree(TMP, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
