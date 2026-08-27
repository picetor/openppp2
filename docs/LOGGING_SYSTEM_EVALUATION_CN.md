# OpenPPP2 日志系统调整评估

> 状态：评估结论已落地基础实现；C++ 原生平台构建和吞吐基准仍需在对应工具链/CI 中完成
>
> 目标：Debug 和 Release 使用同一套日志代码，通过运行时日志等级控制输出；Release 默认只输出 `error`，排查问题时再开启 `warn`、`info` 或 `debug`。

## 1. 结论摘要

建议实施统一的运行时日志系统，不再通过 Debug/Release 编译条件裁剪 `LOG_DEBUG`、`LOG_INFO` 和 `LOG_WARN`。

本方案的目标范围是核心的 Windows、Linux、macOS、Android 和 iOS 编译分支，以及桌面端 TUI/CLI 前端。这里的“全平台”指日志等级、过滤规则和生命周期语义统一；底层输出仍使用平台适配器，例如桌面文件/控制台、Android Logcat 和 TUI/CLI 文件/RPC。

等级从低到高定义为：

```text
none < error < warn < info < debug
```

默认值：

```text
Release：error
Debug：   error
```

Debug 构建不再自动打开全部日志。排查问题时通过命令行、配置文件或 TUI/CLI 设置临时切换等级，不需要重新编译核心。

在默认 `error` 等级下，日志系统只会给每个日志点增加一次内联等级判断；没有达到当前等级的日志不会执行参数求值、格式化、内存分配或文件写入，预计对网络性能影响很小。

本轮已完成统一等级、核心 Release 文件输出、桌面异步 sink、Android 原生日志包装、TUI/CLI 设置与 RPC 调整。TUI 自身诊断文件和 Android UI 的用户提示仍是独立输出，不等同于核心 `LOG_*` 日志。

## 2. 当前实现评估

### 2.1 平台范围和现有入口

| 平台/端 | 当前主要入口 | 统一改造要求 |
| --- | --- | --- |
| Windows 核心 | `LOG_*` → 统一桌面异步 sink | 已接入等级、队列、Release 文件输出 |
| Linux 核心 | 共享桌面 `LOG_*` 分支 | 与 Windows 使用相同等级和队列语义，适配文件/控制台 |
| macOS 核心 | 共享桌面 `LOG_*` 分支 | 与 Linux 使用相同等级和文件语义，保留平台诊断信息 |
| Android 核心 | `LOG_*` → `LogPrint` → `__android_log_vprint` | 已接入统一等级；原生直写通过包装器收口 |
| iOS 核心分支 | 共享非 Android 日志分支 | 验证 Apple 日志 sink 和文件权限，不能假设 Android Logcat 可用 |
| Windows TUI/CLI | 核心 RPC 日志 + Rust 诊断文件 | 核心等级由 TUI/CLI 设置；TUI 自身诊断单独过滤 |
| Linux/macOS CLI | 核心 RPC 日志 + 终端诊断 | 与 Windows CLI 使用同一设置和关闭 flush 语义 |
| Android UI | Logcat/JNI/Telemetry bridge | 统一核心日志等级，同时保留 Android UI 自身 Logcat 分类 |

Android 的 `LOG_*` 宏和桌面宏现在都在调用前判断等级，因此低于当前等级的日志不会执行参数求值。`android/` 中原先的直接 `__android_log_print` 也由 `ppp/stdafx.h` 包装到同一过滤器。

Android UI 自己的 Kotlin `Log.*` 仍用于 UI/JNI 提示，不会被核心等级宏替换；native bridge 的 `__android_log_print` 则统一经过核心等级。

### 2.2 核心运行日志 `LOG_*`

核心桌面端日志宏位于 [`ppp/stdafx.h`](../ppp/stdafx.h)。现在所有构建都保留完整的
`LOG_ERROR/WARN/INFO/DEBUG` 调用和 `--log-file` 支持，由运行时等级决定是否输出。

`PPP_LOG_VERBOSE` 不再作为 Debug-only 编译开关。原先由它保护的额外诊断代码也会编译进
所有构建，但只有运行时等级为 `debug` 时才执行；默认 `error` 不启动诊断 watchdog、
连接/对象采样或额外错误码采集。

当前工作区统计到的日志调用点约为：

| 等级 | 调用点数量 | 主要用途 |
| --- | ---: | --- |
| `ERROR` | 50 | 资源、连接、路由等失败 |
| `WARN` | 19 | 可恢复异常、降级和重试提示 |
| `INFO` | 66 | 启动、连接、切换、清理状态 |
| `DEBUG` | 332 | 连接过程、DNS、RPC、数据路径诊断 |

数量会随代码变更变化，以上数据用于评估日志量级，不作为接口契约。

