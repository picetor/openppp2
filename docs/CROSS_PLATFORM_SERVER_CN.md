# OpenPPP2 服务端跨平台对齐设计

状态：已落地第一阶段代码，Windows/macOS routed-GUA 进入 Preview，macOS NAT66 进入实验 Preview；Windows 默认 NAT66 采用与 IPv4 对齐的用户态 transit，WFP NAT66 保留为可选高级后端
目标：以当前 Linux 服务端为功能基线，补齐 Windows，设计 macOS，评估 Android
优先级：Windows P0，macOS P1/P2，Android Experimental

## 1. 范围纠正与结论

Windows 和 macOS **已经存在 OpenPPP2 协议服务端**。当前 `main.cpp` 默认进入 server mode，三套桌面构建都会编译 `ppp/app/server/`，并通过同一套 `VirtualEthernetSwitcher` 启动监听器。

本设计的目标不是把程序注册成 Windows Service 或 launchd daemon，而是让 Windows/macOS 的协议服务端能力逐项对齐 Linux，重点补齐此前只在 Linux 启用的 IPv6 transit 数据面。

```text
                  VirtualEthernetSwitcher（已有、跨平台）
       TCP / UDP / WS / WSS / CDN / 鉴权 / MUX / 映射 / 管理
                                │
                   IServerTransitPlatform（新增）
                                │
          ┌─────────────────────┼─────────────────────┐
          │                     │                     │
       LinuxProvider       WindowsProvider       DarwinProvider
    TAP + route + NDP     Wintun + IP Helper     utun + route socket
    + ip6tables/nft       + user-mode transit   + pf/ND proxy
                         (optional WFP NAT66)
```

结论：

- IPv4 和协议控制面主体已经跨平台，不应重写。
- 当前实质差距集中在跨平台 NAT66、on-link GUA/NDP、uplink 事件恢复和 Linux 多队列优化。
- Windows 优先完成 IPv6 transit、routed-GUA、状态恢复和测试矩阵；默认 NAT66 与 IPv4 一样走用户态虚拟子网转发，不要求安装自定义 WFP 驱动。
- macOS 已接入 utun、route、forwarding 和专属 pf anchor NAT66 实验后端，待客户端可运行后做端到端验收。
- Android 可以接入现有 IPv4/协议服务端核心，但非 root 环境不适合复制桌面 IPv6 transit 方案。
- systemd/SCM/launchd 托管属于部署增强，不是本轮“服务端对齐”的主线。

## 2. 当前功能矩阵

根据当前代码结构，服务端功能分为三类。

### 2.1 已经共享的服务端能力

以下能力位于公共 `ppp/app/server/`、协议层和配置层，Windows/macOS 构建已经包含：

| 功能 | Linux | Windows | macOS | 说明 |
|---|---:|---:|---:|---|
| server mode 启动 | 有 | 有 | 有 | `--mode=server`，默认即 server |
| PPP TCP/UDP | 有 | 有 | 有 | 公共 acceptor/datagram 实现 |
| WebSocket/WSS | 有 | 有 | 有 | 公共服务端实现 |
| CDN 入口 | 有 | 有 | 有 | 公共 acceptor 分类 |
| 加密、压缩、传输参数 | 有 | 有 | 有 | 公共 transmission 层 |
| GUID 鉴权与会话管理 | 有 | 有 | 有 | 公共 exchanger/managed server |
| MUX 协商与转发 | 有 | 有 | 有 | 公共 mux/protocol 层 |
| IPv4 虚拟地址和地址池 | 有 | 有 | 有 | 公共 IPv4 pool/NAT ownership |
| IPv4 子网互通 | 有 | 有 | 有 | `server.subnet` 公共路径 |
| 端口映射 | 有 | 有 | 有 | `server.mapping` 公共路径 |
| 防火墙规则文件 | 有 | 有 | 有 | 应用层 `ppp::net::Firewall`，非 OS 防火墙 |
| 管理面板/legacy backend | 有 | 有 | 有 | `VirtualEthernetManagedServer` |
| 日志、遥测和统计 | 有 | 有 | 有 | 公共 logger/telemetry |

这些项目需要补自动化验证，但不是重新实现对象。

### 2.2 当前仅 Linux 完整支持的能力

