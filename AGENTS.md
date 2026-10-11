# AGENTS.md — OpenPPP2 项目环境（面向 Codex / 自动化代理）

本文件是代码代理在本仓库工作时的**入口**：环境、构建、测试、自动化接口与红线。
详细的一次性环境搭建见 `docs/DEV_ENVIRONMENT_CN.md`；控制/调试 API 的语义见
`docs/AI_CONTROL_API_CN.md` 与 `docs/AI_DEBUGGING_CN.md`（**以 core 为唯一状态来源**）。

## 1. 仓库速览

| 路径 | 内容 |
|---|---|
| `main.cpp` | 进程入口、命令解析、`get_snapshot`/`run_diagnostics` 的状态组装 |
| `ppp/app/client/` | 客户端：`VEthernetNetworkSwitcher`（TUN 接管/注入/探针）、`VEthernetExchanger` |
| `ppp/ethernet/` | 数据面：`VEthernet`、`VNetstack`、IPv4/IPv6/ICMP 处理 |
| `windows/ppp/tap/` | 驱动层：`TapWindows`（auto/wintun/tap 选择）、`WintunAdapter` |
| `windows/ppp/win32/network/` | Windows 网络配置：地址/路由/DNS/WFP/MTU |
| `ppp/app/rpc/` | `LocalRpcServer`：core 控制 API（四字节大端长度 + UTF-8 JSON） |
| `web/` | 图形宿主 `ppp-web`（内嵌 core）+ 面板静态资源；`web/dist/ppp-web.exe` |
| `tests/` | 回归测试与工具脚本（`tests/tools/`） |
| `docs/` | **中文设计/诊断文档**，改动行为前后都要更新相关 `*_CN.md` |
| `tools/` | 构建与自动化脚本（本文件引用） |

## 2. 环境（Windows）

一次性安装与踩坑记录见 `docs/DEV_ENVIRONMENT_CN.md`。要点：

* 工具链：**Visual Studio BuildTools 2026（PlatformToolset v145）** + **Windows SDK 10.0.26100** + **ATL 组件**。
  vcxproj 写死 `boost_*-vc145-mt-<arch>-1_91.lib`，所以必须是 v145；v143 会在链接期对不上。
* 依赖：vcpkg 固定到 CI 的同一 commit（`VCPKG_REF=19780d9cdf84d0944cf9a318666703b89ab6629c`），
  三元组 `x64-windows-static` / `x86-windows-static`，安装到 **仓库父目录** `..\vcpkg_installed\<triplet>`
  （`ppp.vcxproj` 的 `IncludePath`/`LibraryPath` 就指向那里）。
* 网络受限时：GitHub 直连可能只有 ~14 KB/s，用本机代理 `http://127.0.0.1:8080`（客户端自带 HTTP 代理）
  或镜像前缀 `https://gh-proxy.com/`；`tools/vcpkg-mirror-feed.ps1` 已封装。

## 3. 构建

```powershell
# 只编译（依赖已装好）：ppp.exe
powershell -ExecutionPolicy Bypass -File tools\build-local.ps1 -SkipDependencies

# 同时产出核心静态库与图形宿主 ppp-web.exe（等价于 CI 的两步）
powershell -ExecutionPolicy Bypass -File tools\build-local.ps1 -SkipDependencies -Web

# 从零（会调用 vcpkg 安装依赖，约 50 分钟）
powershell -ExecutionPolicy Bypass -File tools\build-local.ps1
```

产物：`x64\Release\ppp.exe`（独立 core）、`x64\Release\ppp-core.lib`、`web\dist\ppp-web.exe`（内嵌 core 的宿主）。
脚本自动完成 CI 里的隐藏步骤：boost 库名归一化（1.92 → 工程要的 1_91）、`cacert.pem`、
`starrylink.net.key/.pem` 占位文件。CI 的原始命令是：

