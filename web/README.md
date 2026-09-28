# 网页控制端（Windows / C++）

新的桌面入口为 `ppp-web.exe`。浏览器负责绘制，C++ 宿主管理核心生命周期，并通过现有 `ppp_core_command` AI/API 分发器执行查询、配置和出口切换。Rust egui、Slint、TUI/CLI 暂停作为主入口，源码保留。

## 构建与运行

在仓库根目录执行 `./web/build.ps1`。已有最新 `x64/Release/ppp-core.lib` 时可使用 `./web/build.ps1 -SkipCore`。

产物为 `web/dist/ppp-web.exe` 和同目录下的 `web/` 静态资源文件夹。将两者一起放进已有配置目录，双击 EXE，默认打开 `http://127.0.0.1:19999/`。没有 Rust、Node.js、Electron 运行时依赖。当前宿主使用 Win32，仅支持 Windows。

在启动设置中可修改“网页监听端口”（1024–65535），保存后下次启动网页宿主生效；不会中断当前核心。显式 `--port` 参数优先于保存值。默认端口被占用时，先短暂等待旧实例/UAC 交接退出，再依次尝试 20000、20001 等后续 100 个端口；仍不可用则由系统分配空闲端口。浏览器始终打开实际绑定地址，该地址也写入配置目录的 `ppp-web-address.txt`。不会结束占用端口的程序。`--port` 指定的首选端口采用同样策略。

可选启动参数：

```text
ppp-web.exe --base="C:\openppp2" --port=19999
ppp-web.exe --base="D:\github\openppp2" --port=18998 --no-browser
```

静态资源始终来自 EXE 同级的 `web/`，`--base` 指定配置根目录。首次读取 `ppp-web.json`；文件不存在时导入该目录的 `ppp-tui.json` 中结构化字段。保存写入独立 `ppp-web.json`，原 Rust 配置不变。旧版自由格式 `command` 字段不会执行；请在网页结构化设置中核对参数。

## 行为

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

`node --test web/tests/*.test.cjs` 验证接口与生命周期（Node 仅用于开发测试）。测试需要先构建宿主，不会启用 TUN、修改路由或系统代理。
