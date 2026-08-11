# 预热式渐进热切换设计（单配置多入口 · 游戏无感）

> 状态：M1 / M2 / M3 / M4 已实施，M5 部分实施（drain 强制回收 + 被动直切；双传输过渡暂缓）——2026-08-11 完成终审修复
> 实施记录见文末「实施状态」与「审查收尾（2026-08-11）」；全部改动已通过 cl.exe 语法编译验证（Windows x64 Debug，9 个 TU 0 错误）
> 关联文档：[MULTI_ENTRY_CN.md](./MULTI_ENTRY_CN.md)（多入口 + 故障切换）、[CONNECTIVITY_TEST_CN.md](./CONNECTIVITY_TEST_CN.md)（连通性探测）、[MULTI_ENTRY_PPP_CN.md](./MULTI_ENTRY_PPP_CN.md)（PPP 多入口）
> 涉及代码：`ppp/app/client/VEthernetExchanger.*`、`ppp/app/client/VEthernetNetworkSwitcher.*`、`ppp/app/client/ConnectivityProbe.*`、`ppp/app/mux/vmux_net.*`、`ppp/app/protocol/VirtualEthernetLinklayer.*`、`ppp/configurations/AppConfiguration.*`

## 1. 背景与现状

### 1.1 演进历史

- **v1（8/10 之前）**：30% 滞回择优。重连选入口时，当前入口 RTT 在最优 +30% 以内保持，超过则切到最低 RTT 入口。由于 wss 频繁断线重连，每次重连都重新择优 → “入口 IP 一直在换”，实测断流严重（单连接大文件下载 1~3s 被杀）。
- **v2（`62e25841`，当前）**：完全 sticky。当前入口只要可达就永不切换（无论 RTT），只有不可达/黑名单才让最低 RTT 候选胜出。纯故障切换（failover）。
- **问题**：入口“半故障”（高丢包/高 RTT 但未断）时只能被动等断线；断线切换 = mux 整体重建 = 全部连接中断。游戏在切换瞬间必然感知。

### 1.2 现状机制（代码事实）

- 入口选择唯一入口点：`VEthernetExchanger::ProbeSelectServerEndPoint`（`VEthernetExchanger.cpp:227`），仅在 `OpenTransmission → GetRemoteEndPoint`（建立/重连传输）时调用。
- 结果粘性持久化：`probe_server_`（跨重连），`ConnectivityProbe::Result` 缓存（`entry/type/reachable/rtt_ms/stage/timestamp/ttl_ms/penalty_until`）。
- 探测刷新：`VEthernetNetworkSwitcher::OnTickRefreshOutboundProbes`（500ms tick + `ttl-seconds` 过期，`VEthernetNetworkSwitcher.cpp:428`）。
- mux 通道：`MuxConnectAllLinklayers` / `MuxGrowLinklayers` 为每条 linklayer 独立 `ConnectTransmission`（独立 wss 连接），但**全部走同一入口**。
- turbo 池：`set_pool_hard_max` / `turbo_controller_tick` / `retiring_` / `reap_retired_linklayers`（`vmux_net.cpp:140/2035/2000`）——同一入口的通道增长/退休机制已存在，本方案复用之。
- 跨配置热切换范式：`SwitchPrimaryOutbound`（`VEthernetNetworkSwitcher.cpp:1826`）：新建 exchanger → `Open()`（完整握手）→ pending 2s 观察 → 激活 → 旧 exchanger 自然 drain。**“先建好再切换”的范式已在仓库中存在**，本方案将其内化到单配置入口级。

### 1.3 游戏流量的两条路径

- **游戏 TCP**（登录/聊天/交易/部分同步）：走 mux 字节流（4 条 linklayer 通道竞争发送）。
- **游戏 UDP**（FPS/MOBA 主流）：`PacketAction_SENDTO` 封装（`VirtualEthernetLinklayer.cpp:518`），与 mux **共用同一批 linklayer 通道**，不经过 mux 字节流。
- **静态 UDP**（`udp.static.servers`）：独立通道，不在本方案范围（另案）。
- 换入口只影响“客户端 → 隧道服务器”接入段；隧道服务器出口 IP 不变 → **游戏服务器视角无任何变化**。

## 2. 目标、边界与判定标准

### 2.1 目标

