# 客户端隧道连通性测试设计文档（全隧道类型）

> 状态：已实现（桌面端，除安卓）
> 目标版本：`ppp --mode=client`
> 关联：[`MULTI_ENTRY_CN.md`](./MULTI_ENTRY_CN.md)（多入口 + tcping 优选）
> 实现说明：[`CONNECTIVITY_TEST_IMPLEMENTATION_CN.md`](./CONNECTIVITY_TEST_IMPLEMENTATION_CN.md)
> 实现范围（2026-08，用户改口后落地）：A1-A3 主隧道 + `client.servers` 多入口探测、缓存/TTL/惩罚、
> SERVERS 页展示 RTT 与生效入口；B2 代理路径、C1/C2 静态 UDP、周期 TTL 重探、滞回优选暂未实现。

---

## 1. 背景与目标

### 1.1 现状

已核实：客户端**没有任何内置的连通性 / 延迟探测机制**。对
`tcping|latency|RTT|probe|health` 的全局检索仅命中 `mappings` 等无关字样，无真实实现。
隧道是否可用只能靠"直接连一次"判定，且 `server_url_` 缓存（`VEthernetExchanger.cpp:319`）导致
断线重连永远连同一入口，无法自动感知其它入口/其它通道类型是否恢复。

### 1.2 目标

设计一个**统一、可复用**的连通性测试框架，覆盖客户端全部隧道类型，输出两个基本量：

- **可达性**（reachable：该隧道类型/该入口当前能否建立连接）
- **往返延迟 RTT**（用于优选与故障切换）

并作为以下三类行为的唯一数据来源：

1. **tcping 优选**（接 `MULTI_ENTRY_CN.md` ADR-3：按 RTT 升序选最优入口）；
2. **故障切换判定**（接 ADR-2：断线/握手失败后选择下一个可用入口）；
3. **诊断展示**（`GetOutboundStatuses()` 显示当前生效入口 + 各级延迟）。

> 关键点：这不是"单入口 tcping 脚本"，而是覆盖 A（主隧道）/ B（访问路径）/ C（静态 UDP）/
> D（mux）全部通道类型的**分层连通性测试**设计。

---

## 2. 隧道连通性类型清单（已核实，含代码位置）

### A. 主隧道（L4/应用传输，由 `client.server` 的 URL scheme 决定）

`UriAuxiliary::Parse`（`ppp/auxiliary/UriAuxiliary.cpp`）把 scheme 映射到
`ProtocolType`（`ppp/auxiliary/UriAuxiliary.h:17-23`），再由
`VEthernetExchanger::NewTransmission`（`VEthernetExchanger.cpp`）分派传输对象：

| 类型 | scheme | ProtocolType | 传输对象 | 握手层次 |
|------|--------|-------------|---------|---------|
| A1 原生 PPP/TCP | `ppp://` / `tcp://`（或裸地址） | `ProtocolType_PPP`(0) | `ITcpipTransmission` | TCP → PPP/`VirtualEthernetLinklayer` 帧握手 → 加密载荷 |
| A2 明文 WebSocket | `ws://` / `http://` | `ProtocolType_WebSocket` / `Http` | `IWebsocketTransmission` | TCP → HTTP Upgrade → 101 |
| A3 加密 WebSocket | `wss://` / `https://` | `ProtocolType_WebSocketSSL` / `HttpSSL` | `ISslWebsocketTransmission` | TCP → TLS(SNI) → HTTP Upgrade → 101 |
| A4 socks:// 解析 | `socks://` | `ProtocolType_Socks`(-1) | 无专用实现（落入 `ITcpipTransmission`） | 仅解析器支持；客户端隧道无 SOCKS 分支，真正 SOCKS 只在 `server_proxy`（B2） |

> A1 是"最纯"的 TCP 隧道；A2/A3 是 WebSocket 变体（WSS 优选 IP 用法依赖 A3 的
> `websocket.host/sni` 覆写，见 README.md:294）。A4 罕见，作为边界类型。

### B. 到达 VPN 服务器的访问路径

| 类型 | 说明 | 位置 |
|------|------|------|
| B1 直连 | 客户端 socket 直接连 VPN 服务器 | `OpenTransmission`（`VEthernetExchanger.cpp:404-529`） |
| B2 经转发代理 | `client.server_proxy` 非空时，先连 HTTP/SOCKS 代理再经其 CONNECT 到服务器 | `IForwarding`（`ppp/transmissions/proxys/IForwarding.h:25-28`，`ProtocolType_HttpProxy` / `ProtocolType_SocksProxy`）；`VEthernetExchanger::Open`（`:519`） |

