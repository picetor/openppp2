# TUN 模式遗留问题复查（2026-10-11 现场）

> 现场：Windows 11 家庭版 26200，`C:\Users\2233\Desktop\TUI`，客户端 `ppp-web.exe`
> （CI 产物，源码行号与工作区 `653b785` 一致，二进制构建于 `31b2b81` 前后，即**不含**
> `653b785` 的审计重试）。
> 配置：`--config=./config/ZGO.json`（`ppp://23.166.168.33:20000`），`--server-dir=./config`
> （因此 10 个配置都成为可探测出口），`--tun-mux=0`，`bypass_mode=ip`，TUN 适配器 `PPP`
> `192.168.14.25/24`、网关 `192.168.14.1`、ifIndex 71、网卡 metric 5，物理网卡 `WLAN`
> `192.168.51.101`、metric 45。
> 取证方式：现场日志 `ppp-core-debug.log`（截至 00:24:12 已 5.61 MB / 52 622 行，
> 00:00:51 起算 ≈23.4 分钟，即 **≈240 KB/分钟、≈14 MB/小时**，debug 级）、
> 本机 RPC `127.0.0.1:39100` 的 `get_snapshot/get_health/get_outbounds`（只读）、
> `netsh` 地址/路由/接口表、以及从宿主发起的 TCP 连接实测。

## 0. 结论速览

| # | 问题 | 用户可见影响 | 证据强度 |
|---|---|---|---|
| P1 | IPv6 数据面不存在（`::/1`+`8000::/1` 黑洞），但仍把 AAAA 交给应用 | 应用/浏览器偶发数秒卡顿 | 强（路由表 + 快照 + 832 条日志） |
| P2 | 无 IPv6 出口的 IPv6 服务器条目每 5 秒打一条 `ERROR` | 23 分钟 5.6 MB 日志，真实错误被淹没 | 强（288 条 ERROR，节拍=探测周期） |
| P3 | 错误文本以 GBK 写入 UTF-8 日志 | 日志出现乱码、整份日志对 UTF-8 工具不可读 | 强（字节级证据） |
| P4 | 服务器页把**正在工作的活动服务器**显示为「不可达」，其余条目 RTT 失真 | 面板误导，热切换排序依据可疑 | 强（快照 + 面板代码 + 实测对照） |
| P5 | 注入探针结论没有进 RPC/面板 | TUN 交付故障只能靠翻日志发现 | 强（`injection_probe` 在 `main.cpp` 无任何引用） |
| P6 | `--tun-mux=0` 使 mux 实际关闭，每条 TCP 流单开一条传输连接 | 每流一次握手、服务端连接/TIME_WAIT 膨胀、吞吐下降 | 强（快照 + 637 次连接/601 次握手） |
| P7 | TUN MTU=1500、MSS 1460，而传输层是 TCP 封装 | 大包路径余量为负，偶发卡顿/重传 | 强（启动日志 + 配置） |
| P8 | 带 IPv4 选项（IHL>5）的报文被静默丢弃，且不写原因 | 少量报文无声丢失，无法定位 | 中（411 条 `unknown packet`，均为 0x46） |
| P9 | `geoip.dat`/`geosite.dat` 缺失 → 每个包都记 `geo_rules_unavailable` | 每包一条 DEBUG（≈100 条/分钟） | 强（目录与 `routes` 字段对照） |
| P10 | WFP 丢包诊断每 120 秒重试并失败（审计子类别打不开） | 每 2 分钟一条 WARN，永不成功 | 强（12 条 WARN，间隔精确 120 s） |
| P11 | 5 张陈旧隧道网卡残留：静态默认网关 + 静态 DNS + IPv6 路由 | 现场持续污染，是排障噪声源与潜在陷阱 | 强（`netsh` 三张表） |
| P12 | `static_echo` 响应超时 11/21（>50%） | 与「偶发卡顿」同源的可疑点 | 中（计数语义需确认） |

**未被本机证据支持、因此可以排除的方向**（对上一份文档第 3 节未解问题的收敛，见第 3 节）：
重复地址/重复子网、强主机模型（strong host）、Wintun 地址未就绪（DAD）、Hyper-V/WSL 的
WinNAT/FSE 干扰。

## 0.1 本轮已实施的代码修复（待 CI 构建验证）

| 问题 | 改动 | 位置 |
|---|---|---|
| P1 | 新增 `ipv6_dataplane_unavailable_` + `ShouldPreferIPv4()`：**没有可用 IPv6 数据面时就剥离 AAAA**，不再依赖 `udp.dns.prefer_ipv4`；另按「直连名字看宿主 IPv6 出口」补齐（见 P1 补记） | `VEthernetNetworkSwitcher.h/.cpp`（构造函数、`ApplyIPv6Assignment`、`StripAAAADnsResponseIfIPv4Available`、`DispatchLocalDnsQuery`、`Open`） |
| P2 | 探测刷新时，若本机没有物理 IPv6 网关就**跳过 IPv6 候选**（只记一次 INFO），不再每 5 秒一条 ERROR；IPv4 pin 失败也会记一条 DEBUG | `RefreshOutboundProbes` |
| P3 | 新增 `FormatWin32ErrorText()`（`FormatMessageW` + `FORMAT_MESSAGE_ALLOCATE_BUFFER` + UTF-8），替换 6 处 `ec.message()`，并同时打印数值错误码 | `VEthernetNetworkSwitcher.cpp` |
| P4 | 已建立连接时，后台探测失败不再把该出口显示为不可达；`ExchangeToEstablishState` 记 `probe_reachable_=true`；探测失败补日志（protect 失败 / connect 错误码 / 耗时）；RTT<0 时各界面显示「可达/ok」而不是「-1 ms」 | `GetOutboundStatuses`、`VEthernetExchanger.cpp`、`ConnectivityProbe.cpp`、`web/app.js`、`tui/src/*` |
| P5 | 注入探针结论进入 `get_snapshot.dataplane.injection`（sent/received/running/healthy/reported/fallback_proxy），面板「网络」页新增「TUN 注入交付」一行 | `main.cpp`、`web/app.js` |
| P9 | 「geo 规则不可用」只报一次（原本每包一条，≈100 条/分钟） | `GetExchanger` |
| P10 | 丢包诊断订阅失败后本会话不再重试（原本每 120 秒一条 WARN） | `RunTunInjectionSelfTest` |

仍未实施：P6（`tun_mux` 是配置决策）、P7（改 JSON 即可）、P8（需要确定那批 0x46 报文来自谁）、
P11（需要管理员执行清理脚本）、P12（需要先确认 `static_echo` 计数语义），以及第 3 节的根因排查。

---

## P1 IPv6 黑洞路由 + AAAA 未剥离（最值得先修的用户可见问题）

**事实**

- 活动服务器 ZGO 的配置里**没有** `server.ipv6`（`config/ZGO.json` 只有 `server.log/node/subnet/mapping`），
  快照 `network.logical_ipv6` 为空 → 服务端**没有**下发 IPv6 数据面。
- 客户端因此只装黑洞路由（防泄漏）：
  ```
  netsh interface ipv6 show route
    ::/1      metric 1  ifIndex 71  PPP
    8000::/1  metric 1  ifIndex 71  PPP
  ```
  同时 `WLAN` 只有 `fe80::/64`（本机根本没有 IPv6 默认路由/网关）。
- 但 DNS 仍把 AAAA 交给应用：`udp.dns.prefer_ipv4` 默认为 `false`（`AppConfiguration.h:96`），
  ZGO.json 未设置该项；`netsh interface ipv4 show dnsservers` 显示 `PPP` = `127.0.0.1, 192.168.14.1`，
  两条路径都会返回 AAAA。
- 结果在日志里非常直白（截至 00:24:12 共 832 条，≈36 条/分钟）：
  ```
  VEthernetNetworkSwitcher::GetExchanger: destination=2606:4700:57:406a:8fdb:726:3e79:9d05, ... reason=geo_rules_unavailable
  VEthernetExchanger::TranslateIPv6Packet: outbound=main has no usable IPv6 assignment
  ```
  （`2606:4700::/32` 是 Cloudflare，`ff02::16`/`ff02::fb` 是链路本地组播。）

**影响**：任何不实现 Happy Eyeballs 的程序会先连 IPv6（被黑洞吞掉）并等到自身超时；
上一份文档 6.1 已实测 `curl -6 http://[2606:4700:4700::1111]/` 挂 12 秒。写这条的此刻，
日志里仍有应用在反复尝试 `2606:4700:...`。

**修法（已实施，含 01:33 现场的补记）**

