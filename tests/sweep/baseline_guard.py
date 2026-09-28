#!/usr/bin/env python3
"""基线完整性守卫 —— 让「改基线」这件事无法静默发生。

威胁模型
--------
真正的风险不是「有人恶意改源码」（真能改源码的人什么都能改），而是更常见的那种：
**某次改动让回归变红，于是有人顺手重算基线，把红刷成绿，而且没人察觉。**
「回归测试」一旦可以被一条命令静默重置，它就退化成仪式。

本模块用四层把这条路堵死：

L1 表自校验
    每个基线文件末尾内嵌一行 ``# integrity sha256:<全表指纹>``（指纹不含该行本身）。
    加载时先算一遍 —— 不符即硬失败。挡住手滑、单字节误改、编辑器自动换行。

L2 源码内指纹
    每个回归脚本顶部硬编码 ``BASELINE_DIGEST = "<64 位 hex>"``，与表内指纹交叉校验。
    于是「只改 .tsv」改不动：必须连脚本常量一起改。空串视为未初始化，同样拒绝运行。

L3 追加式台账（指纹链）
    ``BASELINE-LOG.md`` 为每个基线维护一条 prev → new 的指纹链。
    守卫要求：全链 prev/new 必须首尾相接，且末条 new == 当前表指纹。
    所以「重算基线」只能**追加**一条带理由的记录，不能改数字、也不能悄悄回滚。
    重算基线强制要求写明理由，理由为空则拒绝登记。

L4 真正的断环之处
    以上三层都在仓库内，理论上可被一起改掉。不可伪造的锚是
    **git 提交历史 + 代码评审**。守卫的价值不是「无法伪造」，而是让任何
    静默改动必然产生 3~4 个文件的显式 diff（.tsv + 脚本常量 + 台账），
    无法混在别的改动里溜过去。评审时只要盯住：diff 里有没有出现这几个文件。
"""
import hashlib
import os
import re
import sys

INTEGRITY_RE = re.compile(r'^#\s*integrity\s+sha256:([0-9a-f]{64})\s*$')
HEX64_RE = re.compile(r'^[0-9a-f]{64}$')
LOG_NAME = "BASELINE-LOG.md"

