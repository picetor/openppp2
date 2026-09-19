# OpenPPP2 AI 调试指南

## 适用范围

本文面向通过 AI 或自动化程序调试 OpenPPP2 core 的场景。控制能力由 C++ core
统一实现，原版 `ppp`、图形 TUI、终端 TUI 和 `ppp-tui-cli` 使用同一套方法与
状态。界面不是另一套控制实现，只负责启动、连接和展示。

## 以原版 core 手动操作为准

API 是原版 core 手动操作的远程入口，不是第二套控制实现。增加或修改 API 时必须
遵守以下规则：

1. API 与原版控制台/TUI 调用同一个 C++ 方法，不得直接改写内部状态来模拟结果。
2. 参数校验、配置重载、连接建立、热切换延迟、路由/DNS 清理和失败处理均由 core
   现有实现负责，客户端不得复制这些逻辑。
3. `accepted` 只表示 core 接受了操作请求，不表示异步操作已经完成。
4. 操作完成后必须重新读取 core 运行态确认，不能把请求参数当成最终状态。
5. 原版界面显示的实时值优先来自 `VEthernetNetworkSwitcher`、活动 exchanger 和活动
   configuration；启动时的 `PppApplication::configuration_` 不能代表热切换后的连接。

服务器页的手动操作与 API 对应关系如下：

| 原版手动操作 | core C++ 入口 | API 方法 |
|---|---|---|
| 选择非活动服务器并确认 | `SwitchPrimaryOutbound(tag)` | `switch_server` |
| 在活动服务器上确认，切换到探测 Rank #1 | `SwitchPrimaryOutboundToRankedFirst(tag)` | `switch_rank1` |
| 查看服务器列表和活动项 | `GetOutboundStatuses()` | `get_outbounds` |
| 查看当前 VPN 服务器 | `GetRemoteUri()` / `GetExchanger()` | `get_snapshot` 的 `vpn_server` |

如果将来加入重连、路由、DNS、代理或其他控制命令，也必须先找到原版手动操作的
core 入口，再将该入口暴露给 RPC 和进程内 C ABI。

## 启用 API

### 原版 core

```text
ppp --mode=client --config=./config/HKBN.json --headless --rpc-listen=39100
```

监听值支持以下形式：

| 设置 | 含义 |
|---|---|
| 留空 | 关闭 TCP API；进程内 C ABI 不受影响 |
| `0` | 监听 `127.0.0.1`，由系统分配空闲端口 |
| `39100` | 等价于 `127.0.0.1:39100` |
| `127.0.0.1:39100` | 固定 IPv4 回环端口 |
| `[::1]:39100` | 固定 IPv6 回环端口 |

core 拒绝非回环地址，不能用 `0.0.0.0` 或局域网地址暴露控制接口。使用端口
`0` 时，应从 core 日志、原版 core 控制台或 TUI 的“运行端点”读取实际端口。

### 图形 TUI / 终端 TUI

在设置中的“AI / API 控制”填写“RPC 监听”。这是当前 TUI 启动的 core 对外
提供的地址。另一个“RPC 地址”是连接已经运行的 core，两者不要混用：

- 启动本地 core：设置“RPC 监听”，保持“RPC 地址”为空。
- 连接已有 core：设置“RPC 地址”，TUI 不再启动本地 core。
- 完全关闭 TCP API：两个地址都留空；TUI 仍通过进程内 C ABI 控制其 core。

Windows 首次创建或恢复虚拟网卡、路由和 DNS 可能较慢。TUI 会在后台等待
core 完成启动，最长 120 秒；这段时间不应把“正在启动”判断为失败。

## Token

`--rpc-token` 可以设置，也可以留空：

- 留空：只允许本机回环连接，无鉴权，适合单用户本机调试。
- 非空：客户端必须传入完全相同的 token，适合本机存在多个用户或自动化程序。

示例：

```text
ppp --headless --rpc-listen=39100 --rpc-token=my-local-token
ppp-tui-cli health --rpc 127.0.0.1:39100 --token my-local-token --json
```

