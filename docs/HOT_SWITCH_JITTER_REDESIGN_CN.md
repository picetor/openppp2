# 热切换重构设计：波动门控 + 加权评分（默认粘性）

> 状态：已实施（2026-08-12）——替换 `PREHEAT_HOT_SWITCH_CN.md` 中的触发判据，保留其预热状态机与链路恢复机制。代码改动完成，待本机编译验证
> 关联文档：[MULTI_ENTRY_CN.md](./MULTI_ENTRY_CN.md)、[PREHEAT_HOT_SWITCH_CN.md](./PREHEAT_HOT_SWITCH_CN.md)、[CONNECTIVITY_TEST_CN.md](./CONNECTIVITY_TEST_CN.md)
> 涉及代码：`ppp/app/client/VEthernetExchanger.*`、`ppp/app/client/ConnectivityProbe.*`、`ppp/configurations/AppConfiguration.*`

## 1. 背景与问题

### 1.1 现状（代码事实）

- 入口选择唯一入口点：`VEthernetExchanger::ProbeSelectServerEndPoint`（`VEthernetExchanger.cpp:227`），仅在建立/重连传输时调用；结果 `probe_server_` 粘性持久化。
- 探测刷新：`VEthernetNetworkSwitcher::OnTickRefreshOutboundProbes`（500ms tick + `ttl-seconds` 过期，`VEthernetNetworkSwitcher.cpp:428`），持续探测 `client.servers` 全部入口。
- 探测结果：`ConnectivityProbe::Result{ entry/type/reachable/rtt_ms/stage/timestamp/ttl_ms/penalty_until }`——**只有单次 RTT 快照，无历史、无波动统计**。
- 当前触发（`HotSwitchPickTarget`，`VEthernetExchanger.cpp:1787`）：
  - 相对阈值 `best_rtt < current_rtt / threshold_rtt_factor`（默认 2.0）或绝对阈值 `current_rtt - best_rtt > threshold_rtt_ms`（默认 100）；
  - 防抖：每个探测周期评估一次，连续 `min_stable_periods`（默认 3）个周期劣化才触发。
- 切换流程（保留）：`Idle → Preheating → Ready(observe_ms) → Draining → Idle`，预热失败黑名单、旧入口恢复回滚、激活后 `lock_seconds` 锁定。

### 1.2 实测问题（2026-08-12 日志）

- 多入口 + flow-v2 下，入口/载波频繁死亡导致 mux 周期重建（6 分钟 4 次 `M4 RETRANSMIT EXHAUSTED` / `M1 UNBUFFERABLE GAP`），下载与测速断流。
- 现有 RTT 触发在"入口半故障（高丢包/高抖动但未断）"时**只看 RTT 差值**，单次快照对毛刺敏感；且探测 RTT 与实际传输质量相关性弱，容易切到"延迟好看但抖动大"的入口，形成"IP 一直在换"的观感。

### 1.3 重构目标

1. **默认不切换（粘性优先）**：当前入口只要链路存活且延迟稳定（波动低于门槛），永不主动切换。
2. **只有两类情况允许切换**：
   - 链路失败（断线/重连/连续探测不可达）→ 无条件 failover（沿用现有逻辑）；
   - 质量劣化达标 → 最近 **3 个探测周期内延迟波动 ≥ 50ms**，且存在综合评分显著更优的备选，才进入预热切换。
3. **持续探测所有入口**，每个入口维护"延迟 + 波动"双指标，用**加权评分**排序，选目标入口。

## 2. 探测数据模型

### 2.1 持续探测

- 保持现有 `OnTickRefreshOutboundProbes`：按 `probe.ttl-seconds`（默认 30s）对所有 `client.servers` 入口并行探测（`probe.parallel=true`），分类 `probe.categories`（wss/ws/tcp...）。
- 探测失败语义：单次失败不惩罚；**连续 2 次失败**标记 `reachable=false` 并进入 `penalty_until` 黑名单（沿用现有惩罚机制）。

### 2.2 每个入口的滑动窗口

新增按入口维护的指标（与 `ConnectivityProbe::Result` 并列，放入 `probe_results_` 扩展或独立 `entry_metrics_`）：

```
EntryMetric {
    entry           : string        // 入口标识 host:port（与探测缓存同 key）
    rtt_samples     : deque<int>    // 最近 N 次探测 RTT（N = jitter-window，默认 3）
    rtt_avg         : int           // 窗口均值（或中位数，见 2.3）
    jitter_ms       : int           // 窗口波动值（峰峰或 MAD，见 2.4）
    samples_full    : bool          // 窗口是否已满 N 次（不满不参与波动判定）
    last_probe_tick : uint64_t
}
```