1. `ipv6_dataplane_unavailable_` + `ShouldPreferIPv4()`：没有隧道 IPv6 数据面时（或在
   `ApplyIPv6Assignment` 里申请失败时）就剥离 AAAA，不再依赖用户开关。
2. **（补记）直接解析的名字也要看宿主自己能不能走 IPv6**：01:33 现场实测发现
   `Resolve-DnsName -Type AAAA -Server 127.0.0.1 www.baidu.com` 仍然返回 2 条 AAAA、日志里
   `STRIPPED` 为 0 条——因为 baidu 属于**直连（bypass）策略**（日志
   `DispatchLocalDnsQuery: completed host=www.baidu.com, direct=1`，
   `transport=direct-udp`），而原来的剥离只对 `!direct_query` 生效。可这台机器的 WLAN 只有
   `fe80::/64`、没有全局 IPv6 地址也没有 IPv6 默认路由，**任何** AAAA 都是死路。
   因此新增 `host_ipv6_uplink_`（在 `Open` 选中物理网卡时按
   `underlying_ni_->IPv6GatewayServer` 计算），并按路径分别判断：
   - 隧道路径：按隧道 IPv6 数据面判断（不变）；
   - 直连路径：宿主有 IPv6 出口就保留 AAAA，没有就剥离；
   - 两条路径都仍受用户显式 `prefer_ipv4` 影响。
   同时给 `StripAAAADnsResponseIfIPv4Available` 增加 `strip_without_a_cache`：
   回环应答路径无法像隧道路径那样「先扣住 AAAA 等 A 记录」，所以在这台完全不能走 IPv6 的机器上
   直接剥离（否则 A 记录还没进缓存时 AAAA 又会漏出去）。
   相关位置：`StripAAAADnsResponseIfIPv4Available` / `DispatchLocalDnsQuery`
   （`ppp/app/client/VEthernetNetworkSwitcher.cpp`）。
3. 注意：进程内 vDNS 缓存与 Windows DNS 客户端缓存里已有的 AAAA 不会因此消失，
   验证时要重启核心（或查一个没缓存过的域名）。

## P2 无 IPv6 出口的 IPv6 服务器条目 → 每 5 秒一条 ERROR

**事实**

- `--server-dir=./config` 让 10 个配置都参与 5 秒一轮的后台探测
  （`RefreshOutboundProbes`，`VEthernetNetworkSwitcher.cpp:427-767`）。
- 每轮开始处会为所有候选逐个 pin 物理路由（同文件 `646-660`），其中 `HKBN`
  的服务器是 IPv6 字面量 `2400:c620:2e:8dfe::1`，于是每轮命中：
  ```
  VEthernetNetworkSwitcher::EnsureWindowsIPv6ServerRoute: physical IPv6 gateway unavailable, remote=2400:c620:2e:8dfe::1
  ```
  共 288 条（截至 00:24:12），≈12.3 条/分钟 = **正好 1 条/探测周期**，且永久如此：
  本机确实没有 IPv6 网关，这个候选永远不可能可用。
- 探测结果的去向：`outbounds[server:hkbn].probe_reachable=false / rtt=-1`，没有任何
  「不可路由」的降级或退避，UI 上也永远是一次「不可达」。

**修法**

- 把「物理网卡没有可用 IPv6 网关」视为**正常跳过**：该地址记一次 DEBUG/INFO，
  仅当网卡（ifIndex/Luid）或地址变化时再记；不要每轮 ERROR。
- 既然 `EnsureWindowsIPv6ServerRoute` 的返回值在 `646-660` 被丢弃，探测也应按返回值
  把该候选标记为「不可路由」并跳过 socket 尝试（现在还会白跑一次 connect）。
- 顺带：日志里 `DoMuxEvents: mux disabled by switcher` 与 `no mux active, cleaning up`
  各 1249 条（≈53 条/分钟，即每 2 秒一对）也是同一量级的噪声，建议降级或去重。

## P3 错误文本以本地代码页（GBK）写入 UTF-8 日志

**事实**：字节级证据（`ppp-core-debug.log`）：

```
TUN injection probe: cannot bind a UDP probe socket on the tunnel address, error=
D4 DA C6 E4 C9 CF CF C2 CE C4 D6 D0 A3 AC B8 C3 C7 EB C7 F3 B5 C4 B5 D8 D6 B7 CE DE D0 A7 A1 A3
```

即「在其上下文中，该请求的地址无效。」的 **GBK** 字节写进了 UTF-8 日志。同一条日志在
grep 里被标记为 `(line is not valid UTF-8)`，本仓库的文本读取工具直接拒绝读该文件。

**影响**：面板/日志分析工具、CI 日志扫描（如 `tests/tools/scan_log_secrets.py`）都会踩到，
整份日志的可用性下降。

**修法（已实施）**：新增文件内静态函数 `FormatWin32ErrorText()`（`FormatMessageW`
+ `FORMAT_MESSAGE_ALLOCATE_BUFFER` + `WideCharToMultiByte(CP_UTF8)`，Winsock 错误走
`ws2_32.dll` 模块表），把注入探针 bind 失败、`SendLocalDnsUdp` 直发失败、本地 DNS 四个 53
端口 bind 失败共 6 处改成 `error=<数值>, detail=<UTF-8 文本>`。
**仍未处理**：仓库里另有 8 处日志仍打印 `ec.message()`（`ITcpipTransmission.cpp:228`、
`RinetdConnection.cpp:108/151`、`LocalRpcServer.cpp:116/138`、`VEthernetExchanger.cpp:1128/1356`、
`VNetstack.cpp:983`）——其中多数已同时打印数值码，只有 `LocalRpcServer` 的两处只打文本。
要彻底解决需要把该辅助函数提到公共头文件，本轮未做（避免为一个日志格式改动引入新文件）。

## P4 面板把正在工作的活动服务器显示为「不可达」，RTT 也与实测不符

**事实**

- 快照（活动出口就是它，`connection=established`、`vpn_server=[23.166.168.33]:20000/ppp+tcp`）：
  ```json
  {"tag":"main","display_name":"ZGO","active":true,"state":1,
   "probe_checked":true,"probe_reachable":false,"probe_rtt_ms":-1,"probe_entry":""}
  {"tag":"server:sg","probe_checked":true,"probe_reachable":true,"probe_rtt_ms":1241,"probe_entry":"168.138.183.201:20000"}
  {"tag":"server:vds","probe_rtt_ms":1175} {"tag":"server:bwgus","probe_rtt_ms":1098}
  {"tag":"server:rfcjp","probe_rtt_ms":940} {"tag":"server:nubehk","probe_rtt_ms":12}
  ```
- `web/app.js:60` 对运行中的出口渲染
  `probe_checked ? (probe_reachable ? rtt+' ms' : '不可达')` → 面板上**正在承载流量的
  主服务器显示为「不可达」**。已从运行中的面板取回 `http://127.0.0.1:19999/app.js`
  （18074 字节）确认该分支就是当前线上版本：`${o.probe_checked?(o.probe_reachable?o.probe_rtt_ms+' ms':'不可达'):'待探测'}`。
- 宿主侧实测（从本机直接 connect，源地址 `192.168.51.101` = WLAN，即确实走物理网卡）：

  | 目标 | 实测 connect | 客户端探测上报 |
  |---|---|---|
  | 23.166.168.33:20000（活动） | 175 / 164 / 166 ms | 不可达 |
  | 168.138.183.201:20000（SG） | 159 / 222 / 158 ms | 1241 ms |
  | 103.73.220.207:16299（nubeHK） | 10 / 12 / 11 ms | 12 ms |

  即：活动服务器实际可达（且 /32 pin 路由有效，宿主的裸 connect 源地址是 WLAN 而不是 TUN），
  客户端却报「不可达」；SG 的探测值比实际大 7 倍。

**结论**：探测结果**不能**直接作为「服务器是否可达」呈现，尤其是活动出口。探测本身
（`ConnectivityProbe::ProbeTcp`，`ppp/app/client/ConnectivityProbe.cpp:56`）只是带超时的
TCP connect + `ProtectWindowsSocket`，而宿主裸 connect 正常，说明问题在探测路径
（pin 结果被丢弃、protect 失败即判不可达、或在同一 io_context 上与数据面争用导致
超时先于完成回调），而不是网络。建议：

1. 活动出口的显示不要用后台探测结果覆盖（或至少标注「探测失败但连接正常」）；
2. 探测失败时补一条可诊断的日志：pin 是否成功、protect 是否失败、connect 的错误码、
   实际耗时；现在这些信息一条都没有，所以只能靠 RPC 快照反推。

