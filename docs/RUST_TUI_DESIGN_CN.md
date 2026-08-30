# Rust TUI 前端设计文档

> 状态：**M1~M6 已实现并通过本机验证；TUI/CLI owned-core 已切换为同进程 C ABI**——
> 核心侧（headless + 本地 RPC + 日志事件流）MSBuild Debug x64 编译通过；
> Rust 侧 `tui/` cargo 工程 `cargo build`（debug/release）零警告、
> `cargo test` 10/10 通过（3 单元 + 7 契约黄金测试，含桌面快照 fixture）。
> 独立核心 attach 的管理员终端冒烟测试仍可单独执行；TUI owned-core 不再依赖该闭环。
> 适用范围：**桌面端**（Windows / Linux / macOS）。**不包含 Android**——Android 已有
> SagerNet 系 App UI（`android_ui/`），本设计不涉及其改造。
> 关联：`tests/contracts/runtime-snapshot/`（运行状态 JSON 契约）、`main.cpp`
> `PppApplication::PrintEnvironmentInformation / HandleConsoleInput / HandleServerSelection`、
> `ppp/app/client/VEthernetNetworkSwitcher`、`ppp/app/rpc/LocalRpcServer`（M1 新增）、
> `tui/`（M2~M6 新增，Rust 前端）。
>
> **实现偏差记录**：RPC 客户端未拆 `client.rs`，直接实现在 `tui/src/rpc/mod.rs`；
> 帮助页为覆盖层（`?` 打开）而非独立第 5 个 Tab；日志事件流经
> `ppp::diagnostics::g_log_sink` hook（`stdafx.h` LOG_TAG 宏统一走
> `LogPrintDesktop`，输出格式不变），OnTick 每秒批量推送 `event:log`，
> `get_logs {since_seq}` 支持追平。
>
> **架构更新**：下文早期的“自动拉起子进程/临时释放核心”描述是历史方案。当前
> `ppp-core` 静态链接进 `ppp-tui`/`ppp-tui-cli`，owned-core 只通过 C ABI 在同一进程
> 内创建、启动、控制和停止；RPC 仅用于主动连接独立运行的 `ppp.exe`。

---

## 1. 背景与动机

### 1.1 现状

openppp2 桌面端的用户界面是**内嵌在 `main.cpp` 里的字符仪表盘**（以下称"内置 TUI"）：

- 渲染：`PppApplication::PrintEnvironmentInformation()`（main.cpp:986）在每个 tick
  （`OnTick`，main.cpp:3339）用 ANSI 转义序列**全屏重绘**到 stdout；
- 输入：`PppApplication::HandleConsoleInput()`（main.cpp:901）——Windows 用
  `_kbhit/_getch`，Unix 用 termios raw + `select` 非阻塞读；
- 页面：4 个 Tab（`console_tab_count = 4`，main.cpp:910）：
  - Tab 0：环境信息总览（GUID、服务器、DNS、带宽、Geo 策略等）；
  - Tab 1：TUNNEL / 链路状态 / TUN 与物理网卡详情（`GetTapNetworkInterface` /
    `GetUnderlyingNetworkInterface`）；
  - Tab 2：ROUTES（分流模式、Geo 规则摘要、Split Rules、直连 DNS）；
  - Tab 3：SERVERS（节点列表，上下键选择、Enter 切换、翻页、探测 RTT 显示）；
- 命令面：`HandleServerSelection(delta, activate)`（main.cpp:846）——activate 时对
  `active` 节点调 `SwitchPrimaryOutboundToRankedFirst(tag)`，否则调
  `SwitchPrimaryOutbound(tag)`；Tab/左右键切页；
- 控制通道与状态源**全部是进程内直接函数调用**，无任何对外接口；
- **没有 headless 模式**：`--log-file` 只重定向 `LOG_*` 调试日志，仪表盘永远占用
  stdout；`--rt=no` 也不改变仪表盘行为。

### 1.2 动机

| 痛点 | 说明 |
|---|---|
| 渲染能力弱 | 手写 ANSI 全屏重绘，无真彩色、无图表、无鼠标、无区域滚动 |
| 布局固定 | 4 个固定 Tab，信息密度不可调，无法扩展（日志、规则、统计图） |
| 输入简陋 | 无组合键、无搜索过滤、无鼠标点击 |
| 无法分离 | 关掉终端 = 杀掉 VPN；无法"后台运行 + 随时 attach" |
| 无法复用 | 状态与命令都锁在进程内，脚本/监控/未来 GUI 都拿不到 |
| 维护成本 | TUI 逻辑与核心逻辑混在 4500 行 `main.cpp` 中 |

### 1.3 目标

1. 用 **Rust 编写独立 TUI 前端**（`ppp-tui`），替换内置仪表盘的交互体验；
2. C++ 核心产出 `ppp-core` 静态库和稳定 C ABI，供 `ppp-tui`/`ppp-tui-cli` 同进程调用；
3. 复用项目已有的 **runtime-snapshot JSON 契约**思路（`schema_version` 版本化），
   前后端可独立演进、互不崩溃；
4. 支持显式连接独立后台核心；owned-core 不通过 RPC 或子进程运行；
5. 桌面三平台一致体验（Windows Terminal / iTerm2 / 主流 Linux 终端）；
6. **交互与视觉显著超越内置 TUI**：
   - 实时流量折线图（rx/tx 双曲线，Sparkline/Canvas）；
   - 多标签页分页布局：仪表盘 / SERVERS / 日志（实时 LOG 流）/ 分流规则状态；
   - 鼠标支持：点击切换标签页、点击选择/切换服务器、滚轮滚动日志；
   - SERVERS 与日志页支持 `/` 增量搜索与过滤；
   - 彩色状态徽标（连通性 / 出口 / MUX 模式）、滚动日志带时间戳。

