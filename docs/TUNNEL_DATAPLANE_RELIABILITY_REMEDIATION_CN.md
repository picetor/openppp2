# Windows 隧道数据面可靠性修复设计

> 状态：设计已冻结，源码部分实施；编译、完整端到端追踪及 Windows 管理员实机门禁尚未完成
>
> 日期：2026-09-14
>
> 范围：Windows 客户端的 TUN/Wintun、SOCKS/HTTP 本地代理、ICMP 静态回声、DNS 回包、TCP MTU/MSS、MUX 与 TUI 核心日志启动参数
>
> 依据：2026-09-13 至 2026-09-14 在真实客户端、真实服务端和当前代码版本上的 ICMP、TCP、SOCKS、DNS、日志与抓包测试
>
> Windows UAC manifest 与资源管理器小黄盾不属于数据面故障，专项根因和发布策略见 `WINDOWS_UAC_MANIFEST_AND_SHIELD_DESIGN_CN.md`。

---

## 1. 结论摘要

服务端入口和物理网络不是本次故障的主要瓶颈。实测到服务端入口的物理 ICMP 丢包率为 0%，平均 RTT 约 85 ms；TCP 端口连接 10/10 成功，平均约 90 ms。主隧道控制连接在观测窗口内没有重连，子传输握手也能持续成功。

客户端数据面存在以下问题：

| 优先级 | 问题 | 证据强度 | 主要影响 |
| --- | --- | --- | --- |
| P0 | `IsDirectProxyAddress` 把指向 TUN 网关的默认路由误判为直连 | 已证实 | SOCKS/HTTP 代理自环、连接超时 |
| P0 | Windows `GetBestInterface` 成功/失败条件写反 | 已证实 | bypass/direct 判断不可靠 |
| P0 | DNS/ICMP 回包到达 core 并提交 Wintun 后未交付给 Windows 应用 | 已证实故障区间，最终丢弃点待确认 | DNS 和 ping 超时 |
| P1 | 两个静态回声 socket 共用接收缓冲区和 endpoint | 已证实代码缺陷 | ICMP/静态 UDP 回包损坏或偶发丢失 |
| P1 | Wintun MTU 为 65535，IPv4 MSS clamp 未接入 | 已证实 | TLS 卡顿、PMTU 黑洞、大流量停顿 |
| P1 | `tun_mux=0`，每个 TCP 流建立独立外层连接 | 已证实配置行为 | 首包慢、并发成本和抖动放大 |
| P2 | TUI 对默认日志路径省略 `--log-file` | 已证实 | 设置 debug 但不生成 core 日志 |

本设计要求先修复确定性错误，再通过可观测性门禁定位 Windows 入站丢弃点，最后启用 MTU/MSS 和 MUX 优化。不得用调整超时、增加重试或关闭校验掩盖根因。

---

## 2. 实测基线

### 2.1 测试结果

| 测试 | 结果 |
| --- | --- |
| 服务端物理 ICMP | 0% 丢包，平均约 85 ms |
| 服务端 TCP 入口 | 10/10 成功，平均约 90 ms |
| 隧道 ICMP | 8/8 超时；其他轮次存在间歇性回包 |
| SOCKS5 数字 IPv4 | 12/12 连接超时 |
| SOCKS5 域名 | 12/12 连接超时 |
| 普通 HTTPS/TCP | 10/12 成功；一次连接超时，一次连接成功后约 31 秒无 TLS 响应 |
| 隧道 DNS | core 通常在 124～942 ms 收到响应，`nslookup` 仍超时 |

### 2.2 当前运行参数

与问题直接相关的配置为：

```text
tun_host=true
tun_mux=0
bypass_mode=ip
udp.static.icmp=true
paper_airplane.tcp=true
Wintun IPv4 MTU=65535
Wintun IPv6 MTU=65535
```

当前 Windows 虚拟接口：

```text
InterfaceAlias: PPP
InterfaceIndex: 57
IPv4:           192.168.13.25/24
Gateway/DNS:    192.168.13.1
Profile:        Public
Connectivity:   NoTraffic
```

### 2.3 关键日志证据

代理失败路径：

```text
selected_outbound=main
bypass_mode=ip
policy_outbound=main
direct=1
RinetdConnection::Open: async_connect failed ... ec=10060
VNetstack::Input: SYN loop existing client
```

DNS 故障路径：

```text
SendLocalDnsUdp: server=1.1.1.1, static=0, main=1
DispatchLocalDnsQuery: response host=example.com ... transport=tunnel-udp, elapsed_ms=298
DNS pipeline: inject source=192.168.13.25:..., dns=192.168.13.1:53, ... output=1
```

与此同步的 `nslookup` 输出仍为：

```text
DNS request timed out.
```

`output=1` 只表示 core 成功构造数据包并由 Wintun API 接受提交，不表示 Windows IP/WFP/UDP 层或目标进程已经接收。

### 2.4 PktMon 原始包离线复核