1. 单配置内（`client.server` + `client.servers`）入口之间运行中自动切换；
2. 切换对现有连接无感：mux 不重建、TCP 不中断、预防性切换 UDP 零丢包；
3. 游戏不掉线：切换影响压到“一次普通网络抖动”的量级。

### 2.2 边界（不做）

- 不做已建 TCP 连接的无感迁移（无流迁移原语；mux 逻辑连接跨通道，天然无需迁移）；
- 不改变出口 IP（游戏服务器视角不变）；
- 不覆盖静态 UDP 通道（`udp.static.servers`）的入口切换；
- `hot-switch.enabled=false` 或 `client.servers` 缺省时，行为与 v2 完全一致。

### 2.3 判定标准（游戏场景量化）

| 指标 | 目标值 | 依据 |
|---|---|---|
| 预防性切换丢包 | 0 | 新通道预热完成才接管 |
| 切换延迟抖动 | <50ms | 同流前后包跨通道的延迟差 |
| 被动切换中断窗口 | <1s | 低于游戏 UDP 掉线判定（3~10s） |
| TCP 连接 | 不断 | mux 不重建 |
| 切换乒乓 | 0 | 防抖策略（§7） |

## 3. 总体架构

主方案：**通道级渐进替换（linklayer 入口参数化 + 探测驱动池管理）**。

- 入口选择下沉到 linklayer：每条通道创建时独立调用 `ProbeSelectServerEndPoint`（复用其缓存/黑名单），按 RTT 分布（如 4 通道 = 最优入口 2 + 次优 2，`channels-per-entry`）。
- 探测驱动热迁移：扩展 `turbo_controller_tick` 为跨入口池管理：
  1. 入口 RTT 显著劣化（§7 触发条件）→ 用最优入口 `MuxGrowLinklayers(1)` 异步建新通道（**预热**，失败不影响现状）；
  2. 新通道 `add_linklayer` 就绪并通过观察期 → 劣化入口的通道标 `retiring_ = true`；
  3. `reap_retired_linklayers` 在途写清零（`inflight_ == 0`）后移除旧通道。
- mux 不重建、连接不断；切换窗口内的跨通道帧重排由 flow-v2 重排缓冲兜底（§6 修复后）。

备选（不首期做）：**双传输过渡**——复刻 `SwitchPrimaryOutbound` 到入口级（新入口建完整主传输 + 新 mux，旧 mux 存量连接 drain），改动大，列为 M5。

## 4. 切换状态机

### 4.1 入口状态（per entry）

```
Unknown → Probing → Healthy(primary) ──劣化──→ Degraded ──恢复──→ Healthy
                        │                          │
                        └──不可达/黑名单──────────→ Blacklisted →(惩罚过期)→ Probing
```

- `Healthy(primary)`：当前主入口（`probe_server_` 指向）。
- `Healthy(backup)`：备用入口，参与预热候选。
- `Degraded`：RTT 劣化或连续失败计数超过阈值（§7），触发预热。
- `Blacklisted`：`penalty_until` 内不参与选择与预热（机制已有）。

### 4.2 切换流程状态（client 级）

```
Idle → Preheating → Ready → Draining → Idle
        │              │        │
        └─失败→回滚────┘        └─超时→强杀
```

| 状态 | 行为 |
|---|---|
| `Idle` | 无切换。 |
| `Preheating` | 异步建 B 通道（`OpenTransmission(B)` + `HandshakeServer` + LAN 交换），流量 100% 走 A。 |
| `Ready` | B 通过 `add_linklayer` 且首个 keepalive 往返成功；进入观察期（`observe-ms`，默认 2000ms），期间 A 恢复健康则回滚。 |
| `Draining` | B 接管；A 通道 `retiring_`，在途清零后回收；超过 `drain-timeout-seconds`（默认 120s）强杀。 |
| 回滚 | 任一阶段失败 → 回收 B 通道，A 保持，状态回 `Idle`；B 连续失败计数 +1。 |

## 5. 切换时序（游戏场景）

### 5.1 预防性切换（核心路径）

