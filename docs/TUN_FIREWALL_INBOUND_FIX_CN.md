# TUN 模式 DNS / ICMP 不通（Windows 防火墙丢弃注入报文）—— 定位与修复

> 现场：Windows 11 + Wintun（适配器 `PPP`，`192.168.14.25/24`，网关 `192.168.14.1`），
> 客户端 `ppp-web.exe`（`--mode=client --tun-mux=0 --bypass-mode=ip`，服务端 `23.166.168.33:20000`）。
> 结论：**隧道数据面正常，卡点只有一个——客户端从用户态注入 TUN 的报文被 Windows 防火墙在入站传输层丢弃。**

## 1. 症状

- `ping 192.168.14.1`（连 TUN 自己的虚拟网关）100% 丢包，`ping 8.8.8.8` / `1.1.1.1` 全丢。
- `nslookup www.baidu.com` 超时（`Server: 192.168.14.1`），`Resolve-DnsName -Server 192.168.14.1` 超时。
- 网页打不开或极慢；但走 `127.0.0.1:53`（核心本地 DNS 代理）的解析正常。

## 2. 实测证据

| 实测项 | 结果 |
|------|------|
| 连接状态（RPC `get_snapshot`） | `phase=connected`、`vpn_server=[23.166.168.33]:20000/ppp+tcp` |
| TUN 默认路由 | 存在：`0.0.0.0/0 → 192.168.14.1 dev 71`，有效 metric 5（优于物理网卡） |
| TCP 经 TUN（裸 IP，不需要 DNS） | ✅ `curl http://1.1.1.1/` → `301` |
| HTTP 代理 8080 / SOCKS5 1080 → 裸 IP | ✅ 均 `301`（隧道端到端正常） |
| HTTPS 经 TUN（需 DNS） | ❌ `000`（解析失败） |
| ICMP | ❌ 全丢 |
| DNS → TUN 网关 `192.168.14.1:53` | ❌ 0/6 应答 |
| DNS → 回环代理 `127.0.0.1:53` | ✅ 6/6 应答 |
| 核心侧计数器 | `local_rx_packets` / `submit_successes` 正常增长，`validate_failures=0` |
| 核心日志 | `DNS pipeline: inject source=192.168.14.25:54921, dns=192.168.14.1:53, bytes=106, output=1` |

决定性对照（自建 UDP socket，直接观测 OS 是否交付注入的应答）：

```
tunnel gateway (injected into the TUN)     replies 0/6    WFP inbound-transport discards +21
loopback proxy (bypasses the firewall)     replies 6/6    WFP inbound-transport discards +3
```

`\WFP Classify\FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD` 在发往 TUN 的 6 次查询期间 **+34**，
同内容查询走回环仅 **+1**（噪声）。即：报文已进入 Wintun 适配器（`Get-NetAdapterStatistics`
的 RX 计数与核心 `submit_successes` 同步增长），但在 WFP 入站传输层被丢弃。

## 3. 根因

**结论：客户端注入 TUN 的报文被宿主网络栈在入站传输层丢弃，因此凡是真正走隧道的流量
（域名解析、ICMP、TCP 建连）都拿不到返回包；能通的那些其实是被旁路的。**

已确认的事实（均为实机实测）：

1. **注入报文本身完全正确**：用 `pktmon` 抓到客户端真实注入的 DNS 应答
   （`192.168.14.1:53 → 192.168.14.25:<port>`），逐字段校验 **IP 头校验和正确、UDP 校验和正确**、
   DNS 载荷正确（QDCOUNT/ANCOUNT 与应答内容一致）。核心日志同步打印
   `DNS pipeline: inject source=192.168.14.25:<port>, dns=192.168.14.1:53`。
2. **宿主协议栈把它丢了**：`FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD` 每轮有效查询
   **+14~+34**（可复现），而同一时刻 `IPPACKET` 与 `ALE` 层丢弃**恒为 0**。
   同一个 socket 收到 0 条；把同样的查询发到回环代理则是 6/6；发到公网解析器 3/3。
