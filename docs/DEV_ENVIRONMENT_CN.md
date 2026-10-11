# 本地开发环境（Windows）

面向需要在**本机**编译/测试 OpenPPP2 Windows 客户端的人或代理。命令均在本机实测通过。
代理工作流入口见仓库根部 `AGENTS.md`；控制/调试 API 见 `docs/AI_CONTROL_API_CN.md`。

## 1. 目标产物

| 产物 | 路径 | 说明 |
|---|---|---|
| 独立 core | `x64\Release\ppp.exe` | `--mode=client/server/proxy`，可独立运行 |
| 核心静态库 | `x64\Release\ppp-core.lib` | `main.cpp` 以 `PPP_CORE_LIBRARY` 编译，供宿主链接 |
| 图形宿主 | `web\dist\ppp-web.exe` | 内嵌 core + 面板资源；与发行版同形态 |

## 2. 工具链

必须使用 **VS 2026 系**（`PlatformToolset v145`），因为 `ppp.vcxproj` 写死了
`boost_*-vc145-mt-<arch>-1_91.lib`；VS 2022 的 v143 会在链接期对不上。

```powershell
winget install --id Microsoft.VisualStudio.BuildTools `
  --override "--quiet --wait --norestart --installPath D:\VS2026 " +
             "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended " +
             "--add Microsoft.VisualStudio.Component.Windows11SDK.26100"
```

实测组件：`cl.exe 19.51.36260`（v145）、Windows SDK **10.0.26100.0**、**ATL**。
ATL 不是 VCTools 工作负载的默认成员，缺了会报 `atlbase.h`/`atlconv.h` 找不到：

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe' modify `
  --installPath D:\VS2026 --add Microsoft.VisualStudio.Component.VC.ATL --quiet --norestart
# 轮询 D:\VS2026\VC\Tools\MSVC\<ver>\atlmfc\include\atlbase.h 出现即可（约 1 分钟）
```

注意：`winget install ... --override "modify ..."` 会被 winget 当作升级而拒绝，直接用 `vs_installer.exe`。

## 3. vcpkg 与依赖

CI 固定 `VCPKG_REF=19780d9cdf84d0944cf9a318666703b89ab6629c`，本地必须用同一 commit，
否则依赖版本与 CI 不一致（例如 boost 1.92 与 1.91 的库名差异）。

```powershell
# 源码（网络差时用 tarball，git clone 容易卡死）
Invoke-WebRequest "https://codeload.github.com/microsoft/vcpkg/tar.gz/19780d9cdf84d0944cf9a318666703b89ab6629c" -OutFile D:\vcpkg-src.tar.gz
New-Item -ItemType Directory D:\vcpkg -Force; tar -xzf D:\vcpkg-src.tar.gz -C D:\vcpkg --strip-components=1
cmd /c 'D:\vcpkg\bootstrap-vcpkg.bat -disableMetrics'   # 下载官方 vcpkg.exe，需已装 VS

# 依赖，安装到仓库父目录（ppp.vcxproj 期望 ..\vcpkg_installed\<triplet>）
$env:HTTP_PROXY='http://127.0.0.1:8080'; $env:HTTPS_PROXY=$env:HTTP_PROXY
D:\vcpkg\vcpkg.exe install boost-asio boost-beast boost-json boost-program-options boost-system `
  boost-thread boost-lockfree boost-uuid boost-context boost-coroutine boost-filesystem `
  boost-stacktrace openssl jemalloc --triplet x64-windows-static --x-install-root=D:\github\vcpkg_installed
```

实测：73 个包约 **51 分钟**，产物 23 个静态库（boost 1.92.0、openssl 3.6.5、jemalloc 5.3.1）。

### 网络受限时的下载通道（实测速率）

| 通道 | 速率 |
|---|---|
| github.com 直连 | ~14 KB/s（经常完全卡死） |
| `https://gh-proxy.com/<原始 URL>` | ~760 KB/s |
| 客户端自带 HTTP 代理 `http://127.0.0.1:8080` | **~1.9 MB/s**（首选） |