- 每次探测成功后：`rtt_samples.push_back(rtt)`，超过 N 则弹出最旧；`samples_full = rtt_samples.size() >= N`。
- 探测失败：**不清空历史**（保留波动记忆），仅更新 `reachable`；连续失败达到黑名单阈值时整体标记不可达。

### 2.3 延迟统计量

- 窗口内样本数少（< N）时用**均值** `rtt_avg = mean(samples)`；
- 样本满 N 后用**中位数** `rtt_med`（抗单次毛刺），文档统一以 `rtt_avg` 表示"延迟基准"。

### 2.4 波动（jitter）定义

两种可选定义，实现建议二选一（默认峰峰）：

```
jitter_pp  = max(samples) - min(samples)          // 峰峰抖动：直观、对 1 次毛刺敏感
jitter_mad = median(|x - median(samples)|)        // 中位绝对偏差：抗噪，推荐
```

- 判定用的波动值 = `max(jitter_pp, jitter_mad)`（固定取两者较大，兼顾敏感与稳定，不提供配置）。
- **波动判定前提**：`samples_full == true`（至少 3 次探测），否则视为"数据不足"，不触发主动切换。

## 3. 触发条件（严格门控）

### 3.1 条件 A：链路失败（无条件 failover，保持现状）

- 隧道断线进入 `ExchangeToReconnectingState`（重连计数触发）；
- 当前入口连续 2 次探测不可达；
- mux 重建次数超过阈值（如 60s 内 ≥ 2 次，可选扩展）。
- 以上直接走现有 failover：黑名单当前入口 → 重选评分最优 → 重建，**不经过预热状态机**（连接已断，无感切换无从谈起）。

### 3.2 条件 B：波动门控的主动切换（默认关闭）

**必要条件（全部满足才允许进入预热切换）：**

```
B1 波动门控：current.jitter_ms >= jitter-threshold-ms (默认 50)
B2 质量劣化：current 综合评分劣于 best 超过 switch-margin（见第 4 节）
B3 持续周期：连续 jitter-window (默认 3) 个探测周期同时满足 B1+B2（防抖）
B4 锁定排除：当前不在 hot_switch_locked_until_ 锁定期内
```

- **B1 是硬门槛**：即使 current RTT 比 best 差 200ms，只要 3 周期内波动 < 50ms（稳定），**不主动切换**——这是"默认不切换"的核心。
- B2 选择目标时使用第 4 节加权评分，而不是单一 RTT。
- 切换动作复用现有预热状态机（`HotSwitchBeginPreheat → Ready 观察 → HotSwitchActivate`），保证 mux 不重建、TCP 不中断。

### 3.3 时序示例（探测周期 30s）

```
t=0    探测轮次 R1：current jitter 样本 [60, 62, 61] → 波动 2ms，不满足 B1，不评估
t=30   探测轮次 R2：current RTT 序列 [60, 150, 62] → 波动 90ms ≥ 50，评估 B2 → 劣化成立 → streak=1
t=60   探测轮次 R3：current RTT 序列 [150, 160, 155] → 波动 10ms... 波动回落，streak 清零
```

- 只有**连续 3 个周期**波动都 ≥ 50ms 且评分劣化，才在 t≈90s 进入预热。
- 单次毛刺（一个周期波动大、下个周期恢复）不会触发切换。

## 4. 综合评分与权重（选目标入口）

### 4.1 归一化

对当轮所有可达、非黑名单入口计算：

```
rtt_norm(e)    = rtt_avg(e)     / rtt_ref      // 延迟相对基准，越小越好
jitter_norm(e) = jitter_ms(e)   / jitter_ref   // 波动相对基准，越小越好
rtt_ref    = 全部可达入口 rtt_avg 的中位数（防单个极值拉偏）
jitter_ref = 全部可达入口 jitter_ms 的中位数，为 0 时取 1
```

### 4.2 加权评分

```
score(e) = weight-rtt * rtt_norm(e) + (1 - weight-rtt) * jitter_norm(e)
```

- 默认 `weight-rtt = 0.6`（jitter 权重自动 = 0.4，只配一个字段）；
- 备选目标 = 所有可达且非当前入口中 `score` 最小者 `best`；
- **切换门槛**：仅当 `score(current) - score(best) >= switch-margin`（默认 0.15，可配）时 B2 成立——避免"几乎一样好也换"；
- 若 `jitter-window` 未满（数据不足），该入口 jitter_norm 记为 1.0（不参与作为 best，但可作为 fallback）。

### 4.3 权重设计依据