不要把 token 写入日志、AI 提示、截图或提交到仓库。API 快照和诊断结果不会
返回 token 本文；只返回 `token_configured` 状态。

## AI 调试顺序

建议 AI 始终按以下顺序操作：

1. `describe_api`：读取当前版本支持的方法、参数和访问类型。
2. `get_health`：确认 core 是否运行、就绪以及当前阶段。
3. `run_diagnostics {"scope":"quick"}`：获取结构化检查结果。
4. `get_snapshot`：仅在需要网络、出口、流量和控制端点细节时读取完整快照。
5. `get_logs`：按游标增量读取日志，避免反复提交整个日志文件。
6. 先解释证据，再执行 `set_log_level`、`switch_server` 等变更操作。
7. 只有用户明确要求时才发送 `shutdown`。

AI 不应把方法名限制在 CLI 预置子命令中。先通过 `describe_api` 发现当前 core 的
全部方法，再用通用 `call <method> [params-json]` 调用；这样新增 core 命令无需同步
增加一个专用 CLI 子命令。

常用命令：

```text
ppp-tui-cli api --rpc 127.0.0.1:39100 --json
ppp-tui-cli health --rpc 127.0.0.1:39100 --json
ppp-tui-cli diagnose quick --rpc 127.0.0.1:39100 --json
ppp-tui-cli snapshot --rpc 127.0.0.1:39100 --json
ppp-tui-cli settings --rpc 127.0.0.1:39100 --json
ppp-tui-cli set '{"block_quic":true,"static_mode":false,"mux":2}' --rpc 127.0.0.1:39100 --json
ppp-tui-cli call run_diagnostics '{"scope":"full"}' --rpc 127.0.0.1:39100 --json
```

设置了 token 时，在每条命令中追加 `--token <token>`。

## 可控制和可读取的内容

| 方法 | 行为 | 是否修改状态 |
|---|---|---|
| `describe_api` | API 版本、方法、参数、安全属性 | 否 |
| `get_health` | 运行、就绪、健康、阶段 | 否 |
| `run_diagnostics` | 配置、网络、出口和控制面检查 | 否 |
| `get_snapshot` | 完整运行快照和 `control_api` 状态 | 否 |
| `get_logs` | 增量读取结构化日志 | 否 |
| `get_outbounds` | 出口列表、连接状态和当前选择 | 否 |
| `get_settings` | 全部启动参数（token/密码/私钥脱敏）与运行设置 | 否 |
| `set_log_level` | 动态修改日志等级 | 是 |
| `update_settings` | 修改 `log_level`、`block_quic`、`static_mode`、`mux`、`mux_acceleration` | 是 |
| `configure_api` | 启用/关闭 API，修改监听、token、客户端上限 | 是，需确认字段 |
| `switch_server` | 请求按原版手动流程切换指定服务器/出口 | 是，异步 |
| `switch_rank1` | 请求按原版手动流程切换至探测排名第一的入口 | 是，异步 |
| `shutdown` | 停止或请求重启 core | 是，需确认字段 |

诊断接口本身不发送测试流量、不修改路由或 DNS，也不会自动切换服务器。

### core 自行开关 API

`configure_api` 必须包含 `confirm: "configure_api"`。专用 CLI 会自动添加确认：

```text
# 改为固定端口并设置 token
ppp-tui-cli api-config '{"enabled":true,"listen":"39100","token":"new-token","max_clients":4}' --rpc 127.0.0.1:39000 --token old-token --json

# 关闭 TCP API；成功响应发出后当前连接会断开
ppp-tui-cli api-config '{"enabled":false}' --rpc 127.0.0.1:39100 --token new-token --json
```

通过进程内 C ABI 调用时，即使 TCP API 已关闭，宿主仍可使用 `configure_api` 重新
开启。修改监听地址时 core 会先打开新监听，再延迟关闭旧监听，响应中的 `listen`
是新连接应使用的实际端点。端口为 `0` 时尤其应读取该返回值。