vcpkg 自身也认 `HTTP_PROXY`/`HTTPS_PROXY`；`tools/vcpkg-mirror-feed.ps1` 封装了
「代理优先、镜像兜底、按 vcpkg 打印的 URL 补齐 downloads 缓存」的循环。

## 4. 构建

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-local.ps1 -SkipDependencies        # ppp.exe
powershell -ExecutionPolicy Bypass -File tools\build-local.ps1 -SkipDependencies -Web   # 再出 ppp-core.lib + ppp-web.exe
```

脚本按 CI 的隐藏步骤补齐了这些，缺一个都会构建失败或链接失败：

1. **boost 库名归一化**：vcpkg 装出 `boost_context-vc145-mt-x64-1_92.lib`，工程链接的是 `...-1_91.lib`，
   CI 会把唯一候选复制成期望名字；脚本做同样的事。
2. **`cacert.pem`**：链接阶段要拷到输出目录，CI 单独下载（`https://curl.se/ca/cacert.pem`）。
3. **`starrylink.net.key` / `starrylink.net.pem`** 占位文件。
4. `web\ppp-web.vcxproj` 需要先有 `ppp-core.lib`（`/p:BuildCoreLibrary=true /p:ConfigurationType=StaticLibrary`）。

耗时参考：全量 Release 约 **18 分钟**（LTCG，4 核）；改动 `main.cpp` 会因属性变化触发全量重编，
exe + 静态库 + web 三条线合计约 **40 分钟**。

## 5. 冒烟验证

```powershell
# 工具链
cmd /c 'call D:\VS2026\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc hello.cpp /Fe:hello.exe && hello.exe'

# 数据面：裸适配器能否把包交给宿主（0=正常，2=失败）
powershell -File tests\tools\VirtualAdapterDeliverySelfTest.ps1

# core 控制 API（core 需以 --rpc-listen=127.0.0.1:39100 启动）
powershell -File tools\core-api.ps1 -Method get_health
```

## 6. 踩坑清单

| 症状 | 原因 / 处理 |
|---|---|
| vcpkg 卡在 `Downloading ... cmake-*.zip` 0 KB/s | GitHub 不通；设 `HTTP_PROXY` 到本机 8080 或改用镜像 |
| `error C1083: atlbase.h` | 缺 ATL 组件，用 `vs_installer.exe modify --add Microsoft.VisualStudio.Component.VC.ATL` |
| `error MSB3030: 无法复制 cacert.pem` | 仓库根缺 `cacert.pem`，脚本会自动下载 |
| `LNK1104: 无法打开 boost_*-1_91.lib` | 未做 boost 名字归一化（用 `tools\build-local.ps1` 即可） |
| 构建脚本报 SwitchParameter 转换错误 | 局部变量名 `$web` 与开关参数 `$Web` 撞名（已修） |
| TAP 模式下宿主只有 APIPA、网关 ARP `Incomplete` | 隧道地址卡在 DAD `Tentative`；`netsh interface ipv4 set interface <idx> dadtransmits=0` 可立即验证 |
| Wintun 下宿主 bind 隧道地址失败 / 抓包见 `Not locally destined` | 地址未被协议栈认领，见 `docs/TUN_MODE_REMAINING_ISSUES_CN.md` |

## 7. 本机实测环境快照

| 项 | 值 |
|---|---|
| VS | BuildTools 2026 18.10.2，`D:\VS2026`，`cl 19.51.36260`（v145） |
| SDK | 10.0.26100.0，含 ATL |
| vcpkg | 工具版 2026-09-26，源码 `D:\vcpkg`（ref `19780d9c…`） |
| 依赖 | `D:\github\vcpkg_installed\x64-windows-static`，23 个 .lib |
| 磁盘 | C: 约 27 GB / D: 约 34 GB 可用（VS 约 3.5 GB，vcpkg 树约 6 GB） |
