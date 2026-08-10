# 会话导出：openppp2 多入口 + tcping 优选设计

> 生成日期：2026-08-09
> 性质：AI 会话完整上下文导出（供后续 AI / 维护者快速恢复上下文）
> 主产出文档：[`MULTI_ENTRY_CN.md`](./MULTI_ENTRY_CN.md)（设计稿，未实现）
> 代码状态：本次会话**未改动任何源码**，仅调研 + 设计文档；后续会话（2026-08-09）用户改口，已实现核心（见 `CONNECTIVITY_TEST_CN.md`）

---

## 1. 会话目标演进（用户原始诉求）

| 序号 | 用户请求 | 结论 / 产出 |
|------|----------|-------------|
| 1 | openppp2 是否支持多入口（优选 IP 不同 IP:端口） | 静态 UDP 原生支持（轮询/聚合）；TCP/WS/WSS 仅单入口，无自动优选 |
| 2 | ws/wss 能否设计多入口 | 产出设计文档 `docs/MULTI_ENTRY_CN.md`（故障切换方案） |
| 3 | 入口选择策略 | 用户选定：**故障切换**；实现范围：**只写设计文档不改代码**；配置格式：**仅 IP:端口 数组** |
| 4 | 如何兼顾 tcping 选择 | 设计 tcping 主动优选（新增 2.2 / 3.7 节） |
| 5 | 是否有主副入口之分 | 明确：配置层面主副分明，行为层面优先级 + 锁定，非永久归属 |
| 6 | 能否不分主次、纯看 tcping | 可以：`client.server` 降级为普通候选，`tcping.enabled` 开关切换两种哲学 |
| 7 | 导出对话为 AI 可读文档 | 本文件 |

**重要约束**：用户明确选择**只提供详细设计文档，不实现代码**。除非用户改口，不得改动源码。

---

## 2. 技术调研结论（已核实，含代码位置）

### 2.1 现状事实

1. **`client.server` 是单字符串**（`ppp/configurations/AppConfiguration.h:245`）：`ppp::string server; ///< Primary VPN server address in "host:port" format.` — TCP/WS/WSS 只能单入口。
2. **`udp.static.servers` 是集合**（`AppConfiguration.cpp:1364`）：`ReadJsonAllAddressStringToSet(...)` 支持 `"1.0.0.1:20000"`、`[IPv6]:port`。
3. **静态 UDP 多入口行为**（`VEthernetExchanger.cpp:2350` `StaticEchoGetRemoteEndPoint()`）：
   - 无 aggligator → **轮询**（round-robin），`static_echo_server_ep_balances_` 取头删尾追加。
   - 有 aggligator（`udp.static.aggligator > 0`）→ `aggligator::client_open` 同时连所有服务器做带宽聚合（`common/aggligator/aggligator.cpp:1184-1232`）。
4. **openppp2 无内置延迟测速/优选**：grep `tcping|latency|RTT|probe|health` 无相关实现。
5. **WSS 优选 IP 加速**（README.md:294）：`server` 填优选 IP:端口，`websocket.host/sni` 填真实域名让 CDN 路由。
6. **`--server-dir` 热切换**（`main.cpp:3237-3351`）：JSON 目录 → Servers 页面（每页 10 节点），确认后重读 JSON 重建连接，2 秒后切流量；去重用 `guid + server`（`main.cpp:3304`）。
7. **geo 多出口**：每 outbound 独立读配置，多出口不支持 `--tun-static=yes`（README.md:93）。

### 2.2 关键架构（多入口相关的链路）

- **`server_url_` 缓存机制**（`VEthernetExchanger.cpp:319` `GetRemoteEndPoint()`）：
  - 首次调用解析 `client.server` → 缓存到 `server_url_`（定义于 `VEthernetExchanger.h:291-299`）。
  - `server_url_.port = 0` **只在构造函数重置**（`VEthernetExchanger.cpp:88`）→ 断线重连永远连同一入口。
