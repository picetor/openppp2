# TUN 模式 IPv6 场景矩阵与运维判据（2026-10 修订）

**这份文档回答什么问题**：在 TUN 模式下，客户端到底能不能走 IPv6、AAAA 记录会不会被剥离、
IPv6 报文最终去了哪里，取决于三个互相独立的变量——**宿主物理网卡有没有 IPv6 出口（H）**、
**服务端有没有 IPv6 数据面（S）**、**客户端有没有把服务端下发的隧道 IPv6 地址真正应用成功（A）**；
本文给出 2×2×2 八种组合下的可观测行为、日志判据与验证命令，并列出现存缺口。
**一句话前提**：客户端的 IPv6 能力来自服务端 INFORMATION 包尾部 ExtendedJson 里的
`AssignedIPv6Mode` 等字段，**不是**本地配置——客户端配置里根本没有 `client.tun.ipv6` /
`client.tunnel.ipv6` 这类键，客户端也从不主动请求 IPv6 地址。

> 适用范围：Windows 为主（泄漏阻断、本地 DNS 代理、`--tun-vnet` 都在这条链上），
> Linux/macOS/Android/proxy-only 的差异见第五节。
> 引用方式：文件 + 函数名（仓库正被并发编辑，行号会漂移）。
> 修订记录：本次已修复 4 处代码（6.1–6.4），另有 2 处部分修复（6.5、6.6）；仍未修复的缺口见 6.7–6.14。

---

## 一、IPv6 能力从哪来

### 1.1 握手链路（服务端 → 客户端）

```text
服务端 Establish() / UpdateIPv6Information() / UpdateIPv6Request()
  → VirtualEthernetSwitcher::BuildInformationEnvelope
  → VirtualEthernetSwitcher::BuildInformationIPv6Extensions(session_id, extensions)
  → envelope.ExtendedJson = extensions.ToJson()
  → VirtualEthernetLinklayer::DoInformation（INFO 头 + 2 字节网络序 JSON 长度 + JSON）
客户端 VirtualEthernetLinklayer::PacketInput（解析 json_len → FromJson）
  → VEthernetExchanger::OnInformation(transmission, InformationEnvelope&, y)
  → （仅主出口且 ExtendedJson 非空）VEthernetNetworkSwitcher::ApplyIPv6Assignment
```

客户端侧调用门槛（`ppp/app/client/VEthernetExchanger.cpp` `VEthernetExchanger::OnInformation`）：

```cpp
information_extensions_ = information.Extensions;              // 缓存，供热切换重放
assigned_ipv6_address_  = information.Extensions.AssignedIPv6Address.is_v6()
    ? information.Extensions.AssignedIPv6Address : boost::asio::ip::address();
if (primary_outbound_.load() && !information.ExtendedJson.empty()) {
    switcher_->ApplyIPv6Assignment(information.Extensions, source);
}
```

要点：

* **非主出口**或 **ExtendedJson 为空**（旧版对端、或服务端根本没发扩展）→ 一次都不进
  `ApplyIPv6Assignment`，客户端按“无 IPv6 数据面”处理（`ipv6_dataplane_unavailable_` 初值为 true）。
* 热切换主出口时 `VEthernetNetworkSwitcher::CompletePendingOutboundSwitch` 末尾会用缓存的
  `target->GetInformationExtensions()` 再调一次 `ApplyIPv6Assignment`（幂等），
  同时把 `prefer_ipv4_` 换成新出口配置的值并 `vdns::ClearCache()`。

### 1.2 扩展字段（客户端能看到的全部 IPv6 信息）

| 字段 | 类型/取值 | 客户端用途 |
|---|---|---|
| `AssignedIPv6Mode` | 0=None / 1=Nat66 / 2=Gua | **唯一的能力信号**；决定 `has_ipv6_dataplane` 与 `nat_mode` |
| `AssignedIPv6Address` | v6 地址 | 是否真的能承载 IPv6 的判据（本次修复后参与判定）；应用到 TAP |
| `AssignedIPv6AddressPrefixLength` | 0 或 128 | 为 0 时客户端按 64 处理 |
| `AssignedIPv6Gateway` | v6 地址 | 决定是否安装 `::/1`+`8000::/1`；缺失则整段跳过 |
| `AssignedIPv6RoutePrefix` / `…RoutePrefixLength` | v6 前缀 / 长度 | NAT66 东西向子网路由（服务端 `server.subnet=true` 且前缀为真前缀时才有） |
| `AssignedIPv6Dns1` / `Dns2` | v6 地址 | 仅 Linux/macOS 客户端会写入 TAP；Windows 会清空 TAP 的 IPv6 DNS |
| `AssignedIPv6Flags` | bit0 = NeighborProxy | 服务端 GUA 模式标识，客户端目前未消费 |
| `IPv6StatusCode` / `IPv6StatusMessage` | 见 1.4 | 仅用于日志/诊断，不参与客户端判定 |

客户端判定（`VEthernetNetworkSwitcher::ApplyIPv6Assignment` 开头，**本次修复后**）：

```cpp
const bool mode_ok = AssignedIPv6Mode == IPv6Mode_Nat66 || AssignedIPv6Mode == IPv6Mode_Gua;
const bool has_ipv6_dataplane = mode_ok &&
    AssignedIPv6Address.is_v6() && !AssignedIPv6Address.is_unspecified();
ipv6_server_has_dataplane_ = has_ipv6_dataplane;
ipv6_dataplane_unavailable_.store(!has_ipv6_dataplane, std::memory_order_relaxed);
```

三个状态位（`VEthernetNetworkSwitcher.h`，均有只读访问器）：