仓库提供零第三方依赖分析器 `tests/tools/analyze_pktmon_pcap.py`。它直接解析 pcapng block，在每个捕获记录中按 IPv4 header checksum 定位 IP 包，并校验可验证的 ICMP/TCP/UDP checksum；同时用 ICMP id/sequence、DNS transaction ID/反向五元组关联请求响应，并提取 TCP SYN MSS。对新构建可以同时传入 `--core-log` 和 `--app-log`，审计 `TUN_RX/POLICY_SELECTED/REMOTE_TX/REMOTE_RX|LOCAL_RX/PACKET_BUILT/WINTUN_ALLOCATED/WINTUN_SUBMITTED/OS_OBSERVED/APP_COMPLETED` 的集合关系，并输出每个缺失阶段的具体 flow ID。

对 `artifacts/tunnel-diagnostics-20260913/tunnel-runtime.pcapng` 的复核结果：

```text
PktMon occurrences:                 656
IPv4 / IPv6 occurrences:           225 / 52
unique IP packets:                  247
IPv4 checksum-failed occurrences:   0
IPv6 checksum-failed occurrences:   12（捕获层/卸载状态尚未关联）
ICMP unique request/reply:          3 / 4
ICMP matched:                       3
ICMP matched latency:               84.641～296.479 ms，平均 221.020 ms
DNS unique query/response:          2 / 2
DNS matched:                        2
DNS matched latency:                0.271～0.331 ms，平均 0.301 ms
TCP SYN MSS occurrences:            1332×2、1400×52、65475×2、65495×10
```

该时间窗内 3 个已观察到的 ICMP request 都存在 reply；另有 1 个 reply 的 request 在采集开始前或未被当前 PktMon 层观察。DNS 的 2 个 transaction ID 都能以反向地址和端口匹配。MSS 65495 和 65475 分别等于 65535−40 与 65535−60，与当时双栈 TUN MTU=65535 的错误配置精确一致，是 MTU/MSS 整改的直接包级证据。

IPv6 的 12 个 checksum-failed occurrence 来自混合 PktMon 组件的旧捕获。由于发送侧 checksum offload 可能在抓取时尚未填入传输层 checksum，没有组件/方向关联时不能把该数字直接解释为线上坏包；新门禁必须把 core 构造校验、Wintun 入站提交点和物理网卡线上观察分开统计。

对稍后失败时间窗 `icmp-stack.pcapng` 的复核结果是：同一个 ICMP request 在两个 PktMon 组件出现，去重后为 1 个 request、0 个 reply，且 `ping` 为 4/4 超时。两份证据共同证明故障具有时间相关性：成功时间窗中远端响应和 Windows IP 组件均可见，失败时间窗中当前证据只证明请求进入 Windows 发送路径，无法证明请求已到达远端或远端回包已进入 core。因此，在带完整 `REMOTE_TX/REMOTE_RX/WINTUN_SUBMITTED/OS_OBSERVED` 关联的新构建复测前，不得把 WFP、远端、传输断链中的任一项写成唯一最终根因。

复核命令：

```powershell
python .\tests\tools\analyze_pktmon_pcap.py `
  .\artifacts\tunnel-diagnostics-20260913\tunnel-runtime.pcapng
python .\tests\tools\analyze_pktmon_pcap.py `
  .\artifacts\tunnel-diagnostics-20260913\icmp-stack.pcapng

# 新构建的自动关联
python .\tests\tools\analyze_pktmon_pcap.py `
  .\artifacts\dataplane-acceptance-...\pktmon.pcapng `
  --core-log .\ppp-core.log `
  --app-log .\artifacts\dataplane-acceptance-...\app-completed.jsonl
