# 多入口优选评估与 PPP 多入口拓展（含探测机制评估）

> 状态：评估稿（2026-08-09）；G1 域名备份入口（ws/wss 与 ppp）、G2 滞回选优、G7 后台刷新 stage 降级均已落地实现。现有 ws/wss 优选 IP + 探测选优已实现（桌面端 + 安卓连接前选优）；
> 本文件评估设计稿与实现差距，给出 **ppp:// 多入口** 的拓展设计，并评估探测机制。
> 关联：[`MULTI_ENTRY_CN.md`](./MULTI_ENTRY_CN.md)（ws/wss 多入口设计稿）、
> [`CONNECTIVITY_TEST_CN.md`](./CONNECTIVITY_TEST_CN.md)（连通性测试设计）、
> [`CONNECTIVITY_TEST_IMPLEMENTATION_CN.md`](./CONNECTIVITY_TEST_IMPLEMENTATION_CN.md)（实现说明）、
> [`MULTI_ENTRY_SESSION_EXPORT.md`](./MULTI_ENTRY_SESSION_EXPORT.md)（会话上下文导出）

---

## 1. 现有方案评估（设计 vs 实现）

### 1.1 已落地且验证过的能力（代码证据）

| 能力 | 代码位置 | 说明 |
|------|---------|------|
| `client.servers` 多入口解析 | `AppConfiguration.cpp` `ReadJsonAllAddressStringToVector` | 保序、去重；`IP:port` / `[IPv6]:port`；**域名也能通过配置校验**（`IPOrHostIsValid` → `IsDomainAddress`） |
| 连接前探测选优 | `VEthernetExchanger.cpp` `ProbeSelectServerEndPoint` | 候选 = `[client.server] + client.servers`；缓存/TTL/惩罚拉黑/并行 Spawn；选 RTT 最低可达入口 |
| 连接即探测（默认开启，`probe.enabled` 开关） | `GetRemoteEndPoint`（`:769`） | `!forwarding && 有协程上下文 && enabled` 时自动执行；有 `server_proxy` 或 `enabled:false` 跳过 |
| 失败拉黑 + 失效缓存 | `ExchangeToReconnectingState`（`:1598`） | 当前失败入口 `reachable=false` + `penalty_until`，`server_url_.port = 0` 使下次重连重新选优 |
| 每 5 秒全配置后台刷新 | `VEthernetNetworkSwitcher.cpp` `RefreshOutboundProbes` | 所有菜单出口 + main 持续探测，结果存 `outbound_probe_statuses_` |
| SERVERS 页展示 | `main.cpp`（`:1723`） | `(12ms)` / `(unreachable)` / ` -> 生效入口` |
| 防自环 | Windows `EnsureWindowsIPv4/6ServerRoute` pin 物理路由；Linux `ProtectorNetwork::Protect` | 探测 SYN 不进 TAP |
| ppp:// 主入口 + IP 备份 | 全链路 | 探测按 TCP（`ProbeTypeFromProtocol` 默认 Tcp），备份继承主 scheme/path，连接用选中候选的 `protocol_type/hostname/address/port` |

### 1.2 设计稿有、实现没有（差距清单）

| # | 差距 | 影响 | 严重度 |
|---|------|------|--------|
| G1 | ~~备份入口域名被静默丢弃~~ **已修复（2026-08-09）**：`ProbeSelectServerEndPoint` 与 `RefreshOutboundProbes` 的备份分支均支持域名解析（锁外 `GetAddressByHostName`），解析失败记不可达、不静默丢弃；拉黑键随 `hostname` 规范化，与探测缓存键一致 | 已实现 | 高 |
| G2 | ~~滞回未实现~~ **已实现（2026-08-09）**：`ProbeSelectServerEndPoint` 选优时，缓存入口（`server_url_`）本轮仍可达且 RTT ≤ 最优 ×1.3 则保持，防止抖动横跳 | 已实现 | 低 |
| G3 | 假阳性惩罚只在“真实连接失败”时触发（`ExchangeToReconnectingState`），探测可达但 TLS/WS 握手挂的入口未在探测层拉黑 | 与 G2 同理，重连循环内被 TTL 缓存兜住 | 低 |
| G4 | B2 代理路径（`server_proxy`）不探测（有意） | 代理不可达 vs 隧道目标不可达无法区分；文档已声明 | 有意 |
| G5 | C1/C2 静态 UDP 未纳入选优；`ProbeUdp` 为 send-only 且未接线 | UDP 数据面（DNS/ICMP）不可用无法提前感知 | 中 |
| G6 | `server_url_` 无锁读写在多 io 线程下存在竞态（既有问题，非本方案引入） | 概率低；重连与启动解析并发时可能读到撕裂值 | 低 |
| G7 | 后台刷新对 **wss 出口每 5 秒做完整 TLS+WS Upgrade**（stage=3 默认），全部菜单出口持续打向服务器 | 服务器端 TLS 握手开销与可能的限流；未连接出口其实不需要 L3 深度 | 中 |

