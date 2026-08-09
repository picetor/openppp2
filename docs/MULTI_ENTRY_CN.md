# 客户端 ws/wss 多入口设计文档（故障切换 + tcping 优选）

> 状态：设计稿（未实现）；2026-08 用户改口后已落地核心（`client.servers` + 探测优选，见 [`CONNECTIVITY_TEST_CN.md`](./CONNECTIVITY_TEST_CN.md)）
> 目标版本：桌面端 `ppp --mode=client`
> 关联代码：`ppp/app/client/VEthernetExchanger.*`、`ppp/configurations/AppConfiguration.*`、`ppp/app/client/VEthernetNetworkSwitcher.cpp`

## 1. 背景与目标

### 1.1 现状（单入口）

客户端入口由 `client.server` 单个字符串决定：

```json
"client": {
    "server": "wss://104.16.1.1:443/tun",
    "websocket": { "host": "your-domain.com", "sni": "your-domain.com" }
}
```

代码链路（`VEthernetExchanger::GetRemoteEndPoint()`，`VEthernetExchanger.cpp:319`）：

1. 首次调用 `UriAuxiliary::Parse(client.server, ...)` 解析出 hostname/address/port/path/protocol。
2. 解析结果**缓存**到 `server_url_` 结构。
3. 之后（含断线重连）只要 `server_url_.port` 有效就直接返回缓存，**不再重新解析**。
4. `server_url_.port = 0` 仅在构造函数（`VEthernetExchanger.cpp:88`）重置。

结论：ws/wss 隧道一次只能连一个入口，重连永远重连同一个入口，无法在多个优选 IP 之间自动切换。

### 1.2 目标

- 在 `client.server` 之外支持一组**备用优选 IP:端口**，断线/握手失败后**自动故障切换到下一个**，建立成功后**锁定当前入口**，不再随机漂移。
- 完全向后兼容：不配置新字段时行为与现状逐字节一致。
- 仅支持 **IP:端口 数组**，复用 `client.server` 的协议（ppp/ws/wss）、路径（`/tun`）与 `websocket.host/sni`（同一源站的所有 CDN 优选 IP 共享同一 Host/SNI，这正是 WSS 优选 IP 加速的用法）。
- 可选集成 **tcping 主动优选**：连接前对全部入口做 TCP 层延迟/连通性探测，优先选择 RTT 最低且可达的入口；故障切换作为探测失效时的兜底。

## 2. 配置设计

### 2.1 新字段

```json
"client": {
    "server": "wss://104.16.1.1:443/tun",
    "servers": [
        "104.16.2.2:443",
        "104.16.3.3:8443",
        "[2400:cb00:2049:1::c629:d7a2]:443"
    ],
    "websocket": { "host": "your-domain.com", "sni": "your-domain.com" }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `client.server` | string | 现有字段，视为**首选入口**（第一个尝试） |
| `client.servers` | string[] | **新增**。备用入口，元素仅 `IP:端口` 或 `[IPv6]:端口` |
| `client.tcping` | object | **新增（可选）**。tcping 主动优选开关与参数，缺省时行为等同 `enabled: false`（纯故障切换） |

### 2.2 tcping 配置

```json
"client": {
    "server": "wss://104.16.1.1:443/tun",
    "servers": ["104.16.2.2:443", "104.16.3.3:8443"],
    "tcping": {
        "enabled": true,
        "timeout-ms": 800,
        "ttl-seconds": 30,
        "parallel": true
    }
}
```

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `enabled` | bool | `false` | 关闭则退化为纯故障切换（兼容现状） |
| `timeout-ms` | int | `800` | 单个入口探测超时；超时视为不可达 |
| `ttl-seconds` | int | `30` | 探测结果有效期；过期后重探 |
| `parallel` | bool | `true` | 并行探测所有入口；`false` 时串行（探测总耗时 = N × timeout-ms） |

### 2.3 语义

- 连接顺序：`server`（首选）→ `servers[0]` → `servers[1]` → … → 回到 `server`（循环）。
- 每个 `servers[i]` 解析时按 `client.server` 的 scheme/path 构造成完整 URL：
  - 例：`client.server = "wss://x:443/tun"`，`servers[0] = "1.2.3.4:8443"` → 实际连接 `wss://1.2.3.4:8443/tun`。