```

`tests/tools/dataplane_app_probe.py` 使用原始 ICMP socket 和自构造 DNS UDP query，控制 ICMP identifier/sequence 与 DNS transaction ID，默认执行 ICMP 100 次以及 DNS A/AAAA/PTR 各 100 次。只有测试进程真实收到且校验响应后才写 `APP_COMPLETED`；超时写 `APP_TIMEOUT`，不得用 core 提交或 PktMon 观察代替。Windows 上原始 ICMP 需要管理员权限。

---

## 3. 目标与非目标

### 3.1 目标

1. 消除 Windows 本地代理和直连流量的路由自环。
2. 使 ICMP、DNS、TCP 小包与大流量都具有稳定、可重复的结果。
3. 明确区分“排队成功”“系统调用成功”“对端响应”“写入 Wintun”“Windows 应用收到”五个阶段。
4. 为 MTU、MSS 和 MUX 提供安全默认值、配置边界和回退路径。
5. 确保 TUI 设置 core 日志后必定产生指定日志文件，失败时可见原因。
6. 为每个修复建立单元测试、集成测试、压力测试和上线门禁。

### 3.2 非目标

1. 本轮不修改隧道协议的加密格式和会话兼容性。
2. 不以永久关闭 Windows 防火墙作为解决方案。
3. 不用无限重试或扩大超时替代正确路由和可靠回包。
4. 不在确认正确性之前默认启用大规模 MUX 并发。
5. 不把 `client.log` 流量审计日志与核心 `LOG_*` 运行日志强行合并。

---

## 4. 必须保持的系统不变量

后续实现和评审必须维护以下不变量：

1. 指向 TUN/TAP 网关的路由永远不能被解释为物理直连路由。
2. 所有需要绕过 TUN 的 Windows socket，在 `connect` 或 `send` 前必须确定物理接口或建立显式 host route。
3. 每个未完成的异步接收操作必须独占可写缓冲区和 source endpoint。
4. 数据面成功日志必须对应已完成的操作，不能把“已投递”记录为“已发送”。
5. Wintun 接口 MTU 不得使用 65535 作为生产默认值。
6. 发往隧道的 TCP SYN 必须宣告不超过隧道有效 MTU 的 MSS。
7. DNS 响应必须保留原始客户端事务 ID、源/目的地址和源/目的端口。
8. MUX 失败必须能够回退到非 MUX 连接，且不能拖垮控制连接。
9. 日志级别和日志目标是两个独立配置；启用级别不等于已经配置输出文件。
10. `REMOTE_TX/REMOTE_RX` 只能表示真实发生的远端发送/接收；DNS cache、请求合并复用和本地产生的 SERVFAIL 必须记为 `LOCAL_RX`，不得为了凑齐链路阶段伪造远端证据。

---

## 5. P0：修复代理路由分类与自环

### 5.1 根因

`VEthernetNetworkSwitcher::IsDirectProxyAddress` 当前使用“内部 RIB 是否存在下一跳”判断目标是否直连。与此同时，初始化过程向同一个 RIB 写入指向 TUN 网关的：

```text
0.0.0.0/0
0.0.0.0/1
128.0.0.0/1
```

因此任意 IPv4 几乎都能命中路由，进而被错误设置为 `force_direct=true`。Windows Rinetd 连接没有 Linux/Android 的 socket protect 机制，所谓直连 socket 再次命中 TUN 默认路由，形成递归 SYN。

相关代码：

- `ppp/app/client/VEthernetNetworkSwitcher.cpp`：`IsDirectProxyAddress`
- `ppp/app/client/VEthernetNetworkSwitcher.cpp`：RIB 默认路由初始化
- `ppp/app/client/proxys/VEthernetLocalProxyConnection.cpp`：`force_direct` 选择

### 5.2 最小安全修复

第一阶段必须检查下一跳是否为当前 TUN 网关：

```cpp
bool VEthernetNetworkSwitcher::IsDirectProxyAddress(
    const boost::asio::ip::address& address) noexcept {
    if (!address.is_v4() || !rib_) {
        return false;
    }

    const uint32_t destination = htonl(address.to_v4().to_uint());
    const uint32_t next_hop = ForwardInformationTable::GetNextHop(
        destination, rib_->GetAllRoutes());

    if (next_hop == IPEndPoint::NoneAddress) {
        return false;
    }

    const std::shared_ptr<ITap> tap = GetTap();
    if (tap && next_hop == tap->GatewayServer) {
        return false;
    }

    return true;
}
```

此补丁用于立即消除当前默认路由造成的自环，但不是长期模型。

### 5.3 长期修复：显式路由来源

RIB/FIB 路由项应增加来源和动作，而不是仅保存 `Destination/Prefix/NextHop`：

```cpp
enum class RouteOrigin : uint8_t {
    TunnelDefault,
    TunnelPolicy,
    PhysicalSystem,
    Bypass,
    GeoDirect,
    ServerPin
};

enum class RouteAction : uint8_t {
    Tunnel,
    Direct,
    Reject
};
```

直连判定只能接受：

```text
Bypass + Direct
GeoDirect + Direct
PhysicalSystem + Direct
ServerPin + Direct
```

`TunnelDefault` 和 `TunnelPolicy` 无论下一跳数值为何都不能转成 direct。

### 5.4 Windows socket 保护

Windows 没有 Android `VpnService.protect()`。需要实现统一的 `ProtectSocketHandler`，优先级如下：

1. 为服务器或 direct 目标创建绑定到物理网关的 `/32` host route；
2. 在可用时设置 `IP_UNICAST_IF`/`IPV6_UNICAST_IF`；
3. 在 `connect()` 前验证 `GetBestInterfaceEx` 返回的接口不是 TUN；
4. 无法保证物理出口时拒绝直连并回退隧道，禁止冒险连接。

所有检查必须发生在 `connect()` 之前。

### 5.5 Windows API 返回值修复

将错误判断：

```cpp
DWORD dwInterfaceIndex;
if (!::GetBestInterface((IPAddr)nip, &dwInterfaceIndex)) {
    return false;
}
```

改为：

```cpp
DWORD interface_index = 0;
const DWORD result = ::GetBestInterface(
    static_cast<IPAddr>(nip), &interface_index);
if (result != NO_ERROR) {
    LOG_WARN("GetBestInterface failed: destination=%s error=%lu",
        destination_text, static_cast<unsigned long>(result));
    return false;
}