桌面日志实现位于 [`ppp/stdafx.cpp`](../ppp/stdafx.cpp)。本轮已处理以下问题：

1. 宏层先过滤，低于等级的日志不会执行 `vsnprintf` 或参数表达式。
2. 桌面日志进入有界队列，由后台线程串行写文件、控制台和 RPC sink。
3. 队列满时对已启用的核心日志短暂背压，不静默丢弃；线程创建/分配失败时回退同步写入。
4. sink、输出流和关闭 flush 有统一锁，支持核心运行期间动态切换等级。

### 2.3 Telemetry 不是当前运行日志的替代品

核心已有 [`ppp/diagnostics/Telemetry.cpp`](../ppp/diagnostics/Telemetry.cpp) 和配置字段，但它是另一套观测系统：

- 等级为 `INFO/VERB/DEBUG/TRACE`，与目标等级不一致；
- 默认关闭；
- 当前配置可以被解析和序列化，但没有发现核心启动流程调用 `SetEnabled`、`SetMinLevel`、`Configure` 等函数；
- 启用后会涉及后台线程、队列、内存分配、JSON 和可选网络发送。

因此本次运行日志调整不建议直接复用 Telemetry。Telemetry 可以继续作为独立的 OTLP/指标/链路追踪功能，默认关闭。

### 2.4 `VirtualEthernetLogger` 需要保持边界

[`ppp/app/protocol/VirtualEthernetLogger.cpp`](../ppp/app/protocol/VirtualEthernetLogger.cpp) 是 `client.log` 对应的协议、流量和连接事件日志，包含 `PACKET`、`TCP CONNECT` 等高频内容。

它与 `LOG_*` 的定位不同：

- `LOG_*`：核心运行诊断，默认 `error`；
- `client.log`：用户显式开启的协议/流量审计日志，可能非常高频。

本次第一阶段不建议把两者强行合并。`client.log` 仍由路径是否为空控制；如果未来需要统一等级，应单独增加 `client-log-level`，不能让打开普通 `debug` 意外开启 Packet 日志。

### 2.5 TUI/CLI 和 Release 嵌入

Rust 前端现在会把核心日志等级纳入启动参数和设置/RPC；核心日志文件作为完整排查来源。TUI 自身的启动诊断仍独立追加到 `ppp-tui.log`，不受核心等级宏过滤。

核心嵌入逻辑位于 [`tui/build.rs`](../tui/build.rs)：

- 显式设置 `PPP_TUI_CORE_PATH` 时可以指定核心；
- 未指定时严格按 Release 候选顺序选择，不再按文件修改时间选择；
- 这样可避免较新的 Debug 核心被嵌入 Release TUI。

Release TUI 构建后应继续验证嵌入核心的 hash/来源；本地构建已用 `x64/Release/ppp.exe` 完成该验证。

## 3. 目标日志行为

### 3.1 等级语义

| 设置 | 输出内容 |
| --- | --- |
| `none` | 不输出 `LOG_*` 日志；启动失败等强制用户提示是否保留由调用场景单独决定 |
| `error` | 只输出错误 |
| `warn` | 输出错误和警告 |
| `info` | 输出错误、警告和信息 |
| `debug` | 输出全部运行日志 |

推荐支持：

```text
--log-level=none|error|warn|info|debug
```

优先级建议为：

```text
命令行参数 > 配置文件 > 默认值 error
```

TUI 和 CLI 的默认启动参数也使用 `error`。排查时可以在设置页调整并通过 RPC 动态更新，不需要停止核心。

### 3.2 日志宏必须在参数求值前过滤

不能只在 `Logger::Write()` 内部过滤，因为 C/C++ 函数调用的参数会先求值。例如下面的地址转换、字符串拼接和远端地址获取，即使日志最终被丢弃，也会产生开销：

```cpp
LOG_DEBUG("remote=%s", socket->remote_endpoint().address().to_string().data());
```

正确方向是让宏或内联包装器先判断等级：

```cpp
#define LOG_DEBUG(...) \
    do { \
        if (Logger::Enabled(LogLevel::Debug)) { \
            Logger::Write(LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)
```

`Enabled()` 应使用 `inline` 和 `memory_order_relaxed` 的原子读取，保证等级切换线程安全，同时尽量降低判断成本。

### 3.3 输出结构

统一的核心日志事件至少应包含：

- 时间戳；
- 等级；
- 线程标识；
- 文件和行号；
- 组件或模块；
- 格式化后的消息。

输出目标分为三个 sink：

1. 文件：`--log-file` 在 Release 和 Debug 都有效；
2. 控制台：保持现有核心控制台行为；
3. RPC：推送给 TUI/CLI 的日志页面。