- `websocket.host/sni` 对所有入口生效（与现状一致，无需改动）。
- `client.servers` 缺省 / 为空 → 完全等同现状。

### 2.4 加载与校验（`AppConfiguration.cpp`）

复用 `udp.static.servers` 已有的解析辅助函数 `ReadJsonAllAddressStringToSet`（`AppConfiguration.cpp:1197`）：

- 对每个元素执行 `Ipep::ParseEndPoint`（支持 IPv4 / `[IPv6]`），端口超出 `(MinPort, MaxPort]` 或地址非法 → 丢弃并告警。
- 加载失败不视为配置错误：`client.servers` 为空时静默退回单入口。

## 3. 运行时设计

### 3.1 数据结构（`VEthernetExchanger.h`）

在 `server_url_` 单值缓存旁新增：

```cpp
ppp::vector<ppp::string>    server_failover_list_;    // 备用入口（IP:port），构造时从配置拷贝
bool                        server_failover_locked_;  // 建立成功后锁定当前入口
bool                        server_failover_used_;    // 是否曾从备用列表取过入口（用于日志/状态）
```

`server_url_` 保持现有语义：**始终是"当前生效入口"的缓存**。故障切换只需在合适的时机让它失效并让下一次 `GetRemoteEndPoint()` 走列表选择。

### 3.2 `GetRemoteEndPoint()` 选择逻辑（`VEthernetExchanger.cpp:319`）

```
if (server_url_.port 有效):
    # 现有快捷路径：除非正在重连切换，否则直接返回缓存
    if (server_failover_locked_):   return 缓存          # 建立成功后锁定
    else:                           走下方重新选择        # 重连时切换入口

# 重新选择：
index = 0 起：
    - 先试 client.server（首选）——若它是上一个生效入口且列表未空，则从列表下一个开始
    - 依次试 server_failover_list_[i]，用 Ipep 解析成 endpoint
    - 构造 URL：scheme://host:port/path（scheme/path 取 client.server 解析结果）
    - 成功 → 写入 server_url_ 缓存 → return
    - 全部失败 → 返回 false（保持现有失败路径：Loopback 进入重连等待）
```

实现注意：

- 只在 `!server_failover_locked_` 时进入切换分支；**进入 `Established` 才置锁**，避免握手成功后流量漂到别的入口。
- 单线程/协程模型：`GetRemoteEndPoint` 在 `Loopback` 协程内被调用，索引推进无需额外锁；但 `server_failover_locked_` 建议用原子量或与 `syncobj_` 保持一致。

### 3.3 切换时机（状态机挂钩）

现有状态机（`VEthernetExchanger`）：

```
Connecting → Established → (断线) → Reconnecting → Connecting → ...
```

| 时机 | 动作 | 代码位置 |
|------|------|----------|
| 握手成功 + `EchoLanToRemoteExchanger` 成功（进入 Established） | `server_failover_locked_ = true` | `Loopback()`，`VEthernetExchanger.cpp:698` 附近 |
| 进入 Reconnecting（断线/握手失败） | `server_failover_locked_ = false`；`server_url_.port = 0`（使缓存失效，下次重连重新选入口） | `ExchangeToReconnectingState()`，`VEthernetExchanger.cpp:1128` |

这样：

- 断线 → 重连 → 换下一个入口 → 成功 → 锁定 → 稳定运行。
- 全部入口都失败 → 循环重试，每次换一个；日志记录每个入口的失败原因（`OpenTransmission` 已有 connect 失败日志，补充入口标识即可）。
- 当前入口恢复后，列表轮一圈会回到它——无需额外"恢复探测"。

### 3.4 受影响代码清单

| 文件 | 改动 |
|------|------|
| `ppp/configurations/AppConfiguration.h` | `client` 结构新增 `ppp::vector<ppp::string> servers;` |
| `ppp/configurations/AppConfiguration.cpp` | 默认清空（~362 行）；`Load` 解析 `client.servers`（复用 `ReadJsonAllAddressStringToSet`） |
| `ppp/app/client/VEthernetExchanger.h` | 新增 3 个字段 + `bool FailoverSelectRemoteEndPoint(...)` |
| `ppp/app/client/VEthernetExchanger.cpp` | `GetRemoteEndPoint()` 加切换分支；`ExchangeToReconnectingState()` 失效缓存；`Loopback()` 握手成功处置锁 |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | 可选：`GetOutboundStatuses()`（1333 行）显示当前生效入口与各入口 RTT，便于 TUI/API 观察切换；`OnUpdate()`（7106 行）周期触发探测结果过期刷新 |
| `ppp/app/client/VEthernetExchanger.cpp`（tcping 部分） | 新增 `TcpProbeEntry()` 探测协程、`FailoverProbeAll()` 并行探测、探测缓存读写 |