- 大流量下载/测速场景：主要杀手是**抖动与丢包**（帧空洞 → NACK → mux 重建），不是平均 RTT，故 jitter 权重不可为 0；
- 游戏/交互场景：RTT 更重要，`weight-rtt` 可调到 0.7~0.8；
- 该权重只决定"切到谁"，不决定"要不要切"（要不要切由第 3 节门控决定）。

## 5. 切换流程（复用现有预热状态机）

```
Idle ──(条件 A 链路失败)──────────────► 直切 failover（重选评分最优，不走预热）
 Idle ──(条件 B 连续 3 周期达标)──────► Preheating（对 best 预热 channels）
 Preheating ──(added>0)──────────────► Ready（observe_ms=2000 观察）
   ├─ 旧入口恢复（波动回落 <50ms）───► 回滚 Idle
   └─ 观察期结束仍达标 ──────────────► Activate（retire 旧入口载波 + regrow）
 Activate ──► 锁定 lock_seconds ──────► Idle（期间不评估主动切换）
```

- 预热/观察/激活/回滚全部沿用 `PREHEAT_HOT_SWITCH_CN.md` 已实现的 M2/M3/M5 机制，本设计**只替换触发判定与目标选择**，不改变切换动作本身。
- 激活后 `probe_server_` 更新为目标入口，后续重连粘性到新入口。

## 6. 防抖与防乒乓

| 机制 | 现状 | 本设计 |
|---|---|---|
| 连续周期防抖 | `jitter-window=3` | 保留，且每个周期必须同时满足 B1+B2 |
| 切换锁定 | `lock-seconds=60` | 保留，锁定期内 B 条件不评估 |
| 预热失败黑名单 | `penalty-seconds=60` | 保留 |
| 旧入口恢复回滚 | Ready 期 RTT 恢复 | 改为"波动回落 < 门槛"即回滚 |
| 单次毛刺 | 无防护 | 中位数 + MAD + 3 周期窗口三重防护 |

## 7. 配置精简评估与最终形态

### 7.1 现状字段评估（删 / 合并 / 保留）

**probe 块（现状 6 字段 → 精简后 1 个必选）**

| 字段 | 默认 | 使用点 | 结论 |
|---|---|---|---|
| `enabled` | true | 主开关；`servers` 为空时无意义，单入口 legacy 兼容 | 保留但可省略（默认 true） |
| `timeout-ms` | 800 | 单次探测超时 | 保留为高级项（可省略） |
| `ttl-seconds` | 30 | 缓存 TTL + 热切换评估周期（`VEthernetExchanger.cpp:1841`） | **保留（核心，唯一周期参数）** |
| `parallel` | true | 仅内联探测路径（`VEthernetExchanger.cpp:419`）；后台刷新本就并行 | **删除**：并行是唯一合理行为，无可调价值 |
| `stage` | 3 | 深度已被代码自动自适应（established→3，否则→1，`VEthernetNetworkSwitcher.cpp:542`） | **删除**：固定 3，配置只影响上限，无实际意义 |
| `categories` | 空=全部 | 类别已由入口 URI 协议自动推导（wss://→wss，ppp://→tcp，`ProbeCategoryFromUriProtocol`） | **删除**：固定空=全部即为默认合理行为 |

**hot-switch 块（现状 11 字段 + 新设计 6 字段 → 精简后 3 个）**

| 字段 | 结论 |
|---|---|
| `enabled` | **删除开关，自动启用**：`client.servers` 非空 ∧ probe 可用 ∧ flow-v2 依赖满足（启动校验保留）即生效；解析保留显式 `false` 作兼容 |
| `jitter-window` / `min-stable-periods` | **合并**：两者都是"3 次探测周期"，统一为一个 `jitter-window`（同时定义波动统计窗口与连续达标周期） |
| `threshold-rtt-factor` / `threshold-rtt-ms` | **删除**：被 `switch-margin`（评分差门槛）取代，B2 只看综合评分 |
| `weight-rtt` / `weight-jitter` | **合并**：只保留 `weight-rtt`，jitter 权重 = 1 - rtt 权重 |
| `preheat` / `observe-ms` / `drain-timeout-seconds` / `lock-seconds` / `penalty-seconds` | **内置固定**（true / 2000 / 120 / 60 / 60）：实现细节，无需暴露 |
| `channels-per-entry` | 保留为高级项（默认 0=粘性单入口，可省略） |

### 7.2 最终配置形态（JSON）

**最简形态**（只有一个必填参数——波动门槛）：