### C. 静态 UDP 通道（数据面旁路）

承载 DNS / ICMP / UDP 数据报，走独立于主隧道的一组 UDP 服务器端点
`udp.static.servers`（`AppConfiguration.h:107`，集合；解析见 `AppConfiguration.cpp:1364`）。

| 类型 | 行为 | 位置 |
|------|------|------|
| C1 轮询 | 无 aggligator 时逐端点轮询发静态回显 | `StaticEchoGetRemoteEndPoint`（`VEthernetExchanger.cpp:2350`，取头删尾追加） |
| C2 aggligator 聚合 | `udp.static.aggligator > 0` 时 `aggligator::client_open` 同时连所有服务器做带宽聚合 | `common/aggligator/aggligator.cpp:1182` |

保活/回显载体：`StaticEchoGatewayServer`（`VEthernetExchanger.cpp:1923`，发 ICMP 到对端）、
`StaticEchoPacketToRemoteExchanger`（`:2108`）。

### D. 多路复用（主隧道之上的子通道）

| 类型 | 说明 | 位置 |
|------|------|------|
| D1 mux 子通道 | 主隧道建立后，`switcher_->mux_` 决定最大连接数，按 `mux.mode`（compat/flow/balance/stripe）开子通道 | `DoMuxEvents`（`VEthernetExchanger.cpp:744`）、`vmux`（`ppp/app/mux/vmux_net.*`）；mux 握手超时配置 `mux.connect.timeout`（`AppConfiguration.h:134`，加载于 `AppConfiguration.cpp:287`） |

### E. 服务端传输变体（客户端可能连到的目标）

服务端 `server.cdn[2] = [明文, TLS]`（`AppConfiguration.h:81`）暴露 CDN 加速端口；
`VirtualEthernetSwitcher` 同时接受 TCP / WS / WSS / CDN1 / CDN2。客户端侧 CDN1/CDN2 等价于
"把 A2/A3 的 ws/wss 打在 CDN 端口上"，探测逻辑复用 A2/A3，仅目标端口不同。

---

## 3. 统一连通性测试框架

### 3.1 单一结果抽象

所有类型、所有入口的探测结果统一为一种结构：

```cpp
struct ConnectivityProbeResult {
    ppp::string   entry;       // 入口标识（见 3.6）
    uint8_t       category;    // A/B/C/D（主隧道/访问路径/静态UDP/mux）
    uint8_t       type;        // tcp | ws | wss | socks | udp | udp-agg | mux
    bool          reachable;   // 是否通过到配置的探测深度
    uint32_t      rtt_ms;      // 往返延迟（到最深可达阶段）
    uint8_t       deepest_stage; // 最深可达层（见 3.2）
    uint64_t      timestamp;   // 探测时刻（Executors::GetTickCount）
    uint32_t      ttl;         // 结果有效期（秒）
    uint8_t       penalty;     // 假阳性惩罚剩余秒（见 3.5）
};
```

### 3.2 分层连通性（故障可定位）

每类隧道按阶段逐级探测，**记录最深可达层**而非单一布尔：

| 层 | 名称 | 判定 |
|----|------|------|
| L0 | 解析/路由 | `GetRemoteEndPoint` 解析成功；`EnsureWindowsIPv4/6ServerRoute` / `Protect` pin 成功 |
| L1 | TCP | `async_connect`（`ppp/coroutines/asio/asio.h:139`）SYN-ACK 成功 |
| L2 | TLS | wss：TLS 握手（SNI）成功 |
| L3 | WebSocket | ws/wss：HTTP Upgrade 收到 101 |
| L4 | 链路层 | PPP/`VirtualEthernetLinklayer` 握手成功（`HandshakeServer`） |
| L5 | 建立 | `EchoLanToRemoteExchanger` → `NetworkState_Established` |

> L0–L3 由探测器主动测试；L4/L5 不在探测层执行（会创建真实会话），
> 只在真实重连路径（`Loopback()`）上记录，作为故障定位的补充信息。

