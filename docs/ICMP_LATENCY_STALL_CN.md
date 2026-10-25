# ICMP/隧道延迟突变：5 秒探测轮次的整表读取（2026-10-25 实测）

> 现场：`ppp-web.exe`（工作区构建），客户端 `--mode=client --tun-host=yes --tun-mux=0`，
> 出口 HKBN `[2400:c620:2e:8dfe::1]:40675`（ppp+tcp，static echo 可用），物理网卡
> `vEthernet (Debian)`，MTU 1400，`client.bandwidth=0`（QoS 闸门不生效），日志 debug。
> 现象：`ping 1.1.1.1` 每几秒出现一次 300–2500 ms 尖峰，下载时也能看到掉速；同一时刻
> 直连服务器 ping 平稳。

## 1. 结论

根因在客户端：**每 5 秒一轮的出口探测（`RefreshOutboundProbes`）会对全部 23 个候选
逐个调用 `EnsureWindowsIPv4/6ServerRoute`，而该函数每次都在数据面 io_context 线程上、
并且持有 `syncobj_` 的情况下重新读取并排序整张 Windows 路由表**
（`GetIpForwardTable(sort=true)` / `GetIpForwardTable2`，单次约 12–50 ms）。

一轮 23 个候选 ≈ 505–801 ms，且**全程占用唯一的数据面线程**，所以：

* 落在窗口里的 ICMP echo 回包只能排队 → 300–2500 ms 的 RTT 尖峰；
* 同一段时间隧道吞吐出现 0.6–0.9 s 的空洞 → 用户看到「掉速」；
* 5 秒周期与用户描述的「好几秒一次」完全一致。

## 2. 诊断是怎么做出来的（本次新增，保留）

### 2.1 快照字段 `dataplane.icmp`（每个 echo 一条样本，环形保留最近 256 条）

ICMP echo 被拆成四段计时，按 `(目的地址, id, seq)` 匹配请求与回包：

| 字段 | 含义 |
|---|---|
| `pre_tx_*` | TUN 收到请求 → 交给出口路径（static echo / 主隧道） |
| `net_*` | 交给出口路径 → 回包到达客户端（含回包在 io_context 里等待处理的时间） |
| `inject_*` | 回包到达 → 写入隧道网卡 |
| `rtt_*` | 请求进 → 回包注入（主机看到的 ping 时间减去宿主栈） |
| `path_static/path_main/path_local` | 走的是 UDP 静态回显通道 / 主 TCP 隧道 / 本地应答 |

日志（debug）：

```text
DATAPLANE ICMP: echo latency, rtt=35ms pre_tx=0ms net=35ms inject=0ms path=static_echo id=1 seq=10 t=...
DATAPLANE ICMP: echo latency spike, rtt=539ms pre_tx=0ms net=539ms inject=0ms path=static_echo id=1 seq=9 t=...
```

`t=` 是 `Executors::GetTickCount()` 毫秒时间戳，用于和下面两类日志对齐。

### 2.2 快照字段 `dataplane.probe`（每轮探测的成本）

```json
{"rounds":30,"pin_calls":689,"pin_last_ms":505,"pin_max_ms":801,
 "pin_slowest_ms":36,"round_max_ms":4814}
```

`pin_last_ms` 就是一轮里「钉路由」这段同步阻塞耗时；`round_*` 还包含候选域名解析的
协程挂起时间，因此不能用它衡量阻塞。

日志（>=50 ms 时以 INFO 打印，便于不开 debug 也能看到）：

```text
VEthernetNetworkSwitcher::RefreshOutboundProbes: route pin round took 589ms for 23 candidate(s) (slowest 47ms, round=589ms), t=...
```

### 2.3 快照字段 `dataplane.tick`

1 秒 tick 在数据面 io_context 上跑；比 1 s 周期晚到即说明循环被阻塞（`late_ticks`、
`last_late_ms`、`max_late_ms`）。修复前一轮内出现 826–991 ms 的迟到。

## 3. 修复

`EnsureWindowsIPv4/6ServerRoute` 不再每次调用都读表：

* 路由表行缓存在 `ipv4_route_rows_` / `ipv6_route_rows_`，TTL 30 s；
* 本客户端增删路由时（`Router::Add`、`RemoveWindowsIPv4/6ServerRoutes`、
  `AddIPv6RouteEntry` 后）立即置零失效；
* 读表失败时返回 `-1`（未知），本轮不再盲加路由，1 秒后重试，避免 23 次重复读表；
* 返回 `1/0/-1` 的语义写进 `IPv4ServerRouteInstalled` / `IPv6ServerRouteInstalled`。

稳态下一轮只剩 23 次内存扫描（0 ms），每 30 秒一次缓存刷新（12–25 ms）。

## 4. 实测对照（同一出口、同一目标 `1.1.1.1`，各 100 包）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 丢包 | 10/100 | 2/100 |
| p50 | 50 ms | 37 ms |
| p95 | 935 ms | 325 ms |
| 最大 | 2454 ms | 946 ms |
| 平均 | 219 ms | 78 ms |
| ≥300 ms 包数 | 20 | 6 |
| `pre_tx_max` | 563 ms | 0 ms |
| 探测 pin 一轮 | 505–801 ms（每 5 s） | 0 ms（每 30 s 一次 12–25 ms） |
| tick 迟到 | 826–991 ms | 仅启动期 2 次 |

## 5. 残余尖峰（仍待处理）

修复后仍有约 6% 的包 ≥300 ms，特征：

* 全部落在 `net` 段（`pre_tx=0`、`inject=0`）；
* 同一时段直连服务器 `ping 2400:c620:2e:8dfe::1`：p50 15 ms、max 227 ms、0 丢包；
* 隧道网关 `ping 192.168.12.1`（服务端 echo-ack，不经过远端主机）：p50 16 ms、
  p95 329 ms、max 1091 ms —— 与 `1.1.1.1` 的分布几乎相同。

即残余尖峰发生在「客户端 ↔ 服务端」这段的用户态处理上（服务端静态回显接收/查表/回发，
服务端为共享节点），与远端 ICMP 处理无关，也不是本机链路。下一步需要在服务端侧计时，
或在客户端加一个独立的 io_context 延迟探针把「本机阻塞」与「对端慢」彻底分开。

另：快照构建线程每 1 秒重建一次完整快照（注释记录单次 250–550 ms，其中 WMI 适配器枚举
30 s 一次、300–400 ms）。它不在数据面线程，但持续占用一个核，值得后续降到按需构建。