三个 sink 应由同一个日志事件驱动，避免核心文件、控制台和 TUI 看到不同等级或不同格式的日志。

## 4. 性能评估

### 4.1 默认 `error`

默认等级下，绝大多数 `DEBUG/INFO/WARN` 日志只执行：

1. 一次等级读取；
2. 一次分支判断；
3. 直接返回。

不会执行：

- 可变参数求值；
- `vsnprintf`；
- 字符串分配；
- 文件写入；
- RPC 序列化；
- 队列入队。

因此在默认 `error` 下，性能影响主要是日志点本身多了一次分支。对于普通启动、连接和 DNS 操作，这个开销预计很小；对于极高频的数据包路径，仍然应通过基准测试确认。

### 4.2 `warn/error`

未触发日志时，`warn/error` 与其他等级一样只产生等级判断开销。

真正产生一条日志时，会有格式化、事件入队和后台 sink 处理成本。正常情况下 `warn/error` 只在状态变化或失败时触发，因此总体影响应较小。

需要重点防止错误风暴，例如：

- 每个失败数据包都输出一条 Error；
- 重连循环每次尝试都输出一条 Warn；
- DNS 请求持续失败时重复输出相同错误。

建议对重复的 Warn/Error 增加去重或限频，并在日志中记录抑制数量。

### 4.3 `info/debug`

开启 `info` 后，启动、连接、切换和清理信息会增加 CPU 和 I/O，但通常可接受。

开启 `debug` 后，当前代码中存在大量连接、DNS、RPC 和数据路径日志。本轮已改为有界异步队列：

- 日志生产者完成等级判断、格式化和入队；
- 后台线程负责文件、控制台和 RPC；
- 队列容量为 8192，满时生产者短暂等待，不能因为 Debug 日志突发而静默丢失已启用的文件日志；
- 实时 RPC/UI 推送仍可以独立限速，必要时只淘汰界面缓存中的旧日志；
- 当前版本已保证队列满不丢失，后续仍应补充磁盘写失败/限速计数。

### 4.4 日志保留和“丢弃”边界

需要明确区分以下两种行为：

1. **等级过滤**：当前等级为 `error` 时，`info/debug/warn` 不生成日志。这是用户主动选择的过滤，不属于日志系统运行中的静默丢失。
2. **输出丢弃**：日志已经达到当前等级，但因为队列、RPC 或缓存满而没有保存。这种情况不应对核心文件日志静默发生。

最终建议的保留策略：

| 场景 | 策略 |
| --- | --- |
| 默认 `error` | 只生成 Error；Info/Warn/Debug 按等级过滤 |
| `warn` / `info` 排查 | 已启用等级的核心文件日志不静默丢弃；队列满时允许生产者短暂阻塞 |
| `debug` 排查 | 以文件为完整记录；必要时降低实时 RPC 推送速度，但不能影响核心文件日志完整性 |
| TUI/CLI 日志页面 | 只保留滚动窗口，旧日志可以从界面内存中淘汰；淘汰不代表核心日志文件丢失 |
| `none` | 用户明确要求完全关闭统一运行日志 |

TUI/CLI 当前的 RPC 日志环形缓冲区和界面日志环形缓冲区都不是永久存储，不能作为排查日志的唯一来源。启用 `info/debug` 时，TUI/CLI 应确保核心 `--log-file` 有效，并把核心日志文件作为完整证据。

如果日志文件打不开、磁盘写入失败或进程被操作系统强制终止，无法保证日志完整性；这些异常必须输出到可用的备用 sink 或记录明确的丢失计数。

因此，本方案不是“所有日志永远都不丢”：低于当前等级的日志会被有意过滤，TUI 界面会淘汰旧日志；但达到当前等级的核心文件日志不应因为异步队列满而被静默丢弃。代价是排查期间开启 `info/debug` 时，极端日志压力下可能让日志生产者短暂等待，这个性能代价只在主动排查时承担。

### 4.5 性能测试要求

不能仅凭代码判断具体百分比。实现后应至少比较以下场景：

| 场景 | 对比项 |
| --- | --- |
| 默认 `error` | 新旧 Release CPU、吞吐、延迟、内存 |
| `warn` | 连接失败和重连场景下的 CPU、日志量 |
| `info` | 启动、切换服务器和清理场景耗时 |
| `debug` | 高连接数、DNS、数据包压力下的吞吐和丢日志量 |
| `none` | 日志关闭后的最低开销 |

目标不是让 `debug` 零开销，而是保证默认 `error` 不改变正常网络性能，同时让 Debug 日志不会阻塞核心网络线程。

## 5. 实施方案

### 阶段一：核心日志基础设施