| 功能 | Linux 当前实现 | Windows/macOS 当前状态 |
|---|---|---|
| server IPv6 配置生效 | `nat66` / `gua` | Windows/macOS `gua` 可启动；macOS PF NAT66 实验后端可启动；Windows NAT66 默认复用用户态 transit，不要求签名 WFP 包 |
| IPv6 transit 虚拟接口 | 独立 TAP `*_t6` | Windows 使用 Wintun/TAP，macOS 使用 utun |
| IPv6 地址租约/静态绑定 | 公共逻辑 + Linux 数据面 | 公共租约逻辑已接入 Windows/macOS transit |
| 每客户端 `/128` 路由 | `TapLinux::AddRoute6/DeleteRoute6` | Windows IP Helper/netsh helper，macOS route helper |
| NAT66 | IPv6 forwarding + ip6tables MASQUERADE/FORWARD | Windows 默认仅提供与 IPv4 对齐的用户态虚拟子网转发；macOS 使用专属 pf anchor 实验后端；Windows WFP callout 为可选透明外网后端 |
| GUA | forwarding + 路由 + uplink NDP proxy | routed-prefix 已实现骨架；on-link NDP proxy 未实现 |
| uplink 变化恢复 | 重放 `/128` route/NDP proxy | Windows/macOS 尚待事件监听和重放 |
| transit 邻居项 | Linux TAP permanent neighbor | 未实现 |
| transit 多队列 SSMT | Linux TAP 多 FD/mq affinity | Windows/macOS 无对应优化 |
| 环境回滚 | 恢复 sysctl、规则、route、neighbor | forwarding 快照已接入；完整 route/NDP 所有权仍待补齐 |

### 2.3 不能混淆的两种 IPv6

- 监听地址支持 IPv6：服务端 socket 能绑定 `::`。
- 隧道 IPv6 数据面：给客户端分配 IPv6，并将数据包送到公网或其他客户端。

本轮对齐的是第二项。监听器绑定 IPv6 并不代表 NAT66/GUA 已可用。

## 3. Linux 参考行为

Linux 是行为基线，而不是要求其他平台复制 Linux 命令。

### 3.1 公共控制面

当前以下逻辑已经在 `VirtualEthernetSwitcher` 中跨平台存在，应继续保持一份实现：

- 解析并规范化 IPv6 CIDR、gateway、DNS、lease-time 和静态地址。
- 在握手中协商 IPv6 mode、地址、网关、DNS 和路由前缀。
- 分配、续期、释放 IPv6 lease，检测地址冲突。
- 建立 `session_id <-> IPv6 address` 所有权表。
- 校验客户端 IPv6 source，防止地址伪造。
- 处理 client-to-client IPv6 转发。
- 将入站公网回包按目的地址送回正确 session。
- MSS clamp、包日志、错误码和运行状态。

### 3.2 Linux 数据流

```text
客户端 IPv6 包
  -> VirtualEthernetExchanger
  -> SendIPv6TransitPacket
  -> server transit TAP
  -> Linux IPv6 forwarding
  -> NAT66 MASQUERADE 或 GUA 路由
  -> 物理 uplink

物理 uplink 回包
  -> conntrack/NDP + /128 route
  -> server transit TAP
  -> ReceiveIPv6TransitPacket
  -> IPv6 ownership table
  -> 对应客户端 session
```

### 3.3 对齐判定

“对齐 Linux”以外部行为判定：

- 同一份服务端配置语义一致。
- 同一客户端能获得一致的地址、DNS、路由和错误结果。
- NAT66、GUA、client-to-client、静态租约行为一致。
- 启动失败必须回滚平台网络状态。
- 网络切换后可恢复或明确降级，不能保持假 Running。
- Linux 特有的具体命令、接口名称和多队列实现不要求相同。

## 4. 平台抽象

当前 `VirtualEthernetSwitcher.cpp` 内散布 `_LINUX` 和 `TapLinux` 调用。应集中到一个服务端 transit provider，避免继续增加平台条件编译。

### 4.1 建议接口