### 1.4 非目标

- ❌ 不用 Rust 重写核心（20 万行 C++ 不现实）；
- ❌ 不把 C++ 核心改写成 Rust；同进程边界使用稳定 C ABI；
- ❌ 不做 Android 版本（Android 已有原生 App）；
- ❌ 不替代管理面板（`go/` Web 管理端职责不变）；
- ❌ 不做 Web 界面（本设计只覆盖终端）。

---

## 2. 总体架构

```
┌──────────────────────────────────────┐
│ ppp-tui / ppp-tui-cli（Rust）         │
│ ratatui/egui + 状态模型 + C ABI 控制  │
└──────────────────┬───────────────────┘
                   │ 同进程 FFI
                   ▼
┌──────────────────────────────────────┐
│ ppp-core（C++ 静态库）                 │
│ TAP/Wintun、DNS、路由、MUX、日志       │
└──────────────────────────────────────┘

独立 `ppp.exe` 仍可作为后台/脚本宿主，TUI 仅在用户显式指定 RPC 时 attach。
```

### 2.1 两种运行形态

| 形态 | 命令 | 说明 |
|---|---|---|
| 同进程 owned-core | `ppp-tui --mode=client ...` | 静态核心在 TUI 进程内启动，不创建子进程、不创建 owned-core RPC |
| 核心后台 | `ppp --mode=client --headless --rpc-listen=127.0.0.1:39100 --rpc-token=<token>` | 无仪表盘、无键盘监听；仅提供 RPC |
| 前端 attach | `ppp-tui --rpc=127.0.0.1:39100 --token=<token>` | 仅连接用户已经运行的独立核心 |

> owned-core 的启动、控制和退出都走 C ABI；attach 模式的 RPC 只对外部核心生效。

### 2.2 为什么保留外部 attach 的 JSON over TCP

- 同进程 owned-core 没有进程边界；核心仍保持 C++ 实现，前端用 Rust 生态自由发挥；
- 本地 TCP（仅回环）比 Unix socket / 命名管道更简单且三平台一致，配合 token 鉴权；
- JSON 帧人类可读、可调试，与项目现有 JSON 文化一致（配置、管理面板、快照契约）；
- 性能足够支持外部后台核心的状态快照和日志事件流。

---

## 3. 核心侧设计（C++ 增量）

> 目标：**尽量不改动现有逻辑**，只新增一个"旁路"模块。全部改动预计 400~600 行，
> 集中在 `main.cpp` + 一个新文件（建议 `main_rpc.h/.cpp` 或放入 `ppp/app/`）。

### 3.1 新增 CLI 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--headless` | 无 | 客户端/服务端模式下**不渲染仪表盘、不监听键盘**（跳过 `HandleConsoleInput` / `PrintEnvironmentInformation` 两个调用点），stdout 只留日志 |
| `--rpc-listen=<ip:port>` | 空（禁用） | 监听本地 RPC；仅允许回环地址（`127.0.0.1` / `::1`），拒绝其它绑定 |
| `--rpc-token=<token>` | 空 | 客户端必须携带此 token 完成握手；空 = 拒绝所有连接（即 `--rpc-listen` 必须配合 token） |
| `--rpc-max-clients=<n>` | 1 | 最大并发 RPC 连接（默认 1，防多开互踩） |

解析位置：`PppApplication::PreparedArgumentEnvironment`（main.cpp:2285）之后、
`PppApplication::Main`（main.cpp:4243）之前，把值存入 `PppApplication` 新成员。

### 3.2 headless 模式的接入点

现有 tick 循环 `OnTick`（main.cpp:3339）中：

```cpp
HandleConsoleInput();        // ← headless 时跳过
PrintEnvironmentInformation(); // ← headless 时跳过
```

改动：`if (!headless_) { ... }` 包住即可。服务端模式同理（服务端本就不渲染输入，
仅跳过仪表盘输出）。**日志输出不变**（`LOG_*` 与仪表盘本来就是两条通道）。

### 3.3 RPC 服务挂载点

在 `Main()` 的 `NextTickAlwaysTimeout(false)` 之前启动：

- 用现有 `ppp::net::Socket` / `boost::asio` acceptor 在 io_context 上挂一个
  **本地 TCP 监听**（不新建线程，复用 `Executors` 的上下文）；
- 每连接：读 token 握手 → 进入"快照推送 + 命令接收"循环；
- `OnShutdownApplication`（main.cpp:4008）中关闭 acceptor 与所有 RPC 连接。

### 3.4 状态快照导出

**不新写状态收集逻辑**——把 `PrintEnvironmentInformation` 已经展示的数据
**结构化导出**。数据源全部是现有公开接口：

| 数据 | 来源（现有） |
|---|---|
| 运行阶段/角色/传输/MUX 模式 | `VEthernetNetworkSwitcher` 成员 + `GetExchanger()` |
| 链路状态/重连次数/当前入口 | `VEthernetExchanger::GetNetworkState / GetReconnectionCount / GetCurrentEntry / GetRankedFirstEntry` |
| 探测结果 | `GetProbeChecked / GetProbeReachable / GetProbeRtt` |
| 出口列表（SERVERS 页数据） | `VEthernetNetworkSwitcher::GetOutboundStatuses()`（返回 `OutboundStatus` 列表，含 tag/display_name/server/state/reconnects/active/server_menu/route_used/multiple_entries/probe_*/current_entry/ranked_first_entry） |
| 流量统计 | `PppApplication::GetTransmissionStatistics`（main.cpp:3306） |
| 网卡信息 | `GetTapNetworkInterface / GetUnderlyingNetworkInterface`（`NetworkInterface` 结构） |
| 分流信息 | `GetGeoRules()`（Direct DNS、Rule Count、Static Networks、Split Rules 摘要） |
| 环境信息 | `PppApplication` 现有成员（GUID、服务器、DNS、带宽…） |