### 1.3 已修复的致命问题（记录在案）

- **锁跨协程挂起死锁**（2026-08-09 崩溃 `0xC0000025`）：旧 `RefreshOutboundProbes` 在
  `SynchronizedObjectScope` 内做 `UriAuxiliary::Parse`（异步 DNS 会 `Suspend()` 挂起栈式协程），
  锁留在协程栈上；同一 io 线程的 TAP 包路径 `GetExchanger → get_active()` 二次加锁 → 同线程死锁。
- 修复原则（后续所有探测代码必须遵守）：**任何可能挂起的操作（DNS、connect、TLS、WS 升级）不得
  在持有 `syncobj_` 的状态下执行**；锁内只做无挂起的快照/写入。
- 该问题与用户观察的“10 小时 VPN 后上不去国内网络”同源：持锁挂起期间数据面整体阻塞。

---

## 2. PPP 多入口拓展设计

### 2.1 与 ws/wss 优选 IP 的本质区别

| 维度 | ws/wss 优选 IP（现状已支持） | ppp 多入口（本设计） |
|------|------------------------------|----------------------|
| 入口语义 | 同一源站/CDN 的多个优选 IP，共享 Host/SNI | 多个独立 ppp 服务器（不同地域/线路），无 Host/SNI |
| 探测深度 | 可到 L3（TLS + WS 101），`stage=3` 默认 | **天然 TCP-only**：ppp 无 WS 层，`ProbeType_Tcp` 分支只做 connect，开销最小 |
| 域名 | 主入口可域名；备份要求 IP 字面量（CDN 优选 IP 场景合理） | **备份域名很常见**（`ppp://kt-nat3.aursys.cfd:10005`），必须支持 |
| 选优价值 | CDN 节点间 RTT 差异决定加速效果 | 线路健康度/可达性优先于纯 RTT；RTT 仍有参考价值 |
| 切换时机 | 重连时切换 | 相同：断线/握手失败 → 重新选优；已建立会话不漂移 |

### 2.2 现状：ppp 多入口的可用链路（已核实）

`client.server = "ppp://23.249.25.106:20000"` + `servers = ["5.6.7.8:20000", "[2400:...]:20000"]`
在**当前代码上端到端可用**：

1. `ProbeSelectServerEndPoint`：主入口完整 URL 解析（`UriAuxiliary::Parse`），备份 `IP:port` 继承主 scheme；
   探测类型 `ProbeType_Tcp`（`ProbeTypeFromProtocol` 对 PPP 返回 Tcp）；
2. 选中最优候选 → `GetRemoteEndPoint` 覆盖 `server_url_`（`protocol_type/hostname/address/path/port`）；
3. `OpenTransmission` 按候选 `remoteEP` 建连 + pin 物理路由，`NewTransmission` 按 `protocol_type` 创建
   `ITcpipTransmission`（ppp/tcp）——备份入口以相同协议连接；
4. 失败 → `ExchangeToReconnectingState` 拉黑 + 失效缓存 → 重连重新选优。

**结论：ppp 多入口 = “IP 字面量备份”已可用；缺的是“域名备份 + DNS 解析接入”。**

### 2.3 缺口 G1 修复设计：域名备份入口（**已实现，2026-08-09**）

目标：`servers` 允许 `host:port`（域名或 IP），探测/连接前解析，行为与主入口域名一致。