- RTT 取**到最深可达层**的耗时。
- 例如"TCP 通、TLS 挂"会记录 `reachable=false, deepest_stage=1, rtt=TCP耗时`，
  面板/日志即可一眼定位是"能连上但证书/SNI 问题"还是"完全连不上"。

### 3.3 生命周期与触发时机

| 时机 | 行为 |
|------|------|
| 首次启动、接管前 | 主动探测全部入口/通道（此时无 TAP 接管，无自环风险） |
| 重连（`ExchangeToReconnectingState`，`:1128`） | 解锁 + 失效缓存 + 重新探测选择 |
| 正常 Established | 锁定，**零探测开销**（只在重连时参与） |
| 周期刷新 | `OnUpdate(now)`（`VEthernetNetworkSwitcher.cpp:7106`，约 500ms tick）检查 TTL 过期 → spawn 后台重探 |

### 3.4 绕过 VPN 自环（强制）

TAP 接管后，探测 socket 的 SYN/握手包若进隧道会自我循环。所有探测 socket 必须走物理网卡：

- Windows：复用 `EnsureWindowsIPv4ServerRoute` / `EnsureWindowsIPv6ServerRoute`（`OpenTransmission` 已对每个 remoteEP 做，`:432/:443`）。
- Linux/Android：复用 `Protect()`（`switcher_->GetProtectorNetwork()`，`:475`）。
- 接管前首次探测天然无此问题，无需额外处理。

> 任何新增探测代码都必须复用上述 pin 路径，禁止另起独立 socket 逻辑造成环路。

### 3.5 缓存 / TTL / 滞回 / 惩罚（复用 MULTI_ENTRY 设计）

- **缓存**：探测结果带 TTL，`ttl-seconds` 内直接复用，避免高频 SYN 被 CDN 限流/封禁。
- **滞回**：当前入口 RTT ≤ 最优 × 1.3 则保持不动，防抖动横跳。
- **假阳性惩罚**：探测可达但实际连接（TLS/WS/链路层）失败 → 临时拉黑（如 30s），
  避免反复撞同一"测得到连不上"的入口。
- **并发/串行**：`parallel=true` 时用 `YieldContext::Spawn` 并行探测全部；否则串行（总耗时 = N × timeout-ms）。

### 3.6 入口标识（entry）

主隧道沿用 `MULTI_ENTRY` 的约定：`client.server` 为第一候选，`client.servers[]` 为其余候选。
标识采用去重友好的 `guid + server:port` 形式（与 `--server-dir` 去重一致，`main.cpp:3304`），
保证同一服务器的多入口不被误判为不同节点。

---

## 4. 每类隧道的具体探测策略

### 4.1 主隧道（A）

| 类型 | 探测动作 | RTT 定义 |
|------|---------|---------|
| A1 ppp/tcp | `async_connect` 计时（探测上限 L1；链路层握手只在真实重连路径做） | 到 L1 |
| A2 ws/http | TCP connect → 发 `GET {path}` + `Host: websocket.host` Upgrade 请求 → 等 101 | 到 L3 |
| A3 wss/https | TCP connect → TLS 握手（SNI=`websocket.sni`）→ Upgrade → 等 101 | 到 L3 |
| A4 socks（无专用实现） | 按 A1 的 TCP 处理（与 `NewTransmission` 落到 `ITcpipTransmission` 一致） | 到 L1 |

统一入探测池（与 `MULTI_ENTRY` ADR-3 一致），按 RTT 升序选最优。

### 4.2 访问路径（B）

- **B1 直连**：即 4.1 各类型。
- **B2 经代理**：一条端到端探测链路——TCP connect 代理 → HTTP/SOCKS 握手 → CONNECT 目标服务器。
  RTT 取**从探测开始到 CONNECT 应答**的端到端耗时（不能拆成两段相加：CONNECT 的 TCP 建连发生在代理内部，
  无法单独测量）；失败时记录故障段："代理不可达"还是"代理可达但隧道目标不可达"。

### 4.3 静态 UDP（C）

- **C1 轮询**：逐个 `udp.static.servers` 端点发独立 UDP 探测数据报，等对端应答；RTT = 应答延迟。
  （探测数据报必须在主隧道建立**前**也能工作，因此不能依赖已建立会话的 `StaticEchoPacketToRemoteExchanger`；
  接管前探测 socket 直接 pin 物理网卡。）