快照字段命名**沿用 Android `get_runtime_snapshot` 契约风格**
（`tests/contracts/runtime-snapshot/*.json`：`phase/role/server/transport/mux_*/traffic/
last_error`），并扩展桌面专属字段（出口列表、网卡、geo 摘要）。

### 3.5 命令通道

| 命令 | 映射（现有方法） | 说明 |
|---|---|---|
| `switch_server {tag}` | `SwitchPrimaryOutbound(tag)` | 等价于现在 Enter 在非 active 节点 |
| `switch_rank1 {tag}` | `SwitchPrimaryOutboundToRankedFirst(tag)` | 等价于 Enter 在 active 节点（重建完整 MUX 代） |
| `server_move {delta}` | 复用 `HandleServerSelection(delta,false)` 的选区逻辑（RPC 侧维护选区） | 兼容旧式"上下键" |
| `reconnect` | 等价于 `SwitchPrimaryOutbound(当前 tag)` 语义（重新拉起连接） | 需在核心侧确认幂等性后实现 |
| `quit` / `restart` | `PppApplication::ShutdownApplication(bool)` | 带确认字段 |

所有命令在**核心 io_context 线程**上执行（`boost::asio::post`），保证与现有
代码路径无并发冲突。

---

## 4. IPC 协议设计

### 4.1 传输与帧

- TCP 回环，**长度前缀 JSON 帧**：`4 字节大端长度 + JSON UTF-8`；
- 每个方向一个简单的请求/响应/通知三消息模型（下详）；
- 连接建立后客户端先发 `hello`（携带 token + 客户端版本 + 期望 schema 版本），
  服务端回 `hello_ack` 或 `error`；鉴权失败立即断开。

### 4.2 消息模型

```text
客户端 → 服务端:  { "id": 1, "method": "get_snapshot", "params": {} }
服务端 → 客户端:  { "id": 1, "ok": true, "result": { ...快照... } }
服务端 → 客户端:  { "id": 1, "ok": false, "error": { "code": 404, "message": "unknown method" } }
服务端 → 客户端:  { "event": "log", "params": { "level": "info", "line": "..." } }   // 通知，无 id
```

### 4.3 方法表（v1）

| method | params | result | 频率 |
|---|---|---|---|
| `hello` | `{token, client_version, schema_version}` | `{server_version, schema_version, session_id}` | 连接时 |
| `get_snapshot` | `{}` | 完整状态快照（见 4.4） | 轮询 1s |
| `get_outbounds` | `{}` | 出口列表（= `GetOutboundStatuses` 序列化） | 随快照或独立 |
| `get_logs` | `{since_seq}` | `{logs: [...]}` + 订阅日志事件流 | 进入日志页时 |
| `switch_server` | `{tag}` | `{accepted: true}` | 用户操作 |
| `switch_rank1` | `{tag}` | `{accepted: true}` | 用户操作 |
| `shutdown` | `{restart: bool, confirm: "shutdown"}` | `{accepted: true}` | 用户操作 |

### 4.4 状态快照 schema（v1，节选）

```jsonc
{
  "schema_version": 1,
  "generation": 42,              // 单调递增，前端可检测变化
  "monotonic_ms": 42000,         // 核心启动后单调毫秒
  "phase": "connected",          // idle|connecting|established|reconnecting|failed
  "role": "client",              // client|server|proxy
  "server": "vpn.example.com:20000",
  "transport": "wss",            // ppp|ws|wss
  "bypass_mode": "geo",          // ip|geo|no
  "capabilities": ["mux.compat", "mux.flow", "mux.balance", "mux.stripe"],
  "requested_mux_mode": "flow",
  "effective_mux_mode": "flow",
  "mux_receiver_ordering": "flow_v2",
  "mux_active_links": 2,
  "mux_fallback_reason": "",
  "p2p_state": "relay",
  "effective_path": "relay",
  "traffic": { "rx_bytes": 10485760, "tx_bytes": 2097152 },
  "connected_monotonic_ms": 30000,
  "network": {
    "tun": { "name": "PPP", "ipv4": "10.0.0.2/30", "ipv6": "fd42::2/64", "dns": ["10.0.0.1"] },
    "nic": { "name": "Ethernet", "ipv4": "192.168.1.5/24", "ipv6": "2409:.../64" }
  },
  "geo": {
    "direct_dns": ["local", "223.5.5.5"],
    "rule_count": 128,
    "static_networks": 4,
    "split_rules": [ { "matcher": "geosite,openai", "outbound": "openai", "display": "OpenAI" } ]
  },
  "outbounds": [                 // = GetOutboundStatuses()
    {
      "tag": "main", "display_name": "main", "server": "ppp://...",
      "state": 1,                // 0=connecting 1=established 2=reconnecting -1=unknown
      "reconnects": 0, "active": true, "server_menu": true, "route_used": true,
      "multiple_entries": true, "probe_enabled": true,
      "probe_checked": true, "probe_reachable": true, "probe_rtt_ms": 42,
      "current_entry": "104.16.1.1:443", "ranked_first_entry": "104.16.2.2:443"
    }
  ],
  "last_error": { "code": 0, "severity": "", "retryable": false, "user_message_key": "", "diagnostic_detail": "" }
}
```