3. **不是 Windows 防火墙**：按隧道网段+接口+协议建立的入站 Allow 规则（读回验证 Enabled/
   Inbound/Allow/Profile=Any/UDP+ICMPv4/`LocalAddress 192.168.14.0/24`/`InterfaceAlias PPP`）
   **完全没有影响**；打开"筛选平台丢包"审计后，我们的报文**没有任何 5152 拦截事件**
   （唯一事件是路由器广播被 `Filter Origin: Stealth` 拦下）。
4. **不是火绒，也不是任何"该层的过滤器"**（**这里更正我早先的判断**）：
   `netsh wfp show state` 导出（7.4 MB / 2515 个过滤器）显示
   **`FWPM_LAYER_INBOUND_TRANSPORT_V4` 零过滤器、零回调**；火绒唯一的入站回调
   `hrwfpdrv` 位于 `FWPM_LAYER_INBOUND_IPPACKET_V4`，而**该层丢弃计数始终为 0**；
   火绒的 NDIS 过滤驱动 `hrndis6` **根本没有绑定到隧道网卡**（PPP 上只有微软协议绑定）。
5. **隧道内的 TCP 同样起不来**：抓包显示宿主对 `142.251.34.78:443` 发出 4 次 SYN，
   **从未收到任何 SYN-ACK** → 应用侧表现为 ~6 秒超时（`duplicate_syn` 一路上涨即由此而来）。
6. **服务器链路正常**：`ping 23.166.168.33` 0% 丢包、平均 154 ms。隧道传输是 **TCP**
   （核心对 `:20000` 有十几条 Established + 八十多条 TimeWait 连接），
   `vpn_server` 里的 `[static]` 只是 `StaticEchoAllocated()`（静态 echo 通道已分配，UDP），
   与"静态模式"无关。

> **早先两个假设都被实测否掉，已更正**：
> ① "Public 配置文件默认入站阻止 + 程序级规则匹配不到无主报文" —— 放行规则实测无效；
> ② "火绒的 WFP 驱动在拦截" —— 该层没有它的过滤器，它的 NDIS 驱动也没绑到隧道网卡。
> 结论是：**丢弃由宿主栈/驱动路径本身产生，而不是任何一条防火墙策略**，
> 所以"在火绒/防火墙里放行"都不会有效。**ICMP 与隧道内 TCP 因此无法用配置绕过**；
> 只有 DNS 可以（走回环代理，见第 4.2 节）。

> **对早期假设的更正**：最初（第 1 轮）把根因归为"Public 配置文件默认入站阻止 + 程序级规则
> 匹配不到无主报文"。方案 A 的实测结果**否掉了这一假设**（放行规则无效），据此更正为上面的结论。
> 放行规则的代码仍然保留：它对没有第三方 WFP 过滤器的机器是合理且无害的加固，
> 但它**无法**解决本机的拦截。

> **尚待最终确认的一步**：上面是强证据链（本机唯一具备传输层拦截能力的驱动就是火绒），
> 但"火绒=拦截者"的最后一步确认需要实机操作——临时退出火绒或关闭其"网络入侵拦截"后重跑
> 探针（见第 5.3 节）；若 TUN 行从 `0/6` 变为 `6/6`，即完全证实。此步尚未执行。

## 4. 修复

分两件事，**有效性不同**：

- **（本机真正有效）不再依赖"注入报文被放行"**：让宿主域名解析走回环代理
  （第 4.2 节）。回环路径不受任何 WFP 过滤器影响，因此 DNS/网页恢复正常。
- **（加固，本机不足以生效）** 隧道网段的入站放行规则（第 4.1 节）：对没有第三方
  WFP 过滤器的机器是合理加固，但**无法覆盖火绒这类驱动**。
- **ICMP 需要用户侧放行**：echo reply 只能靠"注入 TUN"交付，第三方防火墙不放行就无法工作；
  请在火绒中放行（见第 5.3 节），代码无法绕过内核 callout。