**（补记，2026-10-11 01:33 现场，含第 2 条日志后的实测结论）** 新构建里加了这条日志后，原因
当场就出来了：**所有探测失败都是 `timeout_ms=800, completed=1, error=995`
（`WSA_OPERATION_ABORTED`，即超时定时器取消了仍在 pending 的 connect），`elapsed_ms≈800~1271`**。

```text
ConnectivityProbe::ProbeTcp: probe failed, remote=168.138.183.201:20000, timeout_ms=800, completed=1, error=995, elapsed_ms=...
ConnectivityProbe::ProbeTcp: probe failed, remote=23.166.168.33:20000,  timeout_ms=800, completed=1, error=995, elapsed_ms=...
```

而同一时刻宿主实测（同一批地址）：

| 目标 | 宿主实测 | 客户端探测 |
|---|---|---|
| 23.166.168.33:20000（活动） | 177 ms | 有时 1077 ms 成功、有时 800 ms 超时 |
| 168.138.183.201:20000（SG） | **1163 ms** | 800 ms 超时 → 误报「不可达」 |
| 23.249.25.106:20000 | 86 ms | 大概率成功 |
| 175.243.241.212:10005 | **>6050 ms（真不通）** | 800 ms 超时（判定正确） |
| 103.112.1.172:34687 | 2454 ms 后被拒 | 800 ms 超时（判定正确） |

即：`client.probe.timeout_ms` 的默认值 **800 ms**（`AppConfiguration.h:268`）对真正
要十几百毫秒才完成握手的服务器过紧，会把可达的服务器报成不可达；同时探测路径本身
比宿主裸 connect 慢（活动服务器实测 177 ms，探测成功那次却是 1077 ms，说明探测与数据面
争用同一 io_context）。**无需改代码的缓解**：在配置的 `client` 段加
`"probe": { "timeout_ms": 2000 }`（ZGO.json 当前没有 `probe` 段）。


## P5 注入探针结论没有进 RPC/面板（上一份文档 8.4 未完成）

**事实**：`main.cpp` 全文检索 `injection_probe` **零命中**；`get_snapshot` 里也没有
注入探针字段（只有 `dataplane.wintun.*`）。而注入探针恰恰是本机 TUN 失效的**唯一直接判据**：

```
[WARN] TUN injection probe: host received 0/3 injected packets. ...
[WARN] TUN injection fallback: enabled the system proxy 127.0.0.1:8080 ...
```

20 分钟内 12 次结论全部 `0/3`（每 2 分钟一次），另有一次启动期
`cannot bind a UDP probe socket on the tunnel address`（就是 P3 的乱码那条）。

**修法**：把 `injection_probe_{sent,received,healthy,last_ms,reported}` 暴露到
`get_snapshot.dataplane.injection`（`main.cpp:3900-4170` 一带），面板上给一行
「TUN 注入交付：正常 / 被宿主丢弃（DNS、ICMP、TCP 握手都会失败）」。
这样用户不用读日志就能知道 TUN 模式当前是否真的在工作。

## P6 `--tun-mux=0` 让 mux 实际关闭：每条 TCP 流单开一条传输连接

**事实**

- `--tun-mux=<connections>` → `ni->Mux = uint16`，`VEthernetNetworkSwitcher::IsMuxEnabled()`
  定义为 `mux_ > 0`（`VEthernetNetworkSwitcher.h:207`），所以 **0 = 关闭**。
- 快照：`requested_mux_mode=flow`、`effective_mux_mode=compat`、`mux_state=disabled`、
  `mux_active_links=0`、`dataplane.mux.*` 全 0（这些诊断因此全是死值）。
- 23.4 分钟内 `VEthernetExchanger::OpenTransmission: ... connecting to 23.166.168.33:20000`
  出现 **637 次**、`ITransmission::HandshakeServer: success` **601 次** ≈ **每 2.2 秒一条新传输连接**；
  上一份文档也观察到服务端 `:20000` 上十几条 Established + 八十多条 TimeWait。
- 后果：每条流都要付一次 154 ms RTT 的隧道握手（`mux.no mux, using direct sub-transmission`
  在日志里反复出现），与文档 6.2 的「TUN 223 KB/s vs 代理 370 KB/s」方向一致。

**建议**：确认这是有意选择。如果只是 TUI/面板默认值，把 `tun_mux` 设为 ≥1（并让面板把
「0」标注为「关闭 mux」），否则 `mux.flow/turbo` 相关的全部可靠性工作在实际运行中不生效。

## P7 TUN MTU 1500 + TCP 封装

- 启动日志：`VEthernetNetworkSwitcher::Open: TAP MTU=1500, MSSv4=1460, MSSv6=1440, clamp=1`；
  `netsh interface ipv4 show interfaces level=verbose` 里 `PPP` 的 `Link MTU: 1500`。
  （对照：陈旧的 `PPP PRIVATE NETWORK 2 TAP` 仍是 `1400`，是更早一次用 1400 的残留。）
- 配置 `config/ZGO.json` 的 `client.tun.mtu = 1500`，上一份文档 6.2/6.3 已建议回到 1400
  （显式给 mtu 时 MSS 自动推导为 mtu-40=1360）。这条**不需要改代码**，改配置重启即可，
  是目前最省事的一次对照实验。

## P8 带 IPv4 选项的报文被静默丢弃，且不写原因

```
[DEBUG](ppp/ethernet/VEthernet.cpp:417): VEthernet::TAP_PACKET_INPUT: unknown packet,
        packet_length=40, first_byte=0x46, vnet_=1
```

20 分钟内 411 条（长度 40 或 56 字节，`first_byte` 都是 `0x46`，即
version=4、IHL=6，也就是**带 4 字节 IPv4 选项**），≈17.6 条/分钟。`ip_hdr::Parse`
（`ppp/net/native/checksum.cpp:46-118`）会因 version≠4 / `hlen>len` / `ttl<1` /
`src==0||src==255.255.255.255||dest==0` / 校验和（`PACKET_CHECKSUM` 打开时）而返回 null，
但日志只打了长度和首字节，**看不出是哪一条判据失败**。建议把失败判据（以及 src/dst/ttl/proto）
一起打出来，或者按需支持 IHL>5 的报文。

## P9 `geoip.dat` / `geosite.dat` 缺失 → 每包一条 `geo_rules_unavailable`

`get_snapshot.routes` 指向 `geoip_file=C:\Users\2233\Desktop\TUI\geoip.dat`、
`geosite_file=...\geosite.dat`，但 TUI 目录里**没有这两个文件**（只有 `geo-rules.yaml`）。
于是选路逻辑对每个包都记：

```
[DEBUG](VEthernetNetworkSwitcher.cpp:2674): GetExchanger: destination=..., selected_outbound=main, reason=geo_rules_unavailable
```

20 分钟内 **2342 条**（≈100 条/分钟，是这份日志里最大的噪声源）。
`bypass_mode=ip` 下不影响功能，但（a）日志噪声，（b）面板没有把这个
「配置文件缺失」变成可见的状态。建议：缺文件只在启动时记一次 WARN，并在面板「路由」
卡片上标出；若不需要 geo 规则，就不要在启动参数里引用这两个路径。

## P10 WFP 丢包诊断每 120 秒重试一次并失败

```
[WARN](Win32Firewall.cpp:893): Fw::StartDropDiagnostics: FwpmNetEventSubscribe0 refused the subscription
because the Filtering Platform Packet Drop audit is off (0x80320013) and it could not be enabled;
run auditpol /set /subcategory:"{0CCE9225-69AE-11D9-BED3-505054503030}" /failure:enable as an administrator
```

12 条，间隔精确 120 秒（即每次注入探针结论为「坏」时都会重试一次）。本机**打不开**
该审计子类别（需要管理员；本次取证用的 shell 不是管理员，`netsh wfp show state` 也直接
`ERROR_ACCESS_DENIED`）。既然这是「本次运行的进程权限」决定的、且不会自愈，
重试就是纯噪声：建议检测一次（或尝试一次后）就记住结论，只记一条 WARN/INFO。

## P11 5 张陈旧隧道网卡的残留状态

| 适配器 | 状态 | metric | 残留的静态默认网关 | 残留的静态 DNS | 残留 IPv4 地址（DAD） |
|---|---|---|---|---|---|
| PPP 1 | Disconnected | 35 | 10.0.0.0 | 127.0.0.1, 8.8.8.8 | 10.0.0.5（Tentative） |
| PPP 2 | Disconnected | 35 | 192.168.12.0 | 1.1.1.1, 8.8.8.8 | 192.168.12.25（Tentative） |
| PPP PRIVATE NETWORK 2 TAP | Disconnected | 35 | 192.168.13.1 | 1.1.1.1, 8.8.8.8 | 192.168.13.25（Tentative） |
| 以太网 2 | Disconnected | 35 | 192.168.15.1 | 1.1.1.1, 8.8.8.8 | 192.168.15.25（Tentative） |
| 以太网 3 | Disconnected | 35 | 192.168.16.1 | 1.1.1.1, 8.8.8.8 | 192.168.16.25（Tentative） |

