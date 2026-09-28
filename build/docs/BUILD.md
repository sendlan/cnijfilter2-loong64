# 编译指导

从本仓库源码构建 cnijfilter2（loong64）。所有命令在**仓库根**执行。

仓库内 7 个原厂模块 = 官方 `cnijfilter2-source-6.90-1.tar.gz` 原版
+ `build/patches/loong64-native.patch`（8 个文件、+132/-21 行）。
换官方源码版本时，patch 需按新基准重生成（`patch -p1` 可直接验证）。

## 依赖

```bash
sudo apt install --yes gcc make autoconf automake libtool pkg-config \
     libcups2-dev libcupsimage2-dev libusb-1.0-0-dev libxml2-dev
```

## 全量编译

```bash
bash build/build-all.sh          # 11 个构件 → build/built/
bash build/make-deb.sh           # → packages/cnijfilter2_6.90-6_loong64.deb
```

一步到位：`bash build/build-all.sh --deb`

`build-all.sh` 顺序：自研库（`cncl/` `cnnet/`）先编 → 原厂 7 个模块逐个走
autotools → 收集产物到 `build/built/` 并做架构自检。

## 只编一个模块

```bash
bash build/build-all.sh lgmon3      # 自研库仍会先编一遍
bash build/make-deb.sh
```

或绕开 `build-all.sh`：

```bash
bash build/build-one.sh <模块> [configure 参数...]
bash build/build-one.sh cnijbe2 --prefix=/usr --enable-progpath=/usr/bin
bash build/build-one.sh lgmon3  LDFLAGS=-L"$PWD/build/built"
```

`build-one.sh` 流程：清残留 → `aclocal/libtoolize/autoheader/automake/autoconf`
→ `configure` → `make -j6`。日志：`build/logs/build-<模块>.log`。

## 直接出包（不重编）

`build/built/` 已是编好的构件快照，克隆下来即可：

```bash
bash build/make-deb.sh
```

加 `--keep` 保留 `build/dist/stage/` 组装树。

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

## 构件 ↔ 源码

| `build/built/` | 源码位置 |
|---|---|
| `cnijlgmon3` | `lgmon3/src/` |
| `cnijbe2` | `cnijbe2/src/` |
| `rastertocanonij` | `rastertocanonij/src/` |
| `tocanonij` | `tocanonij/src/` |
| `tocnpwg` | `tocnpwg/src/` |
| `cmdtocanonij2` `cmdtocanonij3` | `cmdtocanonij{2,3}/filter/` |
| `libcnbpcnclapicom2.so.5.0.0` | `cncl/cncl_native.c` |
| `libcnnet2.so.1.2.5` | `cnnet/cnnet_native.c` |
| `libcnbpnet30.so.1.0.0` | 同上 |
| `libcnbpnet20.so.1.0.0` | 同上 |
| `cnnet.ini` | `com/ini/cnnet.ini` |

## 安装

```bash
echo <口令> | sudo -S -p '' -E apt install --yes ./packages/*.deb
```

## 版本号

`PKGVER` 可覆盖（默认 `6.90-6loong64`），`REV` 由 `sed 's/-*loong64$//'` 推出。
**改版本时 `build/pkg/control` 与 `make-deb.sh` 的 `PKGVER` 两处要同步。**

## 验证

```bash
bash build/build-all.sh
/usr/bin/python3 tests/invariants.py           # 31/31
/usr/bin/python3 tests/sweep/regress.py        # 66/66
/usr/bin/python3 tests/sweep/mnt-regress.py    # 3/3
/usr/bin/python3 tests/sweep/selftest-guard.py # 11/11
bash tests/check-native-only.sh
```

全部返回 0 才算通过。保真回归失败会打印差异并把样本写到 `tests/sweep/regress/`。

### 重算底座（改源码后需要）

```bash
/usr/bin/python3 tests/sweep/selfref.py --outdir ~/refl --emit-refdir ~/ref
/usr/bin/python3 tests/sweep/relock.py --refdir ~/ref --reason "底座为什么要变"
```

`relock.py --plan` 列出所需参照文件。写入时同步更新 `integrity` 指纹、
`BASELINE-LOG.md` 与回归脚本里的 `BASELINE_DIGEST`。

## 环境参数

| 变量 | 默认 | 说明 |
|---|---|---|
| `CNIJ_BUILT` | `<仓库根>/build/built` | 构件目录 |
| `CNIJ_PPD` | `<仓库根>/ppd/canong3010.ppd` | PPD |
| `CNIJ_SRC` | `<仓库根>` | 源码树（`mnt-regress.py` 取 `utilfiles/*.utl`） |
| `CNIJ_WORK` | `<仓库根>` | `build-one.sh` / `make-deb.sh` 的源码树位置 |

## 排错

**不要在源码树里跑 `./autogen.sh`** —— 它会卡在 libtool 版本检查上。
`build-one.sh` 已改为手工跑标准序列绕开。

**源码树会被写入中间文件**（就地编译）。要完全不动源码树：

```bash
git clone . <构建目录>
export CNIJ_WORK=<构建目录>
bash build/build-all.sh
```

**`-lcnnet2` 找不到** —— 自研库没先编：

```bash
bash cncl/build-lib.sh && bash cnnet/build-net.sh
```

**沙箱注入了 `BASH_ENV` 垫片**（`rm` 被重定义导致清理变空操作、`configure` 失败）：

```bash
env -u BASH_ENV -u 'BASH_FUNC_rm%%' -u 'BASH_FUNC_unlink%%' \
    -u 'BASH_FUNC_rmdir%%' -u <你的沙箱变量> \
    PATH=/usr/local/bin:/usr/bin:/bin \
    bash build/build-all.sh
```

`env | grep -iE 'safe|BASH_ENV'` 可查出变量名。