字段命名与 Android 契约对齐的部分（`phase/role/traffic/last_error/mux_*`）不得随意
改名；桌面扩展字段（`network/geo/outbounds`）是增量，Android 读取器忽略即可
（与 `future_optional_field: "ignored-by-v1-readers"` 的既有约定一致）。

### 4.5 版本与兼容

- `schema_version` 递增规则：字段**只增不删不改语义** → 次版本兼容；破坏性变更
  → 主版本，前后端协商取 min；
- 核心不认识的方法 → `error.code=404`，前端降级隐藏对应 UI；
- 前端不认识的新字段 → 忽略（serde `#[serde(default)]` + `deny_unknown_fields` 关闭）。

---

## 5. Rust TUI 前端设计

### 5.1 技术选型

| 组件 | 选择 | 理由 |
|---|---|---|
| TUI 框架 | **ratatui**（tui-rs 继任者，活跃维护） | 布局系统、Canvas/Sparkline 图表、主题、滚动区域 |
| 终端后端 | **crossterm** | Windows/macOS/Linux 统一；Windows Terminal、iTerm2、主流 Linux 终端全支持 |
| JSON | **serde + serde_json** | 与契约对齐，`#[serde(default)]` 天然容错 |
| 异步 | **不引入 tokio**（首选） | 单线程事件循环：`crossterm::event::poll` + 非阻塞 `TcpStream` 轮询即可；需要时再评估 |
| 错误 | `anyhow` | 快速迭代；协议层可用 `thiserror` |
| 测试 | `cargo test` + 契约 fixture | 复用 `tests/contracts/runtime-snapshot/*.json` 做解析黄金测试 |

### 5.2 工程结构

```
tui/                          # 仓库新增目录（独立 cargo workspace）
├── Cargo.toml
├── src/
│   ├── main.rs               # 入口：参数解析、连接核心或拉起核心
│   ├── app.rs                # 应用状态机（页面、选区、过滤态、错误横幅）
│   ├── rpc/
│   │   ├── mod.rs            # 帧编解码、连接管理、重连
│   │   ├── client.rs         # 请求/响应/通知分发
│   │   └── schema.rs         # 契约类型（serde 定义 + 黄金测试）
│   ├── ui/
│   │   ├── mod.rs            # 布局根（标签栏 + 状态栏 + 帮助行）
│   │   ├── overview.rs       # 仪表盘页（含链路/网卡/MUX 区块 + 流量折线图）
│   │   ├── routes.rs         # 分流页
│   │   ├── servers.rs        # 服务器页（表格 + 选中 + 确认弹窗 + 过滤）
│   │   ├── logs.rs           # 日志页（时间戳 + 级别着色 + 滚动 + 搜索）
│   │   ├── widgets.rs        # 流量 Sparkline、彩色状态徽标、K/V 块（颜色映射集中于此）
│   │   └── theme.rs          # 颜色/风格常量（预留 --theme 覆盖）
│   ├── input/
│   │   ├── mod.rs            # 键盘事件 → 动作
│   │   └── mouse.rs          # 鼠标事件 → 动作（命中测试：标签/行/按钮/滚轮）
│   └── core/
│       ├── in_process.rs     # C ABI 同进程核心句柄
│       ├── config.rs         # 参数透传（--mode/--config/--server-dir...）
│       └── traffic.rs        # 流量速率采样环形缓冲（120 点）+ 单位换算
└── tests/
    └── schema_contract.rs    # 契约黄金测试
```

### 5.3 页面规划（多标签页布局）

**顶部**：标签栏（`仪表盘 | SERVERS | 日志 | 分流`，当前页高亮、可用鼠标点击切换）。
**底部**：状态栏（连接阶段徽标、当前入口、rx/tx 速率、MUX 模式、核心版本、`?` 帮助提示）。

| 页 | 内容 | 组件与规格 |
|---|---|---|
| 仪表盘 Overview | 连接阶段徽标、服务器、传输协议、MUX 模式/链路数、**实时流量折线图**、当前入口 vs Rank#1、连接时长、最近错误 | K/V 网格、**双曲线 Sparkline**（rx 下行绿 / tx 上行蓝，见 §5.5.1）、彩色状态块 |
| SERVERS | 节点表：名称/地址/状态徽标/RTT/当前入口/生效入口；↑↓ 或鼠标选择、Enter 确认弹窗（区分"切换"与"切 Rank#1"）、`/` 增量过滤、鼠标点击行选中 + 双击直接切换 | Table（可点击行）+ Popup 确认框 + 底部过滤输入行（§5.5.3） |
| 日志 Logs | 实时滚动日志（RPC 事件流）、**每行时间戳**、级别过滤、`since_seq` 追平、`/` 搜索、鼠标滚轮滚动 | List（可滚动）+ 底部输入行；时间戳格式 `HH:MM:SS.mmm`（跨天显示 `MM-DD HH:MM:SS`）（§5.5.5） |
| 分流 Routes | 分流模式徽标（ip/geo/no）、Geo 文件路径、Direct DNS、Rule Count、Static Networks、Split Rules 表（规则 → 出口） | 标签 + Table；geo 模式数据来自 `GetGeoRules()` 摘要 |
| 帮助 Help | 键位表 + 鼠标操作说明 | 静态页 |

### 5.4 键位映射（初版）