| 成员 / 访问器 | 含义 | 写入点 |
|---|---|---|
| `ipv6_server_has_dataplane_` / `ServerHasIPv6DataPlane()` | 服务端最近一次扩展是否声明了可用数据面 | 构造函数（false）、`ApplyIPv6Assignment`、`Open`（Windows 置 false） |
| `ipv6_dataplane_unavailable_` / `IsIPv6DataPlaneUnavailable()` | 客户端当前**没有**可用隧道 IPv6 数据面（驱动 AAAA 剥离） | 初值 true、`ApplyIPv6Assignment`（无数据面 / 路由应用失败）、`Open` / Dispose 路径 |
| `host_ipv6_uplink_` / `HostHasIPv6Uplink()` | 物理网卡是否有 IPv6 网关（驱动“直连名字是否保留 AAAA”） | 仅 `VEthernetNetworkSwitcher::Open` 计算一次；`ReleaseAllObjects` 置 false |

### 1.3 服务端在什么条件下下发 IPv6

进程级门槛：

* `VirtualEthernetSwitcher::IsIPv6ServerEnabled()` = `SupportsIPv6DataPlane() && mode ∈ {Nat66,Gua}`。
* `SupportsIPv6DataPlane()`：Linux 恒 true；Windows/macOS 仅在 mode 为 nat66/gua 时 true；其它平台 false。
* `VirtualEthernetSwitcher::Open`：配置了 IPv6 但平台不支持 → 记录 `IPv6Nat66Unavailable` /
  `PlatformNotSupportGUAMode`，`ipv6_runtime_state_ = 3`，**服务端起不来**。
* `OpenIPv6TransitIfNeed()`：为 NAT66/GUA 建 transit TAP；网关取 `GetIPv6TransitGateway()`，拿不到直接失败。
* 运行时状态 `ipv6_runtime_state_`：0=off，1=nat66，2=gua，3=failed（原因在 `ipv6_runtime_cause_`）。

配置键与默认值：

| 键 | 默认 | 说明 |
|---|---|---|
| `server.ipv6.mode` | 空 → None | 只认 `nat66` / `gua` 两个字符串 |
| `server.ipv6.cidr` | NAT66 为空时 `fd42:4242:4242::/64` | 无 `/` 时按 64；GUA 必须是 `2000::/3` |
| `server.ipv6.prefix-length` | 类内初值 128 | **必须严格介于 0 与 128 之间**，否则整个 IPv6 被禁用 |
| `server.ipv6.gateway` | 自动推导 | 必须在前缀内且不是网络地址；否则清空并取前缀第一个主机地址 |
| `server.ipv6.dns1/2` | 空 | 随扩展下发，Windows 客户端不用 |
| `server.ipv6.lease-time` | 300 | ≤0 表示永不过期 |
| `server.ipv6.static-addresses` | 空 | GUID → 固定地址，冲突即整次下发被拒 |
| `server.subnet` | — | 仅决定是否下发 `AssignedIPv6RoutePrefix`（东西向）；**不影响** IPv6 转发是否启用 |
| `server.mapping` | — | FRP 端口映射，按 IPv4/IPv6 分表存在，但与 IPv6 地址分配无关 |

单会话分配（`BuildInformationIPv6Extensions`）：

* 未启用 → `"server-ipv6-disabled"`（状态码 5），Mode 保持 None。
* NAT66：模式 Nat66，地址前缀长度 128；地址来源 = `static-addresses` → 客户端请求（本客户端永不发）
  → 复用旧租约（`"lease-reused"`）→ `build_stable_ipv6()`（HMAC-SHA256 派生 + 8 种异或重试）；
  `server.subnet` 且前缀为真前缀时附 `AssignedIPv6RoutePrefix`；网关可为空。
* GUA：模式 Gua、flag NeighborProxy；必须有 transit 网关（否则 `"server-gua-requires-gateway"`）。
* 任何失败路径都会 `extensions.Clear()`（连状态码一起清），客户端只看得到 `Mode=None`。
* 装数据面（`AddIPv6Exchanger`：`/128` 转路由 + NAT66 邻居 / GUA NDP 代理）失败时，
  三处失败块会清空地址/网关/前缀/DNS/Flags，**并在本次修复中把 `AssignedIPv6Mode` 也置回 None**，
  状态码 6 `IPv6Status_Failed` + `"server-ipv6-dataplane-install-failed"`。

状态码一览（`VirtualEthernetInformationExtensions::IPv6Status`）：
0 None、1 Applied、2 ServerAssigned、3 ClientRequested、4 UnsupportedClient、5 Rejected、6 Failed。

### 1.4 客户端能看到的关键日志（DEBUG）

```text
PacketInput: INFO extensions parsed, json_len=%u, AssignedIPv6Mode=%d, AssignedIPv6Address=%s, AssignedIPv6Gateway=%s, ExtendedJson=%.200s
PacketInput: INFO no extensions in payload, json_len=%u, remaining=%d
VEthernetExchanger::OnInformation: outbound=%s, primary=%d, ExtendedJson.empty()=%d, len=%d, AssignedIPv6Mode=%d
VEthernetNetworkSwitcher::ApplyIPv6Assignment: entered, AssignedIPv6Mode=%d, AssignedIPv6Address=%s, AssignedIPv6Gateway=%s, AssignedIPv6Dns1=%s, AssignedIPv6Dns2=%s
```

---

## 二、客户端如何应用（`ApplyIPv6Assignment`，`ppp/app/client/VEthernetNetworkSwitcher.cpp`）

### 2.1 分支表