```
t0   探测刷新：A.rtt=250ms（基线 100ms），B.rtt=90ms，连续 3 周期满足 §7 触发
t0   进入 Preheating：异步 OpenTransmission(B) + 完整握手（TCP+TLS+WS+mux）
t1   B 通道就绪（t0 + 200~600ms，取决于 B 的 RTT）；期间游戏流量 100% 走 A，零影响
t2   B 加入通道池（add_linklayer），帧开始竞争分发到 A+B
      游戏 UDP：同流前后包可能跨通道，延迟差 = |RTT_A - RTT_B|（目标 <50ms），零丢包
      游戏 TCP：mux 帧跨通道重排，flow-v2 重排缓冲（§6 修复后）兜底，无感
t3   观察期通过：A 的通道标 retiring_
t4   A 通道 inflight_==0 → reap_retired_linklayers 移除
t5   Idle。全程游戏无感知（仅 t2~t3 间最多一次 <50ms 抖动）
```

### 5.2 被动切换（A 已故障，无预热机会）

```
t0   A 通道断开 / 探测不可达
t1   立即用最优可达入口 B 建通道（跳过 Ready 观察期，直接激活）
t2   B 就绪加入；A 剩余通道 retiring
t3   A 移除
影响：t0~t2 窗口（200~600ms）UDP 丢包、TCP 段丢失
判定：UDP 掉线阈值 3~10s → 不掉线；TCP RTO 重传 → 不断连（最多一次卡顿）
说明：此场景是“故障恢复”而非“热切换”，目标只是低于掉线阈值
```

### 5.3 回滚（预热失败）

```
Preheating 中 B 建链/握手失败，或观察期内 A 恢复健康
→ 回收 B 通道，A 继续服务，状态回 Idle
→ B 连续失败计数 +1，达阈值进黑名单（penalty_until）
```

### 5.4 多入口分布（`channels-per-entry` 增强）

- 常驻分布：A×2 + B×2（4 通道时）。单入口故障只损失一半吞吐，不触发全量切换。
- 切换仅发生在“分布失衡 + RTT 显著劣化”时（重平衡），避免无谓切换。
## 6. flow-v2 修复设计（硬前提）

### 6.1 问题机理

- flow-v2（`mode=flow + turbo` 协商）每连接独立 DSN，接收端 `packet_input_flow` 有界重排缓冲（默认 1MB/400ms）。
- 缺口超时/缓冲溢出 → `flow_force_advance`（`vmux_net.cpp:940`）**跳号并继续投递后续帧** → 字节流空洞。
- 客户端/服务器均为 TCP 代理（VNetstack/TcpipConnection），字节流已被两端 ACK，**上层无重传兜底** → 空洞 = 永久数据损坏 → TLS `record layer failure`（实测 1000mb.bin 三次 0.9~1.4s 断开即此路径）。
- 多通道竞争发送天然产生跨通道重排 → 高突发（>10MB/s）下必然触发。热切换会制造重排窗口，**必须先修**。

### 6.2 阶段一：配置级 + 禁止跳号（M1，立即实施）

1. 防御性默认配置：`mux.flow.reorder.bytes = 16777216`（16MB）、`timeout = 2000`（ms）。
2. 行为变更：`flow_force_advance` **禁止跳过数据帧**（`cmd_push`）：
   - 缺口未超时 → 正常缓冲等待；
   - 缺口超时（2000ms）**或**重排缓冲溢出（内存保护）→ 不再跳号投递，改为 `close_exec()`（mux 整体重建，回退“干净重连”语义：连接全断但**数据不损坏**）；
   - 仅 FIN 帧的缺口允许跳过（连接已结束，无后续数据）。
3. 收益：彻底消除“静默字节损坏”；代价：真丢帧时整体重连（与 v2 failover 同级，连接级可感知，但不再有损坏的中间态）。
4. 回归：快/慢读下载不再出现 TLS `record layer failure`；极端丢帧表现为控制台 `Mux State: reconnecting`。

### 6.3 阶段二：per-flow NACK 重传（M4，根治）

目标：缺口不跳号、不重建，通过重传补齐，真正无感。

