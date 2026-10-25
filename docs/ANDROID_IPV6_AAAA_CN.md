# 安卓 IPv6 不可用：AAAA 应答被客户端删除（已修复）

## 1. 症状

安卓客户端连上服务端后：

* IPv4 目标正常；
* 应用拿不到任何 IPv6 地址：`ping6 www.google.com` 报 `unknown host`，`curl -6` 直接 `Could not resolve host`；
* 用 IPv6 字面量（如 `http://[2606:4700:4700::1111]/`）却能通，且出口是服务端自己的 IPv6 地址；
* 换成带 IPv6 的服务端也一样没有 IPv6。

也就是说：**隧道能跑 IPv6，但 DNS 层把 AAAA 全删了**，应用永远不会去连 IPv6。

## 2. 关键证据（Android 16 / RMX3706，debug 构建）

打开 `设置 → 日志级别 = debug` 后，一次 `curl -6 http://example.com/` 产生：

```
DNS pipeline: query host=example.com type=28 address_query=1 geo_direct_dns=0 dest=8.8.8.8
DNS pipeline: rule miss host=example.com, falling back to tunnel
StripAAAADnsResponseIfIPv4Available: no A cache for example.com,
    stripping 2 AAAA records because this host cannot reach IPv6
```

AAAA 查询本身成功（拿到 2 条 AAAA），是客户端自己删掉的。同一次连接里服务端信息扩展是：

```
PacketInput: INFO extensions parsed, AssignedIPv6Mode=1,
    AssignedIPv6Address=fd42:4242:4242:0:ed92:955f:2668:4f8a, ...
```

服务端明确分配了 IPv6 租约，客户端却仍然判定“这台主机到不了 IPv6”。

## 3. 根因

`StripAAAADnsResponseIfIPv4Available()` 的删除条件来自三个信号，在 Android 上全部失真：

### 3.1 `route_applied` 在 Android 恒为 false

`VEthernetNetworkSwitcher::ApplyIPv6Assignment()` 用
`ppp::ipv6::auxiliary::ApplyClientDefaultRoute(...)` 的返回值决定
`ipv6_dataplane_unavailable_`。该函数只实现 Windows/macOS/Linux 三支：

```cpp
bool ApplyClientDefaultRoute(...) noexcept {
#if defined(_WIN32)      ...
#elif defined(_MACOS)    ...
#elif defined(_LINUX)    ...
#else
    return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6Unsupported);
#endif
}
```

安卓的 TUN 由 `VpnService.Builder` 持有，native 侧无 root 改不了地址/路由，所以它必然失败。
但 Android 的 IPv6 数据面根本不依赖它：`VpnService.Builder.addRoute("::", 0)` 已经把 IPv6 包送进隧道，
`OnIPv6PacketInput()` 再把它们原样 `Nat()` 给服务端，由服务端 NAT66 到刚分配的租约上。
把“路由装不上”当成“没有 IPv6 数据面”，就删掉了每一份 AAAA。

### 3.2 平台策略没有传给 core

App 里 `tunIpv6Mode`（auto/block/direct）只用于 `VpnService.Builder` 决定是否 claim `::/0`，
core 完全不知道；core 只能靠桌面那套“路由是否装上”来猜，于是 3.1 必然踩中。

### 3.3 `prefer_ipv4` 与 `host_ipv6_uplink_`

* profile 配置里 `"udp":{"dns":{"prefer_ipv4":true}}` 时 `ShouldPreferIPv4()` 为真，
  即使隧道能跑 IPv6 也会删 AAAA；桌面端这是为了避开 leak-block 黑洞，安卓没有那个黑洞。
* `host_ipv6_uplink_` 只在 `#else`（桌面）分支赋值，Android 分支从不赋值，
  恒为 false；于是“直连解析”（geo 旁路）的域名也一律删 AAAA。

### 3.4 正常情况：服务端真的没有 IPv6

服务端信息扩展会明确回 `IPv6StatusCode=5, IPv6StatusMessage="server-ipv6-disabled"`。
这种服务端确实带不了 IPv6，此时删 AAAA 是对的，保留原逻辑。

## 4. 修复

