# 隧道数据面可靠性整改实施状态

> 对应设计：`TUNNEL_DATAPLANE_RELIABILITY_REMEDIATION_CN.md`
>
> Windows UAC/小黄盾专项：`WINDOWS_UAC_MANIFEST_AND_SHIELD_DESIGN_CN.md`
>
> 更新日期：2026-09-18
>
> 当前结论：源码整改与人工审计仍在进行；本机缺少 C++/Rust 工具链，尚未完成编译，更未完成 Windows 管理员实机门禁。本文中的“已落地”仅表示工作区源码已经修改，不代表设计验收通过。

## 1. 已落地代码

### 1.1 P0 路由正确性

- 修正 Windows `GetBestInterface` 返回码判断：仅 `NO_ERROR` 视为成功；失败记录错误码。
- IPv4 RIB 条目增加 `RouteOrigin` 与 `RouteAction`，并保留旧调用的默认参数兼容性。
- IPv6 客户端策略表同步增加来源与动作。
- 默认 TUN、TUN 策略、旁路、Geo 直连、服务端固定路由均在写入时显式标记。
- 通用 IPv6 RIB 的单条和文件导入 API 同步透传来源/动作；Linux 导入的物理系统路由标记为 `PhysicalSystem/Direct`，避免跨平台出现未分类路由。
- 本地代理只把最长前缀命中的 `RouteAction::Direct` 当作直连，不再把“RIB 中存在任意路由”当作直连。
- 修复同一 destination key 下 `/0`、`/1` 受插入顺序影响的问题，最长前缀查询只接受当前精确前缀。
- 增加 `client.routing.route-origin-policy`，默认 `true`；仅紧急回退时可设为 `false`。
- 增加重复 SYN 计数 `duplicate_syn_count`，用于识别可能的递归代理/隧道路由。
- Windows 出站 socket 收敛到统一保护：在 TCP/WebSocket、连通性探测、Rinetd 直连和 direct DNS 的 `connect`/`send` 前创建物理 host route、设置 `IP_UNICAST_IF`/`IPV6_UNICAST_IF`，并用 `GetBestInterfaceEx` 验证最终接口不是 TUN。
- IPv6 DNS 规则直连分支也在 `send_to` 前执行同一 Windows socket 保护；原实现只有 Linux `protect()`，在 Windows IPv6 默认路由进入 TUN 时存在递归或超时风险。
- 无法证明物理出口时 fail-closed：不执行不安全的直连；Rinetd 将“保护拒绝”与普通连接失败区分，调用方仅对保护拒绝回退主隧道。

### 1.2 Wintun 回包提交

