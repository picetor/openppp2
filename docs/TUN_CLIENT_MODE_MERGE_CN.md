# Client 模式与 TUN 开关合并设计

> 状态：已实现并通过本地 ppp-web 验证
> 日期：2026-10-24

## 目标

不再保留独立的 `proxy` 运行角色。客户端只有一种角色：`client`。是否创建 TUN、是否启动本地 HTTP/SOCKS 监听、是否设置系统代理，是三个相互独立的开关。

允许以下组合：

| 组合 | TUN | 本地 HTTP/SOCKS | 系统代理 | 说明 |
|---|---|---|---|---|
| 纯 TUN | 开 | 可关 | 可关 | 传统客户端 |
| TUN + 本地代理 | 开 | 开 | 可关 | 同时提供局域网代理 |
| 纯本地代理 | 关 | 开 | 可关 | 旧 “proxy 模式” |
| 只有系统代理 | 关 | 至少 HTTP 开 | 开 | 系统代理指向本地 HTTP 监听 |
| 无 TUN、无代理 | 关 | 关 | 关 | 仅建立隧道/控制面，供 RPC、诊断或外部转发 |

TUN 关闭时不需要管理员权限，不创建内核适配器，不修改宿主路由和 DNS。

## 入口与配置

- `--mode=client`：客户端。
- `--mode=server`：服务端。
- `--mode=proxy`：兼容别名，等价于 `--mode=client --tun-enabled=no`；快照 role 仍为 `client`。
- `--tun-enabled=[yes|no]`：是否创建 TUN，默认 `yes`。
- `client.tun.enabled`（JSON）：与 CLI 等价，默认 `true`。
- `client.proxy-only`（JSON）：旧开关，等价于 `client.tun.enabled=false`，保留兼容。
- `client.http-proxy.port` / `client.socks-proxy.port`：本地监听端口，`0` 关闭对应监听。
- `--set-http-proxy=yes` / `system_proxy_enabled`：设置系统代理；需要存在本地 HTTP 监听。

优先级：`--tun-enabled` > `client.tun.enabled` > `client.proxy-only`；`--mode=proxy` 始终关闭 TUN。

## 行为

- 无 TUN 时使用 `TapStub`，只保留进程内协议栈/策略路径。
- 无 TUN 时跳过 Windows 驱动准备、防火墙规则、LSP 安装、默认路由/DNS 接管等宿主侧动作。
- 本地 HTTP/SOCKS 监听相互独立；两者都关闭时仍允许启动。
- 系统代理只在 HTTP 监听存在且端口有效时设置；停止核心时恢复原设置。
- 快照字段：
  - `network.mode`：`tun` 或 `proxy-only`（兼容字段，表示无 TUN）；
  - `network.tun_enabled`：布尔值；
  - `role`：统一为 `client`。

## 验证

在隔离的 ppp-web（端口 20099，`tun_host=false`）上验证：

1. `mode=client, tun_enabled=false, http=0, socks=0`：启动成功，`mode=proxy-only`、`tun_enabled=false`、无适配器、无监听。
2. `mode=client, tun_enabled=false, http=18080, socks=11080`：启动成功，两个监听存在，无 TUN。
3. `mode=proxy`：等同 2，role=client。
4. `mode=client, tun_enabled=true, tun_host=false`：TUN 适配器创建成功，注入探针 3/3，`mode=tun`、`tun_enabled=true`。

全程未修改宿主默认路由或 DNS（`tun_host=false`）。