1. **发送端有界帧缓存**：per-connection 环形缓存（默认 16MB 或 4096 帧，可配；**必须 ≥ `mux.flow.reorder.bytes × 2`** 以覆盖“接收端重排缓冲 + 网络在途”）。
2. **接收端 NACK**：检测到 DSN 空洞 → 发 NACK 控制帧（新 `cmd`，携带 `connection_id` + 缺失 DSN 区间 + 当前 rx_ack）。
3. **发送端重放**：收到 NACK → 从缓存重放缺失帧，标记高优先级，走当前最优通道。
4. **控制帧防丢**：NACK 携带序号；发送端对重复 NACK 做指数退避重发；控制帧走 `tx_ctrl_queue_` 优先队列（已有）。
5. **批量 ACK**：接收端按窗口/时间聚合确认（如每 16 帧或每 50ms 一次），ACK 帧走控制队列，避免每帧 ACK 的 4 通道开销；发送端收到 ACK 后释放对应缓存。
6. **协议协商**：新增 `cmd` 需要两端版本协商（沿用 `ordering_caps` 位图机制扩展 capability 位）；旧端不识别 → 不启用 NACK，回退阶段一语义。

### 6.4 与热切换的关系

- M1 完成后即可安全实施 M2/M3：切换窗口的重排由 16MB/2000ms 缓冲覆盖，跳号已禁止，最坏 = 整体重建（不静默损坏）。
- M4 完成后热切换可进一步收紧（无需 2s 观察期兜底，重传覆盖一切丢帧）。

## 7. 防抖策略

### 7.1 触发条件（全部满足才进入 Preheating）

```
1. probe.enabled && hot-switch.enabled
2. B 可达，且 B.rtt < A.rtt / threshold-rtt-factor（默认 2.0）
   或 A.rtt - B.rtt > threshold-rtt-ms（默认 100ms）
3. 条件 2 连续成立 ≥ min-stable-periods（默认 3 个探测周期 = 3 × ttl-seconds）
4. A 非黑名单；B 非黑名单；B 与 A 不是同一 entry
```

说明：条件 3 是防抖核心——单次探测抖动（DNS/CDN 波动）不触发；只有持续劣化才切换（吸取 v1 30% 滞回过松、频繁跳入口的教训）。

### 7.2 切换后锁定

- 激活成功 → 新入口进入锁定期（`lock-seconds`，默认 60s）：锁定期内即使 RTT 反转也不再次切换（除非当前入口不可达）。
- 锁定期内探测继续刷新缓存，但切换判定挂起。

### 7.3 黑名单与惩罚

- 建链失败/握手失败/连续 3 次探测不可达 → 黑名单（`penalty_until = now + penalty-seconds`，默认 60s，机制已有）。
- 黑名单入口不参与选择与预热。
- 被动切换后，旧入口若恢复可达，仅在其 RTT 显著优于当前且持续稳定后才允许切回（防乒乓）。

### 7.4 示例

```
A=100ms 基线；B=95ms → 不触发（factor 不满足，差 5ms < 100ms）
A=100ms→250ms 持续 3 周期；B=90ms → 触发预热 → 激活 B → 锁定 60s
A=250ms→120ms（恢复）→ 锁定期内不切回；锁定到期后 A=120ms vs B=90ms → 差 30ms < 100ms，仍不切
```

## 8. 配置设计

