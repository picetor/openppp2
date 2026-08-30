# TUI/CLI 与 C++ 核心一体化评估

日期：2026-08-28  
范围：Windows、Linux、macOS 桌面核心，Rust TUI/CLI，TAP/Wintun、路由、DNS、日志、CI 构建和退出清理。

## 结论

项目已改成 TUI/CLI 与 C++ 核心真正同进程运行，并彻底移除“内嵌可执行文件释放到临时目录后再通过 RPC 启动”的方案。

最终推荐结构：

```text
ppp-tui / ppp-tui-cli（Rust）
        │ C ABI / FFI
        ▼
ppp-core（C++ 静态核心库）
        │
        ├── TUN/TAP/Wintun
        ├── DNS、路由、防火墙
        ├── MUX、lwIP/ctcp、VPN 传输
        └── 日志与运行状态
```

`ppp.exe` 可以继续存在，但只应作为共用核心库的薄命令行宿主，不应再作为 TUI 启动的子进程。

本次改造已按该结论落地：新增 `ppp/core/CoreApi.h` C ABI，CMake 和 MSBuild
都能产出 `ppp-core` 静态库；Rust GUI/CLI 构建时必须链接该库并直接持有核心句柄，
目录读取/探测继续由 Rust 完成，停止路径通过核心 API 同步等待网络清理。TUI 不再支持
外置核心路径、内嵌可执行文件或临时目录释放；RPC 仅保留给用户主动连接独立运行的核心。

## 当前实现的问题

历史上的“内嵌核心”实际上是：

```text
Rust TUI → 释放 ppp.exe 到临时目录 → 启动子进程 → localhost JSON-RPC → C++ 核心
```

这套历史代码已从 TUI 删除。它带来的结构性问题包括：

1. 核心使用自身可执行文件目录查找 `Driver`，临时释放目录没有 TAP 驱动文件。
2. TUI、核心和 RPC 各有一套生命周期，关闭顺序复杂，容易留下 DNS、路由或虚拟网卡状态。
3. TUI 启动目录核心后再停止、启动真实核心，存在双核心、锁冲突和状态切换窗口。
4. 日志需要经过文件、stdout、RPC 和 Rust 再转发，排查链路长。
5. TUI/CLI 无法直接访问核心对象，只能把命令序列化成 RPC。
6. “嵌入”会让使用者误以为是单进程，但实际仍然依赖子进程和 Job Object。

这不是 Rust 和 C++ 不能整合，而是当前选择了进程隔离架构。

## RPC 是否必须保留

关于“保留独立核心 + RPC 作为兼容路径”：TUI 自有核心不再需要 RPC；独立运行的
`ppp.exe` 仍可通过 RPC 被 TUI/CLI 主动连接，因此客户端 RPC 代码暂时保留。

但需要区分两个概念：

| 项目 | 是否保留 | 原因 |
|---|---:|---|
| TUI 自有核心的 TCP RPC | 否 | 同进程后直接调用核心 API |
| 内嵌核心子进程 | 否 | 不再释放临时 `ppp.exe` |
| `ppp.exe` 独立宿主 | 可保留 | 服务端、脚本、无界面启动仍需要命令行入口 |
| 核心 C++ 库 | 必须保留 | TUI、CLI、`ppp.exe` 共用唯一实现 |
| 外部进程 attach | 只有需要时保留 | 没有任何 IPC 就无法控制另一个进程 |

如果产品不再需要“CLI 控制已经运行的另一个 TUI/核心”，RPC 可以完全删除。若仍需要后台核心、远程控制或多前端 attach，则必须保留某种 IPC；可以换成命名管道/Unix socket，但不能在逻辑上消除进程间通信。

## 推荐技术方案

### 1. C++ 核心库化

把 `main.cpp` 中的宿主逻辑与核心逻辑分开：

```text
ppp/core/
  CoreRuntime.h/.cpp       生命周期、线程、配置、停止清理
  CoreApi.h                稳定 extern "C" 接口
  CoreSnapshot             状态快照
  CoreCommand              控制命令
main.cpp                   ppp.exe 薄宿主
```

