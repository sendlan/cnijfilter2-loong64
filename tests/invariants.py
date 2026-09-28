#!/usr/bin/env python3
"""自判定测试（零参照物）—— 不读底座清单，也能判定实现是否合理。

与 ``tests/sweep/`` 的分工
--------------------------
* ``tests/sweep/`` 是**保真底座**：把「本实现 == 上游官方实现」这个事实锚在一份
  哈希清单上。它**长期依赖那份清单** —— 因为私有 IVEC 协议没有公开规格，
  「正确」的定义只能由官方实现给出，等价性参照无法自我判定。
* ``tests/invariants.py``（本文件）是**真正的测试底线**：所有判据都能自我判定真伪，
  **不读任何底座、不需要任何上游二进制**。即使把 ``tests/sweep/`` 整个删掉，
  它照样能判断实现是否合理。
* 两层共用的**测试输入**只有一份：``tests/small.raster``（放在 ``tests/`` 根，
  它是输入夹具、不是底座，本身不携带任何「正确性」信息）。

检查项
------
1. 确定性    同一输入跑两次 → 归一化时间戳后逐字节相同
2. 合法性    输出是结构完整的 IVEC 命令流（XML 头 / 命名空间 / 必需标签 / 时间戳格式与时效）
3. 参数生效  Borderless 与 CNGrayscale 必须改变输出；PageSize / MediaType 必须产生
             足够多种不同输出（不能退化成同一个结果）
4. 覆盖率    全部参数组合都必须产出非空、长度合理的输出
5. 维护作业  三个维护作业非空、互不相同、可重复
6. 通路区分  打印链输出纸张指令，维护链不含（证明两条通路没有互相串味）

**只断言实测成立的性质。** 已核实的实现行为：在当前 PPD（G3010，A4 机型）下
``ColorModel`` 与 ``Duplex`` 不改变单页输出，因此它们**不列入**断言 ——
写成「必须生效」会造成假红。

用法
----
    /usr/bin/python3 tests/invariants.py        # 期望全 PASS，rc=0
"""
import datetime as _dt
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # tests
PROJ = os.path.dirname(HERE)                               # 项目根
sys.path.insert(0, HERE)
import params                                              # noqa: E402  参数矩阵（单一来源）

NATD = os.environ.get("CNIJ_BUILT", os.path.join(PROJ, "build", "built"))
PPD = os.environ.get("CNIJ_PPD", os.path.join(PROJ, "ppd", "canong3010.ppd"))
RASTER = os.path.join(HERE, "small.raster")   # 共享测试输入（在 tests/ 根，两层共用）
RTC = os.path.join(NATD, "rastertocanonij")
CMD3 = os.path.join(NATD, "cmdtocanonij3")
UTLDIR = os.path.join(PROJ, "cmdtocanonij3", "utilfiles")
UUID = "job-uuid=urn:uuid:12345678-1234-1234-1234-123456789abc"

A4OPTS = ("PageSize=A4 Resolution=300dpi ColorModel=rgb MediaType=plain "
          "Duplex=None CNGrayscale=False")

XML_HEAD = b'<?xml version="1.0" encoding="utf-8" ?>'
NS_IVEC = b'xmlns:ivec="http://www.canon.com/ns/cmd/2008/07/common/"'
NS_VCN = b'xmlns:vcn="http://www.canon.com/ns/cmd/2008/07/canon/"'
DT_RE = re.compile(rb'<ivec:datetime>(\d{14})</ivec:datetime>')
DATASIZE_RE = re.compile(rb'<ivec:datasize>(\d+)</ivec:datasize>')


def _env(extra=None):
    e = dict(os.environ)
    e.update({"PPD": PPD, "LD_LIBRARY_PATH": NATD})
    if extra:
        e.update(extra)
    for k in ("ELECTRON_RUN_AS_NODE", "NODE_OPTIONS", "CHROME_DESKTOP"):
        e.pop(k, None)
    return e


def run_print(opts):
    raw = open(RASTER, "rb").read()
    p = subprocess.run([RTC, "1", "atree", "test", "1", opts], input=raw,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       env=_env({"CONTENT_TYPE": "application/vnd.cups-raster"}), timeout=180)
    return p.returncode, p.stdout, p.stderr


def run_mnt(utl_name):
    p = subprocess.run([CMD3, "1", "atree", "test", "1", UUID,
                        os.path.join(UTLDIR, utl_name)],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       env=_env(), timeout=120)
    return p.returncode, p.stdout, p.stderr