- Wintun 提交前校验：IP 版本、包长度、配置 MTU、IPv4 头长度与 checksum、IPv4 UDP/TCP pseudo-header checksum、ICMP checksum、IPv4/IPv6 目的地址、IPv6 payload length，以及 IPv6 UDP/TCP/ICMPv6 pseudo-header checksum；IPv6 UDP checksum 为零会被拒绝，IPv4 UDP 的合法零 checksum 被保留。
- IPv6 校验和逐包追踪共用有界扩展头 walker，支持 Hop-by-Hop、Routing、Destination Options、AH 与 Fragment，限制最多 8 个扩展头并拒绝越界/重复 Fragment；分片包只断言当前包能够证明的 IP 与扩展链结构，不把缺少重组上下文的传输层 checksum 伪报为已验证。IPv4 分片采用同样原则。
- 每次 Wintun 提交分配递增 `flow_id`。
- 显式 trace 开启时，Wintun `OnInput` 为 `TUN_RX` 分配 `flow_id`。仅对能够无歧义识别的 ICMP echo（地址、id、sequence）和 DNS（地址、端口、transaction ID）保存 60 秒、有 8192 条硬上限的短期关联；对应 `REMOTE_RX` 命中后取出并删除上下文，使请求、回包、构包、ring 分配和提交共用同一 ID。普通 TCP/未知协议不使用不可靠启发式强行关联。
- Windows 同步 TUN 分派期间使用线程本地且可嵌套恢复的 trace 上下文，使路由/配置决策的 `POLICY_SELECTED` 和远端链路同步接受后的 `REMOTE_TX` 复用该请求的 `flow_id`；trace 关闭时不保存包指针或上下文，计数器仍然工作。IPv4 异步 direct DNS 在真实 `send_to` 成功后，用原请求 DNS transaction ID/五元组只读查找上下文并记录同一 ID；IPv6 direct DNS 在同步发送成功后记录。共享本地解析器也在实际 tunnel/direct 上游发送成功时为首个请求记录 `REMOTE_TX`；cache 命中、合并查询的后续等待者以及本地产生的 SERVFAIL 明确记为 `LOCAL_RX`，只有真实上游响应记为 `REMOTE_RX`，避免用虚构的远端阶段凑齐证据链。
- 共享解析器的 direct UDP 异步发送只有在完成回调无错误且 `transferred == request.size()` 时才记录 `REMOTE_TX`；短写会移除等待中的 upstream request 并进入失败/超时路径，不能伪报发送成功。
- 分开记录 `WINTUN_VALIDATE_FAIL`、`WINTUN_SUBMIT_OK`、`WINTUN_SUBMIT_FAIL`。
- flow-aware TAP 输出在 core 准备向操作系统交付远端包时记录 `REMOTE_RX`，并把同一个 `flow_id` 传给 `PACKET_BUILT`、`WINTUN_ALLOCATED`、`WINTUN_SUBMITTED`；跨平台默认实现仍调用原 `Output`，Windows Wintun 覆盖实现负责关联。adapter 通过独立 out 参数报告 ring slot 是否已分配，快照增加 `remote_rx_packets` 与 `allocated_packets`，不再把接收、构包、分配和提交合成不可区分的布尔阶段。
- 四个成功阶段使用同一结构化字段集合：五元组、地址族、包长、IP 头长、TTL/flags、IP/传输 checksum、DNS transaction ID、ICMP type/code/id/sequence、接口 index/LUID、route origin/action、outbound、错误码、阶段耗时、MTU 和阶段累计数；地址与端口只在显式开启逐包诊断时输出。
- 增加校验失败、构包完成、ring 分配、提交成功、提交失败以及 Wintun ring 分配失败计数；`built_packets` 不再错误复用会被校验失败消耗的 `flow_id`。
- 无效空包不再返回成功。
- 本地生成的网关 ICMP/ICMPv6 回包改记为 `LOCAL_RX` 并单独计数，不再污染 `REMOTE_RX` 和 `correlated_remote_rx_packets` 并造成“远端已经回包”的假证据；Wintun 构包/分配/提交完整性应与 `REMOTE_RX + LOCAL_RX` 的注入集合比较。
- 错误日志按 1 秒窗口限流并报告被抑制数量；逐包成功日志默认关闭，仅在显式传入 `--dataplane-trace=yes` 时输出，计数器始终有效。

注意：`WINTUN_SUBMIT_OK` 只表示数据已交给 Wintun ring，不表示 Windows 应用已经消费；应用交付仍需 ETW/WFP/抓包门禁证明。

### 1.3 静态 Echo

- 每个异步接收操作独占 buffer 与 source endpoint，两个 socket 不再共享可写状态。
- UDP 发送改为真实系统调用完成语义：只有 `send_to` 成功且长度一致才返回成功。
- 增加接收、解包、发送成功和失败计数。
- 连续 3 次真实发送失败后进入 30 秒 degraded 状态，期间 ICMP 使用主隧道。
- 每个 keepalive 窗口没有收到任何有效响应也计为一次 `response_timeouts`；连续发送失败与响应超时共用阈值，解决“UDP send 成功但回包黑洞时永不降级”的漏洞。
- 超时维护在完整 static 模式和仅 `udp.static.icmp` 模式下都会运行，不再被未设置的全局 `StaticMode` 错误绕过。
- 静态 Echo 单次发送失败立即回退主隧道；会话重建时清除 degraded 状态。
- 接收端只接受本会话实际发送过的服务器 endpoint，并校验解包结果、session ID 和协议；非法来源、session mismatch、解包失败、输出失败分别计数。
- 会话清理时同步清空 endpoint allowlist 与轮询列表，旧会话服务器不能残留到新会话。
- 解包或输出失败不再增加有效接收包/入站流量；普通 `send_to` 成功不再清除 degraded，只有收到并处理有效 request/response 后才恢复静态通道。
- 非法来源仅累计聚合计数，不逐包打印地址；degraded 续期与告警使用原子时间窗，黑洞或恶意流量下每 30 秒最多一次，不让诊断日志放大故障。
- 快照同时暴露 `tx_queued`、`tx_completed`、`tx_failed`；当前为同步 `send_to`，因此 queued 严格保持 0，不把“尝试调用”伪装成排队成功。

### 1.4 MTU/MSS