```json
"client": {
    "server": "wss://103.73.220.142:20021/tun",
    "servers": [ "103.73.220.50:20443", "103.135.251.137:51845" ],
    "probe": {
        "enabled":     true,
        "timeout-ms":  800,
        "ttl-seconds": 30,
        "parallel":    true,
        "stage":       3,
        "categories":  ["wss"]
    },
    "hot-switch": {
        "enabled":               true,
        "threshold-rtt-factor":  2.0,
        "threshold-rtt-ms":      100,
        "min-stable-periods":    3,
        "lock-seconds":          60,
        "penalty-seconds":       60,
        "preheat":               true,
        "observe-ms":            2000,
        "channels-per-entry":    0,
        "drain-timeout-seconds": 120
    }
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `enabled` | false | 关闭时完全保持 v2 failover 行为 |
| `threshold-rtt-factor` | 2.0 | 当前入口 RTT 劣于最优的倍数阈值 |
| `threshold-rtt-ms` | 100 | 绝对差阈值（ms），两者满足其一 |
| `min-stable-periods` | 3 | 连续满足触发条件的探测周期数 |
| `lock-seconds` | 60 | 切换后锁定期 |
| `penalty-seconds` | 60 | 黑名单惩罚期 |
| `preheat` | true | 预热式切换；false = 直接切换（被动模式） |
| `observe-ms` | 2000 | Ready 后观察期，期间可回滚 |
| `channels-per-entry` | 0 | 每入口通道数分布（0 = 不分布，全走最优） |
| `drain-timeout-seconds` | 120 | 旧通道退役超时强杀 |

依赖配置：`mux.flow.reorder.bytes ≥ 16777216` 且 `timeout ≥ 2000`（M2 起启动校验，不满足则 `hot-switch` 拒绝启用并告警）。`n`nM4 的 mux 侧配置（默认值已满足，`retransmit` 默认开启但需协商；以下为显式示例）：`n`n```json`n"mux": {`n    "flow": {`n        "reorder": {`n            "bytes": 16777216,`n            "timeout": 2000`n        },`n        "retransmit": {`n            "enabled":          true,`n            "cache-bytes":      33554432,`n            "max-frames":       4096,`n            "ack-every-frames": 16,`n            "ack-delay-ms":     50,`n            "nack-max-retries": 5,`n            "nack-backoff-ms":  100`n        }`n    }`n}`n````n`n| 字段 | 默认 | 说明 |`n|---|---|---|`n| `enabled` | true | 总开关；仍需双方协商（flow-v2 + `ordering_caps_nack`）才实际启用 |`n| `cache-bytes` | 33554432 | 发送端每连接重传缓存字节上限；启动钳制 ≥ `reorder.bytes × 2` |`n| `max-frames` | 4096 | 发送端每连接重传缓存帧数上限 |`n| `ack-every-frames` | 16 | 接收端每投递 N 帧发一次批量 ACK |`n| `ack-delay-ms` | 50 | 接收端批量 ACK 的最大时间间隔 |`n| `nack-max-retries` | 5 | NACK 重试上限，超出后干净重建 mux |`n| `nack-backoff-ms` | 100 | NACK 重发基础退避，指数 ×2 至 1600ms |

## 9. 改动点清单（文件级）

| 文件 | 改动 |
|---|---|
| `ppp/configurations/AppConfiguration.h/.cpp` | `hot-switch` 配置结构、加载、校验、导出 |
| `ppp/app/client/ConnectivityProbe.h/.cpp` | `Result` 增加连续失败计数；NACK 相关（M4） |
| `ppp/app/client/VEthernetExchanger.cpp/.h` | `OpenTransmission` 入口参数化；预热协程（`PreheatTransmission`）、观察/激活/回滚；热迁移调度入口 |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp/.h` | `OnTickRefreshOutboundProbes` 扩展切换判定（§7）；调用热迁移；状态上报 TUI |
| `ppp/app/mux/vmux_net.cpp/.h` | M1：`flow_force_advance` 禁止跳数据帧；M4：per-flow NACK/缓存；暴露 linklayer 入口标记 |
| `ppp/app/protocol/VirtualEthernetLinklayer.*` | linklayer 携带入口标识（entry tag），供池管理区分 |
| `main.cpp`（TUI） | 显示各入口 RTT/状态/通道分布；手动触发重平衡 |
## 10. 验证计划

1. **配置加载**：`hot-switch` 合法/非法/缺失；与 `probe`、`servers` 组合矩阵；依赖校验（reorder 不满足时拒绝启用）。
2. **本地端到端**：本地起 3 个入口（不同端口），对第 2 个入口做流量整形模拟 RTT 劣化，验证 触发→预热→Ready→Draining→锁定→回滚 各状态；kill 入口模拟被动切换。
3. **游戏场景模拟**：恒定 UDP 小包流（50pps）+ 切换，统计丢包率/抖动（目标：预防性 0 丢包、<50ms）；并发 TCP 下载验证不断流。
4. **flow-v2 回归**：`1000mb.bin` 下载 ×10（M1 前后对比）；快读/慢读/并行握手；断流日志零增长。
5. **兼容回归**：`hot-switch.enabled=false` 与单入口配置行为与 v2 完全一致；Windows/Android 控制台显示正常。

## 11. 风险与回退