def norm(d):
    """把时间戳打成固定值 —— 时间天然不可复现，不是「不确定性」。"""
    return DT_RE.sub(b'<ivec:datetime>00000000000000</ivec:datetime>', d)


def sha16(d):
    return hashlib.sha256(norm(d)).hexdigest()[:16]


class Report:
    def __init__(self):
        self.total = 0
        self.bad = 0
        self.fails = []

    def check(self, name, ok, detail=""):
        self.total += 1
        if ok:
            print("  PASS  %s" % name)
        else:
            self.bad += 1
            self.fails.append((name, detail))
            print("  FAIL  %s%s" % (name, ("   — " + detail) if detail else ""))

    def section(self, title):
        print("\n── %s" % title)


def check_structure(r, rp):
    """共用的结构断言：一个合法的 IVEC 命令流该长什么样。"""
    rc, out, err = rp
    r.check("退出码为 0", rc == 0, "rc=%d stderr=%s" % (rc, err.decode("utf-8", "replace")[:120]))
    r.check("以 XML 声明开头", out.startswith(XML_HEAD), repr(out[:40]))
    r.check("含 ivec 命名空间声明", NS_IVEC in out)
    r.check("含 vcn 命名空间声明", NS_VCN in out)
    r.check("含 <cmd> 根元素", out.startswith(XML_HEAD + b"<cmd "))
    m = DT_RE.findall(out)
    r.check("恰好一个 <ivec:datetime>", len(m) == 1, "实际 %d 个" % len(m))
    if len(m) == 1:
        s = m[0].decode()
        try:
            ts = _dt.datetime.strptime(s, "%Y%m%d%H%M%S")
            delta = abs((_dt.datetime.now() - ts).total_seconds())
            r.check("时间戳是真实当前时间（±2 天内）", delta < 2 * 86400,
                    "%s 与系统时间相差 %.0f 秒" % (s, delta))
        except ValueError:
            r.check("时间戳是真实当前时间（±2 天内）", False, "无法解析 %r" % s)
    # 标签闭合平衡（挑有代表性的成对标签）
    for tag in (b"ivec:operation", b"ivec:contents", b"ivec:jobID"):
        opens = out.count(b"<" + tag + b">")
        closes = out.count(b"</" + tag + b">")
        r.check("<%s> 开合平衡" % tag.decode(), opens == closes and opens > 0,
                "开 %d / 闭 %d" % (opens, closes))


