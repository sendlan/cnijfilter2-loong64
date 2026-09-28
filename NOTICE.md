# 授权分层

本仓库整体以 **GPL-2.0** 授权分发：根目录 [`LICENSE`](LICENSE) 是 GPL-2.0 全文
（与 gnu.org `gpl-2.0.txt` 逐字节一致，sha256
`edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6`）。
仓内内容分三层，各层条款如下。

| 层 | 范围 | 授权 |
|---|---|---|
| 上游源码 | 7 个原厂模块：`lgmon3/` `tocanonij/` `tocnpwg/` `cnijbe2/` `rastertocanonij/` `cmdtocanonij2/` `cmdtocanonij3/` | GPL-2，各模块自带 `COPYING` 全文；`lgmon3/` `cnijbe2/` 另有 Canon `LICENSE.canon.txt` |
| 资源 | `ppd/` 下 179 个 PPD、`com/ini/cnnet.ini` | GPL-2，PPD 文件头逐个声明 "GNU General Public License" |
| 本仓库新增与改动 | `cncl/` `cnnet/`、`build/`、`tests/`、根 `README.md` | **GPL-2.0-or-later** |

## 上游的二进制模块链接例外

`cmdtocanonij2/LICENSE` `cmdtocanonij3/LICENSE` 在 GPL-2 之上附加一条例外：允许
链接原厂以二进制形式发布的模块。上游据此链接闭源的 `libcnbpcnclapicom2` 等库；
本仓库以 `cncl/` `cnnet/` 下的自研实现（源码在仓内、GPL-2.0-or-later）替代，
例外的适用范围随之由 GPL-2.0-or-later 代码承接。

## 逐层复核

```bash
# 7 个上游 COPYING，全为 GPL-2
grep -l 'GNU GENERAL PUBLIC LICENSE' */COPYING | wc -l

# 179 个 PPD 全部自带 GPL-2 声明
grep -l -i 'General Public License' ppd/*.ppd | wc -l

# 根 LICENSE 与 gnu.org 原文一致
sha256sum LICENSE
```

## 分发二进制时

`packages/*.deb` 与 `build/built/` 下的构件是本仓库源码编译所得。依 GPL-2 §3
(a)，随二进制分发时须附完整对应源码；本仓库本身即该源码，在二进制旁注明本仓库
地址即可。