- **C2 aggligator**：对 `aggligator::client_open` 的每条链路做可达性/RTT 上报（`client_endpoint`）；
  **聚合的增删与重连是 aggligator 内部行为**（`:1364` 会自动 `client_open` 重开），探测只做只读健康上报，
  不直接"剔除"链路。

> 建立后（L5）可额外复用 `StaticEchoGatewayServer` 保活通道做**带内**健康检查（随业务流量感知），
> 但入口探测（预接管/重连选择）必须用独立 UDP 探测数据报，两者职责不同。

### 4.4 mux（D）

- 属于**派生状态，不做独立探测**：mux 健康 = 主隧道健康（L5）+ `DoMuxEvents` 超时复位状态
  （`VEthernetExchanger.cpp:744` 已有 `mux.update()`/`get_last()` 超时检测并自动 `mux_.reset()`）。
  额外开子通道探测会引入真实 mux 连接、增加复杂度且信息量低。
- mux 不参与入口优选（`MULTI_ENTRY` 已确认"tcping 选的是入口本身，天然兼容"）；
  `categories` 中不再单列 mux，只上报 `DoMuxEvents` 派生状态。

### 4.5 服务端传输变体（E，CDN1/CDN2）

- **不是独立探测类别**：`server.cdn[2]`（`AppConfiguration.h:81`）是服务端监听端口；客户端自身配置
  并不读取它，CDN1/CDN2 只是用户把 `server.cdn` 端口填进 `client.servers` 后得到的 ws/wss 目标。
- 因此 E 不新增探测类型，复用 A2/A3 即可覆盖（CDN 明文 → A2，CDN TLS → A3）。
- 保留为文档说明项，用于解释"TCP 通而 TLS 挂"常见于 CDN 限制的定位。

---

## 5. 配置设计

新增 `client.probe`（可选；缺省/关闭 → 行为与现状完全一致）：

```json
"client": {
    "server": "wss://104.16.1.1:443/tun",
    "servers": ["104.16.2.2:443", "104.16.3.3:8443"],
    "probe": {
        "enabled": true,
        "timeout-ms": 800,
        "ttl-seconds": 30,
        "parallel": true,
        "stage": 3,
        "categories": ["tcp", "ws", "wss"]
    }
}
```

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `enabled` | bool | `false` | 关闭 = 纯故障切换（与现状一致，无任何探测） |
| `timeout-ms` | int | `800` | 单入口/单通道探测超时，超时视为不可达 |
| `ttl-seconds` | int | `30` | 探测结果有效期，过期后周期重探 |
| `parallel` | bool | `true` | 并行探测全部；`false` 串行 |
| `stage` | int | `3` | 探测深度上限（3=到 WebSocket/TLS，1=仅 TCP；**上限 L3，不做 L4/L5**，见 §8） |
| `categories` | string[] | 全部 | 只对列出的通道类型做探测（`tcp/ws/wss`；mux 为派生状态不单列） |

语义：

- 缺省或 `enabled=false` → 不创建任何探测器，`GetRemoteEndPoint` 原逻辑不变，零回归风险。
- `stage` 允许用户在"只测 TCP（快但可能误判 TLS/WS 不可用）"与"测到 TLS/WS（准但慢）"间权衡；
  `MULTI_ENTRY` 的 tcping 只测 TCP 层正是 `stage=1` 的特例；`stage` 不开放 L4/L5（见 §8）。

---

## 6. 代码改动清单（未来实现时参考）

| 文件 | 改动 |
|------|------|
| `ppp/configurations/AppConfiguration.h` | `client` 新增 `struct { enabled; timeout_ms; ttl_seconds; parallel; stage; categories; } probe;` |
| `ppp/configurations/AppConfiguration.cpp` | 默认 `enabled=false`、`timeout-ms=800`、`ttl-seconds=30`、`stage=3`；`Load` 解析 `client.probe` |
| `ppp/app/client/ConnectivityProbe.h/.cpp`（新增） | `ConnectivityProbeResult` / `ConnectivityProbeEntry`；探测器工厂 `Create(type)`；`ProbeAll()` 并发编排；缓存 / TTL / 滞回 / 惩罚 |
| `ppp/app/client/VEthernetExchanger.cpp` | `GetRemoteEndPoint()`（:319）接入探测结果作为候选排序；`ExchangeToReconnectingState()`（:1128）失效缓存 + 解锁；`Loopback()`（:676）握手成功置锁 |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | `OnUpdate()`（:7106）周期检查 TTL 过期 → spawn 重探；`GetOutboundStatuses()`（:1333）显示当前生效入口 + 各级 RTT |
| `ppp/transmissions/proxys/IForwarding.h/.cpp` | 复用 `ConnectToProxyServer`（:86-93）作 B2 探测入口 |
| `ppp/app/client/VEthernetExchanger.cpp`（StaticEcho 区） | 复用 `StaticEchoPacketToRemoteExchanger` / `StaticEchoGatewayServer` 作 C1/C2 探测载体 |