LOG_HEADER = """# 基线变更台账（append-only）

本文件是 `baseline.tsv` 与 `baseline-mnt.tsv` 的**指纹链**。
每次重算底座清单，都会在下方追加一行。

**只允许追加。修改或删除任何历史行 = 破坏指纹链，回归会直接硬失败。**

为什么需要它：底座清单是一串「输出长度 + SHA-256」的派生数据，本身不可读。
若允许随手重算，回归就会退化成「红色就重置」的仪式。台账把每次重算变成
一条必须写明理由的记录 —— 静默改底座在 git diff 上再也藏不住。

## 关于重算：参照可以在纯 loong64 上自产

重算分两步：

1. **取得参照物**：跑「官方实现」，拿到同一批输入的原始输出。**在纯 loong64 上
   这一步零依赖即可完成** —— 产出命令流的构件源码是公开的（GPL-2），与官方
   源码包逐字节相同，且不依赖闭源库。依据在**原厂自己的文档**里（随源码包分发、
   已入库）：`rastertocanonij/README` 写明它 "converting **cupsraster** to the
   **Canon Inkjet Printer command stream**"，`tocnpwg/README` 写明它只依赖
   `libcups`，`cmdtocanonij3/README` 的 `REQUIREMENTS: None`；三个模块的
   `LICENSE` 均为 GPL，其中 `cmdtocanonij3/LICENSE` 另有一条 GPL 例外，
   **明文允许**它与 "the libraries released as the **binary modules**" 链接
   —— 恰好授权本仓库自研的 `libcnbpcnclapicom2`。

   ```bash
   /usr/bin/python3 tests/sweep/selfref.py        --outdir ~/refl --emit-refdir ~/ref
   ```

   它用 loong64 原生 gcc 现编 `rastertocanonij` / `tocnpwg` / `cmdtocanonij3`
   并跑出全量参照。**实测用这份参照封板，得到的底座与「用官方发布的二进制取参照」
   得到的逐字节相同。** 若你另有渠道（别的机器 / 官方发布的别的架构），也可以
   自己产出同样的目录结构 —— `relock.py --plan` 会列出需要哪些文件。
2. **比对并封板**（**仓库内**）：`tests/sweep/relock.py --refdir <目录> --reason "…"`。
   它跑本仓库自己的构件、与参照物逐字节比对（归一化 `datetime`），**全部一致才写**；
   写入时同步完成三件事：封 `integrity` 指纹、追加本台账一行、更新回归脚本里的
   `BASELINE_DIGEST` 常量，最后回读三道锁。

为什么参照仍然必须存在：本协议是厂商私有协议，没有公开规格 ——「什么才算对」
只能由**官方实现**给出。区别只在于「官方实现」可以是我们用官方源码现编的那一份
（`selfref.py`），而不必是官方发布的某个架构的二进制。

`relock.py` 的**入口是仓库内的**，且它本身不跑任何本仓库自带构件之外的东西 ——
参考目录是它唯一的、显式传入的外部输入。日常回归（`regress.py` / `mnt-regress.py`
默认模式）**完全不需要它**。

⭐ 两条硬边界，别越：

1. **底座（`baseline*.tsv`）本身是入 git 的**，随仓库分发 —— 克隆下来即有，
   不需要额外下载、不需要任何外部归档。
2. **读取底座不需要任何模拟器 / 翻译层运行方式**：回归只 exec 本仓库
   `build/built/` 里的原生构件，再把输出指纹与清单比对。**仓库内不存在、
   也不接受「必须先跑一个外部运行方式才能验证」的设计**；需要外部环境的
   只有「取得参照物」这一步 —— 它是一次性维护动作，且**参考物以文件形式交接**，
   运行方式由使用者自选。

## 修订记录

| 时间 | 清单 | prev_digest | new_digest | 比对结论 | 理由 |
|---|---|---|---|---|---|
"""


def _fail(msg: str):
    sys.exit("基线守卫拦截 ｜ " + msg)


def table_digest(text: str) -> str:
    """对「除 integrity 行以外的全部行」求 SHA-256（统一补一个尾部换行）。"""
    lines = [l for l in text.splitlines() if not INTEGRITY_RE.match(l)]
    return hashlib.sha256(("\n".join(lines) + "\n").encode("utf-8")).hexdigest()


def seal(text: str) -> str:
    """去掉旧 integrity 行，追加新的一行。返回封好的全文。"""
    lines = [l for l in text.splitlines() if not INTEGRITY_RE.match(l)]
    body = "\n".join(lines) + "\n"
    return body + "# integrity sha256:" + table_digest(body) + "\n"


