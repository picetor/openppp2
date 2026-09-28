# 多出口与 tun-static 兼容修复设计

日期：2026-09-26。基线：`6e369fcd`。状态：代码已开放多出口静态 UDP，仍需实际网络验收。

本轮已完成：运行时 `update_settings.static_mode` 在值发生变化时报告需要重启，避免仅切换 bool；聚合器改由 exchanger 持有并在实例销毁时关闭；静态 endpoint 随实例初始化并在会话重建时恢复；静态服务器路由在各出口创建时准备；UDP/ICMP 静态策略读取实际选中的出口配置；Windows 静态 UDP 和聚合器连接均绑定并验证物理出口。多出口启动限制已移除。多出口及网络切换仍需实际环境验收。

## 1. 问题与目标

用户日志在 2026-09-26 16:18:41、16:18:54 明确记录：

```text
VEthernetNetworkSwitcher::Open: multi-outbound mode does not support --tun-static=yes
```

目标：允许多个配置出口同时使用静态 UDP 通道；各出口使用自己的服务器、密钥、会话、聚合器和健康状态；主出口切换、单出口重连和运行时开关不会影响其他出口。

兼容现有 CLI、JSON 和服务端线协议。`--tun-static` 继续作为全局使用策略开关，具体静态传输参数取被选中出口的配置。保留单出口行为和 `udp.static.icmp` 的独立启用语义。

日志中的 Wintun 校验失败、MUX M1、物理出口不可用分别需要独立定位，本方案不将它们认定为多出口静态通道冲突的结果。

## 2. 代码证据

| 位置 | 当前行为 | 修复要求 |
| --- | --- | --- |
| `VEthernetNetworkSwitcher::Open` | 曾在配置表数量大于 1 且 static 开启时直接失败 | 已移除限制；每个按需启动的出口独立初始化静态资源 |
| `PreparedAggregator`、`aggligator_`、`GetAggligator` | switcher 共用一个聚合器，使用主配置 | 聚合器属于具体 exchanger 实例 |
| `AddRemoteEndPointToIPList` | 使用主配置的 `udp.static.servers` 和聚合参数 | 各出口自行准备静态 endpoint，向共享路由管理器申请保护 |
| `VEthernetExchanger::StaticEchoGetRemoteEndPoint` | 从 switcher 获取全局聚合器 | 仅从当前出口静态上下文获取 |
| switcher UDP 分派 | 路由选中 exchanger 后仍读取 `configuration_->udp.static_` | 使用选中 exchanger 的配置决定静态策略 |
| `VEthernetExchanger` | 已持有独立密码对象、socket、endpoint 集合、会话 ID 和降级计数 | 复用这些隔离状态，不重复创建另一套密码逻辑 |
| `StaticMode` / `main.cpp update_settings` | setter 直接改 bool；运行时能绕过启动限制 | 共用带校验和资源准备的变更入口 |
| `SendLocalDnsUdp` | 隧道 DNS 使用缓存 upstream 持有的 exchanger，当前明确禁用 static 发送 | 维持该 DNS 策略，审计切换后缓存的归属和失效 |

启动限制有保护作用，单独删除判断会暴露共享资源问题。README 的 DNS 描述仍提到 static/main 竞速，和当前 `static_sent=false` 实现不一致，实施时同步修正文档。

## 3. 所有权与隔离边界

每个实际建立的 `VEthernetExchanger` 拥有一个 `StaticTunnelContext`。第一步可使用成员封装而不拆分新文件，避免改动所有静态协议函数。

上下文包含当前配置引用、聚合器、静态 endpoint 列表、socket、会话相关状态、健康状态和 generation。已有 exchanger 成员可逐步收拢，保证一个状态只有一个所有者。

逻辑归属以“出口实例 ID + 会话 generation”为准，tag 只用于展示。主出口热切换期间新旧实例可能都叫 `main`，不能以 tag 作为资源或回调的唯一身份。即使两出口使用同一个远端 IP/端口，也不共享密码对象、session ID、收发 socket 或聚合器。

聚合器从出口配置构造，接收缓冲区由实例独占或逐操作分配；必须审计 `aggligator` 内部是否仍存在共享可写缓冲区、单例或跨实例回调。不能只移动 switcher 的指针就宣布隔离完成。

物理接口发现和 host route 可以集中管理。路由按地址、地址族和物理接口建立引用计数租约；一个出口退出只释放自己的租约，最后一个引用释放时才移除本程序创建的路由，不删除原有系统路由。

## 4. 数据发送与降级

分派顺序固定为：路由选定出口 → 读取该出口配置 → 判定静态策略 → 尝试该出口静态通道 → 必要时回退该出口主通道。不能在静态发送失败后改用另一个出口，以免改变分流语义。

明确保留现有 DNS 重定向和 QUIC 阻断的优先级；`udp.static.dns/quic/icmp` 的兼容行为逐项以现有实现和测试确认，避免在此改造中无意扩大静态传输范围。

静态发送返回结构化结果：`sent`、`unavailable`、`rejected_before_send`、`failed`，并记录失败阶段。只有确认没有成功交付发送的失败才能对同一个 UDP 包尝试主通道；不能因未收到响应就重发任意业务 UDP 包，避免重复事务。异步排队成功必须与系统调用完成分开计数。

每出口采用 `disabled → preparing → ready → degraded → probing → ready` 状态；所有状态都可进入 `stopping`。连续失败阈值 3 次和 30 秒降级窗口保持现有默认值。普通流量在 degraded/probing 阶段走该出口主通道，探测控制包有独立发送路径，不受“业务静态不可用”的门控影响。