另外还有它们遗留的 IPv6 路由（`netsh interface ipv6 show route`）：

```
Manual 256  ::/1                 15  fd42:4242:4242::1     <- PPP 2（更早一次运行的防泄漏黑洞）
Manual 256  8000::/1             15  fd42:4242:4242::1
Manual 256  fd42:4242:4242::/64  15  PPP 2
```

即**更早一次会话的 IPv6 黑洞路由没有随会话退出而清理**，与活动会话在 `PPP`(71) 上的
`::/1`+`8000::/1` 并存。加上每张卡上的静态 DNS（`PPP 1` 甚至指向 `127.0.0.1`，即只在本客户端
运行时才存在的回环解析器），这些残留项在客户端退出后仍然存在。

**修法**：`tests/tools/Remove-StaleTunAdapters.ps1` 已能清理（默认 dry-run，需管理员 `-Apply`）；
同时建议客户端在**接管前**清理「同名旧适配器 / 同网段旧路由」，避免一台上跑过多版本后累积
（这些残留正是本次排障里最大的噪声源）。

## P12 `static_echo` 响应超时 >50%（待确认语义）

快照：

```json
"static_echo": {"send_packets":21,"tx_completed":21,"receive_packets":20,
                "response_timeouts":11,"receive_errors":11,"consecutive_failures":0,"degraded_until_ms":0}
```

配置 `udp.static.dns/icmp/quic = true`（ZGO 无 `static` 段，走默认），
`vpn_server` 也标了 `[static]`，所以这条 UDP 静态通道是 DNS/ICMP/QUIC 的候选承载。
`response_timeouts` 按 `docs/TUNNEL_DATAPLANE_RELIABILITY_IMPLEMENTATION_STATUS_CN.md:52`
的定义是「keepalive 窗口内没有收到任何有效响应」，11/21 值得单独查（上一份文档 6.2 也看到
它从 1 涨到 37）。它的失败会与「隧道内 DNS/ICMP 卡」互相掩护，建议在探针结论里也报出来。

---

## 3. 注入被宿主丢弃：本轮新增的线索与**否定**结论

上一份文档第 3 节把根因锁定为「注入报文在入站传输层被丢弃」，但没找到是谁丢的，
并留了「火绒是否拦截」这一步和几种猜测。本轮可以**排除**若干猜测（不需要改代码，
全部是现场实测）：

| 猜测 | 结论 | 证据 |
|---|---|---|
| 隧道地址未就绪（DAD 未完成） | **排除** | `netsh interface ipv4 show ipaddresses level=verbose`：`192.168.14.25`（PPP）`DAD State: Preferred` |
| 强主机模型（strong host receive）丢包 | **基本排除** | `netsh interface ipv4 show interfaces level=verbose`：所有接口 `Weak Host Receives: disabled`（即强主机生效），但目的地址确实属于来包网卡（PPP/71），强主机检查不会命中；且该检查在网络层，与「丢在传输层」的层级不符 |
| 陈旧网卡与新网卡地址/子网冲突 | **排除** | 陈旧卡的 5 个地址分属 10.0.0.5 / 192.168.12.25 / 13.25 / 15.25 / 16.25，**没有** 192.168.14.x；且它们都 `Tentative`、接口 `Disconnected` |
| Hyper-V/WSL 的 WinNAT/FSE（IPSNPI 客户端）污染接收路径 | **本机不适用** | 没有 `vEthernet*` 适配器；`Get-Service winnat,SharedAccess` 均为 `Stopped/Manual` |
| Windows 防火墙策略 | 上一轮已排除 | 放行规则无效；`FWPM_LAYER_INBOUND_TRANSPORT_V4` 上零过滤器 |

**本轮新增的可检验方向**（来自对微软文档的核查，供下一步用管理员权限验证）：

1. **传输层没有匹配端点**（`IpDiscardPortUnreachable`）：微软自己给的现场例子就是
   「UDP 包被丢弃，因为没有本地 socket 绑定该端口」（pktmon 的
   `DropReason: INET: transport endpoint was not found`）。本机探针是**先 bind 再注入**，
   所以需要在注入时刻确认端点仍在（`Get-NetUDPEndpoint -LocalPort <端口>`），
   并留意 0.0.0.0 通配绑定或独占端口抢占。
2. **NBL 的 offload/校验和元数据与网卡声明不一致**（`IpDiscardBadChecksum`、
   `IpDiscardUnsupportedOffload`、`IpDiscardIpsnpiInvalidUsoInfo`、`IpDiscardHeaderNotAligned` 等）：
   这些 `IP_DISCARD_REASON` 是 **Windows 11 24H2 起**新增的，正好覆盖本机版本（26200）。
   虚拟网卡/LWF 声明的能力与实际交付的 NBL 标志不一致，是这一类丢弃的经典来源；
   相关旁证是 24H2 上被记录的 UDP 合并（URO）缺少 `UDP_COALESCED_INFO` 元数据问题
   （Firezone PR #14392 / quinn issue #2041）。
   验证：`Get-NetAdapterChecksumOffload -Name PPP`、`Get-NetAdapterRsc -Name PPP`、
   `Get-NetAdapterLso -Name PPP`、`Get-NetOffloadGlobalSetting`、
   `netsh int udp show global`（看 URO），必要时 `netsh int ipv4 set global taskoffload=disabled`
   或 `Disable-NetAdapterRsc -Name PPP -IPv4` 做对照。
3. **直接问内核是谁丢的**（这是唯一能给出名字的手段，需要管理员）：
   `pktmon filter add -c <PPP 的 Id> -t UDP -p <端口>` → `pktmon start --capture --pkt-size 0`
   → 复现 → `pktmon etl2txt` 找 `Drop: ... DropReason ... DropLocation ...`；
   或 `netsh trace start scenario=InternetClient provider=Microsoft-Windows-TCPIP level=5`
   找 `dropped N packet(s) on interface ... Reason = n(...)`；或
   `netsh wfp capture start keywords=19` 确认这些包**没有**任何 `CLASSIFY_DROP` 事件
   （即不是过滤器丢的）。
   （注意：Wireshark/Npcap **无法**抓 Wintun 网卡，这是已知限制，所以必须用 pktmon。）
4. **火绒的确认性测试仍然没做**（上一份文档 5.3）：临时退出火绒或关掉「网络入侵拦截」，
   再跑 `tests/tools/TunInboundProbe.ps1`；TUN 行由 `0/6` 变 `6/6` 即证实。
   微软的 msquic TSG 也明确指出「不兼容的 WFP callout 会改写 TCPIP 接收路径」，
   与「该层没有过滤器也仍然可能是它」并不矛盾——但需要上面第 3 步的 ETW 来定位。

另外，唯一以 `ERROR` 级别出现在日志里的注入探针失败是启动瞬间的那次
**BIND 失败**（`WSAEADDRNOTAVAIL`，即 P3 的乱码行）；之后的 10 次都能 bind 并成功注入 3 个包，
只是宿主收不到——这与「宿主侧丢弃」的结论一致，也说明**不要**把 bind 失败当成根因。

## 4. 需要管理员权限、本轮无法执行的验证

- `netsh wfp show state file=...`（本 shell 直接 `ERROR_ACCESS_DENIED`）、
  `Get-NetAdapterBinding/-ChecksumOffload/-Rsc/-Lso/-OffloadGlobalSetting`（`拒绝访问`）；
- `auditpol /set /subcategory:"{0CCE9225-...}" /failure:enable`（P10）；
- `Remove-StaleTunAdapters.ps1 -Apply`（P11）；
- `netsh int ipv4 set global taskoffload=disabled` / `Disable-NetAdapterRsc`（第 3 节方向 2）；
- `pktmon` / `netsh trace`（第 3 节方向 3）。

## 5. 修复顺序与状态

1. ~~**代码（小、低风险）**：P2 日志去噪/跳过不可路由的 IPv6 候选；P3 错误文本转 UTF-8；
   P9 缺文件只报一次；P10 失败只报一次~~ → **已实施**（见 §0.1）。
2. ~~**代码**：P5 注入探针进 RPC/面板；P4 活动出口显示不要被探测结果覆盖 + 探测失败写原因；
   P1 的 AAAA 自动剥离~~ → **已实施**（见 §0.1）。