| 风险 | 缓解 |
|---|---|
| 服务器限制同会话通道数（如 4 上限） | 预热通道数 ≤ `pool_hard_max`（已有）；跨入口通道仍是同一服务器集群（同 host/SNI，会话语义不变） |
| 探测频率被 CDN 限流 | `ttl-seconds` 默认 30s；黑名单；探测走物理网卡（`EnsureWindowsIPv4ServerRoute`/`Protect` 已有） |
| NACK 风暴（M4） | 批量 ACK、指数退避、控制队列优先级 |
| M1 整体重建导致连接断 | 与 v2 failover 同级（连接断但数据不损坏），仅发生在真丢帧时；M3 后预热窗口已覆盖大部分场景 |
| 热切换制造新断流 | 硬前提 M1 未完成前禁止启用 `hot-switch`（启动校验强制） |

## 12. 实施里程碑

- **M1**：flow-v2 阶段一（防御配置 + 禁止跳号）——独立上线，立即消除静默损坏。✅ 已实施
- **M2**：linklayer 入口参数化 + 通道分布（`channels-per-entry`）。✅ 已实施
- **M3**：预热式渐进热切换（状态机 + 防抖 + 时序 §5）——核心交付。✅ 已实施
- **M4**：flow-v2 阶段二（per-flow NACK 重传）——热切换窗口收紧，真正无感。✅ 已实施
- **M5**（可选）：双传输过渡（复刻 `SwitchPrimaryOutbound` 到入口级）。◑ 部分实施（drain 强制回收 + 被动直切；完整双 mux 过渡由 M3 通道级迁移覆盖，暂缓）

## 实施状态（2026-08-11）

### M1 — 防御配置 + 禁止跳号 ✅

- `ppp/stdafx.h`：`PPP_MUX_FLOW_REORDER_BYTES = 16777216`（16MB）、`PPP_MUX_FLOW_REORDER_TIMEOUT = 2000`（ms）。
- `ppp/app/mux/vmux_net.cpp`：`flow_force_advance` 改为返回 `bool`，缺口头部为数据帧（`cmd_push`）时**不再跳号**，返回 `false`：
  - `flow_evict_expired`（缺口超时）→ `close_exec()` 干净重建；
  - `packet_input_flow` 重排缓冲溢出逐出循环 → 返回 `false`，由调用方 `close_exec()`；
  - 单帧超过重排上限 → 仅 FIN 允许跳过，数据帧返回 `false`。
- 仅 FIN 缺口仍可跳过（连接已结束，无后续数据）。
- 收益：消除静默字节损坏（TLS `record layer failure` 根因）；代价：真丢帧时 mux 整体重建（连接级可感知，数据不损坏）。

### M2 — 入口参数化 + 通道分布 ✅

- `AppConfiguration`：新增 `client.hot-switch` 配置结构（§8 字段全量），加载时校验依赖（`mux.flow.reorder.bytes ≥ 16MB && timeout ≥ 2000ms`，不满足则拒绝启用）。
- `vmux_linklayer` 增加 `entry` 标签；新增 `set_linklayer_entry` / `retire_linklayers_of_entry` / `get_live_linklayer_count`。
- `ProbeSelectServerEndPoint` 增加 `forced_entry`：指定入口可达即选中（不更新 sticky），供预热/分布使用。
- `OpenTransmission` / `ConnectTransmission` / `GetRemoteEndPoint` / `MuxGrowLinklayers` 增加可选入口参数。
- `MuxConnectAllLinklayers`：`hot-switch.channels-per-entry > 0` 时按 RTT 排名分配常驻通道（如 4 通道 = A×2 + B×2），每条通道打 entry 标签。
- `hot-switch.enabled` 时客户端 `pool_hard_max` 提升到 base×3（turbo 关闭也有预热余量；服务器 flow 模式本就接受 base×3）。

### M3 — 预热式渐进热切换 ✅

- `VEthernetExchanger` 状态机 `Idle → Preheating → Ready → Draining → Idle`（§4.2），每 tick（`Update()`）驱动。
- 触发与防抖（§7）：`HotSwitchPickTarget` 按 RTT factor/绝对差判定劣化，每 `ttl-seconds` 一个探测周期计数，连续 `min-stable-periods` 才进入预热；黑名单（`penalty_until`）参与过滤。
- 预热：`HotSwitchPreheat` 协程为 B 入口建 `budget = min(base, hard_max - live)` 条通道（TCP+TLS/WS+mux 完整握手），失败不黑名单单次、连续失败走 `HotSwitchBlacklistEntry`。
- Ready：观察 `observe-ms`；期间 A 恢复则回滚（回收 B 通道）。
- 激活：`HotSwitchActivate` 退役 A 全部通道（`retire_linklayers_of_entry`）→ sticky 切 B → 锁定 `lock-seconds` → 按 base 补足 B 通道。
- 被动模式（`preheat=false`）：跳过观察期直接激活。
- 后台探测：`VEthernetNetworkSwitcher::RefreshOutboundProbes` 逐入口把结果写入 exchanger `probe_results_`（跳过黑名单项，不清除惩罚），状态机使用每入口 RTT 判定。

