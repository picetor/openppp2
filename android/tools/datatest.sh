#!/system/bin/sh
# datatest.sh - 抓取 DATAPLANE 日志 + tun0 计数
echo "=== tun0 stats before ==="
cat /sys/class/net/tun0/statistics/rx_bytes 2>/dev/null
cat /sys/class/net/tun0/statistics/tx_bytes 2>/dev/null
cat /sys/class/net/tun0/statistics/rx_packets 2>/dev/null
cat /sys/class/net/tun0/statistics/tx_packets 2>/dev/null
echo "=== curl TCP test ==="
curl -v -m 8 -o /dev/null http://61.244.242.112/ 2>&1 | tail -20
echo "curl_exit=$?"
echo "=== tun0 stats after ==="
cat /sys/class/net/tun0/statistics/rx_bytes 2>/dev/null
cat /sys/class/net/tun0/statistics/tx_bytes 2>/dev/null
cat /sys/class/net/tun0/statistics/rx_packets 2>/dev/null
cat /sys/class/net/tun0/statistics/tx_packets 2>/dev/null
echo "=== done ==="