3. **零成本、立刻见效（配置，需重启核心）**：`config/ZGO.json` 的
   `client.tun.mtu` 改 1400（P7）；若不想等新构建，也可加 `"prefer_ipv4": true` 先看效果（P1）。
   重启后按上一份文档 6.2 的对照表复测吞吐与卡顿。
4. **配置决策（需要你定）**：P6 的 `tun_mux=0` 是否恢复 mux（会改变连接模型与吞吐特征）；
   P11 用 `tests/tools/Remove-StaleTunAdapters.ps1 -Apply` 清理 5 张陈旧网卡。
5. **仍待定位**：第 3 节方向 1-4（需要管理员权限与用户侧操作），以及 P8（那批
   `first_byte=0x46` 的 40 字节报文来自谁）与 P12（`static_echo` 超时语义）。

## 6. 新构建到手后的验收清单（每条都能一眼看出通过与否）

1. P2：启动核心后 `ppp-core-debug.log` 中
   `physical IPv6 gateway unavailable` **为 0 条**；取而代之的是**一条** INFO
   `this host has no physical IPv6 gateway, so IPv6 peer entries are not probed`。
2. P3：日志仍是有效 UTF-8——`Get-Content -Encoding utf8` 或任何 UTF-8 工具都能整份读完，
   不再出现 `(line is not valid UTF-8)`；错误行形如 `error=10049, detail=在其上下文中…`。
3. P4：`get_snapshot.outbounds` 里活动出口 `probe_reachable=true`（面板不再显示「不可达」）；
   若后台探测失败，日志会出现 `ConnectivityProbe::ProbeTcp: probe failed, remote=..., error=..., elapsed_ms=...`，
   据此判断是 protect 失败（`socket protection failed`）、超时（`error=995`）还是真实错误。
4. P5：`get_snapshot.dataplane.injection` 存在且 `reported=true`；面板「网络」页出现
   「TUN 注入交付」一行；本机应显示「被宿主丢弃」+「系统代理回退：已启用 127.0.0.1:8080」。
5. P1：经回环解析器查询 AAAA 应被剥离（有 A 记录时，**直连域名同样适用**）：
   `Resolve-DnsName -Type AAAA -Server 127.0.0.1 www.baidu.com` 不应再有 AAAA（A 记录正常）；
   日志里应出现 `STRIPPED ... AAAA for <域名>`；同时
   `has no usable IPv6 assignment` 应从 ≈36~68 条/分钟降到 0/接近 0。
   注意：vDNS 与 Windows DNS 缓存里的旧 AAAA 不会消失，需重启核心或查未缓存域名。
6. P9/P10：`reason=geo_rules_unavailable` 只出现在一条 WARN 里；
   `Fw::StartDropDiagnostics … could not be enabled` **只出现一次**。
7. P7（改配置后）：`TAP MTU=1400, MSSv4=1360`，并按上一份文档 6.2 的对照表复测吞吐/卡顿。
8. P4 补充（探测超时）：日志里 `ConnectivityProbe::ProbeTcp: probe failed ... error=995`
   的条数应明显下降、`completed=1` 且 `elapsed_ms≈timeout_ms` 的条目不应再被判成「不可达」——
   做法是在配置 `client` 段加 `"probe": { "timeout_ms": 2000 }`（默认 800 ms 对慢服务器过紧）。

## 7. 实测报告：ICMP、境外网页与 TUN 数据面（2026-10-11 01:33–01:55，构建 `6be4ed2`）

现场：客户端 01:33:16 启动，TUN `PPP` ifIndex=71 `192.168.14.25/24` + 网关 `192.168.14.1`，
物理网卡 `WLAN` `192.168.51.101`，出口 `23.166.168.33:20000`（ZGO），`--tun-mux=0`，MTU 1500。

### 7.1 结果矩阵

| 被测路径 | 测试方法 | 结果 |
|---|---|---|
| **TUN 宿主交付**（客户端注入 → 宿主 socket） | 源地址绑 `192.168.14.25`，向隧道网关 `192.168.14.1:53` 发 6 次 DNS 查询 | **0/6 应答**，每次等满 2500 ms |
| **TUN 内 UDP**（走隧道到公网 DNS） | 同源地址向 `8.8.8.8:53` 发 6 次 | **0/6 应答** |
| 对照：回环代理（不经注入） | 向 `127.0.0.1:53` 发 6 次 | **6/6 应答**（607/767/2/51/0/0 ms） |
| 客户端自测注入探针 | `TUN injection probe` | **0/3**，01:33–01:47 八次全部如此；`dataplane.injection={"sent":3,"received":0,"healthy":false,"fallback_proxy":true}` |
| 期间计数器 | RPC 快照对比（01:45→01:55） | `tun_rx +257`、`submit_successes +43`、`built/allocated +43`、`validate_failures 0`、`correlated_remote_rx_packets 0` —— **客户端确实往网卡里写了 43 个包，宿主一个都没收到** |
| **ICMP（我的沙箱会话）** | `ping 127.0.0.1 / 192.168.51.1 / 192.168.14.1 / 8.8.8.8 / 1.1.1.1 / 9.9.9.9` | 全部 `transmit failed. General failure.`；`tracert` → `Transmit error: code 5`（WSAEACCES）；`DATAPLANE ICMP` 日志 **0 条新增**（我的包从未进入客户端）→ **此项受我的会话限制，见 7.4，需你侧确认** |
| ICMP（用户 01:33 的 ping，历史证据） | 日志 `DATAPLANE ICMP: input ... type=8` ×4（目标 9.9.9.9） | 请求**进过** TUN 并被转发（`remote ... result=1`），但**没有回包** → 回包方向与注入结论一致 |
| 本地 HTTP 代理 8080（`source=local-proxy`） | 手写 HTTP/1.0 请求 | ✅ `www.google.com` 200 + 真首页（3.3 s / 7 KB）、`www.gnu.org` 200、`wikipedia.org` 301、`detectportal.firefox.com/success.txt` 200 `success`；❌ `neverssl.com` **3/3 无响应（≈6 s 超时，字节 0）** |
| 出口 IP（是否真走 VPN） | `http://api.ipify.org/`、`http://icanhazip.com/` 经代理 | ✅ 两者都返回 **`23.166.168.33`** = VPN 服务器地址 |
| 本地 SOCKS5 1080 | 手写握手 + CONNECT `www.google.com:443` | ✅ 问候 `05 00`、应答 `05 00 00 01 127.0.0.1 ...`（status=0 成功） |
| paper-airplane LSP（宿主进程 TCP 被劫持进来） | 日志 `PaperAirplaneConnection::OnConnect` | 19 次 `connected` / 2 次 `remote CONNECT failed`（其中一个是我测的 `93.184.216.34:80`）；宿主侧表现为 `local=127.0.0.1` |
| 国内直连（bypass 分流） | `http://www.baidu.com/` | ✅ 200、31 ms、源地址 `192.168.51.101`（走物理网卡，符合 `bypass_mode=ip` 预期） |
| 客户端自诊断 | RPC `run_diagnostics` | ⚠️ **7/7 全部 pass**、`healthy=true`、`network.mtu configured=1500 effective=1500` —— **完全看不到 TUN 交付已死** |
| HTTPS 端到端（页面渲染） | `curl.exe` / `Invoke-WebRequest` / `SslStream` | ⚠️ **我的会话里 TLS 一律失败**（国内 `baidu.com`、`qq.com` 同样失败 → 环境限制，不作为证据）；可用证据是：经 8080 的 `CONNECT www.google.com:443 / example.com:443 / cloudflare.com:443` 均返回 **200 Connection established**，SOCKS5 CONNECT 也成功 |

### 7.2 结论一：TUN 模式**自己的数据面是坏的**，但客户端用别的路径兜住了

- 需要「客户端注入 → 宿主」这一步的流量（隧道网关 DNS、隧道内 UDP、ICMP 回包、
  隧道内 TCP 握手）**全部失败**；客户端写进网卡的 43 个包在宿主侧一个都没出现
  （`validate_failures=0`，说明是宿主栈收下后丢弃，不是网卡拒绝）。
- 宿主应用能上网，靠的是**另外三条不经过注入的路径**：paper-airplane LSP（把进程的
  TCP 劫持进客户端进程，宿主看到 `local=127.0.0.1`）、本地 HTTP 代理 8080、
  本地 SOCKS5 1080；系统代理被探针判定失败后自动指向 8080，所以浏览器一直能用。