1. ✅ 新增统一 `LogLevel` 和字符串解析函数。
2. ✅ 将 `LOG_ERROR/WARN/INFO/DEBUG` 改为所有构建都编译。
3. ✅ 移除这些日志宏上的 `PPP_LOG_VERBOSE` 编译裁剪。
4. ✅ 将非日志的 watchdog、调试统计等功能改为运行时 `LogLevel::Debug` 控制，避免默认 `error` 启动额外诊断线程。
5. ✅ 实现运行时原子等级和宏层过滤。
6. ✅ 保证日志 sink 多线程安全。

### 阶段二：输出和生命周期

1. ✅ Release 也支持 `--log-file`。
2. ✅ 增加有界异步日志队列。
3. 重复 Warn/Error 的限频或去重仍待后续处理。
4. ✅ 核心退出时先完成 DNS、路由、TUN、代理等网络清理，再 flush 日志队列。
5. 正常关闭已覆盖；强制终止只能尽可能保留日志，不能依赖 flush 完成网络清理。

### 阶段三：配置、RPC、TUI/CLI

1. ✅ 增加 `--log-level`。
2. ✅ 增加 TUI/CLI 启动设置中的日志等级字段。
3. ✅ 增加 RPC `get_log_level` / `set_log_level`，支持运行时调整。
4. ✅ TUI 和 GUI 设置页增加日志等级选择；CLI 通过启动参数传递。
5. TUI 自身的启动诊断仍是独立文件输出，不由核心等级宏过滤；核心日志等级和 TUI 诊断文件不互相覆盖。
6. 保留 `client.log` 独立开关，避免普通 Debug 排查意外开启高频 Packet 日志。

### 阶段四：Release 构建和嵌入

1. ✅ Release 核心和 Debug 核心都编译完整 `LOG_*` 代码。
2. ✅ `tui/build.rs` 优先使用显式 `PPP_TUI_CORE_PATH`。
3. ✅ 未显式指定时严格优先 Release 核心，不按修改时间在 Debug/Release 之间随机选择。
4. CI 先构建当前版本的 Release 核心，再构建并嵌入 Release TUI；本地 Release 构建已验证。
5. ✅ Release help/build smoke test 已验证 `--log-level`、`--log-file` 和 Release 核心嵌入；跨平台运行时 smoke test 仍需在对应 CI 完成。

## 6. 安全和稳定性要求

- RPC token、密码、私钥、完整配置内容不得进入 Debug 日志。
- 当前 RPC 认证失败日志包含收到的 token，改造时必须脱敏。
- 日志文件应考虑最大大小、轮转或启动时截断策略，避免长期运行无限增长。
- 日志失败不能影响 VPN 核心运行；文件打不开时应降级到控制台或仅保留 RPC。
- sink 和日志队列析构顺序必须稳定，不能在核心网络清理过程中访问已关闭的日志对象。
- `none` 只关闭统一 `LOG_*` 日志；直接输出给用户的启动错误、权限错误等提示是否保留，应由调用场景单独决定。

## 7. 验收标准

### 编译和配置

- Debug/Release 均能编译 `LOG_DEBUG/INFO/WARN/ERROR` 调用。
- 默认等级为 `error`。
- 五个等级解析、保存、加载和 RPC 调整正确。
- Release 下 `--log-file` 可正常生成日志。
- Release TUI 嵌入的是当前 Release 核心。

### 过滤行为

- `error` 不产生 Info/Warn/Debug 日志。
- `warn` 产生 Warn/Error。
- `info` 产生 Info/Warn/Error。
- `debug` 产生全部日志。
- `none` 不产生统一运行日志。
- 被过滤的日志不会执行昂贵参数表达式。

### 性能和稳定性

- 默认 `error` 下核心吞吐和延迟没有明显回归。
- Debug 日志开启时不会同步阻塞网络事件循环。
- 日志队列满时核心文件日志不静默丢失；RPC/UI sink 可以限速或淘汰旧缓存，并能报告对应丢弃数量。
- 多线程输出不会出现行级交错。
- 正常关闭、CLI `q`/Ctrl+C、TUI 关闭和窗口关闭后，网络清理日志可以在 flush 后落盘。
- 日志系统故障不会阻止 DNS、路由、TUN 和系统代理恢复。

## 8. 最终建议

采用“完整编译 + 运行时等级过滤 + 异步输出”的方案：

```text
所有构建都保留 LOG_* 代码
              |
              v
       运行时等级判断
              |
      error（默认）
              |
       排查时开启 info/debug
              |
     异步文件 / 控制台 / RPC
```

这样可以满足 Release 可诊断、默认低开销、TUI/CLI 可调整和核心关闭日志可追踪的要求，同时不把协议流量日志和 Telemetry 观测系统混入普通运行日志。