def load_log(log_path: str):
    """读台账 → [[时间, 基线, prev, new, 比对, 理由], ...]"""
    rows = []
    if not os.path.exists(log_path):
        return rows
    with open(log_path, "r", encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if not s.startswith("|"):
                continue
            cells = [c.strip() for c in s.strip("|").split("|")]
            if len(cells) != 6 or cells[0] == "时间" or set(cells[0]) <= set("-: "):
                continue
            rows.append(cells)
    return rows


def _log_entries(log_path: str, baseline_name: str):
    rows = load_log(log_path)
    mine = [r for r in rows if r[1] == baseline_name]
    for i, r in enumerate(mine):
        want = "-" if i == 0 else mine[i - 1][3]
        if r[2] != want:
            _fail("台账 %s 中 %s 的第 %d 条 prev 指纹与上一条 new 不衔接 —— 历史行被改动过。"
                  % (os.path.basename(log_path), baseline_name, i + 1))
        if not HEX64_RE.match(r[3]):
            _fail("台账 %s 中 %s 的第 %d 条 new 指纹格式非法。"
                  % (os.path.basename(log_path), baseline_name, i + 1))
        if not r[5]:
            _fail("台账 %s 中 %s 的第 %d 条没有写明理由。"
                  % (os.path.basename(log_path), baseline_name, i + 1))
    return mine


def load(path: str, expected_digest: str, log_path: str) -> str:
    """加载并三重校验基线文件，返回全文。任一层不过 → 立即退出。"""
    name = os.path.basename(path)
    if not os.path.exists(path):
        _fail("找不到基线清单 %s\n它应随仓库分发。确需重算请用同目录的 "
              "relock.py（必须写明理由，变更记录见同目录 BASELINE-LOG.md）。" % path)

    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    d = table_digest(text)

    hits = [l for l in text.splitlines() if INTEGRITY_RE.match(l)]
    if not hits:
        _fail("%s 缺少 `# integrity sha256:...` 尾行 —— 被截断或手工编辑过。" % name)
    if len(hits) > 1:
        _fail("%s 出现 %d 行 integrity —— 文件被篡改。" % (name, len(hits)))
    inner = INTEGRITY_RE.match(hits[0]).group(1)
    if inner != d:
        _fail("%s 的内容与其自带的 integrity 指纹不符 —— 表被改过。\n"
              "  表内指纹 %s\n  实际指纹 %s" % (name, inner, d))

    if not expected_digest:
        _fail("%s 的脚本内指纹为空（BASELINE_DIGEST 未初始化）—— 拒绝运行。\n"
              "  初始化方法：用同目录的 relock.py 封板并登记该基线（见 BASELINE-LOG.md）。"
              % name)
    if d != expected_digest:
        _fail("%s 的指纹与脚本内 BASELINE_DIGEST 不符 —— 数据文件与校验常量不同步。\n"
              "  脚本常量 %s\n  实际指纹 %s\n"
              "  只改数据文件改不动，这是 L2 在拦。" % (name, expected_digest, d))

    mine = _log_entries(log_path, name)
    if not mine:
        _fail("台账 %s 里没有 %s 的记录 —— 该基线未经登记。" % (os.path.basename(log_path), name))
    if mine[-1][3] != d:
        _fail("台账末条记录的指纹与当前 %s 不符 —— 基线被改动却没有登记。\n"
              "  台账末条 %s\n  当前指纹 %s" % (name, mine[-1][3], d))
    return text


def record(log_path: str, baseline_name: str, new_digest: str,
           compare: str, reason: str) -> str:
    """追加一条台账记录，返回本次的 prev 指纹。"""
    if not reason or not reason.strip():
        _fail("重算基线必须给出理由（一句话说明为什么基线要变）。")
    if "\n" in reason or "|" in reason:
        _fail("--reason 必须是单行、且不含 `|` 字符。")

    mine = _log_entries(log_path, baseline_name)
    prev = mine[-1][3] if mine else "-"

    if not os.path.exists(log_path):
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(LOG_HEADER)
        mine = []
        prev = "-"

    from datetime import datetime
    stamp = datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    line = "| %s | %s | %s | %s | %s | %s |\n" % (
        stamp, baseline_name, prev, new_digest, compare, reason.strip())
    with open(log_path, "a", encoding="utf-8") as f:
        f.write(line)
    return prev


def commit(path: str, text: str) -> str:
    """把（未封口的）全文封上 integrity 后写盘，返回指纹。"""
    sealed = seal(text)
    with open(path, "w", encoding="utf-8") as f:
        f.write(sealed)
    return table_digest(sealed)


def sync_constant(script_path: str, digest: str) -> bool:
    """把脚本里的 BASELINE_DIGEST 常量同步成新指纹（L2）。"""
    with open(script_path, "r", encoding="utf-8") as f:
        txt = f.read()
    new, n = re.subn(r'^BASELINE_DIGEST = "[0-9a-f]*"',
                     'BASELINE_DIGEST = "%s"' % digest, txt, flags=re.M)
    if n != 1:
        return False
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(new)
    return True


def arg_value(argv, name):
    """取 `--name value` 或 `--name=value` 的值；没有则 None。"""
    for i, a in enumerate(argv):
        if a == name and i + 1 < len(argv):
            return argv[i + 1]
        if a.startswith(name + "="):
            return a.split("=", 1)[1]
    return None
