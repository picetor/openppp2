# 客户端连通性探测实现说明（桌面端）

> 状态：已实现（桌面端，除安卓）
> 关联：[`CONNECTIVITY_TEST_CN.md`](./CONNECTIVITY_TEST_CN.md)（设计文档）、[`MULTI_ENTRY_CN.md`](./MULTI_ENTRY_CN.md)（多入口优选）
> 时间：2026-08

---

## 1. 概述

本次修改为客户端（`ppp --mode=client`）增加**隧道入口连通性探测**：

- 在首次连接 / 断线重连前，对主入口（`client.server`）与备用入口（`client.servers`）执行
  TCP / WebSocket / WSS 探测，得到每个入口的**可达性**与**RTT**；
- 按 RTT 自动选优（含 TTL 缓存与失败惩罚拉黑）；
- 把探测结果上报到 SERVERS 切换页（RTT / unreachable / 当前生效入口）。

功能默认关闭（`client.probe.enabled=false`），不开启时与旧行为完全一致，零回归。

---

## 2. 配置

### 2.1 新增 `client.servers`

备用入口列表，有序、去重，格式为 `IP:port`（备份入口必须是 IP 字面量，不解析域名；IPv6 写作 `[::1]:port`）。
备份入口**继承主入口的传输类型（scheme）、WebSocket path 与 websocket.host/sni 覆写**，只替换 IP:port。

```json
"client": {
    "server":  "ppp://1.2.3.4:20000/",
    "servers": ["5.6.7.8:20000", "[2001:db8::9]:20000"],
    "probe": {
        "enabled":     true,
        "timeout-ms":  800,
        "ttl-seconds": 30,
        "parallel":    true,
        "stage":       3,
        "categories":  ["tcp", "ws", "wss"]
    }
}
```

### 2.2 新增 `client.probe`

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `enabled` | `false` | 总开关；`false` = 纯故障切换（不探测），与旧行为一致 |
| `timeout-ms` | `800` | 单入口探测超时（毫秒），下限钳制为 50 |
| `ttl-seconds` | `30` | 探测结果有效期（秒），下限钳制为 1；有效期内命中缓存免探 |
| `parallel` | `true` | 并行探测所有待测入口；`false` 为串行 |
| `stage` | `3` | 探测深度：`1` = 仅 TCP connect；`3` = TLS/WebSocket Upgrade（上限，不做 L4/L5） |
| `categories` | 空 | 只探测列出的类别（`tcp`/`ws`/`wss`）；空 = 全部 |

---

## 3. 文件改动清单

| 文件 | 改动 |
|------|------|
| `ppp/app/client/ConnectivityProbe.h/.cpp`（新增） | 探测原语：`ProbeTcp` / `ProbeWebSocket` / `ProbeWebSocketSSL` / `ProbeUdp`，带 `ProtectSocketHandler` 回调与超时控制；Android 为空实现 |
| `ppp/configurations/AppConfiguration.h/.cpp` | 新增 `client.servers`（保序解析 + 去重）与 `client.probe`（默认值、加载、钳制） |
| `ppp/app/client/VEthernetExchanger.h/.cpp` | `ProbeSelectServerEndPoint`（缓存/TTL/惩罚/并行编排）、`ProbeCandidateEndpoint`（按类型/深度执行探测）、`StoreProbeResult`；`GetProbeRtt/Reachable/Checked/Server` 原子状态；`GetRemoteEndPoint()` 接入探测选优；`ExchangeToReconnectingState()` 失败拉黑 + 失效缓存 |
| `ppp/app/client/VEthernetNetworkSwitcher.h/.cpp` | `OutboundStatus` 新增 `probe_enabled/probe_checked/probe_reachable/probe_rtt_ms(-1)/probe_server`；`GetOutboundStatuses()` 两处填充 |
| `main.cpp` | SERVERS 页显示 `(12ms)` / `(unreachable)` 后缀与 ` -> 5.6.7.8:443` 生效入口 |
| `ppp.vcxproj` / `ppp.vcxproj.filters` | 登记新增的 `ConnectivityProbe.cpp/.h` |

---

## 4. 探测实现（ConnectivityProbe）

| 探测 | 行为 | 记录 |
|------|------|------|
| `ProbeTcp` | 异步 TCP connect，RTT = connect 耗时 | 达 L1 |
| `ProbeWebSocket` | TCP connect + HTTP Upgrade（101） | 达 L3 |
| `ProbeWebSocketSSL` | TCP connect + TLS（带 SNI，镜像真实连接不校验 CA）+ WebSocket Upgrade | 达 L3 |
| `ProbeUdp` | **仅发送**一个数据报：静态 UDP 通道使用应用级回显协议（会话 ID + 密文），独立探测无法复刻，故 reachable = OS 接受数据报，RTT ≈ 0 | 仅发送 |

要点：

- 所有探测 I/O 运行在调用方 `io_context`，每入口一个 deadline 定时器，超时即 `cancel` socket；
- 不做 L4/L5 链路层握手——那会在对端产生真实会话（幽灵会话）；
- `protect` 回调：Windows 上为路由 pinning 预留，Linux 上为 `ProtectorNetwork::Protect(sockfd)`，Android 为空实现（整个功能桌面端专属）。