- 新增配置：

```json
"client": {
  "tun": {
    "mtu": 1400,
    "mss-clamp": true,
    "mss-v4": 1360,
    "mss-v6": 1340
  }
}
```

- 双栈单接口 MTU 规范化为 `1280..1500`；旧值 `65535` 会告警并钳制到 `1500`。
- MSSv4 被限制在 `536..MTU-40`，MSSv6 被限制在 `1220..MTU-60`。
- 只显式配置 MTU 时，MSS 自动取 `MTU-40/MTU-60`；只有显式给出的 MSS 才作为更低的用户上限保留。
- Windows TAP/Wintun 初始化后设置 MTU，并通过接口状态回读验证；失败时停止启动，禁止静默使用 65535。
- IPv4、IPv6 TCP SYN 在隧道封装前执行 MSS clamp；helper 负责 checksum 重算并跳过非 SYN/不适用包，IPv4 对 `MF` 或非零 fragment offset 的所有分片明确禁用 clamp。

### 1.5 TUI/core 日志

- GUI 和 CLI 对默认的非空 `./ppp-core.log` 不再使用“等于默认值就省略”的参数逻辑，因此默认目标会被明确传给 core。
- GUI 和 CLI 在日志等级不是 `none` 时要求路径非空，基于核心工作目录解析并传递绝对路径；启动前以 append-open 实测 Windows ACL 可写性，父目录不存在或不可写时直接阻止启动并在界面报告。等级为 `none` 时明确移除 `--log-file`。
- core 将路径规范成绝对路径，并在打开失败时向 stderr 报告路径、errno 与错误文本；进程内 TUI 通过启动响应收到同一错误，独立 CLI 返回非零，不再假报 `log-open ok` 后继续运行。
- core 日志采用 64 MiB、总计 4 代（当前文件及 `.1`～`.3`）轮转；启动时和运行期每秒检查，轮转在日志输出锁内变换文件句柄，后台日志线程短暂等待而数据面只继续入队，不在 `OnTick` 排空整个 debug 队列。缓冲日志至少每秒刷新一次，退出时强制排空。
- 增加 GUI/CLI 默认日志参数回归测试。
- 增加零依赖日志凭据扫描器；只报告规则名和相对行号，绝不回显命中文本。管理员验收按测试开始时的文件字节偏移只扫描本轮新增日志；日志轮转、文件长度回退或创建时间变化均直接判失败，避免偏移在截断/替换后产生“扫描零字节但通过”的假阴性。
- 移除本地 RPC 握手失败日志中的客户端 token 值；监听启动日志只记录 `authentication=enabled|disabled`，不再出现即使已替换为星号也会混淆扫描语义的 `token=` 字段。
- core 传输握手、会话结束和 keepalive 失败日志不再输出原始 128 位 session ID。独立流量审计 logger 为保留事件关联能力，把原始 GUID 改为 SHA-256 的 96 位截断伪名；管理协议内部仍使用原始 GUID，不改变线上协议或管理 API。

### 1.6 MUX

- 当前代码已有 MUX linklayer 重建、失败退避、单代连接隔离以及活动链路快照。
- 增加 MUX 通道建立、建立失败、代际重置、回退、活动逻辑流、发送队列字节和当前队头停顿时长，并纳入运行时快照。
- 在 `connect_yield` 的逻辑流建立边界记录真实完成耗时（成功样本数、失败数、均值、P50/P95/P99 桶上界和最大值）；这不是底层载波连接耗时，也不把失败请求混入成功延迟分布。
- 按设计门禁要求，本轮没有把 `tun_mux=0` 改为默认开启；必须先通过下面的非 MUX 实机门禁。

### 1.7 运行时诊断快照

- core 快照新增 `dataplane`：重复 SYN、静态 Echo、MUX 与 Wintun 诊断计数。
- Rust TUI RPC schema 与桌面快照 fixture 同步扩展，避免 core 已有数据而前端静默丢弃。

## 2. 已执行的自动验证

执行：

```text
python -m unittest -v tests.test_dataplane_reliability
```

结果：15/15 通过，覆盖：

