# 编译指导

从本仓库源码构建 cnijfilter2（loong64）。所有命令在**仓库根**执行。

原厂 7 个模块 = 官方 6.90-1 源码 + `build/patches/loong64-native.patch`。
换官方源码版本时 patch 需按新基准重生成。

## 依赖

```bash
sudo apt install --yes gcc make autoconf automake libtool pkg-config \
     libcups2-dev libcupsimage2-dev libusb-1.0-0-dev libxml2-dev
```

## 全量编译

```bash
bash build/build-all.sh --deb   # 11 个构件 → build/built/，出包 → packages/*.deb
```

## 单模块

```bash
bash build/build-one.sh <模块> [configure 参数...]
bash build/build-one.sh cnijbe2 --prefix=/usr --enable-progpath=/usr/bin
bash build/build-one.sh lgmon3  LDFLAGS=-L"$PWD/build/built"
```

日志：`build/logs/build-<模块>.log`。

## 出包

```bash
bash build/make-deb.sh                          # 从 build/built/ 组装
CNIJ_BUILT=/path/to/已有构件 bash build/make-deb.sh   # 只改打包脚本时跳过重编
```

加 `--keep` 保留 `build/dist/stage/`。`build/built/` 与 `packages/` 不入库，克隆后需先编一次。

## 编译参数

| 模块 | configure 参数 | 链接的库 |
|---|---|---|
| `cncl` | 不用，直接 gcc | 产出 `libcnbpcnclapicom2.so.5.0.0` |
| `cnnet` | 不用，直接 gcc ×3 | 产出 `libcnnet2` / `libcnbpnet30` / `libcnbpnet20` |
| `lgmon3` | `LDFLAGS=-L build/built` | `-ldl -lusb-1.0 -lcnnet2 -lcnbpcnclapicom2 -lcnbpnet30` |
| `cmdtocanonij2` | `LDFLAGS=-L build/built` | `-lcnbpcnclapicom2` |
| `cmdtocanonij3` | `LDFLAGS=-L build/built` | `-lcnbpcnclapicom2` |
| `cnijbe2` | `--prefix=/usr --enable-progpath=/usr/bin` | `-lcups` |
| `rastertocanonij` | `--prefix=/usr --enable-progpath=/usr/bin` | `-lcups` |
| `tocanonij` | `--prefix=/usr --enable-progpath=/usr/bin` | `-ldl` |
| `tocnpwg` | `--prefix=/usr --enable-progpath=/usr/bin` | `-lcups -lcupsimage -lxml2` |

## 安装

```bash
echo <口令> | sudo -S -p '' -E apt install --yes ./packages/*.deb
```

## 版本号与架构

`PKGVER` 默认 `6.90-6loong64`，可覆盖；改版本时 `build/pkg/control` 与 `make-deb.sh` 两处同步。

`Architecture: all` —— `loong64`（Debian/Loongnix）与 `loongarch64`（AOSC）
同一套二进制，一个包两边装；`build/pkg/preinst` 在解包前校验非 LoongArch 主机拒绝。
要架构专包：

```bash
ARCHS="loong64 loongarch64" bash build/make-deb.sh
ARCH=loong64                bash build/make-deb.sh
DEPS="cups, libcups2, libc6" bash build/make-deb.sh   # 覆盖 Depends
```

## 验证

```bash
/usr/bin/python3 tests/invariants.py           # 31/31
/usr/bin/python3 tests/sweep/regress.py        # 66/66
/usr/bin/python3 tests/sweep/mnt-regress.py    # 3/3
/usr/bin/python3 tests/sweep/selftest-guard.py # 11/11
bash tests/check-native-only.sh
```

全部返回 0 才算通过。改源码后需重算底座：

```bash
/usr/bin/python3 tests/sweep/selfref.py --outdir ~/refl --emit-refdir ~/ref
/usr/bin/python3 tests/sweep/relock.py --refdir ~/ref --reason "理由"
```

## 环境参数

| 变量 | 默认 | 说明 |
|---|---|---|
| `CNIJ_BUILT` | `<仓库根>/build/built` | 构件目录 |
| `CNIJ_PPD` | `<仓库根>/ppd/canong3010.ppd` | PPD |
| `CNIJ_SRC` | `<仓库根>` | 源码树 |
| `CNIJ_WORK` | `<仓库根>` | `build-one.sh` / `make-deb.sh` 的工作源码树 |

## 排错

- 不要在源码树跑 `./autogen.sh`，走 `build-one.sh`。
- `-lcnnet2` 找不到：`bash cncl/build-lib.sh && bash cnnet/build-net.sh`。
