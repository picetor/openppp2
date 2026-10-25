# 安卓 IPv6 数据面「终结模式」实现方案（阶段 2，未实施）

> 状态：**方案，未实施**。当前分支 `inet6` 已落地的能力见
> `docs/ANDROID_IPV6_AAAA_CN.md`（AAAA 不再被删、服务端租约落到 tun0、国内 v6 前缀从 VPN 排除）。
> 本文只描述「像 Windows/sing-box 那样按连接决定 direct/隧道」还差什么。

## 0. 为什么要做

现状（阶段 1 之后）：

* IPv6 可用，源地址是服务端租约，出口是服务端 v6；
* 国内 v6 通过 `VpnService.Builder.excludeRoute()` 排除出 VPN，由内核原生直连；
* 但**分流粒度是 IP 前缀**：`domain-suffix,cn,direct` 这类域名规则对 v6 不生效；
  `udp.dns.prefer_ipv4` 对 v6 也没有意义（已在 Android 上忽略）。

目标：v6 和 v4 同权——每条 TCP 连接都能按规则选择「本机直连」或「交给服务端」，
域名级规则、按 outbound 选路、失败回退全部对 v6 生效。

参考实现思路来自 `D:\github\Exclave`（sing-box 系）：TUN 的 fd 交给进程内用户态
TCP/IP 栈（gVisor）终结，每条连接作为对象交给路由；direct 出站用普通 socket + `protect()`
走物理网络。OpenPPP2 的 v4 已经是同一模型（lwIP 终结 + Rinetd/mux），只差把 v6 接上。

## 1. 现状链路（v4，可作为模板）

```
TAP 读包
  -> VEthernet::PacketInput (ppp/ethernet/VEthernet.cpp:412 版本分派)
  -> VEthernetNetworkSwitcher::OnPacketInput           // vnet gate / 策略
  -> VNetstack::Input(ip_hdr*, tcp_hdr*, len)          // ppp/ethernet/VNetstack.cpp:292
       按 tap->GatewayServer / tap->IPAddress 做地址+端口 NAT 改写，
       维护 TapTcpLink（srcAddr,dstAddr,srcPort,dstPort,natPort）
  -> lwip::netstack::input(pbuf)                       // common/libtcpip/netstack.cpp:877
  -> netif->input == ::ip_input                        // 已配置 ip_input
  -> tcp_input -> netstack_tcp_do_before_accept        // netstack.cpp:777（当前只处理 v4）
  -> netstack::accept -> VNetstack::LwIpBeginAccept
  -> VEthernetNetworkTcpipConnection::ConnectToPeer
       direct: RinetdConnection::Open(tcp::endpoint)  // 家族无关，已支持 v6
       隧道  : mux connect host:port                  // 服务端拨号
回程：lwIP 写 netif output -> TAP（源地址需改写回原目的地）
```

## 2. 还缺的四块

### 2.1 让 lwIP 认 v6（netif 地址 + 输入分派）

* `common/libtcpip/netstack.cpp` 的 `netstack::open()` 里给静态 netif 配 v6：
  `netif_add_ip6_address(netif, &lease, NULL)`（lease 用服务端分配的 `fd42:…/128`；
  在 Android 上就是 `ITap::IPv6Address`），并把 `ip6_addr_state` 置为 `IP6_ADDR_PREFERRED`。
* `VNetstack` 增加 v6 入口（v4 是 `Input(ip_hdr*, tcp_hdr*, len)`）：
  解析 40 字节 `ppp::ipv6::PacketHeader`，仅 TCP 走终结，其余保持现有 `OnIPv6PacketInput` 的 raw `Nat`。
* `lwip::netstack::input()` 已经是 `netif->input == ::ip_input`，v6 报文能被正确分派
  （`LWIP_IPV6=1` 已开，见 `common/lwip/my/lwipopts.h:61`）。

### 2.2 地址/端口改写 + 校验和（v6 版 NAT）

v4 的做法（`VNetstack::Input`）：

* 上行：`ip->dest == tap->GatewayServer` 视为回程；SYN 分配 `natPort` 并记录 link；
* 下行（lwIP 发出的包）：`ip->src = tap->GatewayServer`、`ip->dest = tap->IPAddress`、
  `tcp->dest = listenEP_.Port`。

v6 需要同样的三段，只是：

