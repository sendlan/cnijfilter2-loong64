#!/bin/bash
# 验证 postinst 的「非标准 CUPS 布局」分支：
# 造一个假的 /usr/lib64/cups + 一个返回它的 cups-config 桩，
# 重跑 configure，看链接是否落到那边；测完清理干净。
set -u
FAKE=/usr/lib64/cups
STUB=/usr/local/bin/cups-config
PI=/var/lib/dpkg/info/cnijfilter2.postinst
LINKS=/var/lib/cnijfilter2/extra-links

say() { echo "---- $* ----"; }

say "1) 造场景：$FAKE + cups-config 桩"
sudo mkdir -p "$FAKE/filter" "$FAKE/backend"
sudo tee "$STUB" >/dev/null <<'EOF'
#!/bin/sh
case "$1" in
  --serverbin) echo /usr/lib64/cups ;;
  --datadir)   echo /usr/share/cups ;;
  *) exit 1 ;;
esac
EOF
sudo chmod 755 "$STUB"
echo "cups-config 桩: $(command -v cups-config) -> $(cups-config --serverbin)"

say "2) 重跑 postinst configure"
sudo "$PI" configure

say "3) 检查 $FAKE 下是否建好链接"
ls -la "$FAKE/filter/" "$FAKE/backend/" 2>/dev/null

say "4) 链接清单 $LINKS"
cat "$LINKS" 2>/dev/null

say "5) 通过链接跑一次 filter（验证可用）"
"$FAKE/filter/rastertocanonij" 2>&1 | head -2 || true

say "6) 清理：删桩 + 跑 postrm 删链接"
sudo rm -f "$STUB"
sudo /var/lib/dpkg/info/cnijfilter2.postrm remove
rmdir "$FAKE/filter" "$FAKE/backend" "$FAKE" 2>/dev/null && echo "  假目录已删" || echo "  假目录非空，检查中"

say "7) 复位：重跑 postinst（应回到 /usr/lib/cups 判定）"
sudo "$PI" configure

say "8) 最终校验"
echo -n "cups-config 还在吗: "; command -v cups-config || echo "已移除 ✓"
echo -n "$FAKE 还在吗:    "; [ -d "$FAKE" ] && echo "仍在（异常）" || echo "已清理 ✓"
echo -n "规范 filter 在位:  "; [ -x /usr/lib/cups/filter/rastertocanonij ] && echo "✓" || echo "✗"
echo -n "规范 backend 权限: "; stat -c "%a %n" /usr/lib/cups/backend/cnijbe2
echo -n "私有库可被找到:    "; ldconfig -p | grep -c libcnbpnet30