### 3.5 无需改动的部分（已验证）

- **路由 pin**：`OpenTransmission()` 每次按 `remoteEP` 调 `EnsureWindowsIPv4ServerRoute/EnsureWindowsIPv6ServerRoute`（`VEthernetExchanger.cpp:431/442`）——按当前实际入口 pin 物理路由，多 IP 天然支持。
- **DNS/静态模式兜底**：`StaticEchoGetRemoteEndPoint()`（2350 行）在无聚合器时用 `server_url_.remoteEP` 兜底——`server_url_` 已随切换更新，行为一致。
- **代理（server-proxy）**：`IForwarding` 每次 `SetRemoteEndPoint(hostname, port)` 以当前入口为准，无需改动。
- **`--server-dir` 去重**（`main.cpp:3304`）：比较 `client.guid + client.server` 判定重复节点——同一服务器多入口不视为不同节点，合理。
- **geo 多出口**：每个 outbound JSON 独立支持 `client.servers`，天然兼容。

### 3.6 边界情况

| 场景 | 行为 |
|------|------|
| `servers` 含非法项 | 加载时过滤，日志告警 |
| 所有入口连不上 | 循环重试；`server_url_` 保持最后成功值（或首个），日志持续记录失败 |
| IPv6 入口 | `[2400::1]:443` 格式，`Ipep::ParseEndPoint` 已支持 |
| 热切换节点（--server-dir） | 新建 exchanger，failover 状态从零开始（锁定复位） |
| 配置不填 `client.servers` | 行为与现状完全一致 |
| 探测全部失败（所有入口不可达） | tcping 结果为空 → 顺序故障切换兜底 → 循环重试 |
| CDN 对频繁 SYN 限流 | 命中 `timeout-ms` → 该入口标记不可达并跳过，不惩罚（下一 TTL 重新探测） |
| 探测进行中断线重连 | 用最近一次缓存结果，无缓存则直接顺序故障切换，不阻塞等待探测 |

### 3.7 tcping 主动优选（可选增强）

#### 动机

纯故障切换是**被动**的：连不上才切。CDN 优选 IP 场景下多个入口往往同时可达，但 RTT 差异显著（不同边缘节点）。tcping 把选择变成**主动**：连接前先测，直接选最优。

#### 探测实现（复用现有协程基础设施）

- 探测 = TCP SYN 握手：对每个入口 `async_connect`（`asio.h:139`），测量从发起 connect 到完成的时间 = RTT。
- `async_connect` 无内建超时，用并发竞赛包装：spawn 探测协程 + spawn `async_sleep(y, timeout_ms)`（`asio.h:132`），先完成者胜出，另一路 cancel 并关闭 socket。
- 并行探测：每个入口一个 `YieldContext::Spawn` 子协程（与 `Loopback`/`OpenTransmission` 同款模式），全部完成后合并结果。
- 时间源：`ppp::threading::Executors::GetTickCount()`（毫秒）。

#### 探测结果缓存（`VEthernetExchanger.h`）

```cpp
struct FailoverProbeEntry {
    ppp::string entry;     // "IP:port"
    int64_t     rtt_ms;    // 探测 RTT；-1 = 不可达/超时
    uint64_t    probed_at; // GetTickCount 时间戳
};
ppp::vector<FailoverProbeEntry> failover_probes_;  // 与 failover list 对齐（含 client.server 首项）
bool                          tcping_enabled_;
int                           tcping_timeout_ms_;
uint64_t                      tcping_ttl_ms_;
bool                          tcping_parallel_;
```

#### 选择算法（tcping 优选 + 故障切换兜底）

```
选择入口（!locked_ && 需要重连）:
  # 阶段 1：探测
  if (探测结果过期 || 从未探测):
      并行/串行 tcping 全部入口（含 client.server），写入 failover_probes_
  # 阶段 2：优选
  reachable = probes 中 rtt_ms >= 0 的入口
  if (reachable 非空):
      按 rtt_ms 升序排序
      若当前生效入口 rtt <= 最优 rtt × 1.3（30% 滞回阈值）→ 保持当前入口
      否则 → 选 rtt 最低者
  # 阶段 3：兜底
  else:
      顺序故障切换（3.2 原逻辑）
```

