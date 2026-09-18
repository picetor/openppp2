# Windows UAC Manifest 与“小黄盾”设计

## 1. 结论

当前 TUI、CLI 和 CORE 不显示原版 EXE 的“小黄盾”，原因不是应用图标缺失，而是最终 PE 的 UAC manifest 不再声明 `requireAdministrator`。

- `D:\github\openppp2\icon.ico` 与 `D:\github\openppp2_main\icon.ico` 的 SHA-256 均为 `E64508E0661968D0BED31EE386C7D7D3E5459C9D0DAC1CC236E723A44C8307CF`，图标内容相同。
- 原版 `openppp2_main/ppp.vcxproj` 的 Win32/x64、Debug/Release 四种配置都使用 `RequireAdministrator`。
- 当前 `ppp.vcxproj` 在提交 `1771f6d13d7e39aa8791041853ce98a60db82e10` 中将四种配置全部改成 `AsInvoker`。
- 当前 `tui/build.rs` 生成的 `.rc` 仅包含 `1 ICON ".../icon.ico"`，同时链接给 `ppp-tui.exe` 与 `ppp-tui-cli.exe`；其中没有 `RT_MANIFEST` 或 `requestedExecutionLevel`。
- 当前 GUI 在需要 Client + TUN 时显示应用内 UAC 按钮，并通过 `ShellExecuteW(..., "runas", ...)` 重新启动自身。这会获得管理员令牌，但不会反向改变磁盘上 EXE 的 manifest，因此资源管理器仍不会为该 EXE 固定显示小黄盾。

Windows 资源管理器的小黄盾表示“从 EXE manifest 或兼容性设置判断，启动时会请求提升”，不等价于“这个进程此刻已经是管理员”。实际权限必须通过进程令牌检查，不能通过图标判断。

## 2. 当前行为为何形成

当前产品同时支持两类权限需求：

1. Proxy/部分服务端模式通常可以普通用户运行；
2. Client + TUN 需要创建/配置虚拟网卡、路由、DNS 或 WFP，必须提升。

`AsInvoker + 按需 runas` 允许普通模式不提升，但因为 TUI 已改为“核心静态链接、同进程运行”，Client + TUN 一旦提升，整个 GUI、CLI 和内嵌核心都会处于高完整性级别。它并没有实现真正的前端/核心权限隔离。

CORE 自身仍有运行时管理员检查；该检查只能拒绝权限不足的启动，不能让资源管理器显示盾牌，也不能自动弹出 UAC。

## 3. 必须先确定的产品策略

### 3.1 方案 A：恢复原版视觉和启动语义

若发布要求是“TUI、CLI、CORE 三个 EXE 都必须显示小黄盾，并在启动时请求 UAC”，则三个最终 PE 都必须嵌入 `requireAdministrator` manifest。

实施要求：

- 将 `ppp.vcxproj` 四个配置的 `UACExecutionLevel` 恢复为 `RequireAdministrator`；
- 为 Rust 两个 binary 分别嵌入同一份 Windows manifest，包含：

```xml
<requestedExecutionLevel level="requireAdministrator" uiAccess="false" />
```

- manifest 必须进入最终 `ppp-tui.exe` 和 `ppp-tui-cli.exe`，不能只把 XML 放进发布目录；
- 保留运行时令牌检查作为纵深校验；GUI 的 `runas` 按钮在始终提升模式下应隐藏或变成只读“管理员运行”状态，避免形成似乎还需要二次提升的误导；
- CORE 的 `proxy_mode_` 普通权限能力将不再可用，因为进程启动即提升。

代价：每次启动都弹 UAC；Proxy/服务端模式也以管理员运行；GUI、解析器和网络输入处理全部扩大为高权限攻击面；高/中完整性进程间拖放和部分自动化行为可能受 Windows UIPI 限制。

### 3.2 方案 B：保持按需提升（当前模型）

继续使用 `asInvoker`，TUI 仅在 Client + TUN 启动前调用 `runas`。该方案不会让文件图标固定出现小黄盾，这是预期行为，不能同时把“无盾”当作缺陷。

必须补强：

- CLI 在执行需要 TUN 的命令前明确检测管理员令牌，输出可执行的提升命令或提供显式 `--elevate`；
- GUI 顶栏持续显示“普通权限 / 管理员运行”，并以令牌结果为准；
- 日志记录启动完整性级别、是否经过 `runas`、当前模式及管理员检查结果；
- 文档明确资源管理器盾牌不是运行状态指示器。

### 3.3 方案 C：最小权限分离（长期推荐）

保持 TUI/CLI 为 `asInvoker`，新建一个职责受限的管理员 helper 或 Windows Service，只有网卡、路由、DNS、WFP 和核心数据面在高权限进程中运行。前端通过有 ACL、版本握手和命令白名单的本地 IPC 控制它。

该方案安全边界最清晰，但与当前“核心完全同进程”的实现目标冲突，需要独立项目设计，不能作为修复小黄盾的轻量改动。

## 4. 本轮建议

若“小黄盾与原版一致”本身是明确发布验收项，选择方案 A；它是唯一能保证三个文件在资源管理器中都由 manifest 稳定显示盾牌的方案。若目标只是确保 TUN 能获得管理员权限，则保持方案 B 更合适，当前缺少的不是盾牌资源，而是 CLI 提升体验和权限状态可观测性。

在产品策略未明确前，不应只为图标效果修改 manifest：这会真实改变所有启动路径的权限边界，不是纯 UI 调整。

## 5. 实施与验收门禁

无论选择 A 或 B，都必须对 Release x64 的实际发布文件执行以下检查：

1. 使用 Windows SDK `mt.exe -inputresource:<exe>;#1 -out:<manifest.xml>` 导出最终 manifest；
2. 断言三个 EXE 的 `requestedExecutionLevel` 与选定策略一致；
3. 用普通用户启动 Proxy、Server、Client + TUN，分别记录是否弹 UAC、令牌是否提升、失败信息是否准确；
4. Client + TUN 启动后验证 Wintun、路由、DNS、WFP 创建成功，退出后全部清理；
5. 拒绝 UAC 时不得残留半初始化网卡、路由、DNS、WFP、互斥量或后台进程；
6. GUI 与 CLI 的权限提示必须来自令牌检查，不能来自图标或启动参数推断；
7. 清理 Windows 图标缓存或改用新文件名复核盾牌，避免 Explorer 缓存造成假阴性；
8. 将导出的 manifest、启动日志和清理结果随同构建哈希归档。

当前环境没有 Visual Studio/Windows SDK/Rust 工具链，因此这里只完成了源码级根因确认；尚未生成新 EXE，也没有完成上述最终 PE 验收。
