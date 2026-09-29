# 网页控制端（Windows / C++）

新的桌面入口为 `ppp-web.exe`。浏览器负责绘制，C++ 宿主管理核心生命周期，并通过现有 `ppp_core_command` AI/API 分发器执行查询、配置和出口切换。Rust egui、Slint、TUI/CLI 暂停作为主入口，源码保留。

## 构建与运行

在仓库根目录执行 `./web/build.ps1`。已有最新 `x64/Release/ppp-core.lib` 时可使用 `./web/build.ps1 -SkipCore`。

产物为 `web/dist/ppp-web.exe`，管理页面（HTML/CSS/JS）和项目图标均作为 Windows 资源内置在 EXE 中。复制 EXE 即可打开管理面板，无需额外的 `web/` 文件夹或 `icon.ico`；VPN 配置和驱动仍按原方式提供。双击后自动在默认浏览器打开本机管理面板，并在任务栏通知区域显示项目图标（可能位于系统的隐藏图标区域）。没有 Rust、Node.js、Electron 运行时依赖。当前宿主使用 Win32，仅支持 Windows x64。

在启动设置中可修改“网页监听端口”（1024–65535），保存后下次启动网页宿主生效；不会中断当前核心。显式 `--port` 参数优先于保存值。默认端口被占用时，先短暂等待旧实例/UAC 交接退出，再依次尝试 20000、20001 等后续 100 个端口；仍不可用则由系统分配空闲端口。浏览器始终打开实际绑定地址，该地址也写入配置目录的 `ppp-web-address.txt`。不会结束占用端口的程序。`--port` 指定的首选端口采用同样策略。

可选启动参数：

```text
ppp-web.exe --base="C:\openppp2" --port=19999
ppp-web.exe --base="D:\github\openppp2" --port=18998 --no-browser
```

静态资源始终读取 EXE 内置资源，`--base` 指定配置根目录。首次读取 `ppp-web.json`；文件不存在时导入该目录的 `ppp-tui.json` 中结构化字段。保存写入独立 `ppp-web.json`，原 Rust 配置不变。旧版自由格式 `command` 字段不会执行；请在网页结构化设置中核对参数。`--no-browser` 仅关闭启动时自动打开网页，仍保留托盘图标。

## 行为

- 托盘图标表示管理宿主仍在运行；悬停提示和右键菜单显示核心未启动、操作中、就绪或异常状态，不把图标存在等同于 VPN 已连接。单击图标或右键“打开管理面板”可重新打开网页；右键“退出（停止核心）”等待核心清理网络后退出。Explorer 重启后会重新注册托盘图标。
- 五页沿用确认的 egui 风格设计：等宽字体、固定六卡片、紧凑表格、`{命令} 注释`。
- 页面每秒查询真实核心快照；服务器卡片可选中，按钮执行切换/Rank #1，未启动时从 JSON 配置目录选择并启动。
- TUN 模式需要管理员权限。停止当前核心后点击 UAC，提升后的宿主重新打开网页，再启动核心。
- 核心停止后，网页宿主仍然运行。关闭浏览器标签页不会停止核心；请用“停止核心”或“退出”清理网络状态。
- 应用并重启会先停止当前核心，然后按保存的设置启动。启动失败会显示错误，并保持核心停止。
- 核心请求定时/链路重启时宿主重新启动；意外退出不会无限重启。
- 分流文件支持相对/绝对路径；`./` 和 `../` 相对于保存的工作目录。文件状态通过本机文件系统检查。
- 图表使用实际速率历史，不含示例数据。未启动核心时不伪造网络或延迟。

## HTTP AI/API

所有请求 POST `/api/rpc`，格式 `{ "id": 1, "method": "get_snapshot", "params": {} }`。成功返回 `{ "id": 1, "ok": true, "result": ... }`，失败 `ok=false` 并提供 `error`。

启动器接口：`host.status`、`host.settings`、`host.save`、`host.catalog`、`host.paths`、`host.arguments`、`host.start`、`host.stop`、`host.restart`、`host.elevate`、`host.exit`。

核心接口直接复用 `describe_api` 公布的方法：快照、健康、诊断、设置、日志级别、TCP API 配置、服务器切换和 shutdown。无需 TCP 桥接；TCP RPC 监听可单独开启供其他 AI 客户端使用。

HTTP 仅绑定 `127.0.0.1`，限制 Host 和 Origin，不启用跨源访问；请求使用每次启动随机生成的 Bearer 会话令牌。网页自动获取本机会话，刷新后更新。HTTP 会话令牌和可选的 TCP RPC Token 相互独立。

## 验证

`node --test web/tests/*.test.cjs` 验证接口与生命周期（Node 仅用于开发测试）。测试将 EXE 单独复制到临时目录，核对内置网页与图标，再测试鉴权、核心启动/重启/退出及端口冲突。测试需要先构建宿主，不会启用 TUN、修改路由或系统代理。

Windows CI 复用静态核心库构建宿主，将内置资源的 `ppp-web.exe` 放入 amd64 发布包。托盘人工验收：双击启动、关闭浏览器后仍驻留、从托盘重开面板、核心状态提示刷新、重启 Explorer 后图标恢复、从托盘退出后核心停止且端口释放。