只读参考：`ppp/app/client/VEthernetExchanger.*`、`ppp/app/client/VEthernetNetworkSwitcher.cpp`、
`ppp/coroutines/asio/asio.h`、`ppp/auxiliary/UriAuxiliary.cpp`、`ppp/app/mux/vmux_net.*`、
`ppp/app/protocol/VirtualEthernetLinklayer.h/.cpp`、`ppp/transmissions/proxys/IForwarding.*`。

---

## 7. 验证计划

1. **加载单测**：`client.probe` 缺省/非法/部分字段（stage 越界、categories 含未知类型）的过滤与默认值。
2. **类型覆盖**：本地起 server 的 `tcp / ws / wss / cdn[0] / cdn[1] / udp.static` 各端口，
   客户端逐类开启探测，确认 A/B/C/E 各类型 `reachable + rtt` 正确。
3. **分层定位**：分别制造"TCP 通但 TLS 挂""TCP 通但 ws 不升 101""链路层握手挂"三种故障，
   确认探测记录 `deepest_stage` 且能定位到层。
4. **代理路径 B2**：起一个 HTTP/SOCKS 代理，验证两段可达性判定（代理不可达 vs 代理可达但隧道目标不可达）。
5. **自环**：TAP 接管后触发探测，抓包确认探测 SYN 走物理网卡、不进隧道、无环路流量。
6. **优选联动**：接 `MULTI_ENTRY` 验证——RTT 差异明显选最低；+20% 不切、+200% 切；
   假阳性入口被临时拉黑；TTL 过期后重探切次优。
7. **回归**：`client.probe` 缺省或 `enabled=false` → 与现状逐字节一致，零探测开销、零自环。

---

## 8. 边界与明确不做（本期）

- **不做并发优选**（同时发起完整多入口握手取第一个）——比 tcping 准但引入并发竞争与连接清理，
  复杂度高，列为扩展（`MULTI_ENTRY` §5 同款取舍）。
- **不做每入口独立 Host/SNI**——`websocket.host/sni` 保持对所有入口共享（本期明确不做）。
- **不做健康度加权**（成功率/丢包）——本期仅 RTT + 可达性，扩展成本低但不在本期。
- **不做业务级回环测试**（探测只到配置层，不模拟真实业务流量），避免探测本身造成数据面开销。
- **不做 L4/L5 探测**：`stage` 上限为 L3（TLS/WS 101）。L4/L5 握手会以真实 GUID 建立会话、产生"幽灵会话"，
  且与真实重连路径重复——完整握手/建立只发生在真实 `Loopback()` 重连时，探测层不模拟。
- **探测 TLS 参数必须镜像真实连接**：wss 探测的证书校验、ALPN、SNI 必须与 `ISslWebsocketTransmission`
  完全一致（openppp2 默认不校验 CA，探测若校验会与真实行为产生"测不通但连得上"的假阴性）。

---

## 9. 开放问题（待确认）

- [ ] 探测深度 `stage` 默认取 3（到 ws/TLS）还是 1（仅 TCP，纯 tcping 语义）？（已确认不做 L4/L5）
- [ ] 静态 UDP 探测数据报的格式：直接发普通 UDP 载荷等对端回显，还是需要带静态会话标识
      （`static_echo_session_id_`）？（已确认入口探测用独立数据报，建立后复用保活通道）
- [ ] 探测 TLS 是否镜像真实连接的"不校验 CA"策略（推荐：镜像，避免假阴性）？
- [x] 探测结果已上报 TUI SERVERS 页（`GetOutboundStatuses` + `main.cpp`）。
- [ ] 是否需要英文版（`CONNECTIVITY_TEST.md`）？
- [x] 已实现（桌面端），见顶部"实现范围"。