- 因此「网页能开」**不能**作为「TUN 模式正常」的证据；反过来，任何绕过 LSP/代理、
  必须走 TUN 的程序（服务、部分 UDP 应用、非劫持进程）在这台机器上都会失败。

### 7.3 结论二：新增一个可复现的「境外站点经代理也不通」案例

`http://neverssl.com/` 经本地代理 **3/3 无响应**（客户端读到 REQUEST 后：DNS 经隧道
`direct=0` 成功 → `ConnectBridgeToPeer...destination=neverssl.com:80` → `Mux enter → no mux`
→ 之后**既没有 `connected` 也没有失败日志**，请求一直挂着）。同类还有 LSP 的
`remote CONNECT failed`（`93.184.216.34:80`）。这说明远端桥接**缺少连接超时/失败可见性**：
挂住的连接不会向上层报错，浏览器/curl 只能等到自己超时 —— 与旧文档 6.2 的「偶发卡顿」同源。
建议：给 `ConnectBridgeToPeer`/`PaperAirplaneConnection::OnConnect` 的远端连接加显式超时，
并在超时时打一条 WARN（含 destination 与 elapsed）。

### 7.4 需要你在**自己的窗口**里确认的两件事（我的会话有沙箱限制）

1. ICMP 到底是「发不出去」还是「发出去没有回包」——我的进程里 `ping` 连 `127.0.0.1` 都
   报 `transmit failed. General failure.` / `Transmit error: code 5`，而我的会话不是管理员、
   WMI ping 也被拒（`拒绝访问`），所以这可能是我的会话限制而非客户端问题。
   请在你的 cmd 里跑并把**原文**发我：
   ```bat
   ping 192.168.14.1
   ping 9.9.9.9
   ```
   - 若是 `Request timed out` → 发得出去、回包被丢（与注入结论一致，属客户端问题）；
   - 若是 `transmit failed / General failure / 代码 5` → 连发都被拒，是系统级 ICMP 拦截
     （LSP 或安全软件），那是另一个（更严重的）问题。
2. HTTPS 页面：我的会话里 TLS 全线失败（国内站点同样失败），请在浏览器里开
   `https://www.google.com/` 与 `https://www.cloudflare.com/`；正常应秒开
   （HTTP 与 SOCKS5 的 CONNECT 都已证明隧道能到这些 443 端点）。

### 7.5 一句话总结

**TUN 数据面：坏（注入→宿主 0 交付，已连续复现）；ICMP：回包方向坏（发送方向待你确认）；
境外网页：经 LSP/代理可用（出口 IP = 23.166.168.33，Google/GNU 真页面 200），
但存在「远端连接挂死不报错」的站点（neverssl.com 3/3）；客户端自诊断对这些全都不可见。**

## 8. 管理员级取证（2026-10-11 10:00–10:45，构建 `1c53ad8`）

这一轮拿到了管理员权限，7.4 里"待确认"的两件事都做完了，而且用 pktmon 直接读到了丢弃原因。

### 8.1 更正 7.4 的 ICMP 结论：发送正常，只有"回包"丢失

非管理员会话里的 `Transmit error: code 5` / `transmit failed. General failure.` 是**我的工具环境限制**
（普通用户，WMI ping 也被拒），不是客户端问题。以管理员身份重测：

| 目标 | 结果 | 说明 |
|---|---|---|
| `127.0.0.1` | ✅ `<1 ms` | ICMP 栈本身正常 |
| `192.168.51.1`（物理网关） | ✅ 2 / 12 ms | 物理路径正常 |
| `23.166.168.33`（VPN 服务器，/32 直连 pin） | ✅ 153 / 156 ms | 直连路径正常 |
| `192.168.14.1`（**隧道网关**） | ❌ `Request timed out` | 必须经过注入 |
| `8.8.8.8` / `9.9.9.9` | ❌ `Request timed out` | 同上 |

日志里的最小复现（客户端自己都回答了，宿主还是收不到）：

```text
DATAPLANE ICMP: input src=192.168.14.25 dst=192.168.14.1 type=8 code=0 ttl=128
DATAPLANE ICMP: gateway echo result=1        <- 客户端已生成本地应答并注入
```

### 8.2 pktmon 直接给出丢弃原因：`Not locally destined`

命令（管理员）：`pktmon start --capture --type all --comp all --pkt-size 64 --file-name tun-drop.etl`
→ 期间从隧道地址向网关发 6 次 DNS 查询 → `pktmon stop` → `pktmon etl2txt`。注入应答的完整路径：

```text
[Rx 组件 819] ip: 192.168.14.1.53 > 192.168.14.25.56035: 13249 0/0/0 (34)
[Rx 组件 820] 同一包
[Rx 组件 821] 丢弃： DropReason Not locally destined 、DropLocation 0xE0004101
             删除:ip: 192.168.14.1.53 > 192.168.14.25.56035: 13249 0/0/0 (34)
```

把 112 条丢弃事件归类后，涉及隧道地址的 **16/16 个注入包全部**是
`Not locally destined @ 组件 821 (Rx)`；它们在 819/820 上都正常出现，字节与 DNS 载荷正确。
即：**客户端写进去了、适配器收下了、IP 层判定"目的地址不是本机"**。

同时确认 `192.168.14.25` 本身一切正常：`PPP(ifIndex 71)`、`AddressState=Preferred`
（`netsh ... level=verbose`：`DAD State: Preferred`）、`SkipAsSource=False`、
`PrefixOrigin=Manual`、隔离舱 1、全机没有第二个 `192.168.14.x`。

### 8.3 逐项排除（都做了对照实验）

| 假设 | 实验 | 结论 |
|---|---|---|
| 强主机模型（strong host receive） | `Set-NetIPInterface -InterfaceIndex 71/18 -WeakHostReceive Enabled` 后再探针 | 仍 0/6 → **排除** |
| 8792 条旁路路由抢占地址 | `bypass_mode=no`（接管只装 4 条路由）后重启再探针 | 仍 0/3 → **排除** |
| 地址未就绪 / DAD | 坏状态下查 DAD | `Preferred` → **排除** |
| 隔离舱 / 容器网络 | `netsh int ipv4 show compartments`、`WinNat` 服务 | 单舱 1、WinNat 停止 → **排除** |
| 卸载（checksum/RSC/LSO/URO） | 网卡未声明任何 offload 能力；全局 RSC/URO 开启 | 两种驱动表现一致 → **不是主因**（未再做关 RSC/URO 的对照） |

### 8.4 最可能的根因：2021 年的 Wintun 0.14 驱动 + 2026 年的 Windows 26200

- 本机 `C:\Windows\System32\drivers\wintun.sys` = **0.14**（2021-10），随附
  `Driver\x64\wintun.dll` = **0.14.1**（DLL 与驱动版本不一致）。
- 上游 Wintun **2026 年**在 master 上修了 4 个驱动缺陷，**全部落在注入路径的 NBL / 环形缓冲**上：
  - `driver: introduce NET_BUFFER_LIST_OFFSET helper`（NBL 的 offset 与 flags 放在 MINIPORT_RESERVED[0]）
  - `driver: explicitly terminate NBL`
  - `driver: fix race condition at Ring->Head`
  - `driver: fix missed-wakeup race in ring buffer Alertable signaling`（内存屏障）
- "字节完全正确、IP 层却说不是本地地址"与 **NBL offset/termination 处理不一致**高度吻合；
  0.14 驱动早于这些修复，而系统是 26200（2026 年构建）。

### 8.5 TAP 对照与"间歇性"真相

`--tun-driver=tap` 时：

```text
10:16:23 host received 3/3 injected packets; the client can deliver tunnel traffic to the host stack
10:18:24 host received 0/3 injected packets ...
10:20:27 host received 0/3 ...
```

用 2 秒粒度的独立探测（1 次查询 / 1.2 秒超时）跨重启跟了 2 分半，宿主侧几乎恒为 0，
只有客户端自己那一轮偶尔过 —— 所以**不是"接管某一步把它弄坏了"，而是注入交付本身不稳定**
（偶发成功）。这与上游"missed-wakeup race：包躺在环里直到下一次写入/重传才被处理"的描述一致：
探针那 3 个包偶尔被立刻处理，真实流量基本收不到。

### 8.6 本轮新发现的两个客户端缺陷（可修）

1. **`--tun-driver=tap` 会致命启动失败**：客户端复用了已存在的 TAP 网卡
   （`PPP PRIVATE NETWORK 2 TAP`，MTU=1400），而配置 `client.tun.mtu=1500`：

   ```text
   Open: TAP network interface found
   [ERROR] Open: TAP MTU verification failed, configured=1500, effective=1400
   ```

   `Open` 直接失败、核心起不来（我把该网卡 MTU 设成 1500（`store=active`）后才起来）。
   建议：TAP 模式下**读取并设置**适配器 MTU（能设就设，设不了再报错），不要复用带旧 MTU 的陈旧适配器。