```cpp
enum class ServerIPv6Mode {
    None,
    Nat66,
    Gua,
};

struct ServerTransitCapabilities {
    bool ipv6_transit;
    bool nat66;
    bool gua_routed_prefix;
    bool gua_onlink_prefix;
    bool multi_queue;
};

struct ServerTransitOptions {
    ServerIPv6Mode mode;
    std::string cidr;
    int prefix_length;
    std::string gateway;
    std::string preferred_uplink;
    std::string interface_name;
};

class IServerTransitPlatform {
public:
    virtual ServerTransitCapabilities Capabilities() const = 0;
    virtual Result Open(const ServerTransitOptions&, PacketInput) = 0;
    virtual Result PrepareUplink() = 0;
    virtual Result AddClient(const IPv6Address&, const SessionId&) = 0;
    virtual Result RemoveClient(const IPv6Address&, const SessionId&) = 0;
    virtual Result RefreshUplink() = 0;
    virtual Result Send(const void* packet, int length,
                        const SessionId* affinity) = 0;
    virtual void Close() = 0;
    virtual ServerTransitSnapshot Snapshot() const = 0;
};
```

`Open()` 和 `PrepareUplink()` 分开，便于平台先创建接口，再原子应用 forwarding/NAT/NDP。任一步失败都调用 `Close()` 回滚已经取得的资源。

### 4.2 代码迁移边界

保留在公共层：

- lease、静态绑定、session ownership。
- IPv6 包校验、client-to-client 路由决策。
- 握手扩展、错误响应和状态展示。
- 调用 provider 的时序。

迁移到 provider：

- transit interface 创建、地址和读取循环。
- `/128` route 增删。
- NAT66、forwarding 和 GUA 出口准备。
- neighbor/NDP proxy。
- uplink 发现、变化重放和状态恢复。
- 平台性能优化。

## 5. Windows 对齐方案（重点）

### 5.1 总体方案

Windows provider 使用现有 Wintun 和 IP Helper 基础，不为 server 再造一套虚拟网卡框架：

```text
WindowsServerTransit
├── TapWindows / WintunAdapter        数据包收发
├── NetworkInterface / Router         地址、/128 路由、邻居
├── IP Helper                         uplink、forwarding、状态快照
├── WindowsGuaProvider                routed GUA / on-link GUA
├── WindowsUserModeTransit            IPv4-aligned virtual-subnet forwarding
└── WindowsWfpNat66Provider           optional transparent WFP NAT state
```

transit adapter 必须与客户端 adapter 分名，建议默认 `OpenPPP2 Server IPv6`，并使用稳定 GUID，避免重启后接口索引变化造成残留规则指向错误对象。

### 5.2 先完成无 NAT 的 transit 骨架

第一阶段先打通公共 IPv6 数据流，不同时处理所有出口模式：

1. 允许 Windows server 保留 `server.ipv6`，由启动阶段 provider capability 校验；不再静默改成 `none`。
2. 用 Wintun 创建独立 L3 transit adapter。
3. 给 adapter 设置 transit gateway 地址，避免自动生成错误的 on-link `/64` 路由。
4. 通过 IP Helper 添加/删除每客户端 `/128` route。
5. PacketInput 接入现有 `ReceiveIPv6TransitPacket()`。
6. Output 接入现有 `SendIPv6TransitPacket()`。
7. 完成 client-to-client IPv6 和静态 lease 测试。

这一阶段不宣称公网 NAT66 完成，但已经可以验证公共控制面、client-to-client、routed-GUA 和回包分发逻辑。

### 5.3 Windows GUA

GUA 分两种部署条件：

#### Routed prefix（优先、无需 NDP proxy）

上游路由器已经把一个 IPv6 prefix 路由到 Windows 服务端。这是 Windows 第一条生产路径：

- 探测并锁定物理 uplink interface LUID/index。
- 快照 transit 和 uplink 的 `MIB_IPINTERFACE_ROW`。
- 使用 `SetIpInterfaceEntry` 开启 IPv6 forwarding。
- 为每个客户端安装 transit adapter `/128` route。
- 不在物理 uplink 发布该 prefix，也不做邻居代理。
- Close 时只恢复本进程改变的 forwarding 字段并删除本进程拥有的 route。

#### On-link prefix（需要 ND proxy）

如果客户端 GUA 与物理 uplink 同属一个链路前缀，上游会直接发 Neighbor Solicitation。Windows provider 必须代表客户端响应 NDP：

- 不能把普通静态 neighbor entry 当作 proxy NDP。
- 可在后续 WFP/NDIS 驱动中处理 NS/NA，或明确要求上游静态路由。
- 在 ND proxy 未实现前，capability 报告 `gua_routed_prefix=true`、`gua_onlink_prefix=false`。