| 分支 | 条件 | 动作 | 失败/副作用 |
|---|---|---|---|
| B0 | `IsProxyOnly()` | 打印 `…: proxy-only mode, skipping IPv6 assignment` 后 return | 不改状态；v6 服务器端点由 `EnsureWindowsIPv6ServerRoute` 单独保 `/128` |
| B1 | 恒执行 | 计算 `mode_ok` / `has_ipv6_dataplane`（含地址校验）；写 `ipv6_server_has_dataplane_` 与 `ipv6_dataplane_unavailable_` | 判据 = 模式 ∈ {Nat66,Gua} **且** 地址是 v6 且非 unspecified（本次修复） |
| B2 | `!has_ipv6_dataplane` | `RestoreIPv6Assignment()`；删除接口不是物理网卡的 `geo_dynamic_routes6_`；Windows 上 `ApplyWindowsIPv6LeakBlockRoutes()` | 日志 `…: server has no IPv6 data plane, restoring leak block`；保留直连 /128 |
| B3 | `GetTap()==nullptr` | 打印 `…: GetTap() returned null, returning` | 提前返回；见 6.5 |
| B4 | 地址/网关/前缀/nat_mode 与上次不同 | `RestoreIPv6Assignment()` 重来并重新快照 | 完全相同则跳过（幂等） |
| B5 | 有地址且 `!state.AddressApplied` | `ApplyClientAddress()`；成功则 `tap->IPv6Address = addr` | 失败日志 `…: address not applied (rc=0), keeping tap->IPv6Address=%s`（Android 依赖它保留 TUN 真实地址做翻译） |
| B5b | 服务器端点本身是 v6 | Windows `EnsureWindowsIPv6ServerRoute`；Linux `ip -6 route replace <srv>/128 via <gw6> dev <nic>`；macOS `route -n add -inet6` | 无 v6 网关时 DEBUG `…ApplyIPv6Configuration: skip pinning server IPv6 route…` |
| B6 | 有网关且 `!state.DefaultRouteApplied` | Windows 先 `RemoveWindowsIPv6LeakBlock()`；`ApplyClientDefaultRoute()` 装 `::/1`+`8000::/1`（+ 静态邻居 + ULA 优先策略） | 失败 → 恢复黑洞 + ERROR `…: managed IPv6 route failed, leak block restored=%d`；成功/失败都会写 `ipv6_dataplane_unavailable_.store(!route_applied)` |
| B6b | 没有网关 | **整个 B6 跳过** | `ipv6_dataplane_unavailable_` 停留在 B1 的值（见 6.5） |
| B7 | 有 `AssignedIPv6RoutePrefix` | `ApplyClientSubnetRoute()`（仅 NAT66） | 失败只影响客户端之间互访 |
| B8 | Windows | `DisableClientTemporaryAddress()` 删除 TAP 上除分配地址外的其它 v6 地址（保留 link-local） | 避免服务端 NAT66 `/128` 回程查不到会话 |
| B9 | DNS | Windows：清空 TAP 的 IPv6 DNS（`…: cleared TUN IPv6 DNS (prefer IPv4), ifIndex=%d`）；Linux/macOS：把 `Dns1/2` 写入 TAP | Windows 的物理网卡 DNS 由 takeover 事务管理 |

### 2.2 `::/1` + `8000::/1` 的三个来源与删除点

| 来源 | 安装函数 | 下一跳 | 删除函数 |
|---|---|---|---|
| Windows 防泄漏黑洞 | `VEthernetNetworkSwitcher::ApplyWindowsIPv6LeakBlockRoutes()` | 未指定（on-link → TUN） | `RemoveWindowsIPv6LeakBlock()` |
| Windows 受管数据面 | `ppp::win32::ipv6::auxiliary::ApplyClientDefaultRoute()` | 服务端分配的虚拟网关 | `ppp::win32::ipv6::auxiliary::RestoreClientConfiguration()` |
| Linux / macOS 受管数据面 | `ppp::linux::ipv6::auxiliary::ApplyClientDefaultRoute()` / `ppp::darwin::…` | 服务端网关（网关为空且 nat_mode 时为 dev 路由） | 对应 `RestoreClientConfiguration()` |

黑洞的附加动作（Windows）：`SetIPv6IgnoreDefaultRoutes(物理网卡, true)` +
`CaptureAndDeleteIPv6DefaultRoutes(物理网卡)`，即“删掉物理默认路由并禁止重新学习”，
所以 `::/1`+`8000::/1` 是**唯一**能匹配全局目标的窄路由：
日志 `ApplyWindowsIPv6LeakBlockRoutes: installed selective block on tun=%d, physical=%s, removed_defaults=%d, direct_ipv6=allowed`。
关闭时 `RemoveWindowsIPv6LeakBlock()` 会恢复 `IgnoreDefaultRoutes` 与捕获到的物理默认路由
（`…: restored physical IPv6 defaults=%d`）。

### 2.3 恢复路径

* `RestoreIPv6Assignment()`：INFO `…: restoring interface=%d, address=%s/%d`，调
  `RestoreClientConfiguration()`（删 `/1` 路由、静态邻居、分配地址、DNS、ULA prefix policy，
  恢复原始默认路由），清空 `tap->IPv6Address/IPv6GatewayServer` 与快照标志。
* 调用者：`ApplyIPv6Assignment`（B2/B4）、`RestoreNetworkState`（经 `RestoreNetworkTakeover(true)`）。
  临时主出口故障 → 重新装回黑洞（fail-closed）；只有 `Dispose()` 才把宿主 IPv6 还给物理网卡。
* `Open`（Windows，非 proxy-only）在建 TAP 后立即
  `ipv6_dataplane_unavailable_.store(true)` 并 `ApplyWindowsIPv6LeakBlockRoutes()`，失败则
  `Open: initial IPv6 leak block failed` 并启动失败——**握手前宿主 IPv6 就是黑洞状态**。

---

## 三、DNS / AAAA 决策表

### 3.1 参与决策的输入

| 输入 | 来源 | 含义 |
|---|---|---|
| `prefer_ipv4_` | 配置 `udp.dns.prefer_ipv4`（默认 false）；热切换时按新出口改写 | 用户显式偏好 |
| `ipv6_dataplane_unavailable_` | `ApplyIPv6Assignment` B1/B6 | 隧道不能承载 IPv6 |
| `host_ipv6_uplink_` | `Open` 计算 | 物理网卡能直连 IPv6 |
| 路径 | `direct_query`（geo-rules Direct 或 dns-rules 带 Nic） | 隧道应答 vs 直连应答 |
| A 缓存 | `vdns::QueryCache2(domain, kA)` | 该域名是否已有 A 记录 |
| `udp.dns.cache` | 配置（默认 true） | 是否写 vDNS 缓存（**修复后不再影响是否剥离**） |