启动参数会完整列在 `get_settings.command.arguments` 中，但 token、密码、secret 和
私钥值会脱敏。当前不能热更新的 `--...` 项由 `update_settings` 放入
`restart_required`；调用方必须交给宿主以新参数重启，不能把它误报为已生效。

## 故障定位

### 无法连接

1. 确认界面显示的“运行端点”，端口 `0` 不能直接作为客户端目标。
2. 查询端口是否监听：

   ```powershell
   Get-NetTCPConnection -State Listen | Where-Object LocalAddress -in '127.0.0.1','::1'
   ```

3. 检查 token 是否一致；core token 为空时客户端应省略 `--token`。
4. 检查 core 日志是否出现 `LocalRpcServer: listening on ...`。
5. 若出现 `failed to open local RPC server`，检查地址格式、端口占用，以及地址
   是否为回环地址。

### TUI 显示启动超时

先查看 core 日志。如果已出现 `LocalRpcServer: listening` 或网卡初始化仍在推进，
这是慢启动而不是 API 绑定失败。新版本等待 120 秒；若仍超时，应重点检查网卡
驱动、管理员权限、路由/DNS 清理和安全软件拦截。

### API 正常但隧道不通

API 监听成功只代表控制面正常，不代表数据面已连通。依次检查：

1. `get_health` 的 `ready`、`healthy` 和 `phase`。
2. `run_diagnostics` 的失败项。
3. `get_outbounds` 中当前出口的建立状态。
4. core 日志里的网卡、路由、DNS、握手和传输错误。

不要因为看到 API 已监听就忽略后续的 TAP/Wintun 或服务器路由错误。

### 验证服务器切换

`switch_server` 与原版服务器页使用同一个 `SwitchPrimaryOutbound(tag)`。该方法会重载
目标配置、创建新的主 exchanger，并进入约 2000 ms 的热切换窗口。因此响应中的
`accepted: true` 只说明请求已建立或已经处于同一待切换请求中。

推荐验证流程：

1. 调用 `switch_server {"tag":"server:bwgus"}`。
2. 若 `accepted` 为 `false`，读取 `get_logs` 查找标签不存在、配置加载失败、连接打开
   失败或拓扑不兼容等原因。
3. 若 `accepted` 为 `true`，等待至少一个运行态刷新周期，再调用 `get_outbounds`。
4. 仅当目标项的 `active` 为 `true`，并且 `current_entry`、`state` 符合预期时，才判定
   切换已经完成；当前网络状态枚举中 `1` 表示 established。
5. 用 `get_snapshot.vpn_server` 交叉确认实际远端。该字段和原版界面的 “VPN Server”
   都来自 `client->GetRemoteUri()`，并附带 `[static]`/`[dynamic]` 等显示标记。

`get_health.server` 和 `get_snapshot.server` 按运行阶段选择数据源：连接建立后使用
活动 exchanger 的 `current_entry`；连接中或重连时使用活动 client configuration 的
服务器值。`transport` 同样从活动 configuration 更新，不再回退到热切换前的启动
配置。若需要完整的原版界面显示文本，仍读取 `get_snapshot.vpn_server`。

## 原始 RPC 协议

需要自己实现客户端时，每个消息是四字节大端无符号长度，后接 UTF-8 JSON。
连接后先发送 `hello`；token 为空时仍进行 hello，但 token 字段可为空。随后使用
`describe_api` 返回的契约发起请求。优先使用 `ppp-tui-cli`，避免重复实现帧长度、
请求 ID、鉴权、超时和错误处理。

## 安全边界

- TCP API 永远只监听 loopback。
- AI 默认只执行只读诊断；状态变更应向用户说明目标与影响。
- `shutdown` 必须使用 `{"confirm":"shutdown","restart":false}`，重启时显式将
  `restart` 设为 `true`。
- 日志和快照可能含服务器地址、接口名和网络拓扑，分享前应脱敏。
- 不允许 AI 通过修改系统路由、DNS 或驱动来绕过 core 的控制接口。

接口契约与方法清单另见 [AI_CONTROL_API_CN.md](AI_CONTROL_API_CN.md)。