### 5.4 Windows NAT66

Windows 默认不追求 Linux `ip6tables -t nat ... MASQUERADE` 那种透明公网 NAT66，而是先对齐现有 IPv4 服务端：使用 server-side Wintun/TAP、IPv6 ownership table 和用户态 exchanger 完成虚拟子网及 client-to-client 转发。这样普通安装不加载 OpenPPP2 自定义 `.sys`，也不要求用户处理 WFP 驱动签名。

该模式明确不承诺任意 IPv6 流量透明访问公网；需要公网出口、任意协议、ICMPv6 和完整有状态地址/端口转换时，仍需额外的内核数据面。仓库保留可选的签名 WFP 后端：`windows/wfp/OpenPpp2WfpNat66.c`、固定宽度 IOCTL ABI、INF、WDK 工程和 `tools/sign-openppp2-wfp.ps1`，仅在明确需要透明 NAT66 时单独安装。

```text
client IPv6 packet
  -> VirtualEthernetExchanger
  -> user-mode IPv6 transit TAP
  -> destination ownership lookup
  -> client-to-client / virtual-subnet session
```

可选 WFP 后端的 v1 仍必须覆盖：

- TCP、UDP、ICMPv6 echo；ICMPv6 error 和不具备安全 flow key 的报文 fail-closed 计数并丢弃。
- extension header 安全解析；无法安全解析时丢弃并计数。
- v1 对整个 IPv6 fragment group fail-closed（包括首片），不把只改首片当作可用实现；后续需在 WDK 实机测试中补齐重组路径。
- NAT key 至少包含 protocol、ULA source、source port/id、remote address、remote port/id。
- uplink GUA 变化时停止新建旧地址映射；旧 mapping 排空或超时。
- driver 与用户态通过受 ACL 限制的 IOCTL 交换配置和统计。
- driver 包签名、版本兼容、安装/卸载回滚。

开发期可以使用 WinDivert 一类拦截驱动验证包改写和状态机，但正式发行不应把未控制版本和签名的第三方驱动作为隐式依赖。

### 5.5 Windows 状态所有权与恢复

每项 OS 变更记录“原始值、目标值、是否由本进程创建”：

```cpp
struct OwnedWindowsTransitState {
    NET_LUID transit_luid;
    NET_LUID uplink_luid;
    bool changed_transit_forwarding;
    bool original_transit_forwarding;
    bool changed_uplink_forwarding;
    bool original_uplink_forwarding;
    std::vector<OwnedRoute> routes;
    DriverSessionId nat_session;
};
```

清理原则：

- 只删除本 session 创建且内容仍匹配的 route/rule。
- 不关闭用户原本开启的 forwarding。
- adapter index 仅作缓存，身份以 LUID/GUID 为准。
- 崩溃恢复时枚举带 OpenPPP2 owner/tag 的对象，不能按整个 IPv6 表清空。

### 5.6 Windows 实施顺序

| 阶段 | 内容 | 完成标志 |
|---|---|---|
| W0 | 现有 IPv4 服务端回归测试 | TCP/UDP/WS/WSS、映射、subnet、management 全通过 |
| W1 | transit + Wintun + `/128` route | 代码已接入，需 Windows 实机回归 client-to-client/lease |
| W2 | routed-prefix GUA | forwarding/route 骨架已接入，需上游路由实机验收 |
| W3 | 可选 WFP NAT66 | 仅在需要透明公网 NAT66 时验证 TCP/UDP/ICMPv6、并发、fragment、网络切换 |
| W4 | on-link GUA ND proxy | 无上游静态路由时也能回包 |
| W5 | 性能与稳定性 | 24/72 小时、压力、故障注入通过 |

### 5.7 Windows 不应照搬 Linux 的部分

- 不通过 `system("netsh ...")` 实现主路径；使用 IP Helper，WFP 仅用于可选透明 NAT66 后端。
- 不模拟 Linux TAP 多队列 FD affinity；Wintun 使用自身 ring 和批量处理优化。
- 不依赖 iptables/nftables 文本规则。
- 不因透明 NAT66 未启用就静默把配置改为 `none`；默认用户态虚拟子网转发仍须报告明确 capability，透明后端则单独报告可用性。

## 6. macOS 对齐设计

macOS 与 Windows 使用相同的公共调用路径；当前已接入可编译的 utun/route/forwarding 服务端骨架，待客户端跑通后再做实机端到端验收。