| 键 | 动作 |
|---|---|
| `Tab` / `←` `→` | 切换页面（`1`~`4` 数字键直达对应页） |
| `↑` `↓` | 列表/表格移动（Servers/Logs） |
| `Enter` | 服务器页：弹出确认（当前 active → "切换到 Rank #1"，否则 → "切换节点"） |
| `/` | 进入过滤/搜索输入（Servers 过滤节点名/地址；Logs 过滤日志内容）；`Esc` 退出过滤 |
| `r` | 手动刷新快照 |
| `q` / `Ctrl+C` | 退出（默认分离核心；连按两次或 `--stop-on-exit` 时停止核心） |
| `?` | 帮助页 |
| 鼠标 | 见 §5.5.2 |

### 5.5 交互与视觉规格

#### 5.5.1 实时流量折线图

- **数据**：每轮快照取 `traffic.rx_bytes/tx_bytes` 与上一轮差值得瞬时速率
  （字节/秒），换算自动单位（B/s、KB/s、MB/s、GB/s，保留 1 位小数）；
- **采样**：环形缓冲保存最近 **120 个采样点**（1s 轮询 ≈ 2 分钟窗口）；核心
  重启/重连时清空并标注"重新采样"；
- **渲染**：ratatui `Sparkline`（双曲线，上下或叠加布局），x 轴时间刻度
  （每 30s 一格），y 轴当前峰值刻度；峰值自动缩放、不跳变（变化率 >50% 才重标）；
- **颜色**：rx 下行 = 绿色（`Green`），tx 上行 = 蓝色（`Blue`），图例常显；
- 无数据（速率 0 且窗口空）时显示占位文本 `waiting for traffic...`；
- 同时以数字形式常显：`↓ 1.2 MB/s  ↑ 340 KB/s` 与累计总量（复用快照
  `traffic` 字段）。

#### 5.5.2 鼠标支持（crossterm `EnableMouseCapture`）

| 鼠标事件 | 动作 |
|---|---|
| 点击顶部标签栏 | 切换页面 |
| SERVERS 页点击行 | 选中该节点（高亮） |
| SERVERS 页**双击**行 | 直接发起切换（等价 Enter 确认后确认） |
| SERVERS 页点击列头 | 按该列排序（名称/状态/RTT 循环切换） |
| 日志页滚轮上/下 | 滚动日志（自动跟随模式下滑动则退出跟随，底部出现"回到底部"按钮，点击恢复） |
| 点击状态栏"回到底部" | 恢复日志自动跟随 |
| 点击 Popup 按钮（确认/取消） | 等价 Enter/Esc |
| 悬停（可选） | 表格行高亮提示 |

> 鼠标支持**可开关**（`--no-mouse`），便于纯键盘用户与远程终端（tmux 会话内
> 鼠标透传冲突时关闭）。

#### 5.5.3 SERVERS 搜索/过滤

- `/` 进入过滤输入行（底部），**增量过滤**：匹配节点 `display_name`、`tag`、
  `server`（含 IP/域名）、`current_entry`、`ranked_first_entry`（不区分大小写）；
- 过滤后 ↑↓ 只在可见行内移动；Enter 确认针对当前可见选中行；
- 过滤非空时表头显示 `filter: <关键词> (n/m)`；`Esc` 清空退出，`Enter`（过滤态）
  确认过滤并留在过滤态；
- 过滤不改变核心状态，纯前端行为。

#### 5.5.4 彩色状态徽标

统一徽标组件（圆点/色块 + 文本），映射表：

| 类别 | 状态 | 颜色 |
|---|---|---|
| 连接阶段 | `connected` | 绿色 `Green` |
| | `connecting` / `reconnecting` | 黄色 `Yellow`（闪烁可选） |
| | `idle` / `failed` | 灰色 `DarkGray` / 红色 `Red` |
| 出口（每节点） | `established` | 绿 |
| | `connecting` | 黄 |
| | `reconnecting` | 橙 `LightYellow`/`Yellow` 闪烁 |
| | 不可达（探测失败） | 红 |
| | 未探测（probe off） | 灰 + `(probe off)` 文本 |
| MUX 模式 | `compat` | 蓝 `Blue` |
| | `flow` | 青 `Cyan` |
| | `balance` | 品红 `Magenta` |
| | `stripe` | 紫 `LightMagenta` |
| | 未协商（disabled） | 灰 |
| 分流模式 | `ip` / `geo` / `no` | 绿 / 青 / 灰 |
| 当前入口 | 生效入口 ≠ 配置入口（热切换生效中） | 高亮 `LightCyan` + `->` 前缀 |

> 颜色映射集中在 `ui/widgets.rs` 单一模块，后续支持主题覆盖（`--theme` 预留，
> 不在 v1 范围）。

#### 5.5.5 日志时间戳与滚动

- 每行前缀时间戳：`HH:MM:SS.mmm`（毫秒级），跨天自动切换 `MM-DD HH:MM:SS.mmm`；
  时间戳取**核心侧** `monotonic_ms` 换算（与快照同源，避免两端时钟偏差）；
- 日志行按级别着色（`ERROR` 红 / `WARN` 黄 / `INFO` 默认 / `DEBUG` 灰），
  仅 Debug 构建有 DEBUG 级日志，正常显示；
- 自动跟随模式（默认）：新日志到达滚动到底部；用户上滚即暂停跟随；
- 环形缓冲 5000 行，内存固定；`/` 搜索高亮命中行。

### 5.6 渲染与刷新模型

- **快照轮询**：默认 1s（可 `--refresh-ms` 调整），`get_snapshot` → 更新状态模型 →
  更新流量采样环形缓冲（§5.5.1）→ 重绘；仅在终端尺寸/状态变化时全量重绘，其余增量；