### 3.2 `StripAAAADnsResponseIfIPv4Available(m, strip_without_a_cache)` 的判定

```cpp
if (!ShouldPreferIPv4() && !strip_without_a_cache) return true;      // 不需要 IPv4 偏好
if (m.questions[0].mType != kAAAA)                     return true;  // 非 AAAA 不处理
if (cache_result.empty() || !hasA) {                                 // 没有 A 记录
    if (!strip_without_a_cache) { LOG_DEBUG("…: no A cache for %s, keeping %d AAAA records"); return true; }
    LOG_DEBUG("…: no A cache for %s, stripping %d AAAA records because this host cannot reach IPv6");
}
剥 answers/additions 中的 AAAA；LOG_DEBUG("…: STRIPPED %d AAAA for %s, remaining AAAA=%d"); return true;
```

其中 `ShouldPreferIPv4()` = `prefer_ipv4_ || ipv6_dataplane_unavailable_`。
**注意**：本函数目前**恒返回 true**（无 `return false` 分支），注释里“hold AAAA”的语义已不存在，见 6.6。

### 3.3 决策表（修复后）

| 路径 | 数据面/宿主状态 | 用户 `prefer_ipv4` | A 缓存 | 结果 | 依据 |
|---|---|---|---|---|---|
| 隧道（DNS 回包经 `DatagramOutput` 进出 TAP） | 无隧道数据面（`dataplane_unavailable=true`） | 任意 | 有/无 A 都剥 | **AAAA 必剥，含 AAAA-only** | `DatagramOutput` 传 `strip_without_a_cache = ipv6_dataplane_unavailable_`（本次修复） |
| 隧道 | 有数据面 | false | — | 不剥 | C1 直接放行 |
| 隧道 | 有数据面 | true | 有 A | 剥 | `prefer_ipv4` + A 缓存 |
| 隧道 | 有数据面 | true | 无 A | **保留 AAAA**（纯 IPv6 站点保底） | C6 分支 |
| 直连（Windows 本地 DNS 代理 `DispatchLocalDnsQuery`） | 宿主有 v6 出口（`host_uplink=true`） | 任意 | 任意 | 不剥（除非 `prefer_ipv4`） | `no_ipv6_path=false` |
| 直连（Windows） | 宿主无 v6 出口 | 任意 | 有/无 A 都剥 | **AAAA 必剥** | `no_ipv6_path=true` → `strip_without_a_cache=true` |
| 直连（Linux/macOS） | 任意 | 任意 | 任意 | **不剥**（无本地代理，策略未实现） | 见 6.8 |

### 3.4 修复前后差异（本节是本次改动的重点）

| 场景 | 修复前 | 修复后 |
|---|---|---|
| `udp.dns.cache=false` + 无隧道数据面 | 剥离逻辑整段被跳过 → AAAA 原样交给应用 → 应用连黑洞地址挂到超时 | `DatagramOutput` 中的剥离**移出缓存分支**、无条件执行；`AddCache`/`FlushPendingAAAAResponses` 仍在缓存开关内 |
| 无隧道数据面 + 域名只有 AAAA | 无 A 缓存 → 保留 AAAA（死路） | 隧道路径以 `strip_without_a_cache=true` 调用 → AAAA-only 也剥 |
| 服务端装数据面失败 | 只清地址、保留 Mode → 客户端以为有数据面，既不装 `/1` 路由也不剥 AAAA | 服务端把 Mode 也置 None；客户端要求 Mode + 非空地址 → 判定为“无数据面”，恢复黑洞并剥 AAAA |
| 参数语义 | 单参（只看 `ShouldPreferIPv4()`） | 双参：`strip_without_a_cache` 由调用方按“这条路是否根本不能走 IPv6”决定 |

### 3.5 调用点清单

| 调用点 | 传参 | 位置/说明 |
|---|---|---|
| `VEthernetNetworkSwitcher::DatagramOutput`（IPv4 分支） | `ipv6_dataplane_unavailable_` | 隧道 DNS 回包主路径；仅当 `destinationEP.port()==53` |
| `VEthernetNetworkSwitcher::DatagramOutput`（IPv6 分支） | 同上 | 同上（v6 承载的 DNS 回包） |
| `VEthernetNetworkSwitcher::DispatchLocalDnsQuery` | `no_ipv6_path` | 仅 Windows 本地 DNS 代理 |
| `VEthernetNetworkSwitcher::OnIPv6UdpPacketInput`（直连 DNS 回包） | 默认 false | 见 6.6 的残留死分支 |
| `VEthernetNetworkSwitcher::RedirectDnsServer`（DirectDNS 回包） | 默认 false | 见 6.6 的残留死分支 |

### 3.6 其它 DNS 事实

* Windows 上 TAP 的 IPv6 DNS 会被清空，物理网卡 IPv6 DNS 由 takeover 事务接管
  （`PinDnsToLoopback(ifindex, AF_INET6)` → `::1`；失败日志
  `ApplyNetworkTakeover: cannot pin IPv6 DNS to ::1, ifIndex=%d`）。因此 Windows 上的解析多数走
  `DispatchLocalDnsQuery`，少数绕过本地代理的查询才走 `OnIPv6UdpPacketInput → DatagramOutput`。
* 进程内 vDNS 缓存与 Windows DNS 客户端缓存里**已存在**的 AAAA 不会因数据面变化而失效，
  验证时要重启核心或换一个没查过的域名。

---

## 四、组合矩阵（修复后）

记号：**H** = `host_ipv6_uplink_==true`；**S** = 服务端声明并成功装载 IPv6 数据面；
**A** = 客户端把服务端分配的地址真正应用到 TAP（`state.AddressApplied`）。
“AAAA 是否剥离”一行按第三节决策表。