### 6.1 transit 与路由

- 使用独立 utun 作为 L3 transit interface。
- 当前复用 Darwin IPv6 helper 增删 `/128` route；后续应迁移到 routing socket，避免公共 server 代码继续扩大 shell 依赖。
- 快照并按所有权恢复 `net.inet6.ip6.forwarding`。
- 监听 interface/address route event，uplink 变化时重放 client route。

### 6.2 GUA

- routed prefix 作为第一支持路径，逻辑与 Windows 一致。
- on-link GUA 需要实现并实测 macOS NDP proxy；在此之前明确报 capability 不支持。
- 不把客户端已有的 macOS IPv6 辅助代码直接当作服务端 provider，它们修改的是客户端默认路由/DNS，职责不同。

### 6.3 NAT66

当前实现先确认 PF 状态为 Enabled 且主 ruleset 已挂载 `openppp2/nat66` anchor，再加载 NAT、状态和 transit pass 规则；退出时只 flush 该 anchor。仓库提供 `darwin/pf/install-openppp2-pf.sh` 和 `uninstall-openppp2-pf.sh`，但仍需在真实 macOS 上验证 anchor 挂载、睡眠唤醒和 PF 版本差异。如果系统 PF 未启用、anchor 未挂载或规则校验失败，会返回 `IPv6Nat66Unavailable`，不会覆盖系统主 ruleset。

后续仍需在实机确认不同 macOS 版本的 anchor 挂载方式、睡眠唤醒后的 state 保留，以及 fragment/ICMPv6 error 行为；未完成前保持 Preview。

必须实机验证后才能选择方案，不能仅以配置加载或 `pfctl` 返回成功作为完成。

### 6.4 macOS 发布门槛

- arm64 和 amd64 均编译。
- client-to-client、routed GUA、NAT66 各有双机测试。
- 睡眠/唤醒、Wi-Fi 切换、有线切换后状态可恢复。
- pf/sysctl/route/utun 均无残留。
- 客户端尚未跑通期间保持 Preview，不对外宣称与 Linux 等价。

## 7. Android 评估

### 7.1 已有基础

- Android CMake 已编译公共 `ppp/`，包括 server core。
- JNI 文件已经包含 `VirtualEthernetSwitcher`，但 runtime 字段和生命周期只接了 `VEthernetNetworkSwitcher` 客户端。
- 因此“协议服务端不能编译”不是主要问题，缺的是 JNI bridge、前台服务生命周期和测试。

### 7.2 可实现范围

Android 非 root MVP 可以实现：

- TCP/UDP/WS/WSS/CDN 监听。
- GUID 鉴权、MUX、mapping、管理面板和日志。
- 当前不依赖 server transit TAP 的 IPv4 服务端路径。
- 用户主动启动的 Foreground Service。
- 局域网或具备端口映射条件的实验节点。

### 7.3 不能直接对齐的部分

- 普通应用不能像 Linux 一样创建额外系统 TAP、修改全局路由、开启系统 forwarding 或安装 NAT66/NDP proxy。
- Android `VpnService` 的 TUN 是应用 VPN 接口，不是可直接替代 Linux server transit TAP 的公网转发接口。
- 要支持 server IPv6，需把 NAT/flow termination 下沉为完整用户态网络栈，通过普通 socket 代表每个 flow 外连；这是一条独立的大工程。
- 移动网络常见 CGNAT 和入站过滤，监听成功不等于公网可达。

### 7.4 Android 定位

| 能力 | 结论 |
|---|---|
| IPv4/协议服务端 MVP | 可做 |
| 局域网临时节点 | 可做，优先场景 |
| IPv6 client-to-client（纯用户态） | 可研究 |
| Linux 等价 NAT66/GUA 公网数据面 | 非 root 下不可直接移植 |
| 24×7 生产公网节点 | 不建议承诺 |

Android 排在 Windows/macOS 对齐之后，标记 Experimental。

## 8. 配置与能力语义

删除“非 Linux server 直接清空 IPv6 配置”的行为，改为显式能力协商：

```json
{
  "server": {
    "ipv6": {
      "mode": "nat66",
      "cidr": "fd42:4242:4242::/64"
    }
  },
  "runtime-capabilities": {
    "server-ipv6-transit": true,
    "server-ipv6-nat66": false,
    "server-ipv6-gua-routed": true,
    "server-ipv6-gua-onlink": false
  }
}
```

