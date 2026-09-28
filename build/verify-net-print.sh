#!/bin/bash
# ==================================================================
#  verify-net-print.sh —— 网络打印通路实测（抓包取证）
#
#  再打一页并抓包，确认作业数据真的经 TCP 9100 到达打印机，
#  而不是只写在 CUPS 队列里。
#
#  ⚠️ 这是**在真实网络上跑的一次性诊断**：需要 root（tcpdump）、
#     一张在线打印机与正确的网卡名。它不属于可离线复现的回归——
#     可离线复现的回归见 tests/sweep/（那两条脚本零闭网依赖）。
#
#  用法：
#     QUEUE=G3010-net PRINTER_IP=192.168.1.84 IFACE=enp2s0 \
#       bash build/verify-net-print.sh
#
#  可覆盖的环境变量：
#     QUEUE         CUPS 队列名            （默认 G3010-net）
#     PRINTER_IP    打印机 IP              （默认 192.168.1.84）
#     IFACE         抓包网卡               （默认 enp2s0）
#     SRC_IP        本机发往打印机的源 IP  （默认从路由表推断）
#     TESTPAGE      要打印的文本页文件     （默认自动生成一页）
#     OUTDIR        抓包/日志输出目录      （默认 build/logs/，已被 .gitignore 忽略）
#
#  非交互环境（无 TTY）需要免密 sudo；本仓库**不内置任何口令**。
#  如需从管道喂口令，请自行 export CNIJ_SUDO_PASS=<口令>，
#  例如：CNIJ_SUDO_PASS=xxx QUEUE=... bash build/verify-net-print.sh
# ==================================================================
set -u
cd "$(dirname "$0")/.." || exit 1        # 项目根

QUEUE="${QUEUE:-G3010-net}"
PRINTER_IP="${PRINTER_IP:-192.168.1.84}"
IFACE="${IFACE:-enp2s0}"
OUTDIR="${OUTDIR:-$PWD/build/logs}"

mkdir -p "$OUTDIR"
PKT="$OUTDIR/net-print-verify.pcap"
LOG="$OUTDIR/tcpdump-net-print.log"

# 提权封装：默认交给 sudo 自己提示口令；只有显式给了 CNIJ_SUDO_PASS 才走 stdin。
sudox() {
    if [ -n "${CNIJ_SUDO_PASS:-}" ]; then
        printf '%s\n' "$CNIJ_SUDO_PASS" | sudo -S -p '' "$@"
    else
        sudo "$@"
    fi
}

# 源 IP：用于在抓包里区分「本机 → 打印机 9100」的载荷
SRC_IP="${SRC_IP:-$(ip -4 route get "$PRINTER_IP" 2>/dev/null \
    | sed -n 's/.* src \([0-9][0-9.]*\).*/\1/p' | head -1)}"
[ -n "$SRC_IP" ] || SRC_IP="<未知>"

TESTPAGE="${TESTPAGE:-$OUTDIR/testpage-net.txt}"
if [ ! -f "$TESTPAGE" ]; then
    printf 'cnijfilter2 loong64 网络打印通路自检页\n%s\n' "$(date '+%F %T')" > "$TESTPAGE"
fi

echo "队列=$QUEUE  打印机=$PRINTER_IP  网卡=$IFACE  源IP=$SRC_IP"
echo "自检页=$TESTPAGE  抓包=$PKT"
echo

sudox rm -f "$PKT" 2>/dev/null
sudox timeout 45 tcpdump -i "$IFACE" -n -s 100 -w "$PKT" \
      "host $PRINTER_IP and tcp port 9100" > "$LOG" 2>&1 &
TPID=$!
sleep 2

echo "=== 打印 ==="
lp -d "$QUEUE" "$TESTPAGE" 2>&1
sleep 12

wait "$TPID" 2>/dev/null
sudox chown "$(id -un):$(id -gn)" "$PKT" 2>/dev/null

echo
echo "=== 9100 方向的报文（尾部） ==="
sudox tcpdump -r "$PKT" -n -q 2>/dev/null | tail -8
echo
echo "=== 发往打印机 9100 的报文条数 ==="
sudox tcpdump -r "$PKT" -n 2>/dev/null | grep -c "$SRC_IP.*>.*$PRINTER_IP\.9100"
echo "=== page_log ==="
sudox sh -c 'tail -4 /var/log/cups/page_log 2>/dev/null'
echo "=== 作业历史 ==="
lpstat -W completed -o 2>&1 | tail -4