### M5 — 部分实施 ◑

- drain 强制回收：`vmux_linklayer.retiring_since_` + `reap_retired_linklayers` 超 `drain-timeout-seconds` 强杀（写完成回调持有 linklayer 共享引用，安全）。
- 被动直切路径（`preheat=false`）。
- 完整“双传输过渡”（第二 mux 会话 + 存量连接 drain）暂缓：M3 通道级迁移已覆盖游戏无感场景，且双 mux 会话语义（服务器侧多会话）风险高、收益重叠。

### 已知边界

- `probe.enabled=false` 或 `hot-switch.enabled=false` 或 `client.servers` 为空：行为与 v2 完全一致（纯 sticky failover）。
- 静态 UDP（`udp.static.servers`）不参与热切换（另案）。
- TUI 每入口 RTT/通道分布展示未做（SERVERS 页仍显示 outbound 级延迟）。

### M4 — per-flow NACK 重传 ✅

- `ppp/app/mux/vmux_net.h`：新增 `cmd_nack` / `cmd_ack`（`cmd_max` 之前）、`ordering_caps_nack = 0x02`、`nack_frame { nack_seq, dsn_from, dsn_to }` / `ack_frame { ack_seq, dsn_ack }` 线格式；`flow_rx_context` 增加 NACK/ACK 状态；新增 `flow_tx_cache`（DSN -> 帧）+ `tx_flow_cache_`。
- `ppp/app/mux/vmux_net.cpp`：
  - 发送端有界缓存：flow-v2 数据帧在 `post_internal` 分配 DSN 后入缓存（入缓存深拷贝，避免首传加密原地改写污染重放副本），按 `cache-bytes`（默认 32MB，钳制 ≥ reorder×2）/ `max-frames`（4096）淘汰最旧；`packet_input_ack` 按水位释放；`flow_retx_sweep` 空闲 2 分钟清理。
  - 接收端 NACK：缺口出现（首个 future 帧缓冲）立即发 NACK；`flow_evict_expired` 按指数退避（100ms 起，×2，上限 1600ms）重发；重试超限（默认 5 次）回退 `close_exec()` 干净重建。
  - 发送端重放：`packet_input_nack` 从缓存深拷贝重放缺失区间（单次 ≤ 64 帧），`push_front` 数据队列头部 + 立即排空；请求帧已不在缓存且连接仍活跃 -> `close_exec()` 快速失败。
  - 批量 ACK：每投递 16 帧或 50ms 聚合一次（`flow_maybe_ack` / `flow_send_ack`），释放对端缓存。
  - FIN 关闭缺口（跳号）时清理 NACK 状态；`maybe_release_flow` 同步清理发送缓存。
- 协商：`ordering_caps` 位 `0x02`，仅当双方都声明且 `mux.flow.retransmit.enabled` 才启用；旧端无该位 -> 不启用，完全回退 M1 语义（不发任何 NACK/ACK 帧，协议兼容）。
- 配置：`mux.flow.retransmit.{enabled, cache-bytes, max-frames, ack-every-frames, ack-delay-ms, nack-max-retries, nack-backoff-ms}`，默认 `enabled=true`（协商后才生效）。
### 审查收尾（2026-08-11，终审修复）

以下为 M1–M5 最终审查发现的缺陷及修复，全部已合入工作区并编译复验。