- **滞回（hysteresis）**：避免网络抖动导致入口来回横跳。当前入口 RTT 在最优值 +30% 以内时保持不动，只有显著变差才切换。
- **锁定优先级不变**：进入 `Established` 后仍然锁定，tcping 只在重连时参与选择；正常运行时零探测开销。

#### 周期刷新

- switcher 的 `OnUpdate(now)`（约 500ms tick，`VEthernetNetworkSwitcher.cpp:7106`）检查探测结果是否过期，过期则 spawn 后台协程重探并更新缓存。
- 网络环境变化（CDN 节点故障、ISP 路由变更）后最多 `ttl-seconds` 内自动感知。

#### 关键注意点

| 点 | 说明 |
|----|------|
| **探测流量必须绕过 VPN** | TAP 接管后探测 socket 需走物理网卡（复用 `EnsureWindowsIPv4ServerRoute`/`Protect`），否则 SYN 进入隧道形成自身循环。启动首次探测发生在接管前，无此问题 |
| **不阻塞主连接** | 探测是后台任务，最坏耗时 = `timeout-ms`；重连路径有缓存则直接用，无缓存走顺序兜底 |
| **频率控制** | `ttl-seconds` 默认 30s，避免高频 SYN 被 CDN 限流/封禁 |
| **只测 TCP 层** | tcping 测量 SYN 握手 RTT，不含 TLS 握手延迟；两者强相关，作为优选依据足够，且探测成本远低于完整 TLS 握手 |
| **mux 兼容** | mux 多连接针对同一入口；tcping 选的是入口本身，天然兼容 |

## 4. 验证计划

1. **加载单测**：构造含合法/非法/IPv6 混合 `client.servers` 的配置，确认过滤与默认值。
2. **本地端到端**：
   - 配置 3 个入口，其中第 1、2 个故意失效（不监听端口）；
   - 启动客户端，观察日志：首选失败 → 切换到第 3 个 → 建立并锁定；
   - 杀掉第 3 个入口的服务，观察自动切回/切换。
3. **回归**：不带 `client.servers` 跑现有测试（connect-hang、peer-removed 等既有场景），确认无行为变化。
4. **WSS 优选 IP 专项**：三个不同 CDN 优选 IP + 同一 `websocket.host/sni`，确认 TLS/WS 握手全部走正确 Host/SNI。
5. **tcping 优选专项**：
   - 三个可达入口 RTT 差异明显（本地起 3 个端口，用流量整形/不同回环接口模拟），确认选中 RTT 最低者；
   - 当前入口 RTT 微升（+20%）→ 保持不切；RTT 大涨（+200%）→ 切换；
   - 探测期间断线 → 用缓存结果，不阻塞；
   - TTL 过期后 kill 最优入口 → 自动重探并切到次优。
6. **回归**：`client.tcping.enabled=false` 或省略 → 行为与 2、3 步一致（纯故障切换）。

## 5. 扩展方向（本期不做）

- **并发优选**：同时对多个入口发起完整连接（含 TLS/WS 握手），取首个完成握手者。比 tcping 更准（测的是完整握手），但需引入并发竞争与连接清理，复杂度高。tcping 是它的务实折中。
- **按入口健康度加权**：除 RTT 外记录成功率/丢包，选择时综合加权。tcping 已实现雏形（RTT + 可达性），扩展成本低。
- **每入口独立 Host/SNI**：`servers` 元素升级为完整 URL 或对象，`websocket.host/sni` 按入口覆盖（本期明确不做，保持共享）。
- **探测结果上报**：把各入口 RTT 表接入 TUI/管理面板，便于人工调整优选 IP 列表。

## 6. README 更新建议

在 `README.md` 新增一节"客户端多入口（故障切换 + tcping 优选）"：

- 配置示例（`client.servers` + `client.tcping`）；
- 行为描述：断线自动切换、成功锁定、循环重试；tcping 开启后连接前自动选 RTT 最低入口；
- 与 `udp.static.servers` 的区别：静态 UDP 是**轮询/聚合**，`client.servers` 是 **TCP/WS/WSS 故障切换**，`client.tcping` 是**连接前主动优选**。
