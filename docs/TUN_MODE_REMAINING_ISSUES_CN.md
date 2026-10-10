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
| P1 | 新增 `ipv6_dataplane_unavailable_` + `ShouldPreferIPv4()`：**没有可用 IPv6 数据面时就剥离 AAAA**，不再依赖 `udp.dns.prefer_ipv4` | `VEthernetNetworkSwitcher.h/.cpp`（构造函数、`ApplyIPv6Assignment`、`StripAAAADnsResponseIfIPv4Available`、`DispatchLocalDnsQuery`、`Open`） |
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

**修法**（上一份文档 6.1 的「后续代码改进」仍未实施）

1. 立刻可用（无需编译）：给 `config/ZGO.json` 的 `udp.dns` 加 `"prefer_ipv4": true`，重启核心。
2. 代码：把「剥离 AAAA」的条件从「用户显式开启 prefer_ipv4」改为
   **「有 A 记录 且 客户端当前没有可用的 IPv6 数据面」**（即 `ipv6_client_state_.DefaultRouteApplied == false`
   或未分配 IPv6）。把不可达的 IPv6 地址交给应用本身就是缺陷，不应依赖用户开关。
   相关位置：`VEthernetNetworkSwitcher::StripAAAADnsResponseIfIPv4Available` /
   `FlushPendingAAAAResponses`（`ppp/app/client/VEthernetNetworkSwitcher.cpp:920-1094`）。

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
5. P1：经回环解析器查询 AAAA 应被剥离（有 A 记录时）：
   `nslookup -type=AAAA www.baidu.com`（用 `192.168.14.1` 或 `127.0.0.1` 作服务器）
   返回 AAAA 为空/无记录，而 `nslookup www.baidu.com` 的 A 记录正常；
   同时日志里 `has no usable IPv6 assignment` 应从 ≈36 条/分钟降到 0/接近 0。
6. P9/P10：`reason=geo_rules_unavailable` 只出现在一条 WARN 里；
   `Fw::StartDropDiagnostics … could not be enabled` **只出现一次**。
7. P7（改配置后）：`TAP MTU=1400, MSSv4=1360`，并按上一份文档 6.2 的对照表复测吞吐/卡顿。