- **输入轮询**：`crossterm::event::poll` 统一处理键盘与鼠标事件（`EnableMouseCapture`
  启用时），鼠标事件派发见 §5.5.2；
- **日志事件流**：进入 Logs 页时 `get_logs` 追平历史 + 订阅 `event:log` 通知，
  环形缓冲（默认 5000 行），带核心侧时间戳（§5.5.5）；
- **重连**：核心重启后（连接断开）按指数退避重连 RPC；核心进程被 TUI 拉起时，
  检测子进程退出并提示。
- 终端尺寸变化（`crossterm::event::Event::Resize`）即时重排。

---

## 6. 生命周期与集成

| 场景 | 行为 |
|---|---|
| `ppp-tui` 单独运行 + 核心已在跑 | attach；token 不符或端口不通 → 明确报错 |
| `ppp-tui --mode=client ...` 同进程启动 | 直接调用 `ppp_core_start`，不 spawn 子进程、不等待 `RPC_LISTEN` |
| TUI 退出 | owned-core 通过 `ppp_core_stop` 同步清理；attach 模式仅断开 RPC |
| 核心崩溃 | TUI 显示错误横幅；owned-core 不启动外部替代进程 |
| `Ctrl+C` 于核心终端 | 核心原有行为不变（headless 下直接走 shutdown 路径） |

**随机端口协商**：`--rpc-listen=127.0.0.1:0` 时核心绑定随机端口，headless 模式下
在 stdout 打印唯一一行 `RPC_LISTEN=127.0.0.1:<port>`（该行是 TUI 与核心的私有
约定，普通日志模式不输出）。

---

## 7. 安全考虑

| 风险 | 对策 |
|---|---|
| 任意本地进程控制 VPN | `--rpc-listen` 强制回环；`--rpc-token` 必填（默认拒绝）；token 仅存内存 |
| 中间人/窃听 | 本地回环 + token；如需更强可后续升级 Unix socket 权限或 TLS（非 v1 目标） |
| 命令误操作 | `shutdown` 需 `confirm` 字段；`switch_*` 响应携带目标 tag 回显 |
| 快照敏感信息（GUID/密钥） | 快照**不包含**密钥类字段；GUID 按现有 SERVERS 页显示惯例输出 |
| 多 TUI 同时控制 | `--rpc-max-clients=1` 默认；写操作（switch/shutdown）全局互斥（核心侧单线程 io_context 天然串行） |

---

## 8. 平台与终端要求

| 平台 | 支持 | 说明 |
|---|---|---|
| Windows | ✅ | 建议 Windows Terminal / VS Code 终端；旧 conhost 可用但真彩色/鼠标降级（crossterm 自动处理） |
| Linux | ✅ | 主流终端均可；需要 tty（`ppp-tui` 无 tty 时拒绝启动并提示） |
| macOS | ✅ | Terminal.app 有限支持，建议 iTerm2 / VS Code 终端 |
| Android | ❌ | 明确排除（见 §1.4） |

---

## 9. 工作量与里程碑

| 里程碑 | 内容 | 预估 |
|---|---|---|
| **M1 核心侧** | `--headless` + `--rpc-listen/--rpc-token` + 快照导出 + `get_snapshot`/`get_outbounds`/`switch_server`/`switch_rank1` 方法 | 1~2 天 |
| **M2 TUI 骨架** | cargo 工程 + RPC 客户端 + 总览页 + 状态栏 + 连接/重连 | 1~2 天 |
| **M3 交互闭环** | Servers 页表格 + 确认弹窗 + 切换命令 + 快照轮询 | 1 天 |
| **M4 增强页** | 日志事件流（时间戳+级别着色+滚动+搜索）、Routes 页、流量 Sparkline（采样环形缓冲 + 自动缩放）、彩色徽标组件库 | 2~3 天 |
| **M5 交互打磨** | 鼠标（标签切换/行点击/双击/滚轮）、Servers 过滤、帮助页、`--stop-on-exit`、错误横幅、`--no-mouse` 开关 | 1~2 天 |
| **M6 集成** | CI（cargo build/test + 产物打包进 Release）、契约黄金测试 | 1 天 |

总计约 **7~11 个工作日**（单人），核心侧占比小、风险集中在 Rust 侧 UI 打磨（图表、
鼠标、过滤）。

## 10. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 核心与前端版本漂移 | `schema_version` 协商 + 字段只增不删 + 契约黄金测试 |
| 状态快照与 Android 契约分叉 | 共用字段名与语义（§4.4），必要时核心侧抽公共序列化函数 |
| 命令语义（尤其 `reconnect`）在核心侧无现成方法 | M1 只实现有现成映射的命令；`reconnect` 评估 `SwitchPrimaryOutbound` 幂等性后决定是否提供 |
| 旧终端兼容 | crossterm 自动降级；检测无 tty 直接退出并给出中文提示 |
| Rust 工具链加入构建/CI | 独立 cargo workspace，CI 新增 job；Windows 用 `cargo build --release` 与 MSBuild 并行，不互相阻塞 |
| 日志流高频放大 | 核心侧对 `event:log` 做节流（默认 ≥10ms 聚合）；TUI 环形缓冲 |

---

## 11. 附录：现有可复用接口清单

> 供 M1 实现时对照，全部为现有公开方法（无需改动核心逻辑）：

