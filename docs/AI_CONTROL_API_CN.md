# OpenPPP2 AI 控制 API

## 目标

控制面由 C++ core 实现，GUI TUI、终端 TUI、一次性 CLI 和 AI 自动化只做客户端。
这样状态、诊断和变更语义只有一个来源。

API 的行为以原版 core 手动操作为准。RPC 只负责鉴权、参数解码和调用分派；具体
操作必须进入与原版控制台/TUI 相同的 C++ 方法，不能另行修改状态或复制连接、路由
和 DNS 逻辑。响应中的 `accepted` 表示请求已被 core 接受，不等于异步操作完成。

## 两种传输

同一组方法可通过两种方式调用：

1. 进程内 C ABI：宿主用 `ppp_core_start` 启动，用
   `ppp_core_command` 调用方法，用 `ppp_core_stop` 停止。
2. 本机 RPC：core 使用 `--rpc-listen=<port|ip:port>` 启动，可选
   `--rpc-token=<token>`。协议为“四字节大端长度 + UTF-8 JSON”，并且只允许
   loopback 地址。token 非空时必须匹配；token 为空时允许本机无鉴权连接。

监听地址可写成 `39100`（等价于 `127.0.0.1:39100`）、`0`（自动选择本机
端口）、`127.0.0.1:39100` 或 `[::1]:39100`。空值表示不启动 TCP API。

`ppp_core_api_version()` 可在 core 启动前查询命令 ABI 版本。

## AI 推荐流程

1. 调用 `describe_api`，不要硬编码未知版本的方法。
2. 调用 `get_health` 获取轻量状态。
3. 出现异常时调用 `run_diagnostics {"scope":"quick"}`。
4. 需要细节时调用 `get_snapshot`、`get_logs`。
5. 变更操作只调用清单中 `access=control/lifecycle` 的方法。
6. 停止 core 必须显式发送
   `shutdown {"confirm":"shutdown","restart":false}`。

## 方法

| 方法 | 类型 | 说明 |
|---|---|---|
| `describe_api` | 只读 | 返回版本、方法清单、参数说明和安全属性 |
| `get_health` | 只读 | 返回 `running/ready/healthy/status/phase` |
| `run_diagnostics` | 只读 | 返回结构化检查项和 pass/warn/fail 汇总 |
| `get_snapshot` | 只读 | 完整运行快照 |
| `get_logs` | 只读 | 增量结构化日志 |
| `get_outbounds` | 只读 | 出口状态 |
| `get_settings` | 只读 | 完整启动命令（机密脱敏）和可热更新设置 |
| `set_log_level` | 控制 | 动态调整日志等级 |
| `update_settings` | 控制 | 批量调整可热更新设置 |
| `configure_api` | 控制 | 由 core 自行启停、改绑和调整 API 鉴权 |
| `switch_server` | 控制 | 调用 `SwitchPrimaryOutbound(tag)` 请求切换主出口 |
| `switch_rank1` | 控制 | 调用 `SwitchPrimaryOutboundToRankedFirst(tag)` 请求活动出口切换至 Rank #1 |
| `shutdown` | 生命周期 | 停止或请求重启，必须带确认字段 |

启动不是运行中 core 的 RPC 方法：独立进程由服务管理器/CLI 启动，内嵌模式由
`ppp_core_start` 启动。这样避免“已停止的服务负责启动自己”的循环依赖。

## CLI 示例

```text
ppp-tui-cli api --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli health --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli diagnose quick --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli snapshot --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli settings --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli set '{"block_quic":true,"mux":2}' --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli call get_health '{}' --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli stop --rpc 127.0.0.1:39100 --token <token>
```

core 未设置 token 时，CLI 可省略 `--token`：

```text
ppp-tui-cli health --rpc 127.0.0.1:39100 --json
```

API 响应不返回 RPC token、服务器密钥或协议密钥。诊断方法只读取 core 内存状态，
不发送测试流量、不修改路由/DNS，也不切换服务器。

## 异步切换语义

原版服务器页选择非活动项时调用 `SwitchPrimaryOutbound(tag)`；选择当前活动项时调用
`SwitchPrimaryOutboundToRankedFirst(tag)`。RPC 的两个切换方法直接调用这两个入口。

`switch_server` 成功受理后会先创建目标主 exchanger，再等待约 2000 ms 的热切换
窗口。窗口结束且目标可提升时，core 才更新 `exchanger_`、`primary_outbound_`，应用
目标 client configuration，并清理旧连接状态。因此调用方必须在收到
`accepted: true` 后继续轮询 `get_outbounds`，以目标 `active=true` 和实时
`current_entry`/`state` 作为完成条件。

实时远端显示应读取 `get_snapshot.vpn_server`，其来源与原版界面相同，都是
`client->GetRemoteUri()`。`get_health.server`/`get_snapshot.server` 在连接建立后返回
活动 exchanger 的实际 `current_entry`；连接中或重连时回退到活动 client
configuration，而不是热切换前的启动配置。