const int tap_index = tap->GetInterfaceIndex();
if (tap_index < 0) {
    return false;
}
return interface_index != static_cast<DWORD>(tap_index);
```

若项目已统一使用 `GetBestInterfaceEx`，IPv4 和 IPv6 应收敛到同一封装，不再在业务代码中直接调用 Win32 API。

### 5.6 单元测试

至少覆盖：

| 场景 | 期望 |
| --- | --- |
| 默认路由下一跳为 TUN 网关 | Tunnel |
| `/1` 路由下一跳为 TUN 网关 | Tunnel |
| 更具体 bypass `/24` 路由 | Direct |
| server pin `/32` 路由 | Direct |
| 无路由 | Tunnel/Reject，不能 Direct |
| `GetBestInterface` 返回 0 且为物理接口 | Direct |
| `GetBestInterface` 返回 0 且为 TUN 接口 | Tunnel |
| Win32 API 返回错误 | Tunnel/Reject，不能读取未初始化索引 |

验收日志中不得再出现由本地代理导致的 `SYN loop existing client`。

---

## 6. P0：定位并修复 Wintun 回包未交付

### 6.1 已确认边界

当前证据已经确认：

```text
远端 DNS/ICMP 响应
  -> core 收到
  -> core 构造 IPv4/UDP 或 IPv4/ICMP
  -> WintunAllocateSendPacket 成功
  -> WintunSendPacket 被调用
  -> Windows 应用超时
```

尚未确认的最终原因包括：

1. Windows IP 层因包头、checksum、地址或接口状态丢弃；
2. WFP/防火墙在 Public Profile 下丢弃；
3. compartment/interface 归属不一致；
4. UDP/ICMP 状态跟踪未把回包关联到原请求；
5. Wintun ring 提交后出现未观测到的驱动侧失败。

在完成第 6.3 节诊断矩阵前，不得把其中任一项写成最终根因。

### 6.2 增加分层数据面追踪

为每个测试流生成 64 位 `flow_id`，DNS 使用原始 transaction ID 辅助关联，记录以下阶段：

```text
TUN_RX             Windows -> core
POLICY_SELECTED    路由策略完成
REMOTE_TX          core -> server/remote
REMOTE_RX          server/remote -> core
LOCAL_RX           core 本地生成或复用的响应 -> Windows
PACKET_BUILT       IP/UDP/ICMP 包构造完成
WINTUN_ALLOCATED   ring slot 分配成功
WINTUN_SUBMITTED   WintunSendPacket 已调用
OS_OBSERVED        PktMon/WFP 观察到
APP_COMPLETED      测试进程收到响应
```

日志字段至少包含：

```text
flow_id, protocol, address_family,
src_ip, src_port, dst_ip, dst_port,
packet_length, ip_header_length, ttl, flags,
ip_checksum, transport_checksum,
interface_index, interface_luid,
route_origin, route_action, outbound,
error_code, elapsed_ms
```

高频日志必须支持采样和每秒限频；错误计数通过聚合指标保留，不得静默丢失。

DNS 合并具有严格的证据归属：第一个等待者拥有实际的上游请求，只有其上游发送真实成功后才能记录 `REMOTE_TX`，真实上游响应记为 `REMOTE_RX`；同一 wire query 的后续等待者不重复发送，其个性化 transaction ID 响应记为 `LOCAL_RX`。cache 命中和本地产生的 SERVFAIL 同样记为 `LOCAL_RX`。因此，远端完整链为 `TUN_RX -> POLICY_SELECTED -> REMOTE_TX -> REMOTE_RX -> ...`，本地完整链为 `TUN_RX -> POLICY_SELECTED -> LOCAL_RX -> ...`；两类都可继续到 `OS_OBSERVED/APP_COMPLETED`，但绝不互相冒充。

### 6.3 管理员诊断矩阵

使用同一构建、同一目标和固定测试时间窗完成：

| 组别 | 防火墙 | WFP/PktMon | 预期用途 |
| --- | --- | --- | --- |
| A | 正常 | Wintun + IP + Transport 抓包 | 建立正常故障基线 |
| B | 临时关闭 Public Profile，测试后立即恢复 | 同 A | 判断是否为防火墙/WFP 规则 |
| C | 正常 | WFP drop audit | 获取 filter ID/drop reason |
| D | 正常 | core 生成包落盘 + 离线校验 | 排除包头和 checksum |

每组执行：

```text
ping -n 20 -S 192.168.13.25 1.1.1.1
nslookup -timeout=2 -retry=1 example.com 192.168.13.1
UDP 固定端口 echo 测试
```

必须记录测试开始/结束的单调时钟和墙上时间，便于与 core 日志对齐。

### 6.4 数据包正确性断言

在 Debug/测试构建中，对注入包执行自校验：

1. IPv4 version 为 4，IHL 不小于 20；
2. IPv4 total length 等于提交长度；
3. IPv4 checksum 重新计算为 0；
4. UDP length 等于 IP payload 长度；
5. UDP checksum 重新计算为 0，计算结果为 0 时线上字段写 `0xffff`；
6. ICMP checksum 重新计算为 0；
7. DNS response transaction ID 等于原始客户端 ID；
8. DNS 源为 `192.168.13.1:53`，目的为原客户端 `192.168.13.25:<port>`；
9. 目的 IP 必须是当前 TAP/Wintun 地址；
10. 包长不得超过配置 MTU，除非明确执行分片。

若断言失败，禁止提交 Wintun，并记录一次结构化错误。

### 6.5 根据诊断结果实施

#### 情况 A：WFP/防火墙丢弃

- 精确识别阻断层、filter ID 和条件；
- 为程序、服务或 Wintun 接口建立最小范围规则；
- 规则仅允许已建立流的响应和虚拟网段所需协议；
- 不允许永久关闭防火墙或创建 Any/Any 全放行规则；
- 安装和卸载流程必须对称，升级时幂等更新。

#### 情况 B：包头或 checksum 错误

- 修复 `IPFrame::ToArray`、`UdpFrame::ToIp` 或 ICMP 构造函数；
- 添加固定字节向量测试，并使用第二套独立 checksum 实现校验；
- 对抓包文件执行 Wireshark/TShark checksum 验证。

#### 情况 C：接口/compartment 错误

- 以 Wintun LUID 和接口索引为唯一身份，不依赖易变化的 alias；
- 配置 IP、路由、DNS 和 Wintun session 时确保位于同一 compartment；
- 启动后验证接口状态、地址状态、路由 next hop 和 DNS 绑定；
- 任一项失败时终止启动并给出明确错误，不能继续进入半可用状态。

### 6.6 完成标准

只有同时满足以下条件，才可关闭此 P0：

- 100 次 DNS 查询无应用层超时；
- 100 次 ICMP 的隧道附加丢包不超过物理链路丢包 + 1%；
- PktMon/WFP 不再记录相关丢弃；
- core 的 `WINTUN_SUBMITTED` 数量与 OS 观察到的合法注入包数量一致；
- 重启隧道 20 次结果一致。

---

## 7. P1：修复静态回声并发与错误语义

### 7.1 根因

`VEthernetExchanger::StaticEchoLoopbackSocket` 允许多个 socket 同时执行 `async_receive_from`，但使用 exchanger 成员 `buffer_` 和 `static_echo_source_ep_`。异步操作尚未完成时，另一个 socket 可以覆盖二者。

`StaticEchoPacketToRemoteExchanger` 通过 `boost::asio::post` 安排同步 `send_to`，随后立即返回 `true`。调用方看到的成功只是 enqueue 成功，真实 `send_to` 错误未上报。

### 7.2 接收状态独占

引入每次接收独占的状态：

```cpp
struct StaticEchoReceiveOperation final {
    std::array<Byte, PPP_BUFFER_SIZE> buffer{};
    boost::asio::ip::udp::endpoint source;
};
```

每次调用创建 `shared_ptr<StaticEchoReceiveOperation>`，并捕获到回调中：

```cpp
auto operation = make_shared_object<StaticEchoReceiveOperation>();
socket->async_receive_from(
    boost::asio::buffer(operation->buffer),
    operation->source,
    [self, socket, operation](const error_code& ec, size_t size) {
        self->OnStaticEchoReceive(socket, operation, ec, size);
    });