- `VEthernetNetworkSwitcher::GetOutboundStatuses() -> OutboundStatusList`
- `VEthernetNetworkSwitcher::SwitchPrimaryOutbound(const ppp::string& tag)`
- `VEthernetNetworkSwitcher::SwitchPrimaryOutboundToRankedFirst(const ppp::string& tag)`
- `VEthernetNetworkSwitcher::GetGeoRules()` → `UsesLocalDirectDns/GetDirectDnsServers/GetRuleCount/GetStaticNetworks/GetRuleSummaries`
- `VEthernetNetworkSwitcher::GetTapNetworkInterface() / GetUnderlyingNetworkInterface()`
- `VEthernetExchanger::GetNetworkState/GetMuxNetworkState/GetReconnectionCount/GetProbeChecked/GetProbeReachable/GetProbeRtt/GetCurrentEntry/GetRankedFirstEntry`
- `PppApplication::GetTransmissionStatistics(incoming, outgoing, snapshot)`
- `PppApplication::ShutdownApplication(bool restart)`
- `PppApplication::HandleServerSelection(int delta, bool activate)`（选区逻辑参考）
- Android 契约参考：`tests/contracts/runtime-snapshot/{idle,connected,failed,reconnecting,unsupported-schema}.json`

---

## 12. 跨平台界面落地方案：Windows/macOS GUI 与 Linux TUI

### 12.1 目标与边界

Rust 前端最终提供三种平台产物：

```text
Windows
└── ppp-tui.exe                 Windows GUI

macOS
└── PPP PRIVATE NETWORK 2.app   macOS GUI

Linux
└── ppp-tui-cli                 Linux 终端 TUI
```

Windows 和 macOS 使用同一套 `egui/eframe` 窗口界面，Linux 使用
`ratatui/crossterm` 终端界面。三者共用 RPC、状态模型、核心启动器、服务器目录、
分流设置、命令生成和配置保存逻辑。

本方案不重写 C++ 核心，也不把 Rust 界面逻辑塞回 C++ TUI。C++ TUI 保持原有内容和
命令行行为；Rust 前端只通过本地 RPC 控制 headless 核心。

### 12.2 共享层与渲染层

推荐将 Rust 工程按职责拆分：

```text
tui/src/
├── app/
│   ├── state.rs              # 当前页面、连接状态、焦点和错误状态
│   ├── settings.rs           # ppp-tui.json 读写
│   ├── command.rs            # 启动命令解析与生成
│   ├── server_catalog.rs     # config 目录和服务器 JSON
│   ├── routes.rs             # IP/GEO/关闭分流设置
│   └── controller.rs         # 启动、停止、重启、切换服务器
├── rpc/
│   ├── client.rs
│   └── schema.rs
├── core/
│   ├── in_process.rs         # 静态核心 C ABI 封装
│   └── traffic.rs            # 速率和流量历史
└── ui/
    ├── gui/                  # Windows/macOS egui
    └── terminal/              # Linux ratatui
```

渲染层只做两件事：

1. 把共享状态绘制成当前平台的界面。
2. 把鼠标或键盘事件转换成共享控制器动作。

所有页面使用相同的数据字段：

```text
Overview / Network / Servers / Routes / Settings
```

Rust GUI 不再设置独立 Logs 页面。核心日志和 TUI 日志只在启动设置中配置路径，错误
通过顶部状态区或页面错误栏显示。

### 12.3 Windows 与 macOS GUI

#### 12.3.1 视觉和布局

macOS GUI 直接复用 Windows GUI 的布局、颜色、字号、卡片和交互规则：

```text
顶部标题栏和连接状态
左侧控制面板
中央页面内容
页面内滚动条
底部状态和日志路径提示
```

页面保持一致：

```text
总览
网络
服务器
分流
启动设置
```

Windows 和 macOS 的窗口界面目标是接近完全一致。平台差异只体现在窗口系统、核心
文件、TUN 接口、权限提示和打包方式，不改变用户操作流程。

#### 12.3.2 信息页与混合页交互

信息文字、状态值、统计卡片和流量图不进入 Tab 焦点。Tab 只经过：

```text
按钮
输入框
复选框
模式选择框
服务器卡片或服务器操作按钮
```

纯信息区域支持：

```text
↑/↓              逐行浏览
PageUp/PageDown   分页浏览
Home/End          跳到开头或结尾
鼠标滚轮          页面滚动
```

混合页面遵循以下规则：

- 当前控件获得焦点后自动滚动到可见区域。
- 服务器卡片的整个卡片区域可以鼠标点击选择。
- 服务器列表使用 `↑/↓` 选择，`Enter/Space` 确认。
- 下拉框使用 `↑/↓` 选择，`Enter` 确认，`Esc` 取消。
- 多行命令编辑框获得焦点后，方向键和 Home/End 优先用于文本编辑。
- 切换页面时释放旧页面焦点，避免信息页被导航按钮占用。

#### 12.3.3 Windows 平台适配

```text
核心：静态链接进 ppp-tui.exe
TUN：Wintun/TAP
权限：Client + TUN 通常需要 UAC
打包：ppp-tui.exe + Driver/运行时资源目录
图标：Windows ICO 和 UAC 标识
```

Proxy 模式默认普通权限启动。Client + TUN 模式通过顶部 UAC 按钮或启动按钮触发
管理员重启；Server 模式根据实际网卡和监听配置判断是否需要提权。

#### 12.3.4 macOS 平台适配

```text
核心：静态链接进 ppp-tui
TUN：utun
路径：统一使用 /
核心释放：不释放独立核心文件
打包：PPP PRIVATE NETWORK 2.app 及其驱动/运行时资源
```

推荐的应用包结构：

```text
PPP PRIVATE NETWORK 2.app
└── Contents
    ├── MacOS/ppp-tui
    ├── Resources/ppp-core
    ├── Resources/fonts/
    └── Info.plist
```