- **`Loopback()` 重连循环**（`VEthernetExchanger.cpp:676-757`）：`while (!disposed_) { Connecting → OpenTransmission → HandshakeServer → EchoLanToRemoteExchanger → Established → Run }`；失败 → `ExchangeToReconnectingState()` + `Sleep(reconnection_timeout)`。
- **`ExchangeToReconnectingState()`**（`VEthernetExchanger.cpp:1128`）：设 `NetworkState_Reconnecting`、`reconnection_count_++`。
- **`OpenTransmission()`**（`VEthernetExchanger.cpp:404-529`）：每次按当前 `remoteEP` 调 `EnsureWindowsIPv4ServerRoute`（431）/`EnsureWindowsIPv6ServerRoute`（442）pin 物理路由 — **多 IP 天然支持**。
- **状态机**：`Connecting → Established → (断线) → Reconnecting → Connecting → ...`
- **`UriAuxiliary::Parse`**（`ppp/auxiliary/UriAuxiliary.cpp:101-200`）：支持 `ppp://`/`tcp://`/`ws://`/`wss://`/`http(s)://`/`socks://`。
- **`OnUpdate(now)` tick**（`VEthernetNetworkSwitcher.cpp:7106`）：诊断日志、QoS update、exchanger update、forwarding update — tcping 周期刷新的挂载点。
- **`GetOutboundStatuses()`**（`VEthernetNetworkSwitcher.cpp:1318`）：`status.server = outbound.configuration->client.server`（1333）— 可扩展显示当前生效入口/RTT。

### 2.3 tcping 可用构建块（已核实）

| 组件 | 位置 | 用途 |
|------|------|------|
| `async_connect` | `ppp/coroutines/asio/asio.h:139` | SYN 握手计时点，返回 bool + error_code |
| `async_sleep` / `Timer::Timeout` | `asio.h:132` / `ppp::threading::Timer` | 探测超时竞赛 |
| `YieldContext::Spawn` | 协程框架 | 并行探测子协程 |
| `Executors::GetTickCount()` | `ppp::threading` | 毫秒时间戳 |
| `Stopwatch` | `ppp/diagnostics/Stopwatch.h` | 高精度计时 |
| `PacketAction_ECHO = 0x2F` | `VirtualEthernetLinklayer.h:91`；`DoEcho` `VirtualEthernetLinklayer.cpp:1035-1047` | 隧道内延迟探测（需先建立连接，**不能用于入口预选**） |

---

## 3. 最终设计决策（ADR）

### 3.1 ADR-1：入口配置格式

- `client.server`（现有，string）= 主入口 / 首选 / 必填（向后兼容锚点）。
- `client.servers`（新增，string[]）= 备用入口池，元素仅 `IP:端口` 或 `[IPv6]:端口`。
- 复用 `client.server` 的 scheme/path 与 `websocket.host/sni`（CDN 优选 IP 共享 Host/SNI）。
- 校验复用 `ReadJsonAllAddressStringToSet`（`AppConfiguration.cpp:1197`，内部 `Ipep::ParseEndPoint`）。
- 缺省/空 → 行为与现状逐字节一致。

### 3.2 ADR-2：选择语义（故障切换模式 = 默认）

- 连接顺序：`server` → `servers[0]` → … → 循环回 `server`。
- 建立成功（进入 `Established`）→ 锁定当前入口（`server_failover_locked_ = true`），运行中零漂移。
- 断线/握手失败（`ExchangeToReconnectingState`）→ 解锁 + `server_url_.port = 0` 失效缓存 → 下次重连换下一个入口。
- 主副之分 = **优先顺序，非永久归属**：主入口是每轮重连的第一候选，但不因"主入口恢复"而强制切回。

### 3.3 ADR-3：tcping 主动优选（用户最终倾向"不分主次纯看 tcping"）

- 所有入口（含 `client.server`）统一入探测池，按 RTT 升序选最优，**不分主次**。
- 配置开关（形态 A，推荐）：

```json
"client": {
    "server": "wss://104.16.1.1:443/tun",
    "servers": ["104.16.2.2:443", "104.16.3.3:8443"],
    "tcping": {
        "enabled": true,        // false/缺省 = 纯故障切换（ADR-2）
        "timeout-ms": 800,
        "ttl-seconds": 30,
        "parallel": true
    }
}
```