* 地址换成 `fd42:…::1`（gateway）与租约地址；
* **必须重算 TCP 校验和**：v6 伪首部含 16 字节源/目的地址，v4 那套改完地址后
  lwIP 会用旧校验和校验失败。建议在改写后调用现有 IPv6 校验和辅助
  （`ppp/ipv6/*` 里有 `ComputePseudoChecksum` / `ClampTcpMssIPv6` 可复用）。
* 分片：v6 只有源端分片，分片包没有 TCP 头，直接走 raw `Nat`，不要进终结路径。

### 2.3 链接表泛化

`ppp/ethernet/VNetstack.h`：

* `TapTcpLink` / `TapTcpClient` 的 `uint32_t srcAddr/dstAddr` 改成 `boost::asio::ip::address`
  （或新增 v4/v6 两套表，键为 `address+port` 的哈希）；
* `LwIpAcceptLink(uint32_t,uint32_t,int,int)`、`LAN2WAN_KEY(...)`、`AllocTcpLink(...)`
  同步泛化；
* `FindTcpLink(natPort)` 只按端口查，可保持不变。

### 2.4 accept 回调与回程

* `netstack_tcp_do_before_accept`（`common/libtcpip/netstack.cpp:777`）去掉
  `IP_IS_V4(dest) && IP_IS_V4(src)` 过滤，v6 时构造 `tcp::endpoint(address_v6, port)`；
  注意 `h->dest` 已是主机序（见 `docs/ANDROID_TUN_PORT_ORDER_FIX_CN.md`），**不要再 `ntohs`**。
* `VNetstack::LwIpAcceptLink` 的端口参数约定（`htons` 后入表）对新 v6 路径要一致。
* 回程包由 lwIP 从 netif 写出，`netstack_ip_output_v4` 现在是 v4 专用；
  v6 需要对应的 `netif->output_ip6`（默认 `ethip6_output`/`nd6` 需要链路层，直接写 TAP 更简单：
  自己实现 output_ip6 → 写 TAP fd）。同时把源地址改写回原始目的地、目的地址改写回手机侧地址。

## 3. 需要触碰的文件（预估）

| 文件 | 改动 |
|---|---|
| `common/libtcpip/netstack.cpp` | netif 加 v6 地址、`netif->output_ip6`、`before_accept` 支持 v6 |
| `ppp/ethernet/VNetstack.{h,cpp}` | v6 入口、改写 + 校验和、链接表泛化、`LwIpAcceptLink` |
| `ppp/ethernet/VEthernet.cpp` | v6 分派：能终结的交给 VNetstack，其余维持 raw `Nat` |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | 选择策略：终结 vs raw 转发（按 geo 决策） |
| `android_ui/.../VpnService.kt` | 阶段 2 稳定后，`excludeRoute` 可退化为可选 |

预计 400~700 行改动，主要集中在 `VNetstack`。

## 4. 实施顺序与验证

1. netif 加 v6 地址 + `before_accept` 放行 v6，先只做日志验证 SYN 能到 accept（不拨号）；
2. 接通 direct 拨号（Rinetd 已家族无关），验证一条 v6 直连能通；
3. 补回程改写与校验和，验证双向数据（`curl -6 https://www.jd.com/` 200 且源地址为本地 GUA）；
4. 接隧道路径（mux connect v6 字面量），验证 `curl -6 https://ipv6.icanhazip.com/` 出口为服务端；
5. 回归 v4（`curl -4`）、v6 DNS（`ping6 www.google.com`）、国内/国外分流（`ip -6 route get`）；
6. 最后再决定是否保留 `excludeRoute`（可留作 API<33 的兜底）。

现场判据建议新增：

```
VNetstack::InputV6: entry, flags=…, src=[…]:port, dest=[…]:port
netstack_tcp_do_before_accept: source=[…]:port, destination=[…]:port, result=…
```

## 5. 风险

* v6 校验和/伪首部算错会表现为「SYN 到了、回包被丢」，很难定位，必须逐段验证；
* lwIP 的 `ip6_input` 对 `netif` 地址状态敏感（`IP6_ADDR_PREFERRED` 不设会静默丢弃）；
* 与现有 raw `Nat` 路径共存时要有明确的一刀切规则（建议：仅 TCP 且非分片走终结），
  否则同一目的地会出现两条路径，连接行为不一致。