| # | H | S | A | AAAA 是否剥离 | IPv6 报文去向 | 用户可见后果 | 日志特征 | 相关代码 |
|---|---|---|---|---|---|---|---|---|
| 1 | 有 | 无 | 否 | 隧道路径：**全剥（含 AAAA-only）**；Windows 直连路径 H=true：不剥 | 全局 v6 被 `::/1`+`8000::/1` 黑洞吞掉；物理默认路由被删掉并禁止重新学习；仅 bypass6/geo 直连前缀能出物理网卡 | 浏览器（Happy Eyeballs）较快回落 IPv4；`curl -6`/不做回落的程序挂到自身超时；面板 `logical_ipv6` 为空 | `ApplyIPv6Assignment: server has no IPv6 data plane, restoring leak block`；`ApplyWindowsIPv6LeakBlockRoutes: installed selective block on tun=%d, physical=%s, removed_defaults=%d, direct_ipv6=allowed`；`TranslateIPv6Packet: outbound=main has no usable IPv6 assignment`（每包一条 DEBUG） | `ApplyIPv6Assignment` B2、`ApplyWindowsIPv6LeakBlockRoutes`/`RemoveWindowsIPv6LeakBlock`、`TranslateIPv6Packet`、`Open` |
| 2 | 有 | 有 | 是 | 隧道路径：不剥（除非用户 `prefer_ipv4`）；Windows 直连路径 H=true：不剥 | 期望路径：宿主 v6 → TUN（源 = 分配地址）→ 服务端 transit TAP → 公网；回程按目的 `/128` 回注 TUN；直连前缀仍走物理网卡 | 正常双栈；面板 `logical_ipv6` 显示分配地址；`run_diagnostics` 的 `network.ipv6` 报 `server_dataplane=yes` | `ApplyIPv6Assignment: applying address=… gua=…`；`…: applying default route via gateway=… nat_mode=…`；`RemoveWindowsIPv6LeakBlock: restored physical IPv6 defaults=%d` | `ApplyIPv6Assignment` B5/B6/B9、`WIN32_IPv6Auxiliary::ApplyClientDefaultRoute`、`TranslateIPv6Packet` |
| 3 | 有 | 有 | 否 | 服务端装载失败（Mode=None）→ 退化为第 1 行（全剥 + 黑洞）；仅客户端内核应用地址失败时 → **不剥**，报文被丢 | 退化为第 1 行；或：路由装上了但 `tap->IPv6Address` 为空，报文进 TUN 即被丢 | 前者与第 1 行相同；后者表现为“面板显示有 v6 数据面但 v6 全不通” | `…: managed IPv6 route failed, leak block restored=%d`（路由失败）；`…: address not applied (rc=0), keeping tap->IPv6Address=(none)`（地址失败）；`TranslateIPv6Packet: … has no usable IPv6 assignment` | `ApplyIPv6Assignment` B5/B6、`ApplyClientAddress`、`TranslateIPv6Packet`；服务端 `Establish`/`UpdateIPv6Information`/`UpdateIPv6Request` 失败块 |
| 4 | 有 | 无 | 否 | 同第 1 行（H 只影响直连名字） | 同第 1 行 | 同第 1 行：宿主自身 IPv6 被“安全地”断掉 | 同第 1 行 | 同第 1 行 |
| 5 | 无 | 无 | 否 | 隧道路径：**全剥**；Windows 直连路径 H=false：**全剥** | 宿主本就没有全局 v6；黑洞仅为防泄漏；报文即使进 TUN 也因没有分配地址被丢 | v6 完全不可用；`logical_ipv6` 空；`netsh` 里只有 `fe80::` | 同第 1 行；`run_diagnostics` 报 `server_dataplane=no, host_uplink=no; this host has no IPv6 path at all` | 同第 1 行 + `DispatchLocalDnsQuery` 的 `no_ipv6_path` |
| 6 | 无 | 有 | 是 | 隧道路径：不剥；Windows 直连路径（`direct=1`）H=false：**剥** | 隧道承载全部 v6（宿主无 v6 不影响）：源 = 分配的 ULA/GUA，经服务端 transit 出网 | **能上 v6 网站**，`logical_ipv6` 有值；但被判“直连”的域名只剩 IPv4 → 同一站点两种结果不一致 | `ApplyIPv6Assignment: applying address=…`；`ApplyClientDefaultRoute: ULA prefix policy (fd00::/8 precedence=50 label=1) applied successfully`；`DispatchLocalDnsQuery: … STRIPPED …`（直连名） | `ApplyIPv6Assignment` B5/B6、`host_ipv6_uplink_`（`Open`）、`DispatchLocalDnsQuery` |
| 7 | 无 | 有 | 否 | 同第 3 行（服务端装载失败 → 第 1/5 行；仅客户端地址失败 → 不剥） | 同第 3 行 | 服务端支持却用不了；浏览器回落 IPv4，`curl -6` 挂起 | 同第 3 行 | 同第 3 行 |
| 8 | 无 | 无（旧版对端/无扩展） | 否 | `ApplyIPv6Assignment` 一次都不被调用 → `ipv6_dataplane_unavailable_` 保持初值 true → 隧道路径**全剥** | 同第 5 行 | 同第 5 行 | `PacketInput: INFO no extensions in payload, json_len=%u, remaining=%d`（或 `PacketInput: INFO no extension room, remaining=%d`）；没有 `ApplyIPv6Assignment: entered` | `VEthernetExchanger::OnInformation`（`ExtendedJson.empty()` 门槛）、`ipv6_dataplane_unavailable_` 初值 |

> 表注 1：第 3/7 行的“服务端有数据面但装载失败”在本次修复后**必然**以 `AssignedIPv6Mode=None` 下发，
> 客户端因此落到“无数据面”分支，行为与第 1/5 行一致；残留的 `A=否` 只来自客户端侧
> （内核地址应用失败，或路由应用失败），见 6.5。
> 表注 2：`run_diagnostics` 的 `network.ipv6` 文本三种取值对应：
> `server_dataplane=yes → IPv6 runs through the tunnel`；
> `server_dataplane=no, host_uplink=yes → only directly routed names keep IPv6 …`；
> 两者皆 no → `this host has no IPv6 path at all …`。

---

## 五、平台差异