```json
"client": {
    "server":  "wss://103.73.220.207:16299/tun",
    "servers": [ "103.73.220.50:20443", "103.135.251.137:51845" ],
    "hot-switch": {
        "jitter-threshold-ms": 50
    }
}
```

**完整可调形态**（全部可选，按需覆盖默认）：

```json
"client": {
    "server":  "wss://103.73.220.207:16299/tun",
    "servers": [ "103.73.220.50:20443", "103.135.251.137:51845" ],
    "probe": {
        "ttl-seconds": 30,
        "timeout-ms":  800
    },
    "hot-switch": {
        "jitter-threshold-ms": 50,
        "jitter-window":       3,
        "weight-rtt":          0.6,
        "switch-margin":       0.15,
        "channels-per-entry":  0
    }
}
```

### 7.3 参数表（精简后）

| 参数 | 默认 | 说明 |
|---|---|---|
| `probe.ttl-seconds` | 30 | 探测周期；同时是热切换评估周期（3 周期 = 90s 窗口） |
| `probe.timeout-ms` | 800 | 单次探测超时（高级） |
| `hot-switch.jitter-threshold-ms` | 50 | 波动门槛：3 周期内波动 ≥ 此值才允许主动切换 |
| `hot-switch.jitter-window` | 3 | 波动统计窗口 / 连续达标周期数（即"3 次探测周期内"） |
| `hot-switch.weight-rtt` | 0.6 | RTT 评分权重；jitter 权重 = 1 - 该值 |
| `hot-switch.switch-margin` | 0.15 | 评分差门槛，低于此值不切换 |
| `hot-switch.channels-per-entry` | 0 | 常驻多入口分布（高级；0 = 全部通道粘性当前入口） |

- **兼容性**：旧配置（`parallel`/`stage`/`categories`/`threshold-rtt-*`/`preheat`/`observe-ms` 等）继续被解析，行为与旧默认一致；只是新版本不再需要写它们。
- **默认开启**：多入口 + 探测可用时热切换自动生效，配置中无需 `enabled` 开关；显式写 `"enabled": false` 仍可关闭（兼容）。
### 7.4 全量配置参考（探测 + 热切换）

**可写字段（JSON）——全部可选，缺省即默认**

```json
"client": {
    "probe": {
        "enabled":     true,   // [可选] 默认 true；显式 false 关闭探测/延迟显示（单入口 legacy 兼容）
        "timeout-ms":  800,    // [可选] 默认 800；单次探测超时（ms）
        "ttl-seconds": 30      // [可选] 默认 30；探测周期与缓存有效期（s），同时是热切换评估周期
    },
    "hot-switch": {
        "enabled":              true,   // [兼容] 缺省自动启用；显式 false 关闭热切换
        "jitter-threshold-ms":  50,     // [可选] 默认 50；3 周期内波动门槛（ms）
        "jitter-window":        3,      // [可选] 默认 3；波动统计窗口 / 连续达标周期数
        "weight-rtt":           0.6,    // [可选] 默认 0.6；RTT 评分权重（jitter 权重 = 1 - 该值）
        "switch-margin":        0.15,   // [可选] 默认 0.15；评分差门槛，低于此值不切换
        "channels-per-entry":   0       // [可选] 默认 0；常驻多入口分布（高级；0 = 全部通道粘性当前入口）
    }
}
```

**内置固定参数（不进 JSON，恒生效）**

| 参数 | 固定值 | 说明 |
|---|---|---|
| `probe.stage` | 3 / 1 | 已建立隧道探测到 L3（WS），未建立只 L1（TCP），代码自动选择 |
| `probe.parallel` | true | 全入口并行探测 |
| `probe.categories` | 空 = 全部 | 按入口 URI 协议自动推导（wss / ws / ppp→tcp） |
| `hot-switch.jitter-metric` | max | 波动 = max(峰峰抖动, MAD) |
| `hot-switch.preheat` | true | 预热式渐进切换（mux 不重建、TCP 不断） |
| `hot-switch.observe-ms` | 2000 | 预热完成后的观察窗口 |
| `hot-switch.lock-seconds` | 60 | 激活后锁定，防止切回乒乓 |
| `hot-switch.penalty-seconds` | 60 | 预热失败入口黑名单时长 |
| `hot-switch.drain-timeout-seconds` | 120 | 退役载波排空超时 |

**兼容忽略字段（旧配置仍可写、不报错，但不再生效）**

- `probe.parallel` / `probe.stage` / `probe.categories`
- `hot-switch.threshold-rtt-factor` / `threshold-rtt-ms` / `min-stable-periods` / `preheat` / `observe-ms` / `lock-seconds` / `penalty-seconds` / `drain-timeout-seconds`