2. **注入探针单次结论不可靠**：同一会话内 2 分钟内会在 `3/3 healthy` 与 `0/3` 之间翻转，
   会让面板误报"TUN 交付正常"。建议多轮/多数表决，或直接标注"不稳定"。

### 8.7 仍然正常的部分（本次复测）

经本地 HTTP 代理：`api.ipify.org` → 出口 IP **`23.166.168.33`**；`www.google.com` → 200 + 真首页（5.3 s）；
SOCKS5 CONNECT `www.google.com:443` → 成功。**"网页能开"依然不能作为 TUN 正常的证据**。

### 8.8 建议的处置顺序

1. **换驱动**：把 `wintun.sys` 换成含上述 2026 修复的构建（自建 master，或采用内置较新 Wintun 的发行版），
   再跑一次探针；这是唯一直接针对注入路径根因的动作。
2. **临时可用**：`--tun-driver=tap` + 让 TAP 适配器 MTU 与配置一致（或把 `client.tun.mtu` 设为 1400）。
3. **客户端加固**：把注入健康度并入 `run_diagnostics`（现在 7/7 全绿却完全不可用）；
   探针做多数表决；TAP 启动失败时给可操作的错误而不是直接退出。

> 本轮用到的脚本与原始证据（未入库）：`artifacts/pktmon-tun-probe.ps1`、
> `artifacts/tun-drop.txt`（pktmon 文本）、`artifacts/switch-to-tap.ps1`、`artifacts/tap-test.ps1`、
> `artifacts/tap-battery.ps1`、`artifacts/bisect-bypass.ps1`、`artifacts/flip-timeline.ps1`、
> `artifacts/injection-watch.ps1`、`artifacts/wfp-state.xml`。

## 9. 错误与事故总结（含排查过程本身造成的故障）

### 9.1 排查过程造成的用户侧故障（我的操作失误，已全部还原）

| # | 失误 | 后果 | 防复发 |
|---|---|---|---|
| A1 | 在用户正在使用的机器上**连续重启客户端 6 次**（10:13–10:41，切驱动/改 MTU/改 bypass/跑时间线） | 每次断网 10–30 秒；期间"国内分流 + VPN 网站都失败" | 任何会重启核心的实验，先告知并约定时间窗；能不改配置就不改 |
| A2 | 切 `--tun-driver=tap` **之前没有先核对适配器 MTU** | `TAP MTU verification failed, configured=1500, effective=1400` → 核心**启动失败约 2 分钟**（完全断网） | 切驱动前先读 `Get-NetIPAddress`/MTU；改为"能设就设、设不了才报错" |
| A3 | 用 `bypass_mode=no` 做二分实验 | 实验期间分流规则失效 | 二分实验优先用只读手段；必须改配置时改完立刻还原（本次已还原） |
| A4 | 非管理员会话里用 `ping`/`tracert` 得到 `Transmit error: code 5`，并据此写下"ICMP 连发都发不出去" | 结论错误（那是我会话的限制），会让你误判故障方向 | 工具能力受限时，结论标注"待确认"而不是写进结论；管理员权限下复核（见 8.1） |
| A5 | 一度把"Wintun 从不工作 / TAP 能工作"当成驱动差异 | 结论不准确：实际是注入交付**本身间歇**（3/3 与 0/3 会在 2 分钟内翻转） | 单次探针结论不做因果判断；需要多轮/多会话采样（见 8.5） |
| A6 | 把 800 ms 探测超时导致的"不可达"先当成"探测路径有问题" | 方向对但原因未定；直到加了失败日志才确认是 `error=995` 超时预算过紧 | 先加可诊断日志再下结论（本轮已补日志，见 P4 补记） |

### 9.2 客户端/系统侧的实际错误（按严重度）

| # | 问题 | 证据 | 状态 |
|---|---|---|---|
| C1 | **注入报文被 IP 层丢弃** `Not locally destined`（DropLocation 0xE0004101）→ TUN 数据面无法把任何回包交给宿主 | pktmon 16/16；探针长期 0/3；隧道网关 DNS 0/6、UDP 0/6、ICMP 全超时 | **未修**（根因指向 2021 年 Wintun 0.14 驱动 + Windows 26200；上游 2026 年才修 NBL/环形缓冲） |
| C2 | `--tun-driver=tap` 因 MTU 校验**致命启动失败**，且复用了带旧 MTU 的陈旧 TAP 适配器 | `Open: TAP MTU verification failed, configured=1500, effective=1400` | 未修（本轮靠临时改适配器 MTU 绕过） |
| C3 | 注入探针单次结论不可靠（2 分钟内 `3/3 healthy` ↔ `0/3`） | 10:16:23 3/3 → 10:18:24/10:20:27 0/3；10:21:44 3/3 → 10:23:46 0/3 | 未修（建议多数表决） |
| C4 | `run_diagnostics` 7/7 全绿、`healthy=true`，而 TUN 实际完全不可用 | RPC `run_diagnostics` + 注入探针 0/3 | 未修（建议把注入健康度并入诊断） |
| C5 | `--tun-mux=0` 关闭 mux → **每条流单开一条传输连接** → 经代理 TLS 2.3–5.6 s，重负载站点像"打不开" | `mux_state=disabled`；`connecting to` 637 次/23 分钟；本轮 HTTPS 实测 2.3/6.2/3.0 s | 未修（配置决策） |
| C6 | 远端桥接挂死时不上报：`ConnectBridgeToPeer` 之后既无 `connected` 也无失败 → 上层只能等自己超时 | `http://neverssl.com/` 经代理 3/3 无响应；LSP `remote CONNECT failed`（93.184.216.34） | 未修（建议加连接超时 + WARN） |
| C7 | IPv6 黑洞 + AAAA 交付：应用拿到走不通的 IPv6 地址后卡住 | `::/1`+`8000::/1` 黑洞；`has no usable IPv6 assignment` ≈36–68 条/分钟 | 部分修复（构建 `1c53ad8` 已对隧道路径与直连路径都剥离 AAAA；OS/vDNS 旧缓存需重启或过期才消失） |
| C8 | 陈旧隧道网卡残留（5 张 TAP：地址/默认网关/静态 DNS/IPv6 路由），外加本次 TAP 会话留下的 `192.168.13.25`(Deprecated) 与 3 条 metric 256 路由 | `netsh` 三张表 | 未修（无害但会干扰排查；有 `Remove-StaleTunAdapters.ps1`） |
| C9 | 带 IPv4 选项（IHL=6）的报文被静默丢弃且不写原因 | 411 条 `unknown packet, first_byte=0x46` | 未修（建议打印失败判据） |

### 9.3 本轮已修并推送

| 提交 | 内容 |
|---|---|
| `6be4ed2` | 注入探针进 RPC/面板；日志去噪（IPv6 出站、geo 缺失、审计订阅各只报一次）；错误文本转 UTF-8；活动出口不再被探测结果误报"不可达"；探测失败补原因日志 |
| `1c53ad8` | 直连域名在"宿主无 IPv6 出口"时也剥离 AAAA（`host_ipv6_uplink_` + `strip_without_a_cache`） |
| `006c0f1` / `45d3787` | 现场实测矩阵与管理员级取证（pktmon 丢弃原因、排除项、TAP 对照、时间线）写入本文档 |

## 10. 第二轮代码修复（本次提交，未在现场复测）

第 0 节表里"仍未实施"的条目与第 9.2 节的 C 系列问题逐条落地。原则是：**能自证的才自动处理，
不能自证的只报告不删除**；每条都给出"一眼可验证"的判据。IPv6 组合分析见
`docs/TUN_IPV6_SCENARIO_MATRIX_CN.md`。