只有有效来源、正确会话且通过解包校验的探测响应才能恢复健康；一次本地 UDP send 成功不能恢复。超时统计只针对实际发出的探测窗口，未建立会话的闲置菜单项不累计响应失败。单出口异常不修改其他出口的计数和状态。

## 5. 启动、热切换和退出

启动先完成配置校验，再初始化必要网络资源。仅对主出口和按需创建的分流出口初始化静态上下文；服务器菜单元数据不创建聚合器或探测计时器。

主出口切换复用现有 core 切换入口及就绪判断，不另写一套 RPC 切换流程。目标主通道就绪后可以在静态通道 preparing/degraded 状态下接管；静态不可用时回退目标主通道。切换失败保持原活动出口，响应中的 accepted 仍只表示受理。

切换提交时，主出口指针、配置和 DNS upstream 归属一起更新。旧请求的响应不得作为新请求的响应返回；旧 DNS upstream 注销 handler、结束等待者，后续请求绑定新实例。首先审计并复用现有清理入口，避免重复回调。

静态回调捕获实例弱引用和 generation，回调执行前验证实例仍有效且 generation 一致。重连或销毁先使旧 generation 失效，再取消 timer/接收、关闭聚合器和 socket、清空允许来源、释放路由租约。延迟到达的旧会话包不得恢复新会话健康。

正在 warm-up 的目标实例由自己的上下文管理探测，不能依赖仅遍历已注册 `outbound_exchangers_` 的维护循环。并发修改通过既有 executor/strand 串行化；资源关闭和 I/O 不在全局锁内执行。

## 6. 配置与 API

新增统一 core 入口，例如 `ConfigureStaticMode(bool enabled)`，返回 accepted、effective、pending、reason。启动、TUI 和 `update_settings` 都调用该入口。不得以写入 bool 代替初始化/清理。

开启前校验每个已运行出口的参数并准备资源，失败时回滚本次准备且保持原有效配置。未启动出口在按需创建时执行相同校验。关闭全局 static 后，若出口仍配置 `udp.static.icmp=true`，保留 ICMP 所需通道，快照明确显示启用来源。

全局 static 开关只在进程启动时生效；运行时变更会报告需要重启。多出口启动时每个出口按需准备隔离的静态资源。

快照在每个 outbound 下增加 static 子对象：enabled、enabled_by、state、generation、聚合器状态、发送完成/失败、响应超时、来源拒绝、会话不匹配、回退次数、恢复次数及剩余降级时间。保留现有全局字段并注明它代表主出口，避免破坏旧客户端。

日志携带 outbound tag、实例 ID、generation、阶段及失败原因；不输出密钥、token 或原始会话标识。按出口限流，错误信息区分配置错误、资源创建失败、物理出口保护失败和远端无响应。

## 7. 实施顺序

1. 统一启停校验，补运行时绕过漏洞和准确错误提示；先保持现有兼容限制。
2. 将聚合器及静态 endpoint 准备迁移到 exchanger，审计独占 buffer、路由租约和回调归属，验证单出口兼容。
3. 所有数据路径使用已选出口配置，加入 generation 和出口级降级/恢复，核对 DNS 切换清理。
4. 完成多出口测试后移除启动限制，更新 API schema、GUI/终端状态和 README。

每一步保持可编译；最终交付必须包含带聚合器和不带聚合器的多出口支持，不能以静默关闭 static 或只放宽菜单检查代替。

## 8. 验证与验收

以原生单元/集成测试和实际网络验证为主，源码字符串断言不能证明密钥、会话或出口隔离。

| 场景 | 必须满足的结果 |
| --- | --- |
| 单出口 static 开/关、ICMP 独立开启 | 旧配置可用；参数和线上协议兼容 |
| 两出口不同密钥，聚合器 0/非0 的四种组合 | 每个远端只收到所属出口流量，实例资源完全独立 |
| 两出口使用相同远端 IP/端口 | 正确按会话隔离，不串包，不跨出口解密 |
| 主出口切换 A→B→A，至少 20 轮 | static、DNS、路由归属正确；旧回调不能影响新会话；资源数回到基线 |
| 同一出口不同 entry 切换 | 会话代际与 endpoint 更新正确，保留既有切换语义 |
| A 丢响应、B 正常 | A 独立降级并走 A 的主通道；B 无降级；A 探测恢复后回到 ready |
| 聚合器初始化失败、ring/发送失败 | 返回可定位原因，配置变更回滚或按已定义策略回退，无假成功 |
| 运行时开启、关闭、连续切换 | 与启动校验一致，disabled 后 ICMP 独立模式仍符合配置 |
| 网络切换、拔网线、退出 | socket 保护重新绑定；共享路由不会提前删除；无泄漏和悬空回调 |
| DNS 缓存/等待者跨出口切换 | 新请求不绑定旧 exchanger；旧请求至多结束一次，不复用旧会话响应 |

编译门禁包括 Windows Release x64 和 Rust TUI 测试，以及 Linux 构建验证平台保护。Windows 管理员实机测试抓取物理接口和 TUN 流量，以服务器侧接收记录证明出口归属。Wintun 注入校验失败仍需另行定位，不能通过关闭校验使验收通过。

回滚：部署前一稳定构建或设置 `--tun-static=no`；若需要完全停用静态 Echo，同时关闭各出口的 `udp.static.icmp`。保持多出口普通主通道可用。