- 安全 MTU/MSS 默认值及上下限关系；
- route origin/action 与严格 Direct 判定；
- Windows API 成功条件；
- Windows tunnel/direct/DNS/probe socket 的保护顺序、物理接口校验与保护拒绝回退语义；
- 静态 Echo operation 独占状态；
- 静态 Echo 来源/session/协议校验、有效接收计数与真实响应恢复语义；
- GUI/CLI 日志文件参数；
- Wintun 校验与提交阶段计数。
- Wintun 逐包跟踪默认关闭；
- 运行时快照/TUI schema/fixture 契约；
- 诊断结构与 getter 不允许重复定义。
- 管理员验收脚本的 PktMon `finally` 清理、ICMP/MTU/Wintun 门禁及不修改防火墙约束。
- 零依赖 pcapng 分析器的 IPv4 定位、header/transport checksum 和 TCP SYN MSS 固定向量。
- 零依赖主动探针的 ICMP checksum、DNS name/PTR 编码固定向量，以及 core→OS→`APP_COMPLETED` 完整关联向量。
- 日志凭据扫描器的字节偏移、GUID/token 命中以及“报告不回显命中文本”固定向量；本地 RPC 日志不得打印 token。
- 验收 JSONL 的 nearest-rank P50/P95/P99 和下载吞吐固定向量；失败样本与成功样本分位数分开报告。

同时执行 `git diff --check`，没有空白错误。

## 3. 当前环境未能执行的验证

当前主机没有可用的 MSBuild/Visual Studio Build Tools、CMake、clang/g++ 或 Rust toolchain，因此尚不能在本轮完成 C++/Rust 全量编译。仓库 `.venv` 还指向已不存在的用户 Python；自动测试实际使用 Codex bundled Python 执行。

以下项目必须在安装 v145 工具链并构建新二进制后执行，未通过前不得宣称整改完成：

1. `Release|x64` 全量编译以及 Rust TUI `cargo test`；
2. SOCKS IP 目标 50 次、域名目标 50 次，无自环和 10060；
3. DNS 100 次、ICMP 100 次，core/Wintun/Windows 应用三侧计数一致；
4. Wireshark + pktmon/ETW + WFP 审计确认 `WINTUN_SUBMIT_OK` 后的最终交付点；
5. 10 MB 下载 20 次、100 MB 下载 5 次，无 PMTU 停顿，抓包 MSS 分别不超过配置值；
6. 静态 Echo 双 socket 并发、乱序、ring 满和丢包故障注入；
7. 隧道重启 20 次，地址、路由、DNS、MTU 一致；
8. 非 MUX 基线全部通过后，再进行 5% MUX 灰度和单通道阻塞/断开测试。

仓库同时提供 `tests/tools/Invoke-DataplaneRestartGate.ps1`。它不猜测产品的进程管理方式，而是要求测试人员传入两个明确的 PowerShell 启停脚本；固定 20 轮逐次比较 TUN 地址、IPv4/IPv6 MTU、DNS 和路由签名，执行一次 ICMP/DNS 冒烟，并在停止后要求测试地址、该接口路由和测试 DNS 均已清理。只有 20/20 才输出 `passed=true`；它不会修改防火墙，也不替代其他管理员门禁。

### 3.1 设计阶段覆盖审计

下表中的“源码已改”只说明对应实现已经进入工作区；“关闭条件”全部满足之前，该阶段仍然是开放状态。契约测试只检查关键源码约束，不代替编译、原生单元测试、抓包或实机结果。