| 平台 | 泄漏黑洞（`::/1`+`8000::/1`） | 直连名字按宿主出口剥 AAAA | IPv6 数据面闸门 | 备注 |
|---|---|---|---|---|
| Windows | 有：`ApplyWindowsIPv6LeakBlockRoutes`（删物理默认路由 + `SetIPv6IgnoreDefaultRoutes`） | 有：`DispatchLocalDnsQuery`（H=false 时 `strip_without_a_cache=true`） | `--tun-vnet`（默认 yes）+ `OnIPv6PacketInput` | TAP 建卡时自带 `fd00::…/64`（SkipAsSource、metric 5）；TAP 的 IPv6 DNS 被清空 |
| Linux / macOS | **无**：没有服务端数据面时宿主 IPv6 照常从物理网卡出去（泄漏但可用） | **无**：没有本地 DNS 代理，直连名字的 AAAA 不会被剥 | 同上（`OnIPv6PacketInput` 的 VNet 检查） | Linux 客户端通过 `ApplyClientDns` 把服务端 IPv6 DNS 写入 TAP；Linux/macOS 的 `/1` 路由用 shell 命令安装 |
| Android | 由 VpnService 的 `::/0` 承担（非本代码） | 无 | **跳过 VNet 检查**（`_ANDROID` 例外） | `ApplyClientAddress` 通常失败，`tap->IPv6Address` 由 `android/libopenppp2.cpp` 建 TAP 时镜像 VpnService 地址，`TranslateIPv6Packet` 靠它翻译 |
| proxy-only（`--mode=proxy` / `client.proxy_only`） | 无（不接管主机路由） | 不适用 | `ApplyIPv6Assignment` 直接 return | 只为隧道自身的 v6 服务器端点保留 `/128`（`EnsureWindowsIPv6ServerRoute`） |

---

## 六、缺口清单（已修复 / 仍未修复）

### 6.1 已修复（本次）：服务端“数据面装载失败”不再谎报 Mode
* 现象（修复前）：服务端清空地址/网关/前缀/DNS/Flags 却保留 `AssignedIPv6Mode=Nat66`；
  客户端只看模式 → 认为有数据面，于是既不装 `/1` 路由也不剥 AAAA，应用拿到不可达 AAAA 挂到超时
  （对应矩阵第 3/7 行）。
* 修复：`ppp/app/server/VirtualEthernetSwitcher.cpp` 的 `Establish()`、`UpdateIPv6Information()`、
  `UpdateIPv6Request()` 三处失败块新增 `AssignedIPv6Mode = IPv6Mode_None`；
  客户端 `ApplyIPv6Assignment` 的 `has_ipv6_dataplane` 收紧为
  `mode_ok && AssignedIPv6Address.is_v6() && !is_unspecified()`。

### 6.2 已修复（本次）：隧道路径的 AAAA 剥离不再依赖 `udp.dns.cache`
* 现象（修复前）：`DatagramOutput` 的剥离在 `if (caching && udp.dns.cache)` 内，关缓存即完全不剥；
  且无 A 缓存时保留 AAAA（AAAA-only 站点死路）。
* 修复：剥离移到缓存分支外，对 53 端口无条件执行，并以
  `strip_without_a_cache = ipv6_dataplane_unavailable_` 调用；`AddCache`/`FlushPendingAAAAResponses`
  仍留在缓存开关内。

### 6.3 已修复（本次）：IPv6 状态可见性
* 现象（修复前）：`ipv6_server_has_dataplane_` 只写不读，运维只能从每包 DEBUG 反推。
* 修复：`VEthernetNetworkSwitcher.h` 新增 `ServerHasIPv6DataPlane()` /
  `IsIPv6DataPlaneUnavailable()` / `HostHasIPv6Uplink()`；
  `main.cpp` 的 `get_snapshot` 输出 `network.ipv6.{server_dataplane,dataplane_unavailable,host_uplink,effective}`；
  `run_diagnostics` 新增 `network.ipv6` 检查项（三种组合各一句结论）。

### 6.4 已修复（本次）：`"return false 扣住 AAAA"` 死分支（DatagramOutput 两处）
* 修复：`DatagramOutput` 中两处 `if (!StripAAAADnsResponseIfIPv4Available(m))` 分支（连带
  `pending_aaaa_` 写入）删除。该函数恒返回 true，写入永远不可达。

### 6.5 部分修复：地址已下发但内核应用失败（或网关缺失）时仍可能“以为有路”
* 证据：`ApplyIPv6Assignment` 的 `ipv6_dataplane_unavailable_` 只在 B1（模式+地址）与 B6
  （`!route_applied`）写入；B5 地址应用失败、B6b 无网关（B6 整段跳过）、B3 `GetTap()==null`
  都不会改写它。此时 `/1` 路由可能已装好（B6 成功）但 `tap->IPv6Address` 为空，
  `TranslateIPv6Packet` 直接丢包，而 AAAA 不剥。
* 判断：逻辑**确定**；触发需要“内核地址应用失败/网关缺失”这类环境条件（**推测**其现场概率）。
* 建议：把 `has_ipv6_dataplane` 的 B5/B6 结果合并为一个“可承载”判定
  （地址应用成功 && 路由应用成功），或在 B5 失败时把 `ipv6_dataplane_unavailable_` 置 true。

### 6.6 部分修复：`StripAAAADnsResponseIfIPv4Available` 恒返回 true 导致的残留死分支
* 证据：函数体所有出口都是 `return true`；`OnIPv6UdpPacketInput`（v6 直连 DNS 回包）与
  `RedirectDnsServer`/DirectDNS（v4 直连回包）两处 `if (!StripAAAADnsResponseIfIPv4Available(m))`
  分支仍在，分支内的 `pending_aaaa_` 写入永不可达；`FlushPendingAAAAResponses` 因此永远遍历空表。
  函数注释已在本次修正（死分支本身仍在）；`DatagramOutput` 内的两处同类死分支已按 6.4 删除。