### 4.1 入站放行规则（加固）

- `windows/ppp/win32/network/Firewall.h`
  新增 `Fw::AllowTunnelInbound(rule_name, interface_name, local_addresses, enabled)`。
- `windows/ppp/win32/network/Win32Firewall.cpp`
  通过 `INetFwPolicy2`/`INetFwRule` 创建 `<rule_name> UDP` 与 `<rule_name> ICMPv4` 两条
  **入站 Allow** 规则：`Profiles=ALL`、`Direction=In`、`Action=Allow`，
  **`LocalAddresses=<隧道网段>`**（入站规则的有效作用域；`Interfaces` 属性文档上只对出站规则有效，
  这里仅作 best-effort），建前先删同名规则、建后回读校验、失败即返回 false。
  属性设置顺序遵循 [INetFwRule 文档](https://learn.microsoft.com/en-us/windows/win32/api/netfw/nn-netfw-inetfwrule)
  的要求：**ICMP 规则必须先设协议再 `Add`**，否则改动会被拒绝且规则静默丢失——
  因此协议在 `Enabled`/`Add` 之前最后设置，`put_Interfaces` 的失败也不再污染 `hr`。
- `main.cpp`（`PppApplication::OnTick`）
  服务端分配的地址要到连接后才可知，因此在每秒 tick 里从 `ITap::IPAddress/SubmaskAddress`
  计算隧道网段（`<net>/<prefix>`），**仅在网段变化时**重建规则；`Dispose()` 中按名删除。
  说明：`ITap::IPAddress` 就是最终生效地址——Wintun 注入校验要求 `ip->dest == IPAddress`
  且现场 `validate_failures=0`，可反证二者一致。
### 4.2 隧道网卡解析器改为回环代理优先（本机有效）

- `ppp/app/client/VEthernetNetworkSwitcher.cpp`（`ApplyNetworkTakeover`，DNS 交接处）：
  TUN 的解析器列表由 `{网关}` 改为 **`{127.0.0.1, 网关}`**（当 `need_loopback_v4` 为真），
  回环代理优先、隧道网关保留为回退。理由：宿主域名解析不应依赖"注入报文被放行"这一环；
  回环代理不被任何 WFP 过滤器影响，且走的是**完全相同的分派路径**——
  `DispatchLocalDnsQuery` 内部完成 direct/tunnel 判定（同文件 `4369-4472`），
  其完成回调统一做 AAAA 剥离、`ObserveGeoDnsResponse` 与 `vdns::AddCache`（`4533-4556`），
  因此回环路径与 TUN 内查询在策略、缓存、geo 观测上等价，差别仅在交付方式。
- **必须改在 `system_dns_strings` 上，而不是单独 `SetDnsAddresses` 一次**：
  DNS guard 每 30 秒执行 `ClearDnsAddresses(dns_if_index)` +
  `SetDnsAddresses(dns_if_index, system_dns_strings)` 把该列表重新下发
  （同文件 `5679-5681`，lambda **按值捕获** `system_dns_strings`）。
  方案 A 的手工 DNS 修改之所以"看不出效果"（`nslookup` 仍显示 `192.168.14.1`），
  正是被这个 guard 在 30 秒内覆盖——已实测确认。

## 5. 验证

### 5.1 一条命令完成（推荐）

管理员 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\openppp2\tests\tools\Invoke-TunFixCheck.ps1 -Apply
```

它做两件**互相独立、可回滚**的事，然后自动跑探针 + `ping` + `nslookup`：

1. **防火墙**：创建 `openppp2 TUN test UDP` / `openppp2 TUN test ICMPv4` 两条入站 Allow
   （协议 + `LocalAddress <隧道网段>` + `InterfaceAlias`，与代码实现一致）→ 修复 ICMP 与注入类 DNS。
2. **DNS 加固**：把隧道网卡的解析器从虚拟网关改为**回环代理 `127.0.0.1`**。
   回环路径不受防火墙影响（实测 6/6），因此**无需等编译**就能立刻恢复域名解析与网页访问；
   原值备份到 `%TEMP%\openppp2-tun-dns-backup.txt`。

> ⚠️ 本机实测（见第 3 节）：第 1 项**不会生效**，因为拦截者是火绒的 WFP 驱动；
> 第 2 项也会在 30 秒内被客户端的 DNS guard 覆盖。要立刻见效请用第 5.3（放行火绒）
> 或第 5.4（`-KeepDnsSeconds`）两种方式。

回滚（管理员）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\openppp2\tests\tools\Invoke-TunFixCheck.ps1 -Rollback
```

（重启核心后，客户端会自行把隧道网卡 DNS 重新设为虚拟网关，所以第 2 项本身也是自恢复的。）

### 5.2 手工分步（等价）

管理员 PowerShell（两条作用域与代码实现一致：
`-LocalAddress` 是入站规则真正起作用的**必要**作用域，`-InterfaceAlias` 为附加收紧）：

```powershell
New-NetFirewallRule -DisplayName 'openppp2 TUN test UDP'    -Direction Inbound -Action Allow -Protocol UDP    -InterfaceAlias 'PPP' -LocalAddress 192.168.14.0/24 -Profile Any
New-NetFirewallRule -DisplayName 'openppp2 TUN test ICMPv4' -Direction Inbound -Action Allow -Protocol ICMPv4 -InterfaceAlias 'PPP' -LocalAddress 192.168.14.0/24 -Profile Any
```

普通权限运行探针与手工检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/tools/TunInboundProbe.ps1
ping 192.168.14.1
nslookup www.baidu.com
```

期望：两行均 `replies 6/6`、丢弃计数不再增长、ping 通、nslookup 出结果。
验证后清理临时规则：`Remove-NetFirewallRule -DisplayName 'openppp2 TUN test *'`。

### 5.3 本机必做：放行第三方安全软件（火绒）

第 3 节已证明：本机的拦截来自**火绒的 WFP 驱动 `hrwfpdrv`**，Windows 防火墙规则无法覆盖它。
因此**第 5.1/5.2 节的防火墙部分在本机不会生效**（已实测），必须二选一：

- **放行**：在火绒中为隧道放行——"网络入侵拦截"/"联网控制"/"自定义规则"里
  允许 `ppp-web.exe` 与隧道网卡的 **UDP、ICMP**（或把该隧道网络标记为受信任）。
- **确认性测试**：临时退出火绒、或关闭其"网络入侵拦截"，再运行探针：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/tools/TunInboundProbe.ps1
ping 192.168.14.1
```

如果 TUN 行由 `0/6` 变成 `6/6`、`ping` 出现回包、丢弃计数不再增长，即**证实拦截者是火绒**。

> **DNS/网页不需要这一步**：第 4.2 节的改动让宿主解析走回环代理（不受 WFP 过滤影响）即可恢复。
> 只有 **ICMP** 与"注入式交付"必须依赖用户侧放行——代码无法绕过内核 callout。

### 5.4 不改代码时的临时 DNS 缓解

客户端的 DNS guard 每 30 秒会把自己那份解析器列表重新写回隧道网卡
（第 4.2 节），所以**手工把隧道网卡 DNS 改成 `127.0.0.1` 会在 30 秒内被覆盖**（已实测）。
需要在不重编译的情况下立刻恢复解析时，用脚本的临时重写循环：

```powershell
# 管理员；每 10 秒重写一次，持续 15 分钟（到期自动停止）
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\openppp2\tests\tools\Invoke-TunFixCheck.ps1 -Apply -KeepDnsSeconds 900
# 之后回滚
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\openppp2\tests\tools\Invoke-TunFixCheck.ps1 -Rollback
```

`-Rollback` 会同时停掉这个循环、删除测试规则、恢复隧道网卡原解析器。

### 5.5 在安全软件照常运行的前提下解决（三个层次）

目标：**火绒保持开启、不做任何放行**，隧道仍可用。按可行性分三层：

| 层次 | 覆盖症状 | 状态 |
|---|---|---|
| **L1 解析走回环代理**（第 4.2 节，已实现） | DNS、网页（HTTP/HTTPS） | ✅ 代码已就绪，与安全软件无关 |
| **L2 把隧道网卡设为"专用网络"** | ICMP + 注入式交付（DNS/ICMP 都能走） | ⏳ 待验证，见下 |
| **L3 在安全软件内放行**（第 5.3 节） | 同上 | 兜底方案，需要用户配置一条规则 |

**L2 的原理与判定**：新创建的隧道网卡会被 Windows 归类为 **Public（公用/不受信任）**，
部分第三方 WFP 驱动会按"不受信任网络上的无主入站报文"直接丢弃。用一条命令即可验证：

```powershell
# 管理员；自动判定并在无效时回滚网络类别
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\openppp2\tests\tools\Try-TunNetworkZone.ps1 -Keep
```

- 输出 **`Verdict: SUCCESS`** → 触发条件就是网络类别，**客户端可以在接管时自己设为 Private**，
  于是"火绒全程正常、零配置"即可工作（需把这一步写进代码）。
- 输出 **`Verdict: NOT the trigger`** → 安全软件是按别的依据过滤的（例如
  接口类型/无主报文），只能走 L3 放行；此时 L1 仍然让 DNS 与网页正常工作，仅 `ping` 不可用。

> 关于 ICMP：echo reply 只能由客户端**注入 TUN** 交付，纯代码无法绕过内核 callout，
> 所以 ICMP 必然落在 L2 或 L3。对实际上网影响很小（浏览器走 TCP/HTTPS），
> 但 PMTU 依赖 ICMP——用第 6.2 节的 `tun.mtu=1400` 可以规避由此引起的偶发卡顿。

## 6. 遗留问题（与本次根因无关，另行处理）

### 6.1 IPv6 应答导致部分应用停顿（已实测，非本次根因）

TUN 会安装 `::/1` + `8000::/1` 黑洞路由（刻意防泄漏），TUN 自身没有 IPv6 地址，
而 DNS 仍会把 AAAA 记录交给应用。实测 Windows 的源地址选择：

```
对全局 IPv6 目标做 UDP connect：
  dst=2606:4700:4700::1111  ->  source=[fe80::1ceb:7785:756d:cca9%71]   ← 活动 TUN 的 link-local
  dst=2001:4860:4860::8888  ->  source=[fe80::1ceb:7785:756d:cca9%71]
curl -6 http://[2606:4700:4700::1111]/  ->  code=000，耗尽 12s 超时（挂起，而非快速失败）
```

即：全局 IPv6 目标被路由到 TUN 黑洞，源地址取自 TUN 的 link-local，报文被客户端丢弃，
**不做 Happy Eyeballs 的应用会等到自己超时**。带 Happy Eyeballs 的浏览器会较快回落到 IPv4。

- **不是由遗留 TAP 网卡引起的**（那些 `fd00::...` 地址处于 `Tentative` 状态、未被选中）。
- **无需重编译的缓解**：在 `config\ZGO.json` 的 `udp.dns` 里加 `"prefer_ipv4": true`
  （该键默认 `false`；现有 10 个服务器配置里只有 `HKBN.json` 设了它）。它会在存在 A 记录时
  剥离 AAAA，从而避免应用尝试被黑洞的 IPv6。改完在网页端“应用设置并重启核心”即可生效。
- **后续代码改进（未实施）**：客户端在**没有 IPv6 数据面**时（`ipv6_client_state_.DefaultRouteApplied`
  为假、即只装了黑洞路由）应自动剥离 AAAA，而不是依赖用户显式开启 `prefer_ipv4`——
  把一个不可达的 IPv6 地址交给应用本身就是缺陷。

### 6.2 吞吐与"偶发卡顿"（已量化，尚未定位到根因）

同一台机器、同一时刻的对照（都经隧道，只有入口不同）：

| 测试 | 结果 |
|---|---|
| 1 MB 纯 HTTP **经 TUN**（ctcp 路径） | ✅ `200`，1 048 576 字节，**4.69 s / 223 KB/s** |
| 同一文件**经核心 HTTP 代理 8080** | ✅ `200`，1 048 576 字节，2.83 s / 370 KB/s |
| 国内站点（bypass，物理网卡） | ✅ `301`，0.08 s |
| `http://neverssl.com/`（约 4 KB，经 TUN） | ⚠️ `200` 但耗时 **16.3 s** |
| `https://example.com/`（经 TUN，TLS1.3） | ⚠️ **时好时坏**：同一条命令先是 ALPN 协商成功后 **31 s 收不到任何字节**，重跑 1.7 s 成功 |
| `https://www.baidu.com/`（经 TUN） | ⚠️ 一次 90 s 未建连 |

要点：

- **隧道吞吐本身可用**（223 KB/s），大文件可以跑完，因此不是"隧道带宽坏了"。
- 部分 90 s 级"卡住"其实是 **curl 的 DNS 阻塞**：`curl --max-time` 不会中断 Windows 上的域名解析，
  而宿主解析当前被防火墙问题拖住（即使给了 `--resolve`，curl 仍可能去查 AAAA）。这与第 2 节同源。
- 但仍存在**真实的偶发卡顿**（tunnel 内 TLS 握手后 0 字节），且核心计数器佐证连接层不稳：
  `duplicate_syn_count` 从 894 涨到 1858（宿主反复重传 SYN）、
  `static_echo.response_timeouts` 从 1 涨到 37、`receive_errors` 从 9 涨到 45。
- **无需重编译的缓解建议**（改 `config\ZGO.json` 后重启核心）：
  `client.tun.mtu` 由 `1500` 改回 `1400`（只会显式给 mtu 时，MSS 会自动推导为
  `mtu-40 = 1360`），提高封装后的路径余量。改完再重复上表的对照以确认卡顿是否消失。

### 6.3 其它

- `config\ZGO.json` 的 `client.tun.mtu = 1500`：隧道有封装开销，建议回到文档推荐的 `1400`。
- 遗留 5 个旧 TAP 网卡（`PPP 1`/`PPP 2`/`以太网 2`/`以太网 3`/`PPP PRIVATE NETWORK 2 TAP`），
  各带一条 metric 36 的 `0.0.0.0/0`（当前不抢占选路，但属残留），可用
  `tests/tools/Remove-StaleTunAdapters.ps1` 先看再删（默认 dry-run，需管理员 `-Apply`）。
- `dataplane.delayed_syn.closed_before_replay` 在现场二进制里为 `376/495`（约 76%），
  说明延迟 SYN 回放路径有损耗；但**当前工作区源码里不存在该诊断块**（见第 7 节），暂无法定位。

### 6.4 已核对但确认无缺陷的点

- `appsettings.json` 缺失 `udp.dns` 时不会丢默认值：`Loaded()` 在
  `AppConfiguration.cpp:659-660` 对 `timeout < 1` 回退到 `PPP_DEFAULT_DNS_TIMEOUT`。
- `prefer_ipv4` 的 `false` 默认是**刻意设计**（`AppConfiguration.h:96` 的类内初始值），
  不是"缺键即丢失默认值"缺陷；但也正因为它默认关闭，本机才出现 6.1 的现象。

## 7. 注意：运行中的二进制与工作区源码版本不一致

现场 `get_snapshot` 返回 `dataplane.delayed_syn.{stored,replayed,closed_before_replay,...}`，
且日志中 `VNetstack.cpp` 的行号（367）与工作区（370）相差 3 行；而
`D:\github\openppp2` 全仓库检索 `delayed_syn` / `closed_before_replay` / `take_rejected`
**均无任何匹配**。

因此：`C:\Users\2233\Desktop\TUI\ppp-web.exe`（CI 产物，日志内 `__FILE__` 为 `D:\a\openppp2\...`）
**不是**由当前工作区源码构建的。重新构建前请先确认工作区与 CI 所提交的版本一致，
否则本次修复可能打在不同版本上，或重建后丢失 `delayed_syn` 诊断。

## 8. 探针与日志（本次新增）

这一节就是"让这类故障能被立刻看见"的部分。

### 8.1 客户端自测：注入投递探针（代码内）

`ppp/app/client/VEthernetNetworkSwitcher.cpp` 新增
`RunTunInjectionSelfTest(now)`（`OnTick` 中每 120 秒触发一次，失败则 5 秒后重试）：

1. 在隧道地址上绑定一个 UDP socket（临时端口）；
2. 用**与 DNS 应答完全相同的构造路径** `DatagramOutput(host_ep, gateway_ep, ...)`
   向宿主注入 3 个探针包（源=隧道网关，目的=该 socket）；
3. 等待每个包最多 700 ms，统计宿主是否真的收到；
4. 打印结论并记录计数。

对应日志（`log_level=debug` 时都会出现）：

| 日志 | 含义 |
|---|---|
| `TUN injection probe: host received 2/3 injected packets; the client can deliver tunnel traffic to the host stack` | 注入路径正常（INFO，仅在首次） |
| `TUN injection probe: host received 0/3 injected packets. Packets written into the tunnel adapter are discarded before they reach the host stack, ...` | **注入被宿主栈丢弃**（WARN）——即本文档描述的故障状态，附带排查指引（`TunInboundProbe.ps1`、`--tun-driver=tap`） |
| `TUN injection probe: cannot bind a UDP probe socket on the tunnel address, error=...` | 探针自身无法绑定（ERROR） |
| `TUN injection probe: tap not ready, nothing injected` | 网卡尚未就绪（DEBUG，5 秒后重试） |

头文件里对应 `injection_probe_sent_ / received_ / last_ms_ / running_ / healthy_ / reported_`
等原子计数，便于后续接入 RPC 快照（尚未接入，见 8.4）。

### 8.2 脚本探针：`tests/tools/TunInboundProbe.ps1`（已增强）

新增**分层 WFP 丢弃计数**，这是最快的判据：

```
WFP discard growth for 6 queries to the tunnel gateway, by layer:
  IPPACKET   +0
  TRANSPORT  +20
  ALE        +0
  transport-only growth with a flat ALE layer = not Windows Firewall rule processing
```

- `TRANSPORT` 涨、`ALE` 平 → **不是** Windows 防火墙的规则判定，加放行规则没有意义；
- 同时仍会列出具备拦截能力的第三方 WFP/NDIS 驱动，但**只作为线索**（本机实测该线索是误导的：
  该层没有过滤器，且那个产品的 NDIS 驱动并未绑定隧道网卡）。

### 8.3 脚本探针：`tests/tools/Invoke-WintunInjectTest.ps1`（新增，独立复现）

不依赖客户端，自己用 `wintun.dll` 打开网卡、开 session、构造完整 IPv4/UDP 报文并注入，
然后检查宿主 socket 是否收到；带 `-Tos / -Id / -NoDf / -BadUdpChecksum` 开关用于**逐字段二分**
（客户端注入报文使用的正是 `tos=0x68 / id=0 / DF=1 / 计算校验和`）。

注意：`WintunStartSession` **同一网卡同时只能有一个会话**，客户端运行时它会失败
（脚本会打印 `win32=1247` 并说明），所以它适合在客户端停止时使用，或改用 8.1 的客户端自测。

### 8.4 尚未完成

- 探针结果**还没有**接入 RPC 快照 / 网页端展示（需要在 `IVirtualEthernet` 上加访问器），
  目前只能从核心日志读取；
- 注入被丢弃的**确切原因**仍未定位（已排除：防火墙策略、该层 WFP 过滤器、火绒与其余第三方
  NDIS 过滤驱动）。下一步方向：用 8.3 的测试器在**空闲网卡**上做字段二分，
  以及对比 `--tun-driver=wintun` 与 `--tun-driver=tap` 两条收包路径。