```powershell
msbuild ppp.vcxproj /p:Configuration=Release /p:Platform=x64 "/t:Clean;Build" /p:VcpkgEnableManifest=false /m
msbuild ppp.vcxproj /p:Configuration=Release /p:Platform=x64 /p:BuildCoreLibrary=true /p:ConfigurationType=StaticLibrary /t:Build /p:VcpkgEnableManifest=false /m
msbuild web\ppp-web.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

## 4. 自动化接口

### 4.1 Core 控制 API（推荐，无需面板）

core 以 `--rpc-listen=127.0.0.1:39100` 启动；协议 = 四字节大端长度 + UTF-8 JSON；
每个连接先 `hello`（token 为空即无鉴权）。方法清单见 `docs/AI_CONTROL_API_CN.md`。

```powershell
powershell -File tools\core-api.ps1 -Method hello
powershell -File tools\core-api.ps1 -Method get_health
powershell -File tools\core-api.ps1 -Method run_diagnostics -ParamsJson '{"scope":"quick"}'
powershell -File tools\core-api.ps1 -Method get_snapshot
powershell -File tools\core-api.ps1 -Method get_logs
powershell -File tools\core-api.ps1 -Method set_log_level -ParamsJson '{"level":"info"}'
```

只读优先：`describe_api` → `get_health` → `run_diagnostics` → `get_snapshot`。
变更只调用 `access=control/lifecycle` 的方法；`accepted` 不代表完成，必须回读运行态。

### 4.2 图形宿主 / 面板 API

`ppp-web.exe` 的面板在 `http://127.0.0.1:19999`；token 从 `/session.js` 取（64 位十六进制），
调用 `POST /api/rpc`，头 `Authorization: Bearer <token>`，方法：`host.status`、`host.settings`、
`host.save`（改设置）、`host.start`/`host.stop`/`host.restart`、`host.arguments`。

```powershell
$t = (Invoke-WebRequest http://127.0.0.1:19999/session.js -UseBasicParsing).Content -replace '.*"([0-9a-f]{64})".*','$1'
Invoke-RestMethod http://127.0.0.1:19999/api/rpc -Method Post -ContentType application/json \
  -Headers @{Authorization="Bearer $t"} -Body '{"id":1,"method":"host.status","params":{}}'
```

## 5. 测试与现场取证

| 目的 | 命令 |
|---|---|
| 裸适配器能否把包交给宿主（TUN 投递判据） | `powershell -File tests\tools\VirtualAdapterDeliverySelfTest.ps1`（0=正常，2=投递失败） |
| 用本地构建复现/对比驱动 | `artifacts\repro-local-client.ps1 -TunHost no\|yes`、`artifacts\tun-driver-matrix.ps1 -Drivers auto,wintun,tap` |
| 被实验改坏的物理网卡恢复 | `artifacts\restore-network.ps1` |
| 抓包 | `pktmon filter add X -t udp -p <port>`；`pktmon start --capture --pkt-size 128 --file-name x.etl`；`pktmon stop`；`pktmon etl2txt x.etl -o x.txt` |

看丢弃原因就 grep `DropReason`：本机出现过 `Not locally destined`（地址未被协议栈认领）与
`INET: checksum is invalid`（构造包错误）。

## 6. 红线与习惯

1. **不要提交 `artifacts/`**（现场脚本与抓包），保持工作树干净。
2. **不要硬改 `ppp.vcxproj` 里写死的 boost 名字**；构建脚本会按 CI 的方式归一化。
3. **任何会接管宿主网络（`--tun-host=yes`）的实验结束后必须恢复**：`artifacts\restore-network.ps1`，
   并重启用户原来的客户端。
4. 结论必须来自证据（日志/抓包/API 回读），不能只凭参数推断；本仓库鼓励把失败现场写进
   `docs/*_CN.md` 的对照表里。
5. 修改行为后同步更新对应中文文档；提交信息用一句话英文，说明「为什么」。

## 7. 已知问题（当前调查状态）

* **TUN 注入在 Wintun 驱动下失败**：客户端探针 `host received 0/3`，日志报
  `cannot bind a UDP probe socket on the tunnel address`；pktmon 可见 `Not locally destined` 丢弃，
  即宿主未把隧道地址当作可用本地地址。
* **TAP 驱动注入正常（3/3）但宿主拿不到地址**：客户端用 netsh 设静态地址 → DAD（ARP 探测）在隧道
  网卡上永不完成 → 地址停在 `Tentative`/被移除 → 宿主退回 APIPA（169.254/16），ARP 以 APIPA 为源，
  网关永远 `Incomplete`。手动 `netsh interface ipv4 set interface <idx> dadtransmits=0` 后地址立刻可用。
* 两个方向的问题互补出现，说明不是单纯的驱动好坏；详见 `docs/TUN_MODE_REMAINING_ISSUES_CN.md`。

## 8. 常用开关语义（已核对源码，勿凭名字猜）

| 开关 | 真实含义 |
|---|---|
| `--tun-host=yes\|no` | 是否做**宿主接管**（路由/DNS/宿主代理）；`VEthernetNetworkSwitcher::UpdateNetworkTakeover()` 的第一道门 |
| `--tun-driver=auto\|wintun\|tap` | 适配器驱动选择（`auto` 优先 Wintun，失败回落 TAP） |
| `--tun-vnet` | 子网转发（`ni->VNet`） |
| `--tun-static` | 静态隧道模式（`ni->StaticMode`，与静态回显通道不是一回事） |
| `--tun-route` | **仅 Linux**（`TapLinux::CompatibleRoute`） |
| `--tun-protect` | 解析进 `ni->ProtectNetwork`，**全代码无人读取** |