def main():
    for path, what in ((RTC, "rastertocanonij"), (CMD3, "cmdtocanonij3")):
        if not os.path.exists(path):
            sys.exit("找不到原生构件：%s\n先跑 bash build/build-all.sh" % path)
    if not os.path.exists(PPD):
        sys.exit("找不到 PPD：%s" % PPD)
    if not os.path.exists(RASTER):
        sys.exit("找不到测试输入：%s" % RASTER)

    r = Report()
    print("自判定测试（零参照物）｜不读底座清单、不需要任何上游二进制")
    print("构件 %s" % NATD)

    # ── 1. 确定性 + 2. 合法性 ─────────────────────────────────────────────
    r.section("1/6 确定性（同输入两次必须一致）")
    rc1, o1, e1 = run_print(A4OPTS)
    rc2, o2, e2 = run_print(A4OPTS)
    r.check("两次退出码均为 0", rc1 == 0 and rc2 == 0, "rc=%d/%d" % (rc1, rc2))
    r.check("两次输出长度相同且非空", len(o1) == len(o2) and len(o1) > 0,
            "%d vs %d" % (len(o1), len(o2)))
    r.check("归一化时间戳后逐字节相同", norm(o1) == norm(o2))

    r.section("2/6 合法性（IVEC 命令流结构）")
    check_structure(r, (rc1, o1, e1))
    r.check("含纸张指令 <ivec:papersize>", b"<ivec:papersize>" in o1)
    r.check("含介质指令 <ivec:papertype>", b"<ivec:papertype>" in o1)
    r.check("含颜色模式 <ivec:printcolormode>", b"<ivec:printcolormode>" in o1)
    ds = DATASIZE_RE.findall(o1)
    r.check("含 <ivec:datasize> 且为正整数", len(ds) == 1 and int(ds[0]) > 0,
            "值=%s" % (ds[0].decode() if ds else "缺失"))

    # ── 3. 参数确实生效 ───────────────────────────────────────────────────
    r.section("3/6 参数生效（不能退化成同一个输出）")
    opts = lambda **kw: ("PageSize=%s Resolution=300dpi ColorModel=%s MediaType=%s "
                         "Duplex=%s CNGrayscale=%s" % (
                             kw.get("ps", "A4"), kw.get("cm", "rgb"),
                             kw.get("mt", "plain"), kw.get("dp", "None"),
                             kw.get("cg", "False")))
    _, a4, _ = run_print(opts(ps="A4"))
    _, a4bl, _ = run_print(opts(ps="A4.bl"))
    r.check("Borderless 生效：A4 != A4.bl", norm(a4) != norm(a4bl),
            "sha16 %s vs %s" % (sha16(a4), sha16(a4bl)))
    _, cgf, _ = run_print(opts(cg="False"))
    _, cgt, _ = run_print(opts(cg="True"))
    r.check("CNGrayscale 生效：False != True", norm(cgf) != norm(cgt),
            "sha16 %s vs %s" % (sha16(cgf), sha16(cgt)))

    ps_vals = [c[1] for c in params.build_combos() if c[0] == "PageSize"]
    ps_uniq = {}
    for v in ps_vals:
        _, o, _ = run_print(opts(ps=v))
        ps_uniq.setdefault(sha16(o), []).append(v)
    r.check("PageSize 产生 >=15 种不同输出（共 %d 个取值）" % len(ps_vals),
            len(ps_uniq) >= 15, "实际 %d 种" % len(ps_uniq))

    mt_vals = [c[1] for c in params.build_combos() if c[0] == "MediaType"]
    mt_uniq = {}
    for v in mt_vals:
        _, o, _ = run_print(opts(mt=v))
        mt_uniq.setdefault(sha16(o), []).append(v)
    r.check("MediaType 产生 >=4 种不同输出（共 %d 个取值）" % len(mt_vals),
            len(mt_uniq) >= 4, "实际 %d 种" % len(mt_uniq))

    # ── 4. 覆盖率 ─────────────────────────────────────────────────────────
    r.section("4/6 覆盖率（全部参数组合都必须产出合理输出）")
    combos = params.build_combos()
    bad_rc, bad_len, bad_dt = [], [], []
    for kind, val, o_ in combos:
        rc, out, _ = run_print(o_)
        if rc != 0:
            bad_rc.append("%s=%s" % (kind, val))
        elif len(out) < 1000:
            bad_len.append("%s=%s(%d)" % (kind, val, len(out)))
        if not DT_RE.search(out):
            bad_dt.append("%s=%s" % (kind, val))
    r.check("全部 %d 组退出码为 0" % len(combos), not bad_rc, "失败: %s" % bad_rc[:5])
    r.check("全部输出长度 >= 1000 字节", not bad_len, "过短: %s" % bad_len[:5])
    r.check("全部输出都含合法时间戳", not bad_dt, "缺失: %s" % bad_dt[:5])

    # ── 5. 维护作业 ───────────────────────────────────────────────────────
    r.section("5/6 维护作业（非空 / 互不相同 / 可重复）")
    mnt = {}
    for name, utl in params.MNT_CASES:
        rc, out, err = run_mnt(utl)
        mnt[name] = out
        r.check("%s 退出码为 0 且非空" % name, rc == 0 and len(out) > 0,
                "rc=%d len=%d" % (rc, len(out)))
    names = [n for n, _ in params.MNT_CASES]
    shas = [sha16(mnt[n]) for n in names]
    r.check("三个作业输出互不相同", len(set(shas)) == len(shas),
            "sha16 = %s" % ", ".join(shas))
    _, again, _ = run_mnt(params.MNT_CASES[0][1])
    r.check("同一作业重复运行结果一致", norm(again) == norm(mnt[names[0]]))

    # ── 6. 通路区分 ───────────────────────────────────────────────────────
    r.section("6/6 通路区分（打印链与维护链不串味）")
    r.check("打印链含 <ivec:papersize>、维护链不含",
            b"<ivec:papersize>" in a4 and all(b"<ivec:papersize>" not in mnt[n] for n in names))
    r.check("维护链含 <ivec:inkgroup>（清洗/检查类指令）",
            any(b"<ivec:inkgroup>" in mnt[n] for n in names))

    # ── 汇总 ──────────────────────────────────────────────────────────────
    print("\n" + "=" * 60)
    print("合计 %d/%d 通过" % (r.total - r.bad, r.total))
    if r.fails:
        print("失败项：")
        for name, detail in r.fails:
            print("  - %s%s" % (name, ("  [%s]" % detail) if detail else ""))
    return 1 if r.bad else 0


if __name__ == "__main__":
    sys.exit(main())