**完整示例（含上下文，可直接粘贴）**

```json
"client": {
    "guid":   "{0AC65AE6-AB23-48DA-B4DF-61B039622EEB}",
    "server": "wss://103.73.220.207:16299/tun",
    "servers": [
        "103.73.220.142:20022",
        "103.73.220.207:16299",
        "103.73.220.50:20443",
        "103.112.1.172:34687",
        "103.135.248.35:33561",
        "103.135.251.137:51845",
        "191.101.132.203:21110",
        "23.249.18.144:8523",
        "23.249.18.223:46800"
    ],
    "probe": {
        "enabled":     true,
        "timeout-ms":  800,
        "ttl-seconds": 30
    },
    "hot-switch": {
        "enabled":             true,
        "jitter-threshold-ms": 50,
        "jitter-window":       3,
        "weight-rtt":          0.6,
        "switch-margin":       0.15,
        "channels-per-entry":  0
    }
}
```
## 8. 实施改动点（代码）

1. **`ConnectivityProbe.h` / `ConnectivityProbe.cpp`**：`Result` 新增 `rtt_samples` 环形窗口、`samples_full`、`jitter_ms` 三字段；新增静态 `ComputeJitter`（波动 = max(峰峰抖动, MAD)）。`StoreProbeResult` 成功时 push 历史样本并重算波动，失败保留原窗口。
2. **`AppConfiguration.h/.cpp`**：`probe` 删除 `parallel/stage/categories`（内置固定：并行、深度自适应、类别由 URI 推导）；`hot_switch` 新增 `jitter_threshold_ms=50`、`jitter_window=3`、`weight_rtt=0.6`、`switch_margin=0.15`，`enabled` 默认 `true`（多入口自动启用，`servers` 为空或显式 `false` 关闭）；旧字段兼容读取但忽略；启动校验保持（`reorder.bytes >= 16MB` 等，不满足自动禁用）。
3. **`VEthernetExchanger.cpp`**：
   - 匿名命名空间新增 `MedianOfSortedValues` / `HotSwitchQualityScore` 辅助函数（原方案的 `HotSwitchEntryJitter/Score` 合并为此实现）；
   - 重写 `HotSwitchPickTarget`：B1 波动门控（窗口满且 `jitter >= jitter-threshold-ms`）→ B2 评分差（`score(current) - score(best) >= switch-margin`，无 RTT 兜底，见 §7）→ B3 连续 `jitter-window` 周期防抖；
   - `HotSwitchEntryProbe` 扩展返回 `jitter_ms` 与 `samples_full`；
   - `HotSwitchOldEntryRecovered` 判定改为"旧入口波动回落 < 门槛"；
   - `ProbeSelectServerEndPoint` 失败重选路径（sticky 失效时）改用加权评分选目标，sticky 语义不变；
   - `StoreProbeResult` 维护窗口（宽度取 `hot_switch.jitter_window`）。
4. **文档**：`README.md` 热切换段落更新为新字段；`PREHEAT_HOT_SWITCH_CN.md` 标注"触发判据已被本文替换"。

**实施状态（2026-08-12）**：以上 1–4 全部完成，代码待本机编译验证（Windows x64 Debug）。


## 9. 场景矩阵

| 场景 | 行为 |
|---|---|
| 当前入口稳定（波动 <50ms），即使 RTT 较差 | **不切换**（默认粘性，下载不换 IP） |
| 波动 ≥50ms 且评分显著劣化，连续 3 周期 | 预热切换（mux 不重建，TCP 不断） |
| 波动单次毛刺后恢复 | 不切换（窗口 + 连击防抖） |
| 当前入口断线 / 连续 2 次探测失败 | 无条件 failover（黑名单 + 重选评分最优） |
| 游戏 UDP | 走主链路不经过 mux 字节流；切换只影响接入段，服务端出口 IP 不变，不掉线 |
| 数据不足（窗口未满） | 不触发主动切换，只做 failover |

## 10. 验证方案

1. 配置 `enabled=true`、`jitter-threshold-ms=50`、探测 30s：稳定线路持续下载 10 分钟，`HotSwitchTick` 无任何 switch 日志；
2. 人为制造波动（限速/丢包模拟，使 3 周期 jitter ≥50ms）：观察 `Preheating → Ready → Activate` 完整链路，下载不中断；
3. 单次毛刺注入：确认 streak 不累计、不切换；
4. 断线注入：确认 failover 直切，黑名单生效；
5. 回归：单入口配置、`probe.enabled=false`、`hot-switch.enabled=false` 三类场景行为不变。