| 编号 | 问题 | 改动 | 位置 |
|---|---|---|---|
| P8 / C9 | IHL=6 的报文被静默丢弃，只打长度与首字节 | `ip_hdr::Parse` 增加 `IPParseReason` 出参，新增 `ParseReasonName`/`ProtocolName`；TAP 入包失败时打印 `reason=`、version/ihl/proto/ttl 与累计丢弃数；"不支持的协议"（IGMP 等）按协议只报一次、之后每 1000 个包一条汇总；`dataplane.unparsable_packets`/`unparsable_protocol_mask` 进快照 | `ppp/net/native/ip.h`、`ppp/net/native/checksum.cpp`、`ppp/ethernet/VEthernet.h/.cpp`、`main.cpp` |
| C2 | `--tun-driver=tap` 因 MTU 校验**致命启动失败**（复用了陈旧 TAP 适配器） | 先读回适配器真实 MTU；不一致时最多 3 次设置（间隔 100 ms）；仍不一致**不再退出**：按适配器实际 MTU 继续，WARN 同时给出 `netsh interface ipv4 set subinterface ... mtu=` 与 `Remove-StaleTunAdapters.ps1` 两条修法，并让 TCP MSS clamp 跟随实际 MTU；`GetTunnelMtu()` 进 `network.tun.mtu`，`network.mtu` 诊断改用该值（Wintun/TAP 两种驱动都适用） | `VEthernetNetworkSwitcher.h/.cpp`、`main.cpp` |
| C3 | 注入探针单次结论不可靠（2 分钟内 `3/3` ↔ `0/3`） | 每轮结果进 16 轮滚动历史（每轮 2 bit），得出 `state=unknown/healthy/unstable/broken`；系统代理回退与丢包诊断订阅改为"2 连败才接管、2 连胜才撤销"；日志只在首次结论与状态翻转时输出（原来的每轮 WARN 已消除） | `VEthernetNetworkSwitcher.h/.cpp`、`main.cpp` |
| C4 | `run_diagnostics` 7/7 全绿而 TUN 实际不可用 | 新增 `network.injection` 检查：`broken` → **fail**，`unstable`/未出结论 → warn；`dataplane.injection` 增加 `state/last_round_delivered/delivered_rounds/failing_rounds/healthy_streak/failing_streak/last_success_ms` | `main.cpp` |
| P12 | `static_echo.response_timeouts` 语义不明（11/21 无法解读） | 新增 `keepalive_windows`/`keepalive_replies`：每个 keep-alive 窗口到点记一次，窗口内收到过报文则记 reply，于是超时可读成比例；诊断新增 `dataplane.static_echo`（≥5 窗口且 0 应答 → fail，过半未应答 → warn） | `VEthernetExchanger.h/.cpp`、`main.cpp` |
| C6 | 远端桥接挂死不上报：`ConnectBridgeToPeer` 之后既无 `connected` 也无失败 | `MuxOrConnect`/`MuxOrAccept` 在等待对端应答的 `ITransmission::Read` 外挂看门狗：超过 `tcp.connect.timeout` 秒 → WARN（含 host/port/超时秒数）并 `Dispose` 传输，让上层立即失败并关掉本地 socket，而不是挂到浏览器自己超时（覆盖 neverssl.com 与 LSP 两条路径） | `ppp/app/protocol/VirtualEthernetTcpipConnection.cpp` |
| P11 / C8 | 5 张陈旧隧道网卡残留，只能靠人工 `netsh` 发现 | 启动时枚举"带 openppp2 标记（`PPP PRIVATE NETWORK 2`）且不是本次所选适配器"的网卡，WARN 一次（含 ifIndex、状态、残留网关/DNS/地址）；`network.stale_tunnel_adapters` 进快照，`run_diagnostics` 新增 `network.stale_adapters`（列出前 3 条 + 清理命令）。**只报告不删除**：删除需要管理员，仍由脚本执行 | `VEthernetNetworkSwitcher.h/.cpp`、`main.cpp` |
| P4 补充 | 800 ms 探测超时把**可达**服务器报成"不可达"（error=995） | 默认 `client.probe.timeout_ms` 800 → 2000；JSON 里 `client.probe.timeout-ms` 仍可覆盖（下限 50） | `AppConfiguration.h/.cpp` |
| P6 | `--tun-mux=0` 的实际后果在面板上看不出来 | 面板 MUX 指标在 `mux_state=disabled` 时显示"已关闭（每条 TCP 流单开一条传输连接）"；设置页本来就写了"0 关闭" | `web/app.js` |
| P1 补强 | 关掉 DNS 缓存（`udp.dns.cache=false`）时，隧道侧 AAAA 完全不剥离 | 剥离从 `if (caching && udp.dns.cache)` 中移出并**无条件执行**，以 `strip_without_a_cache = ipv6_dataplane_unavailable_` 调用；`AddCache`/`FlushPendingAAAAResponses` 仍留在缓存开关内；顺带删掉永远不执行的"扣住 AAAA 等 A 记录"分支（该函数恒返回 true） | `VEthernetNetworkSwitcher.cpp` |
| P1 补强 | 服务端 v6 安装失败时**保留 `AssignedIPv6Mode=Nat66`**，客户端据此误判"有 v6 数据面"，于是不装 `::/1` 又不再剥离 AAAA | 服务端三处 `server-ipv6-dataplane-install-failed` 路径补 `AssignedIPv6Mode = IPv6Mode_None`；客户端 `ApplyIPv6Assignment` 收紧为"mode ∈ {Nat66,Gua} **且** 有非 unspecified 的 v6 地址"才算有数据面 | `ppp/app/server/VirtualEthernetSwitcher.cpp`、`VEthernetNetworkSwitcher.cpp` |
| 可观测性 | v6 状态只能靠逐包 DEBUG 推断 | `get_snapshot` 新增 `network.ipv6.{server_dataplane,dataplane_unavailable,host_uplink,effective}`（`effective` ∈ tunnel / direct-only / none），`run_diagnostics` 新增 `network.ipv6` 一句话解释当前组合 | `main.cpp` |

### 10.1 本轮验收清单（新构建）

1. P8：日志里不再出现裸的 `unknown packet, packet_length=..., first_byte=0x46`；取而代之是
   `reason=unsupported-protocol, version=4, ihl=6, proto=2(igmp), ttl=...`，且同协议只出现一次；
   `get_snapshot.dataplane.unparsable_packets` 持续增长而日志不增长。
2. C2：把 `--tun-driver=tap` 配在 MTU=1400 的陈旧适配器上启动：核心**不再退出**，日志出现
   `the tunnel adapter carries MTU=1400, not the configured 1500`，`network.tun.mtu=1400`，
   `run_diagnostics.network.mtu` 为 fail（配置与实际不一致）而连接正常。
3. C3/C4：`get_snapshot.dataplane.injection.state` 会随滚动窗口在 `healthy/unstable/broken` 之间变化；
   现场那台机器应长期是 `unstable` 或 `broken`；`run_diagnostics` 出现 `network.injection`，
   坏状态下不再是 7/7 全绿。
4. C6：对已知挂死站点（`http://neverssl.com/`）经本地代理请求，`tcp.connect.timeout` 秒后
   必出现 `VETcpip: the peer did not answer the connect request within N s`，客户端 socket 被关闭。
5. P11：启动时若机器上还有旧隧道网卡，出现一条 `N tunnel adapter(s) from an earlier session are still installed`；
   `get_snapshot.network.stale_tunnel_adapters` 列出它们；跑 `Remove-StaleTunAdapters.ps1 -Apply` 后该列表清空。
6. P12：`dataplane.static_echo.keepalive_windows` 与 `keepalive_replies` 随时间增长，
   `response_timeouts/keepalive_windows` 就是"未应答窗口比例"；该比例过半时 `run_diagnostics.dataplane.static_echo` 报 warn。
7. P4 补充：慢服务器（实测 1163 ms）不再被判"不可达"；`probe failed ... error=995` 条数明显下降。
8. P1 补强：`udp.dns.cache=false` 时 `Resolve-DnsName -Type AAAA -Server 127.0.0.1 <任意域名>` 仍被剥离
   （日志 `STRIPPED ... AAAA`）；服务端 v6 安装失败时 `get_snapshot.network.ipv6.server_dataplane=false`。

### 10.2 本轮仍未处理

- **第 3 节根因（C1）**：注入报文被 IP 层以 `Not locally destined` 丢弃，仍指向 2021 年的 Wintun 0.14 + Windows 26200；
  本轮只让这个问题**变得可见**（C3/C4/P5），换驱动那一步仍需现场执行。
- **P7**：`client.tun.mtu` 回 1400 是配置改动，代码侧只保证"配置与实际不一致时不再致命"。
- **P11 的删除动作**：需要管理员，仍由 `tests/tools/Remove-StaleTunAdapters.ps1 -Apply` 执行。
- **P6**：`--tun-mux` 是否恢复 ≥1 仍是配置决策，本轮只补齐面板可见性。
- **P8 的协议支持**：IGMP/OSPF 等协议仍不被用户态栈承载，本轮只把"为什么丢"写清楚。
- IPv6 部分的剩余缺口（见 `docs/TUN_IPV6_SCENARIO_MATRIX_CN.md` 第六节）**未经现场复测**。
