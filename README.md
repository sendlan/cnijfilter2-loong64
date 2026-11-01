# cnijfilter2 —— Canon IJ 打印机驱动 · loong64 全原生

**中文** · [English](README.en.md)

Canon `cnijfilter2` 的 LoongArch 原生重建：全部 ELF 均为本架构编译，不依赖模拟层。

| | |
|---|---|
| 目标平台 | Loongnix 25 / LoongArch 3A6000（亦可用于其它 loong64 发行版） |
| 上游源码 | Canon `cnijfilter2` 6.90-1（`cnijfilter2-source-6.90-1.tar.gz`） |
| 对齐目标 | 官方 6.90 的运行时行为（逐字节） |
| 产物 | `packages/cnijfilter2_6.90-6_all.deb`（180 KB）—— `Architecture: all`，一份包在 `loong64`（Debian/Loongnix）与 `loongarch64`（AOSC）上都可安装 |
| 许可 | GPL-2（上游源码与 PPD 均为 GPL-2；改动部分 GPL-2.0-or-later） |

**7 个原厂模块**（`lgmon3` `tocanonij` `tocnpwg` `cnijbe2` `rastertocanonij`
`cmdtocanonij2` `cmdtocanonij3`）用上游源码编译，移植改动见
`build/patches/loong64-native.patch`。

**4 个库为原生实现**：`libcnnet2` `libcnbpnet20`
`libcnbpnet30` `libcnbpcnclapicom2`，见 `cncl/` `cnnet/`。

---

## 编译

依赖与详细步骤见 [`build/docs/BUILD.md`](build/docs/BUILD.md)。

```bash
sudo apt install --yes gcc make autoconf automake libtool pkg-config \
     libcups2-dev libcupsimage2-dev libusb-1.0-0-dev libxml2-dev

bash build/build-all.sh                                    # 编 11 个构件
bash build/make-deb.sh                                     # → packages/（一个 all 包）
echo <口令> | sudo -S -p '' -E apt install --yes ./packages/*.deb
```

`Architecture: all` 包通吃两个架构名：`loong64`（Debian/Loongnix）与
`loongarch64`（AOSC）是同一套二进制，仅 dpkg 字段不同；`preinst` 会拒绝
非 LoongArch 主机。需要架构专包时：

```bash
ARCHS="loong64 loongarch64" bash build/make-deb.sh   # 出两个架构专包
ARCH=loong64                bash build/make-deb.sh   # 只出 loong64
DEPS="cups, libcups2, libc6" bash build/make-deb.sh  # 覆盖 Depends（非 Debian 系）
```

只想编一个模块：`bash build/build-all.sh lgmon3`
只改打包脚本时：`bash build/make-deb.sh`（需先跑过一次 `build/build-all.sh`）

---

## 验证

```bash
/usr/bin/python3 tests/invariants.py           # 基准（零参照物）        31/31
/usr/bin/python3 tests/sweep/regress.py        # 底座·打印链路           66/66
/usr/bin/python3 tests/sweep/mnt-regress.py    # 底座·维护链路            3/3
/usr/bin/python3 tests/sweep/selftest-guard.py # 守卫对抗自测           11/11
bash tests/check-native-only.sh                # 交付面自证
```

- `invariants.py`（基准）：零参照物，自带判据。
- `sweep/baseline*.tsv`（底座）：与官方实现比对出的输出指纹，随仓库分发。
- 改源码后重算底座：`selfref.py` 原生编译参照构件 → `relock.py` 封板比对（详见 BUILD.md）。
- `baseline_guard.py` 三道锁防底座被静默改写。

---

## 目录结构