- 三阶段选择算法：① 缓存过期 → tcping 全部入口（并行）→ ② RTT 升序选最优（带 30% 滞回）→ ③ 全部不可达 → 顺序兜底。
- **滞回（hysteresis）**：当前入口 RTT ≤ 最优 × 1.3 则保持不动，防抖动横跳。
- **假阳性惩罚**：tcping 可达但实际连接（TLS/WS）失败 → 临时拉黑（如 30s），避免反复撞同一"测得到连不上"的入口。
- **锁定优先级不变**：进入 `Established` 后锁定，tcping 只在重连时参与；正常运行时零探测开销。
- **周期刷新**：`OnUpdate(now)`（约 500ms tick）检查 TTL 过期 → spawn 后台重探。
- **探测流量必须绕过 VPN**：TAP 接管后探测 socket 需 pin 物理网卡（复用 `EnsureWindowsIPv4ServerRoute`/`Protect`），否则 SYN 进隧道自我循环；启动首次探测发生在接管前无此问题。
- 可选简化：**连接即探测**（省独立探测阶段，边用边学），权衡是首次选择可能非最优。

### 3.4 无需改动的部分（已验证）

- 路由 pin（`OpenTransmission` 按当前 remoteEP pin，多 IP 天然支持）。
- DNS/静态模式兜底（`StaticEchoGetRemoteEndPoint` 用 `server_url_.remoteEP`，随切换更新）。
- server-proxy（`IForwarding::SetRemoteEndPoint` 以当前入口为准）。
- `--server-dir` 去重（`guid + server`，同服务器多入口不视为不同节点，合理）。
- geo 多出口（每 outbound 独立支持 `client.servers`）。

---

## 4. 代码改动清单（未来实现时参考）

| 文件 | 改动 |
|------|------|
| `ppp/configurations/AppConfiguration.h` | `client` 新增 `ppp::vector<ppp::string> servers;` + `tcping` 配置结构 |
| `ppp/configurations/AppConfiguration.cpp` | 默认清空（~362 行）；`Load` 解析 `client.servers`（复用 `ReadJsonAllAddressStringToSet`）+ `client.tcping` |
| `ppp/app/client/VEthernetExchanger.h` | `server_failover_list_` / `server_failover_locked_` / `server_failover_used_`；`FailoverProbeEntry` 探测缓存 + `TcpProbeEntry()` / `FailoverProbeAll()` |
| `ppp/app/client/VEthernetExchanger.cpp` | `GetRemoteEndPoint()`（319）加切换分支；`ExchangeToReconnectingState()`（1128）失效缓存；`Loopback()`（676）握手成功置锁 |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | 可选：`GetOutboundStatuses()`（1333）显示当前生效入口 + RTT；`OnUpdate()`（7106）周期触发探测刷新 |

---

## 5. 验证计划（设计文档第 4 节摘要）

1. 加载单测：合法/非法/IPv6 混合 `client.servers` 过滤与默认值。
2. 端到端：3 入口前 2 个失效 → 自动切第 3 个并锁定；杀掉第 3 个 → 自动切换。
3. 回归：不带 `client.servers` 行为不变。
4. WSS 专项：多优选 IP + 同一 Host/SNI，TLS/WS 握手正确。
5. tcping 专项：RTT 差异明显时选中最低；+20% 不切、+200% 切；探测中断线不阻塞；TTL 过期 kill 最优 → 重探切次优。
6. 回归：`tcping.enabled=false` → 纯故障切换。

---

## 6. 开放问题 / 下一步（待用户确认）

- [ ] 是否将"主副语义"明确补入设计文档 2.2（语义）节。
- [ ] 是否按"不分主次 + 假阳性惩罚 + 滞回保留"改写 2.2 / 3.7（用户已表达倾向，待确认落笔）。
- [ ] 英文版文档（`MULTI_ENTRY.md`，曾提议未回复）。
- [ ] README 更新（设计文档第 6 节建议，未执行）。
- [ ] 是否开始实现（用户目前选择不实现）。

---

## 7. 参考文件索引

- 主设计文档：`docs/MULTI_ENTRY_CN.md`
- 本导出：`docs/MULTI_ENTRY_SESSION_EXPORT.md`
- 关键源码（只读）：`ppp/app/client/VEthernetExchanger.h/.cpp`、`ppp/configurations/AppConfiguration.h/.cpp`、`ppp/app/client/VEthernetNetworkSwitcher.cpp`、`ppp/coroutines/asio/asio.h`、`ppp/app/protocol/VirtualEthernetLinklayer.h/.cpp`、`ppp/auxiliary/UriAuxiliary.cpp`
- 仓库记忆（repo memory，排障历史）：`openppp2-prefer-ipv4.md`、`openppp2-connect-hang.md`、`openppp2-peer-removed.md` 等 20 余篇