```

不得捕获裸 `this` 作为唯一生命周期保证；以 `weak_ptr` 或 `shared_ptr` 控制销毁，并确保 `Dispose()` 后不再重挂接收。

### 7.3 发送结果语义

发送接口改成异步完成回调或明确命名：

```cpp
enum class SendDisposition {
    Rejected,
    Queued
};

using SendCompletion = function<void(const error_code&, size_t)>;
```

指标必须分开：

```text
static_echo_tx_queued
static_echo_tx_completed
static_echo_tx_failed
static_echo_rx_packets
static_echo_unpack_failed
static_echo_session_mismatch
static_echo_output_failed
```

### 7.4 安全校验

- 校验回包 endpoint 是否属于当前允许的静态回声服务器集合；
- 校验 session ID、协议、密文和包长；
- 解包失败不得增加有效 incoming packet 计数；
- 连续错误超过阈值时将静态 ICMP 标记为 degraded，并回退主隧道 ICMP 通道；
- 恢复必须通过真实 request/response 探测，不能用 send-only 结果判定。

---

## 8. P1：MTU、MSS 与 PMTU 设计

### 8.1 当前问题

Wintun 当前 MTU 为 65535，IPv4 SYN 可宣告 MSS 65495。隧道外层还包含传输协议、加密和可能的 TCP/WebSocket/TLS 开销，远端会发送远大于真实路径 MTU 的分段。

这可解释“TCP 已连接但 TLS 或下载长时间没有数据”的一部分现象，但不能解释小 DNS/ICMP 包，因此不得把 MTU 当作所有故障的唯一原因。

### 8.2 配置模型

建议增加：

```json
{
  "client": {
    "tun": {
      "mtu": 1400,
      "mss-clamp": true,
      "mss-v4": 1360,
      "mss-v6": 1340
    }
  }
}
```

约束：

```text
IPv4 MTU: 576..1500，默认 1400
IPv6 MTU: 1280..1500，默认 1400
MSSv4 <= MTU - 40
MSSv6 <= MTU - 60
```

如果配置只提供 MTU，MSS 自动计算；显式 MSS 只能进一步降低，不能超过自动上限。

### 8.3 应用位置

1. Wintun 创建、获得接口索引并配置地址后立即调用 `SetInterfaceMtu`；
2. 读取接口状态确认设置已生效，失败则启动失败或明确降级；
3. IPv4 客户端 SYN 在进入隧道前调用 `ClampTcpMssIPv4`；
4. IPv6 SYN 使用对应 clamp；
5. 修改 TCP option 后重新计算 TCP checksum；
6. 已分片包、非首片和非 SYN 不执行 MSS 修改。

### 8.4 PMTU 行为

- 保留有效 ICMP Fragmentation Needed/Packet Too Big；
- 禁止无条件丢弃用于 PMTU 的 ICMP；
- 对外层 TCP 隧道监控长时间无前进的连接，记录发送序列、ACK 前进和队列长度；
- 不以清除 DF 作为默认方案，优先正确 MTU/MSS。

---

## 9. P1：MUX 启用与隔离

### 9.1 前置门禁

启用 MUX 前必须完成：

- P0 路由自环修复；
- DNS/ICMP 回包交付修复；
- MTU/MSS 生效验证；
- 非 MUX 基线达到验收标准。

### 9.2 推荐策略

建议默认建立 2～4 个 MUX 通道，而不是所有流共享单通道：

```text
control plane: 独立连接
mux channel 0: 交互/低延迟
mux channel 1: 普通网页
mux channel 2+: 大流量或备用
```

流分配采用连接级散列与负载共同决策。单通道出现拥塞、超时或协议错误时，只迁移/失败该通道，不能处置主控制连接。

### 9.3 回退

- MUX 握手失败：单流回退非 MUX；
- 通道持续无进展：关闭该通道并补建；
- 达到错误阈值：会话级临时关闭 MUX，保留核心隧道；
- 配置保留 `tun_mux=0` 紧急回退能力。

### 9.4 指标

```text
mux_channels_active
mux_streams_active
mux_stream_open_latency_ms
mux_queue_bytes
mux_head_of_line_stall_ms
mux_channel_resets
mux_fallback_connections
```

---

## 10. P2：TUI/core 日志启动契约

### 10.1 根因

GUI 和终端启动路径使用 `set_optional_if_not_default(..., "--log-file", ..., "./ppp-core.log")`。当用户选择默认路径时，参数被省略；core 的日志路径保持空字符串，最终不会打开文件。

### 10.2 修复规则

日志等级和日志目标必须独立生成参数：

```text
log_level != none 且 log_file 非空
    -> 总是传 --log-file