- **发送缓存共享 buffer → 深拷贝**（`vmux_net.cpp` `post_internal`）：NACK 重传缓存原先与首传共享同一帧 buffer；`ITransmissionBridge::Write` 的加密路径（`Transmission_Payload_Encrypt_Partial`，masked_xor + shuffle）在无 transport cipher（明文/无密文配置）时会原地改写源 buffer，导致缓存内容被污染、NACK 重放二次混淆。修复：入缓存即深拷贝；拷贝失败降级为“只发不缓存”，NACK 到来触发干净重建，绝不发送损坏数据。
- **恢复正常选路路径的探测显示**（`VEthernetExchanger.cpp` `ProbeSelectServerEndPoint`）：正常选路分支误删 `probe_rtt_ms_/probe_reachable_/probe_checked_` 三行更新（只保留失败分支写入 -1/false），导致 TUI 显示颠倒（成功显示旧值、失败才更新）。已恢复三行 `store`，并删除残留的错位预热注释；forced-entry 预热路径仍不写显示字段（设计不变）。
- **`MuxGrowLinklayers` 悬垂指针**：`HotSwitchActivate` 传局部 `&target`，`YieldContext::Spawn` 异步到 mux strand 后 `target` 已销毁（UB）。修复：`MuxGrowLinklayers` 改按值接收 `ppp::string entry`，lambda 内构造 `entry_ptr` 使用（`VEthernetExchanger.h/.cpp` 签名与全部调用点同步修改）。
- **热切换 mux 操作收敛到 mux strand**（跨线程竞争）：exchanger 线程（Default io_context）原先持 `syncobj_` 直接改 `rx_links_/tx_links_`（`retire_linklayers_of_entry`/`get_live_linklayer_count`/`get_pool_hard_max`），与 mux strand 线程（Scheduler io_context，多线程 run）无锁的 `update() → reap_retired_linklayers()` 并发，同一 vector 读写存在数据竞争。修复：`HotSwitchBeginPreheat` 协程改为 `YieldContext::Spawn(..., mux strand, ...)`；`HotSwitchActivate`/`HotSwitchRollback` 的 retire/regrow 通过 `boost::asio::post(*strand, ...)` 派发到 mux strand 执行，全部 mux 内部访问 strand-affine。
- **`StoreProbeResult` 不再清除黑名单**：内联探测路径（`ProbeSelectServerEndPoint` 直接调用）原先会以新结果覆盖 `penalty_until`，绕过 `HotSwitchBlacklistEntry` 的惩罚期。修复：`StoreProbeResult` 内部检查已有记录 `penalty_until > now` 时拒绝覆盖（对 switcher 与内联两条路径统一生效）。
- **hot-switch 依赖不满足时输出日志**（`AppConfiguration.cpp`）：`reorder.bytes/timeout` 低于要求而禁用 hot-switch 时新增 `LOG_ERROR`（含当前值与要求值），不再静默失效。
- **格式与注释**：修正 `MuxConnectAllLinklayers` 中 48 空格异常缩进；`ResetHotSwitchState` 补充注释说明 `hot_switch_locked_until_` 有意不重置（防切换乒乓）；`HotSwitchActivate` regrow 注释改为与实现一致（补 base、头寸允许时利用 turbo 余量）。
- 文件卫生：全部 `.cpp/.h` 保持 UTF-8 无 BOM / CRLF（无 loneLF）；`vmux_net.h` 的既有乱码为 HEAD 自带，不处理。
- cl.exe 语法编译复验（9 个 TU 全部 0 错误）：`vmux_net.cpp`、`VEthernetExchanger.cpp`、`VEthernetNetworkSwitcher.cpp`、`VEthernetNetworkTcpipConnection.cpp`、`client/proxys/VEthernetLocalProxyConnection.cpp`、`server/VirtualEthernetExchanger.cpp`、`server/VirtualEthernetNetworkTcpipConnection.cpp`、`AppConfiguration.cpp`、`main.cpp`。

### 启用方式（用户侧）

1. 单入口配置即自动获得 M1（flow-v2 禁止跳数据帧，缺帧干净重建），无需任何配置。
2. 启用 M3 热切换需同时满足：client.servers 非空、client.probe.enabled=true、client.hot-switch.enabled=true，且 mux.flow.reorder.bytes >= 16777216、mux.flow.reorder.timeout >= 2000（不满足时启动校验自动关闭 hot-switch）。
3. mux.turbo 可保持 `false`（热切换会自行把客户端 pool_hard_max 提到 base×3）。
4. 完整 MSBuild 链接验证尚未在沙箱内执行（FileTracker 限制），建议按 .agents/LOCAL_BUILD_ENV.md 手动跑一次 Debug x64 构建。