```
cnijfilter2/
├── build/                        构建与打包
│   ├── build-all.sh / build-one.sh / make-deb.sh
│   ├── verify-net-print.sh       网络打印通路在线自测（需 root + 在线打印机）
│   ├── test-postinst-layout.sh   postinst 布局分支自测（需 sudo）
│   ├── docs/BUILD.md             ★ 编译指导
│   ├── patches/loong64-native.patch   相对原厂 6.90-1 的全部改动
│   └── pkg/{control,postinst,postrm}  deb 元数据
│
├── cncl/      CNCL API 库（原生实现） ├── cnnet/  网络传输库（原生实现）
│
├── lgmon3/ tocanonij/ tocnpwg/         ┐
├── cnijbe2/ rastertocanonij/           │ 原厂源码模块（6.90-1）
├── cmdtocanonij2/ cmdtocanonij3/       ┘
│
├── com/ini/cnnet.ini             原厂配置
├── ppd/                          179 个 PPD（官方 6.90 全量，GPL-2）+ NEWS
├── packages/                     ★ 构建产物唯一汇总处
├── tests/
│   ├── small.raster              测试输入（两层共用）
│   ├── params.py                 参数矩阵单一来源
│   ├── invariants.py             ★ 基准：自判定测试（零参照物）
│   ├── check-native-only.sh      ★ 自证：零异架构痕迹
│   └── sweep/                    ★ 底座：保真回归
│       ├── regress.py / mnt-regress.py    打印链路 66 组合 / 维护链路 3 作业
│       ├── selfref.py            自产参照（用仓库内官方源码原生编参照构件）
│       ├── relock.py             封板（读参照目录 → 逐字节比对 → 写底座）
│       ├── baseline_guard.py / selftest-guard.py   三道锁 / 守卫对抗自测
│       ├── BASELINE-LOG.md       底座变更台账（append-only）
│       └── baseline.tsv / baseline-mnt.tsv   底座
└── README.md
```

---

## 源码与资源版本

源码全量取自官方 6.90-1（与 PPD 同版，179 个 PPD 全量），仅叠加
`build/patches/loong64-native.patch` 的 8 处移植改动；PPD 可打印区均为自带，
无外部补充。

---

## 关键事实与坑

**网络链路**：发现 = SNMP v1（community `canon_admin`，Canon 企业号 1602）→
打印 = TCP 9100 裸口 → 状态 = CHMP over TCP 80（硬要求 `X-CHMP-Version: 1.0.0`；
**POST 只拿到 200 空 ack，必须再 GET 同一 URL 才有 chunked body**）。广播会被
conntrack 拦掉 → 退化为单播 ARP + /24 扫描。

**CNCL 语义**：`CNCL_GetInfoResponse` / `CNCL_GetStatus` 官方头写 `unsigned short`，
**实际返回完整 32 位负值** → 实现返回 `int`，否则 `err < 0` 判断失效；5 张字典表
**索引即返回值**。作业完成判定：jobinfo 消失后 `GetStatus2` 必须返回 **10**，
恒返 0 会让作业永不结束。

**构建要点**：老 autotools 树要手动跑序列（不要用 `./autogen.sh`）；gcc 10 起要
显式 `-fcommon`；三库用符号版本脚本 `.map` 限定导出前缀（否则各导 62 个
同名符号互相覆盖）；`libxml2` soname 分叉（`.so.2` / `.so.16`）用 dlopen 双探测。

---

## 许可与复核

源码均在 GPL-2 下分发：根目录 [`LICENSE`](LICENSE) 为 GPL-2.0 全文，上游各模块带
`COPYING`，PPD 文件头自带声明，我们新增/修改的代码为 GPL-2.0-or-later。
分层明细见 [`NOTICE.md`](NOTICE.md)。

仓库的可执行文件只有一类：用本仓库源码现场编译出来的 LoongArch 构件。
复核（期望只输出 `LoongArch`）：

```bash
find . -not -path './.git/*' -type f | while read -r f; do
    head -c4 "$f" 2>/dev/null | grep -q ELF && \
    readelf -h "$f" 2>/dev/null | sed -n 's/.*\(系统架构\|Machine\): *//p'
done | sort -u
```