---

## 5. 入口选优（ProbeSelectServerEndPoint）

流程：

1. **解析候选**：`entries = [client.server] + client.servers`。主入口走完整 URL 解析（scheme/path/端口）；
   备份入口按 `IP:port` 解析并继承主传输；`socks://` 归一到 TCP 探测。非法条目直接过滤。
2. **路由保护（Windows）**：探测前对所有候选 IP 执行
   `EnsureWindowsIPv4/6ServerRoute`，把路由 pin 到物理网卡，防止探测 SYN 进入 TAP 自环。
3. **缓存判定**：`probe_results_`（entry → `Result`）中：
   - `penalty_until > now` → 惩罚期内视为不可达（拉黑），不再探测；
   - 结果有效（`reachable && stage >= 配置 stage && now < timestamp + ttl`）→ 免探，直接用缓存 RTT；
   - 其余进入本轮探测任务。
4. **执行探测**：`parallel=true` 且任务数 > 1 时用 `YieldContext::Spawn` 并行，
   通过 `state.pending.fetch_sub(1) == 1` 唤醒父协程；`Spawn` 失败也补计数，避免死锁。
   串行模式逐个探测。类别过滤：`categories` 非空时只探测命中的类别。
5. **选优**：在所有可达候选中取最小 RTT；无可用候选时回落到旧的主入口逻辑（`return false`）。
6. **状态落盘**：写入缓存 `StoreProbeResult`，并更新原子状态
   `probe_rtt_ms_ / probe_reachable_ / probe_checked_ / probe_server_`，供切换页读取。

---

## 6. 与既有连接流程的集成

- `GetRemoteEndPoint()`：当 `probe.enabled && !forwarding && !_ANDROID && 有协程上下文` 时，
  先执行探测选优并覆盖 `server_url_` 各字段；有转发代理（`server_proxy`）时不探测。
- `ExchangeToReconnectingState()`：入口握手/连接失败进入重连时，把**当前失败入口拉黑**
  （`reachable=false`，`penalty_until = now + ttl`），并置 `server_url_.port = 0` 使缓存失效，
  下一次连接重新走探测选优——实现“自动故障切换”。
- 已建立的连接不受影响；探测只影响首次连接与重连时的入口选择。

---

## 7. 状态上报与切换页展示

- `VEthernetNetworkSwitcher::OutboundStatus` 新增：
  `probe_enabled / probe_checked / probe_reachable / probe_rtt_ms(-1) / probe_server`；
  `GetOutboundStatuses()` 在主出口与多出口两条路径都填充。
- `main.cpp` SERVERS 页（tab 3）：
  - 启用且已探测时，在入口状态后追加 `(12ms)` 或 `(unreachable)`；
  - 生效入口与配置端点不同（探测选中备份入口）时显示 ` -> 5.6.7.8:443`。

---

## 8. 平台差异

| 平台 | 行为 |
|------|------|
| Windows | 探测前 pin 候选路由到物理网卡（防自环）；完整实现 |
| Linux | 探测 socket 用 `ProtectorNetwork::Protect(sockfd)` 防自环；完整实现 |
| Android | `ConnectivityProbe` 为空实现，探测选优整体禁用（桌面端功能） |

---

## 9. 编译验证

- `MSBuild ppp.vcxproj`（Debug|x64，v145 / Windows SDK 10.0.26100.0）全量编译**通过**：
  退出码 0，`error C`/`MSB` 错误 0 条，仅有源码编码（C4819/C4828）与增量链接（LNK4075）等无害警告；
  并成功链接出 `x64\Debug\ppp.exe`。
- `ConnectivityProbe.cpp/.h` 已登记进 `ppp.vcxproj` / `ppp.vcxproj.filters`。

---

## 10. 边界与已知限制

- **UDP 探测为 send-only**：静态 UDP 通道（`udp.static.servers`）未纳入本次入口选优。
- **代理路径不探测**：配置 `client.server_proxy` 时跳过探测（避免“代理不可达 vs 隧道目标不可达”语义混淆）。
- **不做 L4/L5**：`stage` 上限 3；不模拟完整握手（避免幽灵会话）。
- **不做周期 TTL 重探与滞回切换**：TTL 到期后仅在下次首次连接/重连时重探；
  设计文档中的“+20% 不切 / +200% 切”滞回策略本期未实现。
- **备份入口必须为 IP 字面量**：不解析域名。
- **缓存键为 `host:port`**：同一入口的探测结果在 TTL 内复用。
- 本地仓库当前把 `ConnectivityProbe.cpp/.h` 与本文档系列加入了 `.gitignore`（WIP 状态），
  正式提交前需要处理（见 `git status`）。

---

## 11. 建议验证（联调）

1. 关闭/开启 `probe.enabled` 各跑一次，确认开启后 SERVERS 页出现 `(RTTms)` 后缀；
2. 配置多个 `client.servers`，拔掉主入口再重连，确认自动切到备份入口且页面上出现 ` -> 备份:port`；
3. 用 `stage=1` 与 `stage=3` 各测一次，确认耗时与判定深度符合预期；
4. 观察 TAP 网卡抓包，确认探测 SYN 走物理网卡、不进隧道（无自环）。