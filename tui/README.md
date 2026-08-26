# ppp-tui — Rust native desktop client for openppp2

独立 Rust 原生窗口客户端，连接 C++ 核心（`ppp --headless --rpc-listen`）的本地
JSON-RPC 通道。窗口使用鼠标点击操作，不再依赖 Windows Terminal，也不显示 Logs 页面。

## 构建

需要 Rust 工具链（stable；本机 1.97 已验证）。

```powershell
cd tui
cargo build --release
# 产物: target\release\ppp-tui.exe
```

默认构建会把仓库中可用的 `ppp.exe`（优先 `x64\Release`，其次
`x64\Debug`）内嵌进 `ppp-tui.exe`。也可以显式指定核心产物：

```powershell
$env:PPP_TUI_CORE_PATH = "D:\path\to\x64\Release\ppp.exe"
cargo build --release
```

或使用打包脚本：

```powershell
.\build-standalone.ps1 -CoreConfiguration Release
# 当前只有 Debug 核心时：
.\build-standalone.ps1 -CoreConfiguration Debug
```

发布时只需要 Rust TUI 可执行文件和用户自己的配置/规则文件，不需要另行
分发 `ppp.exe`。启动时 TUI 会把内嵌核心释放到私有临时目录，以 headless
隐藏子进程运行，不会打开外置控制台窗口；退出时自动停止并清理该临时目录。
C++ TUI 源码不参与改动。

> 2026-08-14 验证：`cargo build`（debug/release）零警告；`cargo test` 12/12
> 通过（3 流量采样单元测试 + 7 契约黄金测试）。首次构建需访问 crates.io
> 拉取依赖（国内网络请配置 rsproxy 镜像或代理，见下）。

### 国内镜像（可选）

`%USERPROFILE%\.cargo\config.toml`：

```toml
[source.crates-io]
replace-with = "rsproxy-sparse"

[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"

[net]
git-fetch-with-cli = true
```

## 运行

### 方式一：attach 已运行的 headless 核心（推荐）

管理员终端启动核心：

```powershell
x64/Debug/ppp.exe --mode=client --config=./appsettings.json `
    --headless --rpc-listen=127.0.0.1:39100 --rpc-token=你的令牌