* 判断：**确定**（功能上等价于“已剥离”，无行为缺陷；属死代码 + 文档误导）。
* 建议：删除这两处分支与 `PendingAAAAResponse`/`FlushPendingAAAAResponses`，并把注释改为
  “恒返回 true，调用方直接按剥离结果重编码”。

### 6.7 仍未修复：static/echo DNS 路径不剥 AAAA
* 证据：`ppp/app/client/VEthernetExchanger.cpp` `StaticEchoPacketInput` 的 UDP 分支把 DNS 应答
  直接 `switcher_->Output(ip.get())` 写入 TAP，只在 `udp.dns.cache` 为真时 `AddCache`，
  **完全不经过 `StripAAAADnsResponseIfIPv4Available`**。
* 判断：**确定**；影响面限于使用 static-UDP/echo DNS 通道（`udp.static.dns`）的部署（**推测**其占比）。
* 建议：该分支在 `Output` 前复用同一剥离判定（`ipv6_dataplane_unavailable_` 作为
  `strip_without_a_cache`）。

### 6.8 仍未修复：Linux/macOS 没有“直连名字按宿主出口剥 AAAA”
* 证据：`DispatchLocalDnsQuery` 及其调用者位于 `#if defined(_WIN32)`；`host_ipv6_uplink_`
  只在其中被读。
* 判断：**确定**（Linux/macOS 上 `host_uplink=false` 时，直连策略域名的 AAAA 仍会交给应用，死路）。
* 建议：在 Linux/macOS 的 DNS 应答出口（隧道 DNS 回包路径）按路径类型复用同一判定。

### 6.9 仍未修复：`state.DefaultRouteApplied` 幂等跳过后不复核路由表
* 证据：B6 的前置条件是 `!state.DefaultRouteApplied`；一旦成功，后续相同 assignment 不再重装
  `/1` 路由也不复核；`state` 只在 `RestoreIPv6Assignment`/`RestoreNetworkTakeover` 或
  assignment 变化时清空。
* 判断：**推测**（需要现场路由表被外部清空的证据）。
* 建议：在 `OnTick` 的 debug 周期里抽查一次 `::/1`+`8000::/1` 是否仍在，缺失则重装并置
  `ipv6_dataplane_unavailable_`。

### 6.10 仍未修复：`TranslateIPv6Packet` 的翻译限制
* 证据：`checksum_offset` 只识别 TCP/UDP/ICMPv6，其它 next-header（含分片/扩展头）→ `return false`；
  outbound 要求 `source == tap_address`，inbound 要求 `destination == assigned`。
* 判断：**确定**；桌面端因 `tap_address == assigned` 走 identity 提前返回而不受影响，
  **Android**（必须翻译）与“地址应用失败仍想用 v6”的场景会丢包。
* 建议：为需要翻译的平台补扩展头支持，或在文档/日志里明确该限制。

### 6.11 仍未修复：桌面 IPv6 数据面隐含依赖 `--tun-vnet=yes`（无提示）
* 证据：`OnIPv6PacketInput` 在 `!IsVNet()` 时只记一行
  `…OnIPv6PacketInput: IsVNet()=false` 就丢包，仅 Android 例外；
  `main.cpp` 默认 `GetCommandArgument("--tun-vnet", argc, argv, "y")`。
* 判断：**确定**（关 VNet 后即使服务端有数据面、地址也分配成功，v6 仍全丢）。
* 建议：面板/文档显式提示耦合；或在 `ApplyIPv6Assignment` 成功但 `!IsVNet()` 时打一条 WARN。

### 6.12 仍未修复：`--system-network-preferred-ipv6` 的语义容易被误读
* 证据：`windows/ppp/net/proxies/HttpProxy.cpp` `PreferredNetwork(false)` 只是把
  `DisabledComponents` 写 `0x00`（默认），`true` 写 `0x20`（优先 IPv4）。
* 判断：**确定**；它不是“启用 IPv6”，也不解除组策略/注册表层面的 IPv6 禁用。
* 建议：帮助文本与文档明确表述为“恢复默认前缀策略”。

### 6.13 仍未修复：`RemoveWindowsIPv6LeakBlock` 的 ULA prefix-policy 还原分支是死代码
* 证据：`ipv6_block_prefix_policy_applied_` 全仓库只有声明与一次读取/清零，从未置 true；
  实际还原走 `RestoreClientConfiguration` 的 `state.PrefixPolicyApplied`。
* 判断：**确定**（异常路径若只调 `RemoveWindowsIPv6LeakBlock`，`fd00::/8` 策略可能残留；影响 **推测**）。
* 建议：删除该死分支，或在安装 ULA 策略时同步置位该标志。

### 6.14 仍未修复：GUA 模式也会安装 `fd00::/8` 优先策略
* 证据：`WIN32_IPv6Auxiliary::ApplyClientDefaultRoute` 在 `gateway.is_v6()` 分支里、
  判断 `nat_mode` **之前**调用 `SetIPv6PrefixPolicyPreferULA()`。
* 判断：**推测**（GUA 模式下把 ULA 优先级提到 50，理论上可能把源地址选择引向物理网卡的 ULA 地址；
  需要真机 `netsh interface ipv6 show prefixpolicies` 验证）。
* 建议：仅在 `nat_mode==true`（NAT66/ULA）时安装该策略。

**计数**：已修复（本次）4 条（6.1–6.4）；部分修复 1 条（6.5）；仍未修复 9 条（6.6–6.14）。

---

## 七、如何验证（逐条可判）

### 7.1 Windows 路由/接口侧

