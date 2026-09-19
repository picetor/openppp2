# OpenPPP2 AI 控制 API

## 目标

控制面由 C++ core 实现，GUI TUI、终端 TUI、一次性 CLI 和 AI 自动化只做客户端。
这样状态、诊断和变更语义只有一个来源。

## 两种传输

同一组方法可通过两种方式调用：

1. 进程内 C ABI：宿主用 `ppp_core_start` 启动，用
   `ppp_core_command` 调用方法，用 `ppp_core_stop` 停止。
2. 本机 RPC：core 使用 `--rpc-listen=127.0.0.1:<port>` 和
   `--rpc-token=<token>` 启动。协议为“四字节大端长度 + UTF-8 JSON”，只允许
   loopback 地址并要求 token 握手。

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
| `set_log_level` | 控制 | 动态调整日志等级 |
| `switch_server` | 控制 | 切换主出口 |
| `switch_rank1` | 控制 | 切换至探测排名第一入口 |
| `shutdown` | 生命周期 | 停止或请求重启，必须带确认字段 |

启动不是运行中 core 的 RPC 方法：独立进程由服务管理器/CLI 启动，内嵌模式由
`ppp_core_start` 启动。这样避免“已停止的服务负责启动自己”的循环依赖。

## CLI 示例

```text
ppp-tui-cli api --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli health --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli diagnose quick --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli snapshot --rpc 127.0.0.1:39100 --token <token> --json
ppp-tui-cli stop --rpc 127.0.0.1:39100 --token <token>
```

API 响应不返回 RPC token、服务器密钥或协议密钥。诊断方法只读取 core 内存状态，
不发送测试流量、不修改路由/DNS，也不切换服务器。