| 阶段 | 当前状态 | 已有证据 | 尚缺的关闭条件 |
| --- | --- | --- | --- |
| A：路由正确性 | 源码已改，未关闭 | route origin/action、最长前缀、Win32 返回码、socket 保护的源码契约测试通过 | Windows 编译；SOCKS IPv4 与域名各 50 次；HTTP CONNECT 50 次；证明无递归 SYN、10060 和 TUN 自环 |
| B：Wintun 回包 | 源码证据链已实施，未关闭 | 同步隧道、direct DNS 和共享本地解析器的首个真实上游请求均可关联 `TUN_RX/POLICY_SELECTED/REMOTE_TX`；ICMP/DNS 的 `REMOTE_RX/PACKET_BUILT/WINTUN_ALLOCATED/WINTUN_SUBMITTED` 可复用请求 ID；cache、合并等待者和本地 SERVFAIL 使用 `LOCAL_RX`，不污染远端证据；零依赖探针只在应用真实收到响应后写 `APP_COMPLETED`；分析器可审计整条阶段链并将提交 ID 与 PktMon、应用包键关联 | Windows 编译；在新构建上实际证明 `OS_OBSERVED/APP_COMPLETED`；C++ 原生固定向量；并发 DNS 合并运行测试；DNS A/AAAA/PTR 各 100 次及 ICMP 100 次；WFP A/B/C 矩阵；各侧计数一致；重启 20 次 |
| C：静态 Echo | 源码已改，未关闭 | operation 独占状态、来源/session/协议校验、黑洞降级和聚合计数契约通过 | C++ 编译及 sanitizer/等价内存检查；双 socket 并发、乱序、丢响应、ring 满、旧会话包故障注入；验证自动回退与恢复 |
| D：MTU/MSS | 源码已改，未关闭 | 默认值、边界、接口回读、IPv4 分片跳过及双栈 clamp 契约通过 | IPv4/IPv6 固定包向量；实机 MTU 回读；抓包核对 MSS/checksum；10 MB×20、100 MB×5 无 PMTU 黑洞或异常重传 |
| E：MUX | 诊断/回退指标已实施，未关闭 | 通道、代际、队列、HOL 和逻辑流建立成功样本/失败/均值/P50/P95/P99 桶上界/最大值已进入快照；百分位目标秩计算避免整数溢出，桶上界再钳制到真实最大值以保证分位数不大于 max；默认仍关闭 | 小规模通道池策略；非 MUX 完整基线；阻塞/断开故障注入；吞吐、CPU、内存对照；5% 灰度与自动停止条件 |
| F：日志契约 | 源码与扫描门禁已改，未关闭 | 默认/绝对路径、可写性、打开失败、轮转和 schema 的源码契约通过；零依赖扫描器不会回显命中凭据，验收脚本只扫描本轮新增日志；PowerShell AST 通过 | C++/Rust 编译；默认、自定义、空、不可写路径的可执行测试；运行时日志等级切换；64 MiB 轮转压力与数据面延迟对照；对真实运行日志执行扫描并归档结果 |
| 发布门禁 | 未开始 | 设计中已有阈值和回滚开关 | 24 小时稳定性；物理网络切换；退出后路由/DNS/WFP 清理；全部阶段证据归档并由同一构建复现 |

当前 core 内部阶段关联只能证明请求路径已选择/接受发送以及待注入包已提交 ring，不能证明包已被 Windows 网络栈或发起请求的应用消费。`WINTUN_SUBMIT_OK` 不得作为 DNS/ICMP 根因已关闭的依据。IPv6 扩展头 walker 目前也只有 Python 固定向量和源码契约证据；在 C++ 原生固定向量和 Windows 构建运行通过前，不能计入第 6.4 节完整包正确性门禁。

## 4. 实机验收命令建议

构建并以管理员身份启动新版本后，至少采集：

仓库已提供可重复执行的管理员验收脚本：

```powershell
.\tests\tools\Invoke-DataplaneAcceptance.ps1 `
  -TunAddress 192.168.13.25 `
  -DnsServer 192.168.13.1 `
  -PingTarget 1.1.1.1 `
  -SocksProxy 127.0.0.1:1080 `
  -CoreLog .\ppp-core.log `
  -PythonExecutable 'C:\path\to\python.exe'
```

扩展运行门禁使用显式 `-FullGate`；以下 URL 和哈希必须替换为测试环境中稳定、可信的目标，不能照抄占位值：

```powershell
.\tests\tools\Invoke-DataplaneAcceptance.ps1 `
  -FullGate `
  -TunAddress 192.168.13.25 -DnsServer 192.168.13.1 `
  -SocksProxy 127.0.0.1:1080 `
  -SocksIpv4Url 'https://<numeric-ip>/health' `
  -SocksDomainUrl 'https://<domain>/health' `
  -HttpProxy 127.0.0.1:8080 `
  -HttpConnectUrl 'https://<domain>/health' `
  -HttpUrl 'https://<domain>/health' `
  -Download10MbUrl 'https://<domain>/fixed-10mb.bin' `
  -Download10MbSha256 '<64-hex-sha256>' `
  -Download100MbUrl 'https://<domain>/fixed-100mb.bin' `
  -Download100MbSha256 '<64-hex-sha256>' `
  -CoreLog .\ppp-core.log `
  -PythonExecutable 'C:\path\to\python.exe'
```