核心内部继续使用现有 C++ 类型、Boost.Asio、Json 和全局网络实现；跨 Rust 边界只使用 C ABI，不能暴露 C++ 类、`std::string`、异常或 `Json::Value`。

建议第一版接口为：

```cpp
ppp_core_create(const char* const* argv, int argc);
ppp_core_start(ppp_core_handle*);
ppp_core_command(handle, method, params_json, result_json);
ppp_core_snapshot(handle, snapshot_json);
ppp_core_set_log_level(handle, level);
ppp_core_stop(handle);
ppp_core_destroy(handle);
```

字符串接口只是 ABI 边界格式，不再经过网络；后续可将高频快照改成 C 结构体，降低序列化开销。

### 2. Rust 侧直接控制

Rust 增加一个 `core::in_process` 模块：

- `CoreHandle` 持有不透明 C++ 核心句柄；
- TUI 和 CLI 共用同一个 Rust 控制层；
- 核心状态通过快照或事件回调进入 Rust；
- C ABI 已提供日志回调能力；当前 TUI/CLI 不把核心日志直接绘制到界面，核心日志仍由 C++ 统一写入文件和 ring buffer，后续可按需接入 Rust 事件队列；
- 不再等待 `RPC_LISTEN=`，不再创建 TCP 连接，不再生成 token；
- 不再使用 `spawn_embedded`、`Job Object` 和临时核心目录。

`ppp-tui` 与 `ppp-tui-cli` 只负责界面和命令映射，所有网络动作仍由 C++ 核心执行。

### 3. 取消双核心目录流程

现有“目录核心 + 真实核心”流程应改为：

- 服务器目录由 Rust 直接读取和探测；当前已有 `server_catalog.rs`、`probe.rs`，可以继续使用；
- 用户选择节点后，在同一个 `CoreRuntime` 中加载正式配置并启动；
- 如果以后需要配置热切换，使用核心内部的重载/切换 API，不再停止一个进程再启动另一个进程。

这一步对消除 `10048` 单实例锁冲突和第二核心残留很重要。

## 网络、TAP/Wintun 与退出清理

一体化不会让 Windows 驱动变成内存对象；TAP/Wintun 仍然是操作系统资源。但驱动的打开、安装、接口命名、路由、DNS 和恢复都由同一个核心运行时管理，不再由“核心可执行文件所在目录”决定。

应将 Windows 驱动访问抽象成平台服务：

```text
CoreRuntime::start
  → NetworkEnvironment::prepare
  → TapBackend::open
  → VPN/route/DNS start

CoreRuntime::stop / destructor
  → VPN stop
  → route restore
  → DNS restore
  → TAP/Wintun close
```

正常关闭时，同进程 RAII 和显式 `stop()` 能显著简化清理；但任务管理器强制结束、系统崩溃仍不能保证析构函数执行。若必须覆盖强杀场景，需要额外的 Windows 服务/恢复代理，这会重新引入一个受保护进程，属于可靠性方案而非 TUI 核心一体化的必要条件。

客户端 TUN 模式仍需管理员权限。单进程方案最简单的做法是整个 TUI 在启动核心前完成 UAC 提升；如果要求普通权限界面操作管理员核心，就必须使用服务或 helper 进程，不能同时满足“完全同进程”和“UI 不提权”。

## 日志与性能

日志应保留现在的统一等级：`none < error < warn < info < debug`，默认 `error`。

一体化后：

- 核心日志过滤仍在 C++ 生产端完成；
- 同进程 TUI/CLI 不经过 stdout、文件追加和 RPC 三次转发；当前核心日志仍写入 C++ 文件/ring buffer，C ABI 回调保留给后续界面日志事件接入；
- `warn/error` 的额外性能影响应接近当前核心日志系统；
- `debug/info` 仍可能产生格式化、锁竞争和 I/O 成本，但不会产生网络 RPC 开销；
- 文件 sink 仍应使用有界队列和后台线程，不能因为 TUI 卡顿阻塞核心数据面。

因此一体化不会自动让 debug 变快，但会减少一层 IPC 和日志复制；真正影响性能的仍是启用日志后的格式化和输出量。

## 构建系统评估