| 文件 | 修改 |
|---|---|
| `ppp/tap/ITap.h` | 新增 `int IPv6Mode`（0=auto/block，1=direct），承载 VpnService 的 IPv6 策略 |
| `android/libopenppp2.cpp` | 网络接口新增 `IPv6Mode` 字段并拷到 tap；新增 JNI `set_ipv6_mode(int)` |
| `android_ui/.../libopenppp2.kt` | 新增 `external fun set_ipv6_mode(mode: Int): Int` |
| `android_ui/.../bg/VpnService.kt` | `set_network_interface` 之后调用 `set_ipv6_mode(if (blockIpv6) 0 else 1)` |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | ① 构造函数：Android 初始 `ipv6_dataplane_unavailable_=false`（不再先按“无 IPv6”处理）；② `ApplyIPv6Assignment`：不再让 Android 的 `route_applied` 覆盖该标志，只跟随服务端扩展（disabled ⇒ 才删除）；③ Android 忽略 `prefer_ipv4`；④ Android 把 `host_ipv6_uplink_` 视为可用 |

最终判定：

```
server 报 server-ipv6-disabled  -> 删 AAAA（服务端带不了 IPv6）
server 分配了 IPv6 租约          -> 保留 AAAA（走隧道 + NAT66）
扩展还没到                      -> 保留 AAAA（Android 无黑洞，等 Happy Eyeballs 兜底）
```

## 5. 验证（同一台手机，debug 构建，服务端 AssignedIPv6Mode=1）

```
日志: ApplyIPv6Assignment: android vpn ipv6 mode=0, server_dataplane=1, aaaa_unavailable=0

ping6 -c1 www.google.com          -> 2001:4860:482c:7700::, 64 bytes, ttl=114, 311 ms
curl -6 http://example.com/       -> 200  0.12 s
curl -4 http://example.com/       -> 200  0.39 s
curl -6 https://ipv6.icanhazip.com/ -> 2400:c620:2e:8dfe::1   （= 服务端 IPv6 出口）
curl -6 https://[2606:4700:4700::1111]/cdn-cgi/trace -> ip=2400:c620:2e:8dfe::1
```

对照：把同一个客户端连到只开 IPv4 的服务端（StatusCode=5）时，日志仍会
`stripping N AAAA ... this host cannot reach IPv6`，IPv4 正常——这是预期行为。

## 6. 让 tun0 真正持有服务端租约（对齐 Windows 的地址落点）

上面修完后 IPv6 已经可用，但和 Windows 还有一处结构差异：Windows 把服务端分配的
`fd42:…/128` 真正配到 TAP 上，安卓的 TUN 地址被 `VpnService.Builder` 固定为黑洞 ULA
`fd00:6f70:656e:7070::2`，于是每个 IPv6 包的源地址都要由
`VEthernetExchanger::TranslateIPv6Packet` 逐包改写。

现在改成“记住租约、下次直接用它建隧道”：

1. native 新增 `VEthernetNetworkSwitcher::ServerAssignedIPv6Address()` /
   `VEthernetExchanger::GetAssignedIPv6Address()`（加锁读 `assigned_ipv6_address_`），
   JNI 暴露 `get_ipv6_lease()`；
2. `VpnService` 在链路轮询里每秒读一次，拿到新租约就写进
   `DataStore.tunIpv6Lease`（落在 `configuration.db`）；
3. 下次 `startVpn()` 若缓存里有合法地址，就
   `Builder.addAddress(<lease>, 128)` + `addRoute("::", 0)`，并把同一个地址作为
   `set_network_interface(..., ipv6)` 传给 core 当翻译锚点；
4. 服务端这次若给了别的地址，`TranslateIPv6Packet` 照旧在 `<tun 地址> ↔ <新租约>`
   之间改写，所以缓存过期不会断网，只是退化成翻译模式。

效果（实测，同一台 RMX3706 / Android 16）：

```
第一次连接： tun0 = fd00:6f70:656e:7070::2        （黑洞 ULA，翻译模式）
            logcat: OpenPPP2VpnService: ipv6 lease remembered: fd42:4242:4242:0:ed92:955f:2668:4f8a
之后连接：   tun0 = fd42:4242:4242:0:ed92:955f:2668:4f8a/128 scope global
            （服务端租约真正落在网卡上，源地址不再逐包改写）

ip -6 addr show tun0 -> inet6 fd42:4242:4242:0:ed92:955f:2668:4f8a/128 scope global
ping6 www.google.com   -> 64 bytes, ttl=114, 169 ms
curl -6 http://example.com/ -> 200
curl -4 http://example.com/ -> 200
curl -6 https://ipv6.icanhazip.com/ -> 2400:c620:2e:8dfe::1（服务端 v6 出口，colo=HKG）
```

注意：缓存是**全局**的（同一个客户端 GUID 对同一服务端的租约稳定）。换服务端后第一次连接会
先用旧地址、由翻译层兜住，拿到新租约后再覆盖。连到 `server-ipv6-disabled` 的服务端时，
隧道上会留着一个用不上的 v6 地址，但不影响 IPv4，也不影响下次连回 v6 服务端。

