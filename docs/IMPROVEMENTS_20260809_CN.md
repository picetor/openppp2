# 本次改进与修复说明（2026-08-09）

> 范围：客户端（`ppp --mode=client`，桌面端 + 安卓）连通性检测、多入口优选与稳定性修复；安卓支持连接前选优与故障切换，5 秒后台刷新与 SERVERS 页展示仍为桌面端。
> 关联：[`CONNECTIVITY_TEST_CN.md`](./CONNECTIVITY_TEST_CN.md)、[`CONNECTIVITY_TEST_IMPLEMENTATION_CN.md`](./CONNECTIVITY_TEST_IMPLEMENTATION_CN.md)、
> [`MULTI_ENTRY_PPP_CN.md`](./MULTI_ENTRY_PPP_CN.md)（评估与 PPP 多入口拓展）。

---

## 一、崩溃修复（0xC0000025，死锁导致进程终止）

### 现象
- 启动后 1 分钟内崩溃，产生 dmp；`std::terminate ← std::system_error`（同线程重复加锁）。
- 崩溃线程持锁后再次进入 `GetExchanger → get_active()` 数据面路径。
- 另一独立现象：长时间运行后“上不去国内网络”的滞留——与本次崩溃同根同源。

### 根因
- `RefreshOutboundProbes` 旧实现把 `UriAuxiliary::Parse`（内部异步 DNS，会挂起栈式协程）
  放在 `SynchronizedObjectScope`（`syncobj_`）内；挂起时锁留在协程栈上。
- 同一 io 线程随后处理 TAP 包 → `GetExchanger` 对同一互斥锁二次加锁 → 死锁。
- 触发条件：配置含域名型服务器（如 `ppp://kt-nat3.aursys.cfd:10005`），启动后首个 5 秒探测即解析挂起。

### 修复（`ppp/app/client/VEthernetNetworkSwitcher.cpp/.h`）
1. 锁内只做**只读快照**（拷贝 `outbound_configurations_`），不再有任何可挂起调用；
2. `UriAuxiliary::Parse`（含 DNS）移到锁外循环，基于快照构建探测任务；
3. 探测结果写入用无挂起的短锁；
4. `next_outbound_probe_refresh_` / `outbound_probe_refreshing_` 改为 `std::atomic`，
   门闩用 `exchange(true)` 原子抢占，杜绝多 io 线程双开探测。

### 纪律（后续新增探测代码必须遵守）
- 任何可能挂起的操作（DNS、connect、TLS、WS 升级）不得在持有 `syncobj_` 时执行；
- 共享探测状态只允许“短锁 + 无挂起”读写。

---

## 二、连通性检测功能（默认开启，`client.probe.enabled` 开关）

### 能力清单
| 能力 | 说明 |
|------|------|
| 连接前探测选优 | 首次连接 / 重连前对 `[client.server] + client.servers` 全部入口探测，选 RTT 最低可达入口 |
| 分层探测 | L1 TCP connect；L3 TLS(SNI)+WebSocket 101（镜像真实连接不校验 CA）；不做 L4/L5（避免幽灵会话） |
| 缓存 / TTL / 惩罚 | `probe_results_` 按入口缓存（默认 TTL 30s）；真实连接失败入口拉黑（penalty） |
| 故障切换 | 重连时自动重选；`ExchangeToReconnectingState` 失效缓存 + 拉黑当前失败入口 |
| 每 5 秒全配置后台刷新 | `RefreshOutboundProbes` 覆盖所有菜单出口 + main，结果存 `outbound_probe_statuses_` |
| SERVERS 页展示 | `(12ms)` / `(unreachable)` / ` -> 生效入口` |
| 防自环 | Windows 先 pin 候选路由到物理网卡（/32、/128）；Linux 用 `ProtectorNetwork::Protect` |
| 配置 | `client.probe.enabled` 主开关（默认 `true`）；`false` 关闭连接前选优与 5 秒后台刷新，SERVERS 页显示 `(probe off)`；可调 `timeout-ms/ttl-seconds/parallel/stage/categories` |