```

（headless 且端口为 0 时，核心会在 stdout 打印
`RPC_LISTEN=127.0.0.1:<实际端口>` 一行供自动发现。）

再启动 Rust 客户端：

```powershell
tui\target\release\ppp-tui.exe --rpc 127.0.0.1:39100 --token 你的令牌
```

### 方式二：窗口内启动内置核心（推荐）

```powershell
tui\target\release\ppp-tui.exe
```

窗口启动后可自由进入“总览 / 网络 / 服务器 / 分流 / 启动设置”，不强制固定操作顺序。
启动后，Rust 界面会先启动一个目录核心：它使用 `--mode=proxy` 和
`--catalog-only=yes`，只读取工作目录下的服务器目录（默认 `./config`），启动 RPC，
不加载主 `--config`、不创建 TUN、不建立 VPN 链路，也不打开 HTTP/SOCKS 监听。
“服务器”页会显示配置名、GUID、主入口和多入口；即使目录为空，目录核心也会保持运行。
用户选中服务器后点击“进入总览并启动”，程序会停止目录核心，将选中的 JSON 作为
`--config` 启动真正的内置核心，随后进入“总览 / 网络”等运行页面。
也可以先在“启动设置”中修改目录和启动参数，然后点击“刷新服务器配置”。
默认启动参数由结构化设置生成，不再重复填充 `--mode`、RPC、TUN 默认值。也可以填写外部核心路径；
留空外部路径即使用内置核心。窗口内还支持填入 RPC 地址和 Token 连接已有核心。

“启动命令接口”提供启动模式（`client` 客户端、`proxy` 无 TUN/无监听的目录或控制模式、
`server` 服务端）、配置文件、服务器目录（默认 `./config`）、TUN IP/网关/掩码、
TUN Host/VNet/Static/Flash、TCP/IP CC（auto/lwIP/ctcp）、MUX 通道与加速、链路重连、Block QUIC、分流模式、
HTTP/SOCKS 端口和核心日志文件路径；同时可设置 TUI 日志文件。所有路径字段保存和
显示为 `/`，Windows 路径仍可读取。核心默认值不会重复写入命令；只有修改后的值
才会出现在“启动命令预览”中。客户端 TUN 模式会显示 UAC 盾牌提示，Proxy 和服务端
不会误报管理员权限。

HTTP/SOCKS 端口留空时不覆盖选中的 `--config`，界面会读取当前主配置中的默认端口；
核心已运行时优先显示运行快照中的实际端口，因此切换 Main/配置后不会继续使用旧的
固定值。启动命令预览是只读的多行滚动区域，长命令可以在区域内查看。
软件设置默认保存到 `./ppp-tui.json`，下次启动自动读取；设置页标题右侧的“保存设置”
按钮可以随时手动保存。

设置页的“分流模式”决定实际使用哪些文件：`ip` 使用 IP/DNS 分流文件，`geo` 才解析
`geo-rules.yaml`、`geosite.dat` 和 `geoip.dat`，`no` 不读取用户分流规则。

“高级启动参数”只用于未被界面接管的额外 CLI 参数。也可以粘贴完整启动命令，点击
“从高级命令导入到上方设置”，程序会提取已支持的字段并去除重复参数；TUI 自动注入
的 `--headless`、RPC 地址、RPC Token 不需要填写。

“连接方式”中可以开启或关闭 `TUN VPN 模式`，以及开启 `系统代理`。TUN 模式关闭时
核心使用 `--mode=proxy` 运行，并自动将 `--proxy-http-port` 与
`--proxy-socks-port` 设为 `0`。这是一种无监听的 proxy 控制模式：不创建 TUN 和本地
HTTP/SOCKS 端口，但仍会加载主配置、`./config` 服务器目录以及 GEO/分流文件，保持核心
连接以便浏览和切换服务器。若要给其他程序提供代理，可在高级参数中填写有效监听端口。
真正核心运行后修改这两个开关，点击“应用设置并重启核心”生效；服务器页可随时从左侧进入，
点击服务器的“切换”或“Rank #1”即可切换节点。

客户端 TUN 模式需要管理员权限时，顶栏的 UAC 盾牌是可点击按钮：点击后会先停止当前
由窗口启动的核心，再通过 Windows `runas` 请求管理员身份重新启动同一个 TUI 和原有
参数。已经是管理员运行时只显示状态，不重复拉起窗口。

命令行仍支持快速打开设置或连接已有核心：

```powershell
tui/target/release/ppp-tui.exe --rpc 127.0.0.1:39100 --token 你的令牌
tui/target/release/ppp-tui.exe --mode=client --config=./appsettings.json
```

也兼容传统的核心启动写法。`start`、`ppp.exe`（包括 `start "标题" ppp.exe`）
会被 TUI 自动识别并去除，剩余参数直接用于内置核心；因此不需要把核心文件另行
放在 TUI 旁边：

```powershell
tui/target/release/ppp-tui.exe start ppp.exe --mode=client --config=./config/rfcJP.json `
    --tun-mux=0 --tun-host=yes --tun-vnet=yes --tun-gw=192.168.12.1 `
    --tun-ip=192.168.12.32 --tun-flash=yes --server-dir=./config `
    --tun-mask=24 --link-restart=3 --tun-static=no --tun-mux-acceleration=3 `
    --block-quic=yes --bypass-mode=ip --log-file=./ppp_win.log --log-level=error
```

`--mode=client` 且启用 TUN 时核心会要求管理员权限；`--mode=proxy` 和
`--mode=server` 按核心配置运行，不会因为界面图标而强制整个 TUI 使用管理员权限。

### 一次性 CLI 控制命令

`ppp-tui-cli` 默认启动交互式终端界面；如果第一个参数是控制命令，则只连接已有
headless 核心、执行一次操作并退出。控制命令和 GUI/TUI 使用同一套 RPC 动作定义：

```powershell
$rpc = "127.0.0.1:39100"
$token = "你的令牌"