脚本会记录 UTC 与绝对单调时钟起止值，执行基础 ICMP/DNS、可选 50 次 SOCKS 请求，在 `finally` 中停止 PktMon，并输出接口、路由、JSONL 明细、汇总 JSON、ETL 和 pcapng。提供 `-PythonExecutable` 时还会执行 ICMP 100 次和 DNS A/AAAA/PTR 各 100 次的主动探针，生成 `app-completed.jsonl`，随后自动调用分析器生成 `packet-analysis.json`；只有每个成功应用响应都能关联到 core 完整注入链和 `OS_OBSERVED` 时应用关联门禁才通过。`-CoreLog` 是必填项，且必须指向已经由 `--dataplane-trace=yes` 实例创建的文件，禁止绕过 Wintun 日志门禁。core 日志采用测试前后计数差值，避免历史日志污染本次结论；读取前等待 1.5 秒覆盖缓冲刷新窗口，若测试期间发生日志轮转则本轮日志门禁直接失败。显式输出目录必须为空。脚本还验证 `POLICY_SELECTED ⊆ TUN_RX`、`REMOTE_TX ⊆ POLICY_SELECTED`，以及 `REMOTE_RX ∪ LOCAL_RX = PACKET_BUILT = WINTUN_ALLOCATED = WINTUN_SUBMITTED`。它不会关闭防火墙或修改 WFP 策略；防火墙 B/C 组必须在隔离测试机按第 6.3 节人工授权执行。

提供 Python 时还会生成 `performance-summary.json`，按测试类别给出成功/失败以及成功样本的延迟 P50/P95/P99/max；下载同时给出吞吐 P50/P95/P99/min/max。该文件用于同机、同目标、同构建条件下的基线对照，不单独定义“快”的绝对阈值，也不能用成功样本分位数掩盖失败样本。

默认脚本输出的 `passed` 只代表 `gate_scope=automated_base_only` 的自动化基础门禁通过。`-FullGate` 才覆盖 SOCKS IPv4/域名各 50 次、HTTP CONNECT 50 次、HTTPS 100 次、10 MB×20 和 100 MB×5，并以文件最小字节数和预置 SHA256 防止错误页假通过；其结果为 `gate_scope=automated_extended_runtime`。`summary.json` 始终列出 `manual_gates_remaining`；在 WFP/PktMon 丢弃复核、重启、故障注入、物理切换和 24 小时门禁完成前，两种档位都不能据此关闭整改。

20 轮重启使用：

```powershell
.\tests\tools\Invoke-DataplaneRestartGate.ps1 `
  -StartScript .\tests\environment\start-instrumented-client.ps1 `
  -StopScript .\tests\environment\stop-instrumented-client.ps1 `
  -TunAddress 192.168.13.25 `
  -DnsServer 192.168.13.1 `
  -ExpectedMtu 1400 `
  -Cycles 20
```

示例中的两个 lifecycle hook 不是仓库现成文件，必须由实际部署环境提供并接受评审；禁止把 token、密码或代理 userinfo 写入 hook 或证据目录。

验收运行时应给 core 增加 `--dataplane-trace=yes`。传入 `-CoreLog` 后，脚本会要求至少出现一次 `WINTUN_SUBMIT_OK`，且 `WINTUN_VALIDATE_FAIL`、`WINTUN_SUBMIT_FAIL`、重复 SYN 都为零；同时自动核对 TUN 地址存在、有效 MTU 等于 `-ExpectedMtu`（默认 1400），并要求 ICMP 回包不少于 99%。

手工复核命令：

```powershell
Get-NetIPInterface -InterfaceAlias PPP | Format-Table AddressFamily,InterfaceMetric,NlMtu
Get-NetRoute -AddressFamily IPv4 | Sort-Object RouteMetric | Format-Table DestinationPrefix,NextHop,InterfaceIndex,RouteMetric
pktmon start --capture --pkt-size 0
ping -n 100 1.1.1.1
Resolve-DnsName -Name example.com -Server 192.168.13.1
pktmon stop
pktmon etl2pcap "$env:SystemRoot\System32\PktMon.etl" --out ppp-dataplane.pcapng
```

验收日志应能看到成对的 `WINTUN_SUBMIT_OK flow_id=...`，且不能出现持续增长的 `WINTUN_VALIDATE_FAIL`、`WINTUN_SUBMIT_FAIL` 或 `duplicate_syn_count`。

## 5. 回滚

- 路由来源策略：`client.routing.route-origin-policy=false`（仅紧急使用）；
- 静态 Echo：关闭 `udp.static.icmp`；
- MSS：`client.tun.mss-clamp=false`；
- MTU：修改 `client.tun.mtu`，仍受生产安全边界限制；
- MUX：保持 `tun_mux=0`；
- 逐包数据面日志：保持 `--dataplane-trace=no`（默认）；故障定位时短时开启，采集后立即关闭。