```
候选构建（ProbeSelectServerEndPoint 备份分支）:
  1. Ipep::ParseEndPoint(entry) → host_string + port
  2. Ipep::ToAddress(host_string, false) 成功 → 直接构造候选（现状）
  3. 失败且 Ipep::IsDomainAddress(host_string)：
       a. 用 GetAddressByHostName / UriAuxiliary_ResolveEndPoint（协程上下文，可挂起——
          必须在锁外）解析出 IP
       b. 解析成功 → 构造候选 { hostname = 域名, address = 解析 IP, entry 键 = "域名:port" }
       c. 解析失败 → 本轮不可达（记录 checked=true, reachable=false），不丢弃配置
```

关键决策点：

| 决策 | 建议 | 理由 |
|------|------|------|
| 缓存键 | **`域名:port`**（而不是解析后的 IP:port） | DNS 漂移时结果仍归属同一入口；与 `ExchangeToReconnectingState` 的拉黑键一致 |
| 解析时机 | 每次探测轮（连接选优 + 5s 后台刷新） | 自动感知 DNS 变化（CDN/故障转移），TTL 由系统 DNS 缓存自然控制 |
| 连接地址 | 用**本轮解析出的 IP** 建连（`remoteEP`），`hostname` 保留域名 | 与主入口域名行为一致；`OpenTransmission` 按 remoteEP 连接 |
| 域名不可解析 | 该入口不可达，不参与选优；不阻断其它入口 | 避免一个坏域名拖垮全部 |
| 防自环 | 解析出的 IP 同样进 `EnsureWindowsIPv4/6ServerRoute` pin 路由 | 与现有 IP 候选一致 |

配置示例（与现有格式兼容，无需新字段）：

```json
"client": {
    "server":  "ppp://kt-a.aursys.cfd:20000",
    "servers": [
        "kt-nat3.aursys.cfd:10005",
        "38.49.57.29:20000",
        "[2400:cb00:2049:1::c629:d7a2]:20000"
    ],
    "probe": {
        "enabled":     true,
        "timeout-ms":  800,
        "ttl-seconds": 30,
        "parallel":    true,
        "stage":       1,
        "categories":  ["tcp"]
    }
}
```

> ppp 出口建议 `stage=1`（仅 TCP connect）：探测成本最低，且 ppp 没有可测的更高层；
> `categories=["tcp"]` 可显式排除 ws/wss 探测。注意 `stage`/`categories` 是全局的，
> 若同一配置内混用 ws/wss 与 ppp 入口，应让探测类型按入口协议自动决定深度
> （ppp 一律 L1，ws/wss 用配置 stage）——这也是本设计建议的默认行为。

> `probe.enabled:false` 关闭探测：连接前选优与 5 秒后台刷新均停用，入口回退主入口 `client.server`，
> `client.servers` 多入口仅参与启用时的选优；SERVERS 页显示 `(probe off)`。

### 2.4 与其它子系统兼容性（已核实）

| 子系统 | 兼容性 |
|--------|--------|
| mux | 多子通道针对同一入口；选优只决定入口，天然兼容 |
| 静态 UDP（`udp.static.servers`） | 与 ppp 主隧道独立；`StaticEchoGetRemoteEndPoint` 用 `server_url_.remoteEP`，随入口切换更新 |
| geo 多出口 / `--server-dir` | 每 outbound 独立配置，各自支持 `client.servers` + probe |
| server-proxy | 有 `server_proxy` 时不探测（B2 语义），入口由转发代理路径决定 |
| IPv6 | 备份 `[IPv6]:port` 已支持；域名解析需同时支持 A/AAAA（跟随现有 `GetAddressByHostName` 行为） |

---

## 3. 探测机制评估

### 3.1 分层模型（合理）

| 层 | 探测 | 覆盖隧道类型 |
|----|------|-------------|
| L1 | TCP connect（RTT = 握手耗时） | A1 ppp/tcp、A2/A3 的 TCP 底、B2 代理 TCP |
| L3 | TLS(SNI, 不校验 CA) + WS 101 | A2 ws / A3 wss（镜像真实连接策略，避免假阴性） |
| send-only | UDP 数据报（不等待回显） | C1/C2 静态 UDP（应用级回显协议无法复刻，故只测“OS 接受”） |

- ppp 多入口只消费 L1，成本最低；ws/wss 消费 L3，判定更准。
- 不做 L4/L5（避免幽灵会话）是正确的边界；真实握手留给 `Loopback()` 重连路径。

### 3.2 缓存 / TTL / 惩罚（已实现，设计合理）