### 入口选优行为
- 探测深度：ppp 入口天然 L1（TCP-only）；ws/wss 入口默认 L3；
- **滞回**：缓存入口本轮仍可达且 RTT ≤ 最优 ×1.3 时保持当前入口，防抖动横跳；
- **全部不可达**：回落到旧的主入口逻辑，不影响连接。
- **开关关闭（`probe.enabled:false`）**：不探测、不后台刷新，入口始终为主入口 `client.server`，
  SERVERS 页显示 `(probe off)`（2026-08-09 应需求新增，默认 `true`）。

---

## 三、多入口拓展（ws/wss 与 ppp 通用）

### 域名备份入口（G1，已实现）
- `client.servers` 元素支持 `IP:port`、`[IPv6]:port`、`host:port`（域名）；
- 域名在探测前解析（协程内、锁外 `GetAddressByHostName`），连接用解析 IP，`hostname` 保留域名；
- 缓存 / 拉黑键统一为 `域名:port`（与解析 IP 解耦，DNS 漂移不失效）；
- 解析失败：该入口记不可达并保留展示，不静默丢弃、不阻断其它入口；
- DNS 变化由每次探测轮（连接选优 + 5s 后台刷新）自动重新解析感知。

### 后台刷新 stage 降级（G7，已实现）
- 未建立连接的出口后台刷新只做 L1（TCP connect）；
- 已建立连接的出口保持配置 stage——避免每 5s 对空闲 wss 服务器做完整 TLS 握手。

### 配置示例
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
        "stage":       3,
        "categories":  ["tcp", "ws", "wss"]
    }
}
```

---

## 四、涉及文件

| 文件 | 改动 |
|------|------|
| `ppp/app/client/VEthernetNetworkSwitcher.cpp/.h` | 崩溃修复（锁范围）、5s 后台刷新、域名备份、stage 降级、原子门闩、`outbound_probe_statuses_` |
| `ppp/app/client/VEthernetExchanger.cpp/.h` | 连接前探测选优、域名备份解析、滞回、拉黑键统一、失败拉黑 + 缓存失效 |
| `ppp/app/client/ConnectivityProbe.h/.cpp` | 探测原语（TCP / WS / WSS / UDP send-only） |
| `ppp/configurations/AppConfiguration.h/.cpp` | `client.servers`、`client.probe`（`enabled` 开关，默认 `true`） |
| `main.cpp` | SERVERS 页 RTT / unreachable / 生效入口展示 |
| `docs/*` | 设计、实现说明、评估与 PPP 拓展、本说明 |

---

## 五、已知边界（本次未处理）

- **B2 代理路径探测**：配置 `client.server_proxy` 时跳过探测（避免“代理不可达 vs 隧道目标不可达”语义混淆）；用户当前配置未使用。
- **C1/C2 静态 UDP 选优**：`udp.static.servers` 未纳入入口选优；`ProbeUdp` 为 send-only（静态回显协议无法独立复刻），用户当前配置未使用。
- **L4/L5 探测**：不做完整链路层握手（避免幽灵会话），真实握手由重连路径完成。
- **每入口独立 Host/SNI**：`websocket.host/sni` 对所有入口共享（CDN 优选 IP 场景的既定语义）。

---

## 六、验证建议

1. 新构建启动，SERVERS 页 5 秒内出现各出口 `(RTTms)` / `(unreachable)`；
2. 域名备份入口（如 `kt-nat3.aursys.cfd:10005`）被探测并可被选为生效入口（` -> 域名:port`）；
3. 拔掉当前入口 → 重连自动切换（日志 + 页面一致）；
4. 连续运行（含域名入口的探测刷新）不再崩溃、数据面无滞留；
5. 未连接出口仅产生 TCP 探测（抓包确认无周期性 TLS 握手）。