## 7. 国内 IPv6 直连、国外 IPv6 走隧道

### 7.1 现象

分流模式（geo/basic）下，国内 IPv6 站点看到的是**服务端**的 IPv6，而不是本地宽带/流量的 IPv6。

### 7.2 根因

geo 规则的判定本身是对的：

```
GetExchanger: destination=2409:8c54:871:3053::ac, action=direct, selected_outbound=direct, rule=1
```

但 Android 上“direct”只有 IPv4 能真正落到本地：

* IPv4：客户端在 lwIP 里终结 TCP，由 `Rinetd` 用**受保护套接字**拨号，出口是物理网卡；
  `GetExchanger()` 在 Android 上返回主出口只是“载体”，真正的直连发生在 Rinetd 里。
* IPv6：没有 accept 钩子（`netstack_tcp_do_before_accept` 只处理 v4），`OnIPv6PacketInput()`
  只能把原始报文 `Nat()` 给服务端。于是“判定 direct”之后，包还是到了服务端，由服务端 NAT66
  出去——国内站点因此看到服务端的 v6。

### 7.3 修复

不再尝试在客户端内部“把 v6 包塞回物理网卡”（那需要 root），而是在建立隧道时把**国内 v6 前缀
从 VPN 路由里排除**，交给内核按物理网卡原生路由：

```kotlin
// VpnService.startVpn()，仅当启用了分流（routeMode 非空）且 blockIpv6
if (routeMode.isNotEmpty()) {
    applyDirectIpv6Exclusions(builder)   // 逐行读 files/rules/ipv6.txt
}
```

* 数据源就是随包的国内 v6 前缀表 `files/rules/ipv6.txt`（2040 条，含 `2408::/13`、`2409:8000::/20`、
  `240e::/24`、`2001:250::/35` 等）；用户可以自行增删后重连生效。
* 依赖 Android 13（API 33）的 `VpnService.Builder.excludeRoute(IpPrefix)`；更低版本会打印
  `ipv6 direct exclusions need Android 13` 并保持“国内 v6 也走隧道”的旧行为。
* 全局代理（未启用分流）时不排除，所有目的地照旧走隧道。

### 7.4 验证

```
日志: OpenPPP2VpnService: ipv6 direct exclusions applied: 2040 prefixes (skipped 0)

ip -6 route get 2409:8c54:871:4::87
  -> via fe80::1204:c1ff:fe03:5cf4 dev wlan0 table 1030 src 2409:8a55:d821:...   （本地 GUA）
ip -6 route get 2606:4700:4700::1111
  -> dev tun0 table 1150 src fd42:4242:4242:0:ed92:955f:2668:4f8a              （服务端租约）

curl -6 https://www.jd.com/      -> 200  0.12 s   （本地直连）
curl -6 https://www.taobao.com/  -> 200  0.14 s   （本地直连）
ping6 www.baidu.com              -> 2409:8c54:... 22 ms（本地直连）
curl -6 https://ipv6.icanhazip.com/ -> 2400:c620:2e:8dfe::1（服务端出口）
curl -4 http://example.com/      -> 200
```

客户端日志里只会出现国外 v6 目的地的 `GetExchanger` 记录，国内 v6 只剩 DNS 应答观测行
（`DirectDNS response: host=www.jd.com ... 2409:...`），说明这些包根本没有进入隧道。

### 7.5 已知限制

* 排除表是**IP 前缀**，不是域名：某条 `domain-suffix,cn,direct` 的域名若解析到国外 v6，
  仍然会进隧道（IPv4 那边则能靠 Rinetd 精确直连）。
* Android < 13 没有 `excludeRoute`，国内 v6 仍会走服务端。

## 8. 现场判据（下次抓现场先看这三行）

```
PacketInput: INFO extensions parsed, AssignedIPv6Mode=?, IPv6StatusCode=?
ApplyIPv6Assignment: android vpn ipv6 mode=?, server_dataplane=?, aaaa_unavailable=?
StripAAAADnsResponseIfIPv4Available: ... STRIPPED n AAAA for <domain>
```

* `IPv6StatusCode=5` → 服务端侧没开 IPv6，客户端无能为力；
* `server_dataplane=1` 却仍出现 `STRIPPED` → DNS 路径回归；
* `aaaa_unavailable=0` 且应用仍无 IPv6 → 检查 `tun0` 是否真的拿到了 `::/0`（`ip -6 route show dev tun0`）。