处理规则：

- 用户明确配置 `nat66`：Windows 默认启动用户态虚拟子网转发，不要求签名 WFP；macOS 仍需 PF anchor 实验后端。只有显式选择可选透明 WFP 后端且驱动不可用时，才返回 `IPv6Nat66Unavailable`。
- 用户明确配置 `gua` 但仅支持 routed prefix：启动前检查上游路由条件，失败时返回可操作错误。
- 配置未启用 IPv6：正常运行 IPv4，不创建 transit provider。
- 不允许静默降级后仍向客户端声称 server IPv6 可用。

同一配置文件可跨平台共享，但结果必须可预测；必要时增加平台 override，而不是运行时偷偷改写用户配置。

## 9. 测试基线

### 9.1 公共协议回归

每个平台都运行：

- TCP、UDP、WS、WSS、CDN 建连。
- GUID 正确/错误/重复策略。
- 单连接与 MUX。
- IPv4 TCP/UDP/ICMP、client-to-client subnet。
- TCP/UDP port mapping。
- management policy 拉取、缓存和离线继续运行。
- 正常退出、异常断链、配置错误、端口冲突。

### 9.2 IPv6 对齐用例

- 动态租约、静态租约、重复地址、租约回收。
- TCP、UDP、ICMPv6 出站和回包。
- client-to-client，`server.subnet` 两种配置。
- 多客户端相同远端五元组，验证 NAT state 隔离。
- extension headers、fragment、PMTU、ICMPv6 Packet Too Big。
- uplink GUA 变化、网卡禁用/恢复、睡眠唤醒。
- 启动中途失败、强制杀进程后的残留审计。
- Linux 与 Windows/macOS 使用相同客户端和相同预期结果。

### 9.3 性能指标

- 吞吐、P50/P95/P99 RTT、CPU、内存、丢包率。
- 1、10、100、1000 session 分级测试。
- Wintun ring backpressure；若启用可选透明后端，再覆盖 WFP NAT table 上限。
- 24 小时功能稳定测试，Windows NAT66 完成后增加 72 小时 soak。

## 10. 实施路线

### M0：建立准确基线

- 为 Windows/macOS 加现有 IPv4 server CI/集成测试。
- 生成 Linux/Windows/macOS 功能矩阵，区分“已编译”和“已实测”。
- 修正帮助和文档中将 Windows/macOS server 表述为不支持的内容。

### M1：抽象 Linux provider

- 引入 `IServerTransitPlatform`。
- 把当前 Linux-only route/NDP/NAT/TAP 代码迁入 `LinuxServerTransit`，保证 Linux 行为零回归。
- 公共 switcher 不再直接引用 `TapLinux`。

### M2：Windows transit + routed GUA（代码骨架已完成，实机验收进行中）

- 接入 Wintun、IP Helper route/forwarding 和状态恢复。
- 开启 Windows server IPv6 配置校验。
- 完成 W0～W2 测试。

### M3：Windows NAT66（默认用户态，WFP 可选）

- 默认用户态虚拟子网转发与 IPv4 对齐，完成 client-to-client/lease/route 实机回归。
- 仅在需要透明公网 NAT66 时，才安装 WFP callout，并在 Windows 10/11 x64/ARM64 实机完成签名、安装回滚、fragment/MTU/uplink 切换和 soak 验证。

### M4：macOS provider（utun/forwarding/pf anchor 已接入，端到端待客户端跑通）

- 实现 utun/route/forwarding。
- 实测并选择 pf NAT66 或公共用户态 NAT engine。
- 客户端跑通后完成端到端验收。

### M5：Android Experimental

- 增加 JNI server lifecycle 和独立 foreground server process。
- 先交付 IPv4/协议服务端局域网 MVP。
- 单独立项评估用户态 IPv6 flow stack，不阻塞桌面平台。

## 11. 非本轮主线

- Windows SCM、macOS launchd 和 Linux systemd 安装管理。
- 服务端 GUI/Web 管理器重做。
- 为了表面一致而在 Windows/macOS 调用 Linux 风格 shell 命令。
- Android 非 root 环境强行模拟系统级 NAT66/GUA。
- 在 provider 未完成时静默开启或伪造 IPv6 capability。