- `probe_results_` 以 `host:port` 为键；TTL 内免探（默认 30s），惩罚期内视为不可达；
- 连接选优与 5s 后台刷新**共用同一套探测原语**，但缓存独立
  （交换机 `outbound_probe_statuses_` 只管展示，exchanger `probe_results_` 只管选优）——
  职责清晰，无锁竞争；
- 已落地（G7，2026-08-09）：后台刷新对**未建立连接的出口降级为 stage=1**，
  已连接出口保持配置 stage——展示 RTT 用 L1 足够，避免每 5s 对全部 wss 服务器做 TLS 握手。

### 3.3 并发与互斥（已修复 + 必须遵守的纪律）

- 并行探测用 `YieldContext::Spawn` + `state.pending.fetch_sub(1) == 1` 唤醒父协程，
  Spawn 失败也补计数（无死锁）；`state`/`works` 在父协程栈上，父协程挂起期间有效；
- **纪律（写入代码评审清单）**：
  1. 任何探测/解析/建连代码不得持有 `SynchronizedObjectScope` 跨 `Suspend()`；
  2. 探测协程对共享状态（`outbound_probe_statuses_`、`probe_results_`）只做“短锁 + 无挂起”写入；
  3. 后台刷新与连接选优的缓存必须分区，避免互相污染 TTL/惩罚状态；
  4. 域名解析在锁外执行（G1 实现时同样适用）。

### 3.4 频率与流量控制（评估）

| 现状 | 影响 | 建议 |
|------|------|------|
| 5s 全配置刷新 × 13 出口（用户实测配置） | 每 5s 最多 13 个并行短连接；wss 出口为完整 TLS+WS 握手 | 未连接出口 stage=1；可考虑刷新周期按出口数自适应（如 >10 出口时 10s） |
| 连接选优每次重连触发 | TTL 缓存兜底，重连高频时不会重复探测 | 无需改动 |
| `parallel=false` 时串行 N × timeout | 最坏 N×800ms 阻塞重连 | 建议重连路径强制并行（文档已提示） |

---

## 4. 实现路线图（建议）

| 优先级 | 事项 | 涉及 |
|--------|------|------|
| ~~P0~~ | ~~G1：域名备份入口~~ **已完成（2026-08-09）**：`VEthernetExchanger.cpp`（`ProbeSelectServerEndPoint` 解析 + `ExchangeToReconnectingState` 拉黑键）、`VEthernetNetworkSwitcher.cpp`（`RefreshOutboundProbes` 备份分支） | 已落地 |\n| ~~P1~~ | ~~G7：后台刷新按连接状态降级 stage~~ **已完成（2026-08-09）**：未建立连接的出口后台刷新只做 L1（TCP），已连接出口保持配置 stage | 已落地 |\n| ~~P2~~ | ~~G2：滞回选优~~ **已完成（2026-08-09）**：`ProbeSelectServerEndPoint` 选优保持缓存入口（RTT ≤ 最优 ×1.3） | 已落地 |
| P1 | G7：后台刷新按连接状态降级 stage（未连接出口 L1） | `VEthernetNetworkSwitcher.cpp` |
| P1 | G5：静态 UDP 探活接入展示（send-only 已有原语，接 `outbound_probe_statuses_` 或独立状态位） | `VEthernetExchanger.cpp` / switcher |
| P2 | G2/G3：滞回 + 探测层假阳性拉黑（连接选优阶段） | `ProbeSelectServerEndPoint` |
| P2 | G6：`server_url_` 读写加锁或原子化 | `VEthernetExchanger` |
| 文档 | 本文档落地后，更新 `MULTI_ENTRY_CN.md` / `CONNECTIVITY_TEST_CN.md` 的“状态”与“边界”节 | docs |

### 验证计划（ppp 多入口专项）

1. 配置：主入口域名 + 备份 `[域名, IP, IPv6]` 混合；确认备份域名解析成功后被探测并可选优；
2. kill 最优入口 → 重连自动切次优（日志与 SERVERS 页 ` -> 入口` 一致）；
3. 域名不可解析时该入口显示 `(unreachable)`，不阻断其它入口连接；
4. 主域名 DNS 变更（hosts 指向新 IP）→ 下一轮刷新自动感知（≤5s 展示 / TTL 后选优）；
5. 回归：纯 IP 备份（现状配置）行为不变；`server_proxy` 存在时仍跳过探测。