| # | 命令 | 通过判据 | 对应组合 |
|---|---|---|---|
| V1 | `netsh interface ipv6 show route` | 出现 `::/1` 与 `8000::/1` 且 `ifIndex` = TUN；**无数据面**时下一跳为空/on-link，**有数据面**时为服务端分配的网关地址 | 1/5/8 vs 2/6 |
| V2 | `netsh interface ipv6 show route level=verbose` 或 `Get-NetRoute -AddressFamily IPv6` | 物理网卡上**没有** `::/0`（黑洞生效），或已恢复（数据面生效/断开后） | 1/5 vs 2/6 |
| V3 | `netsh interface ipv6 show addresses`（或 `Get-NetIPAddress -AddressFamily IPv6`） | TUN 上出现服务端分配的 `/128`；无数据面时 TUN 只有 `fd00::…/64`(SkipAsSource) 与 `fe80::` | 2/6 vs 1/5 |
| V4 | `netsh interface ipv6 show neighbors` | 数据面生效时存在网关的**永久**邻居，MAC 为 `00-00-00-00-00-01` | 2/6 |
| V5 | `netsh interface ipv6 show prefixpolicies` | 数据面生效时多出一行 `fd00::/8`（Precedence=50，Label 与 `::/0` 相同）；断开后应消失 | 2/6 |
| V6 | `netsh interface ipv6 show dnsservers` | TUN 适配器**没有** IPv6 DNS（Windows 行为） | 全部 |
| V7 | `netsh interface ipv6 show interface <物理ifIndex>`，或 `Get-NetIPInterface -AddressFamily IPv6` 查看该网卡的 IgnoreDefaultRoutes/DisableDefaultRoutes | 黑洞生效时物理网卡为 enabled（属性名在不同 Windows 版本可能是 IgnoreDefaultRoutes 或 DisableDefaultRoutes） | 1/5/8 |
| V8 | `curl -6 -m 8 http://[2606:4700:4700::1111]/` | 组合 1/5/8：应**快速失败或 8s 超时**（不允许“12s 挂起无响应”）；组合 2/6：应返回响应或明确的网络错误 | 全部 |

> 说明：V8 的“快速失败”依赖宿主的源地址选择；现场文档记录过全局目标被选到 TUN 的 link-local
> 源地址后**挂 12 秒**，这属于第 1/5/8 行的典型现象。

### 7.2 RPC 侧（需 `--rpc-listen` 与 `--rpc-token`，仅本机）

| # | 方法/字段 | 通过判据 |
|---|---|---|
| V9 | `get_snapshot` → `network.ipv6.server_dataplane` | 与服务端配置一致（服务端 `server.ipv6.mode` 非 none 且装载成功 → true） |
| V10 | `get_snapshot` → `network.ipv6.dataplane_unavailable` / `effective` | 组合 2/6 应为 false / `"tunnel"`；1/5/8 应为 true / `"none"`（宿主有 v6 时为 `"direct-only"`） |
| V11 | `get_snapshot` → `network.ipv6.host_uplink` | 与物理网卡是否存在 IPv6 网关一致 |
| V12 | `get_snapshot` → `network.logical_ipv6` / `network.tun.ipv6_address` | 组合 2/6 非空且等于分配的地址；1/5/8 为空 |
| V13 | `run_diagnostics`（`scope=quick` 或 `full`）→ 检查项 `network.ipv6` | 文本必须是三种之一：`IPv6 runs through the tunnel` / `only directly routed names keep IPv6 …` / `this host has no IPv6 path at all …`；与 V10 一致 |
| V14 | `get_snapshot` → `network.tun.ipv6_gateway` | 组合 2/6 等于服务端下发的网关（即 V1 的下一跳） |

### 7.3 日志关键字（debug 级）

| 关键字 | 含义 | 出现在 |
|---|---|---|
| `ApplyIPv6Assignment: entered` | 收到扩展并进入应用流程 | 2/3/6/7（8 不出现） |
| `…: server has no IPv6 data plane, restoring leak block` | 判定无数据面 | 1/4/5/8，以及 3/7 的服务端装载失败 |
| `…: applying address=` / `…: applying default route via gateway=` | 数据面正在应用 | 2/6 |
| `…: address not applied (rc=0)` | 内核地址应用失败 | 3/7 的残留场景（6.5） |
| `…: managed IPv6 route failed, leak block restored=` | 路由应用失败并回退黑洞 | 3/7 |
| `ApplyWindowsIPv6LeakBlockRoutes: installed selective block` | 黑洞生效 | 1/4/5/8 |
| `RemoveWindowsIPv6LeakBlock: restored physical IPv6 defaults=` | 黑洞撤销、物理默认路由恢复 | 2/6 |
| `TranslateIPv6Packet: outbound=%s has no usable IPv6 assignment` | v6 报文在入口被丢（每包一条） | 1/3/4/5/7/8 |
| `OnIPv6PacketInput: IsVNet()=false` | VNet 关闭导致 v6 全丢 | 6.11 |
| `StripAAAADnsResponseIfIPv4Available: STRIPPED` / `… keeping %d AAAA records` / `… because this host cannot reach IPv6` | AAAA 剥离 / 保留 / 无条件剥离 | 第三节决策表 |
| `DispatchLocalDnsQuery: … transport=direct-udp` 或 `… transport=tunnel-udp` | 该次解析走的是直连还是隧道 | 1/5/6 |
| `RefreshOutboundProbes: this host has no physical IPv6 gateway …` | 无 v6 出口时跳过 IPv6 服务器条目（只报一次） | 1/4/5/8 |

### 7.4 DNS 侧抽查

| # | 命令（Windows） | 通过判据 |
|---|---|---|
| V15 | `Resolve-DnsName -Type AAAA -Server 127.0.0.1 <一个只有 AAAA 的域名>` | 组合 1/5/8：**查不到 AAAA**（被无条件剥离）；组合 2/6：能查到 |
| V16 | `Resolve-DnsName -Type AAAA -Server 127.0.0.1 <国内直连域名>` | 宿主无 v6（H=false）：查不到；宿主有 v6：能查到 |
| V17 | 关闭 `udp.dns.cache` 后重复 V15 | 组合 1/5/8 仍应查不到（本次修复点；修复前会查到） |

> 注意：V15–V17 需先清掉已有缓存（重启核心；必要时 `ipconfig /flushdns`），
> 因为 vDNS 缓存与 Windows DNS 客户端缓存里的旧 AAAA 不会自动失效。