ppp-tui-cli status --rpc $rpc --token $token
ppp-tui-cli logs --since-seq 0 --rpc $rpc --token $token
ppp-tui-cli switch main --rpc $rpc --token $token
ppp-tui-cli switch-rank1 main --rpc $rpc --token $token
ppp-tui-cli log-level info --rpc $rpc --token $token
ppp-tui-cli stop --rpc $rpc --token $token
ppp-tui-cli restart --rpc $rpc --token $token
```

加上 `--json` 可输出适合脚本处理的 JSON。`status` 是运行快照，`logs` 支持
`--since`/`--since-seq`，`switch` 支持额外的 `--rank1`。一次性命令必须提供
`--rpc` 和 `--token`；不带这些控制子命令时，原有的 `--mode`、`--config` 等参数
仍然启动交互式 TUI。

核心日志等级支持 `none < error < warn < info < debug`，默认是 `error`。排查问题时可使用
`--log-level=info` 或 `--log-level=debug`，也可以在 TUI 设置页运行时调整；`--log-file` 在
Release 和 Debug 核心中都有效。

`TCP/IP CC` 设置对应核心的 `--lwip` 参数：`auto` 不传参数，保留核心的平台/驱动默认值；
`lwIP` 传 `--lwip=yes`，使用内置 lwIP 协议栈；`ctcp` 传 `--lwip=no`，使用核心的非
lwIP TCP 路径。当前默认建议使用 `auto`：Windows 使用 Wintun 时默认走 `ctcp`，使用 TAP
时默认走 `lwIP`；Linux/macOS 默认走 `ctcp`。该选项只对客户端 TUN 模式生效，Proxy/Server
模式不会创建 TUN。

启动设置页还覆盖核心的 DNS、实时调度、自动重启、物理网卡/网关、TUN 适配器和平台专用
参数（Windows 驱动与 DHCP 租约、Linux 路由保护/分流网卡、Linux/macOS SSMT 与混杂模式）。
服务端模式下可编辑 `--firewall-rules`；其余未在页面单独展示的核心参数仍可放入高级启动命令。
生成实际启动命令时，TUI 会按当前模式和运行平台移除不适用参数，避免旧配置残留。

## 操作

- 左侧按钮切换“总览 / 网络 / 服务器 / 分流 / 启动设置”。
- 终端版（`ppp-tui-cli`）界面与窗口版对齐：顶部状态栏 + 左侧导航 + 内容区，
  共 5 页（总览/网络/服务器/分流/启动设置），无日志页。所有键位在 tmux 内可用：
  数字 `1-5` 切页（F 键仅作增强）、`↑↓/j/k` 移动、`Enter` 执行、
  `p/o` 启动/停止核心、`/` 过滤服务器、`?` 帮助、`q` 退出（二次确认）；
  设置页 `Enter` 进入字段编辑（直接打字，`Esc` 取消，`Tab` 提交并下一个）。
  不带核心参数启动时，CLI 会先启动无 TUN/无监听的目录核心并显示 TCP 探测延迟预览；
  命令行带 `--mode`、`--config` 等核心参数时会直接启动实际核心，行为与窗口版一致。
- 启动后（窗口版总览页、服务器页；终端版服务器页）直接对 `./config/*.json` 的
  服务器地址做 TCP 延迟探测（每 5 秒刷新），无需启动核心即可看到各服务器延迟；
  只有点击“选择并启动”才会真正启动核心。核心启动后服务器页优先显示核心自身的探测结果。
- 启动设置中的核心日志默认为 `./ppp-core.log`，TUI 日志统一写入 `./ppp-tui.log`。
  核心的 stdout 保留输出（RPC_LISTEN、崩溃前最后输出）也追加到核心日志文件；
  TUI 的启动诊断、panic、运行日志全部合并进 `./ppp-tui.log`。TUI 日志路径留空时
  完全不写 TUI 文件日志；日志开关是运行时设置，同一份 release 程序即可承担正常运行
  和问题诊断，不需要另外打包一个 debug 版。
- “服务器”页的“切换”和“Rank #1”按钮直接提交切换请求，不再弹确认框。
- 关闭窗口或点击“停止核心”会先请求内置核心优雅退出，等待 DNS、路由和 TUN
  状态恢复后再结束进程；只有核心无响应时才强制结束。连接已有核心时只断开客户端。
- 核心作为 TUI 的受控子进程：Windows 上通过 Job Object（KILL_ON_JOB_CLOSE）保证
  TUI 无论正常退出、崩溃还是被强制结束，核心都不会残留成孤儿进程；核心异常退出
  （崩溃、被杀、或核心自身 --link-restart 触发重启）时 TUI 会自动重新拉起（最多
  连续 3 次），并给出明确提示。内嵌核心进程名为 ppp-tui-core.exe。
- `--link-restart` / `--auto-restart` 现在由启动设置页直接配置并转发给核心；核心执行
  优雅重启后，TUI 会重新连接。TUI 另外保留核心崩溃/被外部终止时的有限次数自动拉起，
  并通过 Job Object 约束子进程生命周期。

## 测试

```powershell
cargo test          # 契约黄金测试 + 流量采样单元测试（离线可跑）
```

契约测试把 `tests/contracts/runtime-snapshot/*.json` 编译进测试二进制，
验证 Rust schema 与核心/Android 快照契约的前向兼容（未知字段忽略）。
