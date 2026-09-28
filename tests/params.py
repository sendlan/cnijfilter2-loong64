#!/usr/bin/env python3
"""测试参数矩阵 —— 打印链路组合 + 维护作业清单（**单一来源**）。

`tests/sweep/regress.py`、`tests/sweep/mnt-regress.py` 与 `tests/invariants.py`
共用本模块，避免同一份参数清单在多处重复维护。

⚠️ `build_combos()` 返回的**顺序与内容**是保真底座（`baseline.tsv`）的一部分 ——
改动它会直接导致回归失配，必须同步重算底座（入口 `tests/sweep/relock.py`，
见 `tests/sweep/BASELINE-LOG.md`）。
"""

RES = "Resolution=300dpi"


def build_combos():
    """打印链路参数矩阵：页面尺寸 / 介质类型 / 颜色 / 双面 / 无边距。

    返回 ``[(kind, value, "k=v k=v ..."), ...]``，顺序稳定。
    """
    combos = []

    ps = ("Letter Letter.bl legal A5 A4 A4.bl A3 A3.bl a3plus a6 B5 B5.bl B4 oficio foolscap "
          "4x6 4x6.bl 5x7in 5x7in.bl 8x10 8x10.bl 10x12 7x10 l l.bl 2l Postcard Postcard.bl "
          "envelop10p envelopdlp businesscard businesscard.bl square89 square4in square127 "
          "square127.bl square12in").split()
    for v in ps:
        combos.append(("PageSize", v,
            f"PageSize={v} {RES} ColorModel=rgb MediaType=plain Duplex=None CNGrayscale=False"))

    mt = ("plain glossygold photopaperpro2 proplatinum procrystalgrade photopaperpro superphoto "
          "luster semigloss glossypaper matte photopaper envelope ijpostcard postcard label "
          "highres photo greetingcard cardstock").split()
    for v in mt:
        combos.append(("MediaType", v,
            f"PageSize=A4 {RES} ColorModel=rgb MediaType={v} Duplex=None CNGrayscale=False"))

    for cm in ("rgb", "gray"):
        for cg in ("False", "True"):
            combos.append(("Color", f"{cm}/{cg}",
                f"PageSize=A4 {RES} ColorModel={cm} MediaType=plain Duplex=None CNGrayscale={cg}"))

    for dp in ("None", "DuplexTumble", "DuplexNoTumble"):
        combos.append(("Duplex", dp,
            f"PageSize=A4 {RES} ColorModel=rgb MediaType=plain Duplex={dp} CNGrayscale=False"))

    for v in ("A4", "A4.bl"):
        combos.append(("Borderless", v,
            f"PageSize={v} {RES} ColorModel=rgb MediaType=plain Duplex=None CNGrayscale=False"))

    return combos


# `--quick` 时每个类别保留的代表性取值
QUICK_KEEP = {"PageSize": ["A4", "Letter", "square127", "l"],
              "MediaType": ["plain", "glossygold", "photopaper"],
              "Color": ["rgb/False", "gray/True"],
              "Duplex": ["None", "DuplexNoTumble"],
              "Borderless": ["A4", "A4.bl"]}


# 维护链路作业清单（命令纯离线生成，不需要打印机在线）
MNT_CASES = [
    ("cleaning",    "cleaning.utl"),
    ("nozzlecheck", "nozzlecheck.utl"),
    ("autoalign",   "autoalign.utl"),
]