当前核心构建分为 CMake、Visual Studio 工程和 Rust `build.rs` 三套路径。改造必须先统一“核心源文件集合”，否则静态库会出现平台漏编、重复符号或 Debug/Release 依赖不一致。

推荐构建目标：

```text
ppp-core-static        C++ 静态库
ppp                    C++ 薄命令行宿主
ppp-tui                Rust + ppp-core-static
ppp-tui-cli            Rust + ppp-core-static
```

风险主要包括：

| 风险 | 等级 | 说明 |
|---|---:|---|
| C++ 全局状态 | 高 | `DEFAULT_`、`GLOBAL_`、`Executors`、lwIP 全局回调需要生命周期收口 |
| Windows 静态链接 | 高 | MSVC CRT、OpenSSL、Boost、jemalloc 与 Rust 链接参数要统一 |
| Linux/macOS 链接 | 中高 | 现有 CMake 静态依赖和平台库要转成可复用 target |
| UAC 与退出 | 高 | 同进程提权简单，但强杀恢复仍需额外机制 |
| FFI ABI | 中 | 只要限制为 C ABI、UTF-8、明确 free 函数，风险可控 |
| UI 迁移 | 中 | 当前 UI 已经有快照/命令模型，迁移点集中在 RPC 客户端 |
| 性能 | 低 | 删除本地 TCP 和 JSON 帧传输，通常是净收益 |

## 实施顺序

### 阶段 A：接口和测试，不改变运行方式

1. 抽取 `CoreRuntime` 和 C ABI，但先让 `ppp.exe` 使用它。
2. 保留现有 RPC，确保快照、日志和控制命令契约不变。
3. 增加核心启动/停止、DNS、路由、TUN 恢复测试。

### 阶段 B：Windows 同进程原型

1. MSVC 生成核心静态库。
2. Rust `build.rs` 链接静态库并生成 bindgen-free 的手写 FFI 声明。
3. TUI 默认使用同进程核心。
4. 优先验证 Wintun+ctcp 与 TAP+lwIP 两组组合。
5. 验证正常关闭、窗口关闭、启动失败和重复启动。

### 阶段 C：移除 TUI 自有核心的 RPC/子进程链路

1. ✅ 删除 TUI owned-core 的 `Launcher`、嵌入核心和临时释放逻辑。
2. ✅ 把 owned-core 的 `CoreCommand` 改为直接 C ABI 调用。
3. 保留独立核心 attach 所需的 `RpcClient` 和日志/快照通道。
4. ✅ 目录核心改为 Rust 读取，消除双核心启动。

### 阶段 D：多平台和最终清理

1. 接入 Linux/macOS CMake 静态库 target。
2. 更新 Windows、Linux、macOS CI，构建 TUI/CLI 与同一核心库。
3. 保留 `ppp.exe` 作为独立核心宿主，但不再作为 TUI 子进程。
4. 独立 `ppp.exe` 继续使用自己的 RPC 服务；TUI 不为同进程核心创建 RPC 服务。

## 最终验收条件

- TUI/CLI 启动时不生成 `ppp-tui-core.exe` 临时文件，也不启动核心子进程。
- owned-core 日志中不再出现 `RPC_LISTEN=`、RPC token 和核心 TCP 回环连接；显式 attach
  独立核心时仍允许出现这些信息。
- TAP+lwIP 只从最终 TUI/CLI 的发布目录读取 `Driver`，不依赖临时目录。
- Wintun+ctcp、TAP+lwIP 均可连接和断开。
- 正常退出后 DNS、路由、TUN/TAP 状态全部恢复。
- TUI 与 CLI 使用同一套核心 API、日志等级和状态快照。
- `ppp.exe` 独立命令行、服务端和现有脚本仍可编译运行。
- Windows、Linux、macOS CI 均能生成对应 TUI/CLI 产物。

## 最终建议

核心库化、C ABI 和同进程链路已经完成。继续维护时应保持 TUI/CLI 只通过 C ABI
控制 owned-core；独立 `ppp.exe` 的 RPC 仅作为明确的外部 attach 功能，不应重新成为
TUI 的启动后备路径。