开发阶段可以按 root 启动进行验证。正式发布时再评估特权辅助程序或 Network
Extension，避免要求用户长期从终端使用 `sudo` 启动 GUI。

### 12.4 Linux 终端 TUI

#### 12.4.1 终端绘制原则

Linux 版不使用 `eframe` 窗口，而是使用真正的终端 TUI。绘制采用 ASCII 边框，避免
不同终端字体对 Unicode 框线宽度的影响：

```text
+--------------------------------------------------------------------+
| PPP PRIVATE NETWORK 2       connected       Main: rfcJP            |
+--------------------------------------------------------------------+
| Overview | Network | Servers | Routes | Settings                   |
+--------------------------------------------------------------------+
| Connection Control                                                 |
| [x] TUN VPN       [ ] System Proxy       [Stop Core]               |
+--------------------------------------------------------------------+
| State       connected       Server       23.249.25.106:20000       |
| Main        rfcJP           Duration     00:02:45                  |
| RX          505 B/s         TX           0 B/s                     |
+--------------------------------------------------------------------+
| Connection Information                                             |
| Server      [23.249.25.106]:20000/ppp+tcp [dynamic]                |
| GUID        {...}                                                  |
| Transport   ppp                                                    |
| Bypass      geo                                                    |
+--------------------------------------------------------------------+
| rx                                                                 |
| ▁▁▂▃▁▁▅▆▃▁▁▁▂▁▁▁▁▃▅                                             |
+--------------------------------------------------------------------+
```

Linux TUI 与 GUI 的字段、颜色语义、页面顺序和操作结果保持一致，但不追求像素级
一致。终端窗口过小时显示布局不足提示，不截断核心操作按钮。

#### 12.4.2 Linux 键盘操作

```text
F1-F5             切换页面
Tab/Shift+Tab     切换可操作控件
↑/↓               浏览信息或选择列表项
Enter/Space       执行或确认
PageUp/PageDown   分页滚动
Home/End          跳到开头或结尾
Esc               关闭弹窗或取消编辑
Ctrl+S            保存设置
Ctrl+R            应用设置并重启核心
Q                 退出前端
```

信息页的标签和状态文本不进入 Tab。服务器行、按钮、复选框、输入框和模式选择框
进入焦点顺序；焦点移动时自动调整列表滚动位置。

鼠标捕获默认关闭，保证终端可以使用系统文本选择和复制。后续可增加可选的
`--mouse`，仅在本地终端中启用点击标签页、点击服务器行和滚轮滚动。

#### 12.4.3 Linux 运行方式

Linux 默认按 root 环境设计，不在 TUI 内增加复杂的权限提升流程：

```bash
sudo ./ppp-tui-cli --mode=client --config=./config/HKBN.json
```

Proxy 模式仍然可以不使用 TUN；如果关闭 TUN，核心只提供连接和代理控制能力，不
修改本机路由和 DNS。

### 12.5 三个平台的同进程核心

`build.rs` 按 Rust target 链接对应的 `ppp-core` 静态库：

```text
Windows：x64/Release/ppp-core.lib 或 x64/Debug/ppp-core.lib
Linux：  bin/libppp-core.a
macOS：  bin/libppp-core.a
```

构建时将核心静态链接进 Rust 前端。运行时由同一进程通过 C ABI 创建、启动和停止核心，
不释放临时文件，也不通过本机回环 RPC 控制 owned-core。独立运行的 `ppp.exe` 仍可作为
脚本/服务端宿主，并由用户显式 attach。

每个平台分别构建自己的可执行文件；“单文件”指每个平台的发布产物包含对应平台
核心，不意味着一个 Windows 文件同时运行 Linux 或 macOS 核心。

### 12.6 平台评估

| 平台 | 界面 | 核心基础 | 主要适配工作 |
|---|---|---|---|
| Windows | GUI | `_WIN32`、Wintun/TAP、WMI 已有基础 | UAC 重启、Windows 图标、单文件发布 |
| macOS | GUI | Darwin/utun 已有基础 | `.app` 打包、可执行权限、权限和签名 |
| Linux | ASCII TUI | Linux/TUN、CMake 已有基础 | ratatui、终端事件、核心打包 |

预期一致性：

```text
Windows GUI ↔ macOS GUI：视觉和布局约 95% 以上一致
Windows/macOS GUI ↔ Linux TUI：字段、功能、状态和操作逻辑一致
```

### 12.7 分阶段实施

#### Phase A：共享业务层

- 从当前 GUI 中抽出启动、停止、重启和 attach 逻辑。
- 抽出命令参数生成与配置文件保存。
- 抽出服务器目录、分流文件和状态快照读取。
- 保持现有 Windows GUI 视觉不变。

#### Phase B：macOS GUI

- 让 `build.rs` 链接 macOS `ppp-core` 静态库。
- 验证同进程核心的权限和驱动资源处理。
- 使用现有 `eframe` 页面验证总览、网络、服务器、分流和启动设置。
- 增加 `.app` 打包脚本，签名和公证放到发布阶段。

#### Phase C：Linux TUI

- 增加 `ratatui/crossterm` 终端二进制。
- 复用共享 RPC 和控制器。
- 实现 ASCII 页面、焦点、列表选择、弹窗和滚动。
- 默认不捕获鼠标，提供可选鼠标模式。

#### Phase D：跨平台验证

```text
Windows GUI：Proxy / Client + TUN / Server
macOS GUI：  Proxy / Client + utun
Linux TUI：  Proxy / Client + TUN / Server
```

每个平台都验证：静态核心启动、配置目录读取、服务器选择、停止核心、DNS/路由
恢复、日志路径和窗口/终端退出恢复。