log_level == none
    -> 可不传 --log-file
log_file 为空但 level != none
    -> TUI 设置校验失败，禁止启动
```

GUI 和终端路径应调用无“默认省略”语义的参数函数：

```rust
set_optional_command_argument(&mut args, "--log-file", &settings.log_file);
```

### 10.3 路径语义

- TUI 启动前把相对路径规范化为基于 TUI 工作目录的绝对路径；
- 在界面显示最终绝对路径；
- 启动前验证父目录存在且可写；
- core 打开失败必须通过启动响应/RPC 返回 Win32 error 或 `std::error_code`；
- 禁止静默退回空日志。

### 10.4 日志轮转

Debug 数据面日志可能快速增长，应提供：

```text
max_file_size_mb: 64
max_files:        4
flush_interval:   1s
```

凭据、GUID、token 和密钥必须脱敏；地址和端口在用户明确开启网络诊断时允许记录。

### 10.5 凭据扫描门禁

仓库使用零依赖扫描器 `tests/tools/scan_log_secrets.py` 检查 URI userinfo、Authorization/Proxy-Authorization、password/token/secret/API key/private key/debug key 赋值和 GUID。扫描报告只能包含规则名与相对行号，禁止回显命中文本。管理员验收记录启动时 core 日志字节长度，只扫描本轮追加区间；若期间发生轮转，字节偏移失去可信度，本轮直接失败而不是跳过。验收汇总只记录代理是否配置，不回写可能含有 userinfo 的代理参数。

core 运行日志不得输出原始 session ID。独立流量审计日志若确需跨事件关联，使用原始 GUID 的 SHA-256 前 96 位作为稳定伪名；原始 GUID 只保留在内存和既有管理协议字段中，不写入运行/审计日志。伪名用于关联而非认证，禁止把它作为 token 或权限凭据。

该扫描器是发布门禁而非脱敏实现的替代品：任何命中都必须回到日志产生点移除或不可逆脱敏，禁止通过放宽规则、删除报告或把真实凭据加入白名单使门禁变绿。

---

## 11. 配置兼容与迁移

### 11.1 默认值

建议新版本默认：

```text
tun.mtu=1400
tun.mss-clamp=true
tun_mux=0（第一阶段保持；通过门禁后再改默认）
core log level=error
core log file=./ppp-core.log
```

### 11.2 旧配置

- 缺少 MTU 字段时使用 1400；
- 旧配置显式设置 65535 时输出警告并钳制到 1500，除非存在仅测试构建的危险开关；
- 旧 `tun_mux` 数值保持解析兼容；
- 日志文件路径的含义不变，只修复参数被省略的问题。

### 11.3 能力协商

本轮路由、Wintun、MTU/MSS 和日志修复均为客户端本地行为，不应修改协议版本。MUX 新行为若改变帧语义，则必须通过已有能力协商启用；未协商时保持旧行为。

---

## 12. 实施拆分

每个阶段必须独立可构建、可测试、可回滚。

### 阶段 A：P0 路由正确性

改动：

1. 修复 `GetBestInterface` 判断；
2. 修复 `IsDirectProxyAddress`，排除 TUN 下一跳；
3. 增加 route origin/action；
4. 增加路由分类单元测试；
5. 增加递归 SYN 检测计数。

通过门禁：SOCKS IP/域名各 50 次无自环、无 10060。

### 阶段 B：Wintun 回包诊断与 P0 修复

改动：

1. 引入 `flow_id`；
2. 增加包头自校验；
3. 增加 Wintun 提交指标；
4. 执行管理员诊断矩阵；
5. 根据证据修复 WFP、包头或 compartment。

通过门禁：DNS、ICMP 各 100 次达到第 6.6 节标准。

### 阶段 C：静态回声

改动：

1. 每次接收独占 buffer/endpoint；
2. 发送改为真实完成语义；
3. 增加解包、session、output 错误指标；
4. 增加静态通道 degraded/fallback 状态机。

通过门禁：并发和故障注入测试无竞态、无内存错误、可自动回退恢复。

### 阶段 D：MTU/MSS

改动：

1. 增加配置与钳制；
2. Windows 接口设置 MTU；
3. IPv4/IPv6 MSS clamp 接入；
4. 增加 checksum 和抓包测试。

通过门禁：抓包值正确，10 MB/100 MB 测试无 PMTU 停顿。

### 阶段 E：MUX

改动：

1. 小规模通道池；
2. 通道隔离与回退；
3. 指标和压力测试；
4. 灰度启用。

通过门禁：P95 首包延迟优于非 MUX，故障率不得升高。

### 阶段 F：TUI 日志契约

改动：

1. 始终传递启用状态下的日志文件；
2. 规范化和展示绝对路径；
3. 打开失败返回 UI；
4. 日志轮转和脱敏。

通过门禁：默认路径、自定义路径、不可写路径和动态等级切换全部有自动测试。

---

## 13. 测试设计

### 13.1 单元测试

- RIB/FIB 最长前缀与 route origin；
- Win32 API 返回码封装；
- IPv4/IPv6 MSS option 修改；
- TCP/UDP/IP/ICMP checksum；
- DNS transaction ID 恢复；
- 静态回声并发 operation 生命周期；
- TUI 命令行参数生成。

### 13.2 集成测试

| 类别 | 数量 | 通过标准 |
| --- | ---: | --- |
| 隧道 ICMP | 100 | 附加丢包 ≤ 1% |
| DNS A/AAAA/PTR | 各 100 | 0 超时，ID/端口匹配 |
| SOCKS IPv4 | 50 | 100% 成功，无自环 |
| SOCKS 域名 | 50 | 100% 成功，无额外 10 秒解析阻塞 |
| HTTP CONNECT | 50 | 100% 成功 |
| HTTPS 短连接 | 100 | 无连接后长期无数据 |
| 10 MB 下载 | 20 | 无停顿，checksum 正确 |
| 100 MB 下载 | 5 | 吞吐稳定，无异常重传峰值 |
| 隧道重启 | 20 | 地址、路由、DNS、MTU 一致 |

仓库管理员脚本 `tests/tools/Invoke-DataplaneAcceptance.ps1` 提供两个明确档位。默认档位只产生 `gate_scope=automated_base_only`，用于快速检查 ICMP、DNS、可选 SOCKS、MTU、PktMon 和 core 阶段一致性。`-FullGate` 产生 `gate_scope=automated_extended_runtime`，并强制要求不同的 SOCKS 数字 IPv4/域名 URL、HTTP CONNECT 代理、HTTPS URL、Python 应用探针，以及 10 MB/100 MB 下载 URL 与预先独立获得的 SHA256；它固定执行表中的 50/50/50/100/20/5 次数。下载不仅校验哈希，还分别要求至少 10,000,000 和 100,000,000 字节，防止错误页或小文件假通过。

`automated_extended_runtime` 仍不包含重启、WFP A/B/C、故障注入、物理接口切换和 24 小时稳定性，不能被写成发布门禁全部通过。下载 SHA256 必须来自可信发布源或测试前的独立带外计算，禁止用本轮收到的文件反推期望值。

重启门禁由 `tests/tools/Invoke-DataplaneRestartGate.ps1` 单独执行。测试人员必须提供受版本控制、可审计且不含凭据的 `StartScript`/`StopScript`，脚本固定要求 20/20：每轮启动后 TUN 地址出现、双栈 MTU/DNS/路由签名一致、ICMP/DNS 冒烟成功；停止后测试地址、对应接口路由及测试 DNS 均消失。该脚本只验证重启一致性和退出清理，不得代替 WFP、故障注入、物理切换和 24 小时门禁。

### 13.3 故障注入

- 丢弃静态回声响应；
- 延迟或乱序两个静态 socket 的回调；
- Wintun ring 暂时满；
- 物理接口切换；
- DNS 上游一个成功、一个超时；
- MUX 单通道阻塞和断开；
- core 日志路径不可写；
- 服务端入口短暂不可达后恢复。

### 13.4 性能测试

比较修复前后：

```text
TCP connect P50/P95/P99
TLS first byte P50/P95/P99
DNS latency P50/P95/P99
吞吐量
重传率
CPU
内存
日志开启/关闭开销
每秒外层新建连接数
```

Debug 日志测试与默认 error 日志测试必须分开，避免把诊断 I/O 当成数据面性能。

---

## 14. 发布、灰度与回滚

### 14.1 发布顺序

1. 先发布路由分类和 Win32 API 修复；
2. 再发布 Wintun 诊断版，仅对测试用户开启详细追踪；
3. 根据证据发布回包修复；
4. 发布 MTU/MSS，默认 1400；
5. MUX 从 5% 用户开始灰度；
6. 最后考虑把 MUX 改为默认开启。

### 14.2 回滚开关

必须保留：

```text
route-origin-policy=false（仅紧急回退）
static-echo=false
mss-clamp=false
tun-mtu=<旧值，测试用途>
tun-mux=0
dataplane-trace=false
```

P0 路由自环修复不建议长期回滚；若产生兼容问题，应修正规则数据，而不是恢复“任意 RIB 路由即直连”。

### 14.3 自动停止灰度条件

满足任一项立即停止扩大灰度：

- DNS 超时率高于基线 0.5 个百分点；
- TCP 连接失败率高于基线 0.5 个百分点；
- 出现新的路由自环；
- core 崩溃率或内存使用显著上升；
- MUX P95 首包延迟劣化超过 10%；
- Windows 网络配置在进程退出后未恢复。

---

## 15. 验收清单

### 正确性

- [ ] TUN 默认路由不会被判断为 direct。
- [ ] `GetBestInterface` 正确处理 `NO_ERROR` 和错误码。
- [ ] SOCKS/HTTP 日志不再出现自递归 SYN。
- [ ] DNS 回包能够到达发起请求的 Windows socket。
- [ ] ICMP 回包能够到达 `ping`/ICMP API。
- [ ] 静态回声每个异步接收拥有独立状态。
- [ ] Wintun 注入前包头与 checksum 自校验通过。
- [ ] MTU 和 MSS 在系统状态及抓包中一致。
- [ ] MUX 故障不影响控制连接。
- [ ] 默认 core 日志路径能够创建并写入。

### 可靠性

- [ ] 隧道连续运行 24 小时无异常重连风暴。
- [ ] DNS、ICMP、TCP 压力测试达到门禁。
- [ ] 物理网络切换后自动恢复。
- [ ] 日志轮转不会阻塞数据面或耗尽磁盘。
- [ ] 进程退出后路由、DNS、防火墙规则完整恢复。

### 可观测性

- [ ] 每个失败可以定位到路由、远端、包构造、Wintun、WFP 或应用阶段。
- [ ] queued 与 completed 指标严格分离。
- [ ] 重复错误有限频和抑制计数。
- [ ] 日志不包含凭据和密钥。

---

## 16. 预期结果

完成全部阶段后，预期行为为：

1. SOCKS、HTTP CONNECT、普通 TUN TCP 使用相同且明确的路由策略，不再自环。
2. DNS 和 ICMP 回包可以从远端稳定交付到 Windows 应用，而不仅是到达 core。
3. TCP 不再宣告与真实隧道路径不匹配的超大 MSS，大流量和 TLS 不再出现 PMTU 型停顿。
4. MUX 降低逐连接握手开销，同时保持通道级故障隔离和非 MUX 回退。
5. core 日志路径、等级、轮转和失败提示行为明确，默认路径不再静默失效。
6. 新的结构化指标能够把“线路慢”“服务端慢”“策略错误”“驱动丢包”和“应用未接收”区分开。

本方案的核心原则是：先保证路由和回包正确，再优化吞吐与延迟；所有成功状态必须代表真实完成阶段，所有未证实的系统丢弃原因必须通过抓包和 WFP 证据闭环。
