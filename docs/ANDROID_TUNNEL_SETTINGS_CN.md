# 安卓（Exclave）隧道参数设置：现状与改造方案

> 目标：按 openppp2 原版 CLI 隧道参数（README「命令行接口（原版）」），
> 补齐/重写安卓客户端设置项，逐步去掉硬编码与"只有 UI 没有接线"的半成品。

## 1. 现状：参数链路

```
OpenPPP2Instance.buildVpnOptionsJson()          android_ui/.../bg/proto/OpenPPP2Instance.kt:106
  └─ 生成 vpnOptions JSON（tunIp/tunPrefix 来自 DataStore，默认 10.0.0.2 / 24，tunMaskFor() 换算掩码）
        │
        ▼
VpnService.startVpn()                           android_ui/.../bg/VpnService.kt:271
  ├─ Builder.addAddress(vpnIp, vpnPrefix)       → Android 系统 TUN 接口地址（:461）
  ├─ libopenppp2.set_network_interface(fd, mux, vnet, blockQuic, staticMode,
  │        vpnIp, vpnMask, IPV6_BLOCK_ADDRESS)  → 原生 network_interface（:546）
  ├─ libopenppp2.set_app_configuration(json)    → 原生 AppConfiguration（:405）
  ├─ set_bypass_ip_list / set_dns_rules_list / set_geo_rules / set_dns_bcl
  └─ set_mux_acceleration（effectiveMuxAcceleration > 0 时）
        │
        ▼
原生 android/libopenppp2.cpp
  ├─ set_network_interface：校验 IP/Mask（prefix 16~30），
  │    网关 = FixedIPAddress(ip, mask)（网络地址 +1），
  │    主机 = FixedIPAddress(ip, gw, mask)（去掉网络号/广播号）
  └─ VEthernetNetworkSwitcher 用 IPAddress/GatewayServer/SubmaskAddress 做路由判断
```

要点：Android 上 TUN 的 IP 由 **VpnService.Builder.addAddress** 设置，
原生侧通过 **set_network_interface(ip, mask)** 拿到同一份地址做逻辑判断，
两处必须一致，否则路由/网关判断错乱。

## 2. 已做（现有设置项 ↔ 原版 CLI 对照）

| 原版 CLI | 安卓设置项 | UI | 默认值 | 接线 |
|---|---|---|---|---|
| `--dns`（隧道 DNS） | tunnelDns1 / tunnelDns2 | EditText | 8.8.8.8 / 8.8.4.4 | OpenPPP2Instance.kt:113 + VpnService set_dns_bcl |
| （直连 DNS） | directDns1 / directDns2 | EditText | 223.5.5.5 / 119.29.29.29 | OpenPPP2Instance.kt:115 |
| `--tun-mux` | tunMux | SimpleMenu（0/1/2/3） | 0 | OpenPPP2Instance.kt:124 + set_network_interface |
| `--tun-mux-acceleration` | tunMuxAcceleration | EditText（-1 不启用） | -1 | VpnService.kt:322 + set_mux_acceleration |
| `--tun-vnet` | tunVnet | Switch | false | set_network_interface |
| `--tun-static` | tunStatic | Switch | false | set_network_interface |
| `--block-quic` | blockQuic | Switch | false | set_network_interface |
| `--tun-ip` | tunIp | EditTextPreference（IP 校验） | 10.0.0.2 | OpenPPP2Instance.kt:106 替换硬编码 → Builder.addAddress + set_network_interface |
| `--tun-mask` | tunPrefix | EditTextPreference（16~30） | 24 | tunMaskFor() 换算掩码；addAddress(vpnIp, prefix) 同源 |
| `--bypass-mode` | bypassModeType | RouteFragment | geo | VpnService routeMode（basic/geo） |
| `--bypass` / `--bypass6` | rules/ip.txt、ipv6.txt | 资产文件 | — | VpnService.kt:298 + set_bypass_ip_list |
| `--dns-rules` | rules/dns-rules.txt | 资产文件 | — | VpnService + set_dns_rules_list |
| `--geosite` / `--geoip` | rules/GeoSite.dat、GeoIP.dat | 资产文件 | — | set_geo_rules |
| `--tun-protect` | 内核 protect(fd) 桥 | 固定启用 | yes | VpnService set_protect_enabled |
| `--tun-route` | VpnService addRoute(0.0.0.0/0) | 固定 | — | VpnService.kt:466 |
| MTU | mtu | EditText | 1400 | Builder.setMtu + 原生 |
| 任意 `--xxx` | extraArgs | 多行 EditText | 空 | ⚠️ 只解析了 `--tun-mux`、`--tun-mux-acceleration` 两项（VpnService.kt:321），其余被忽略 |

## 3. 计划做

### P0 虚拟网卡参数 —— ✅ 已完成（tunIp / tunPrefix）

| 参数 | 设置项（Key） | UI | 默认值 | 接线点 |
|---|---|---|---|---|
| `--tun-ip` | tunIp | EditTextPreference（IP 校验） | 10.0.0.2 | OpenPPP2Instance.kt:110 替换硬编码 → Builder.addAddress + set_network_interface 的 ip |
| `--tun-mask` | tunPrefix | EditTextPreference（数字 16~30） | 24 | tunMaskFor() 换算掩码；addAddress(vpnIp, prefix) 同源，与原生校验（16~30）对齐 |
| `--tun-gw` | （不需要） | — | 原生自动=网络地址+1 | `set_network_interface` 内部 `FixedIPAddress(ip, mask)`（libopenppp2.cpp:1397）自动算出网关 = 网络地址+1（`Ipep.cpp` `__fistIP = __networkIP + 1`），**无需 UI/无需改 JNI**；除非要允许用户覆盖原生默认才需要新 setter |
| `--tun-host` | （不需要） | — | 即 tunIp 生效值 | **非独立设置项**：`set_network_interface` 会对 tunIp 做二次修正（合法则保留，否则=网络地址+2），修正后的值就是 TUN 接口上实际生效的主机地址——即"开启 VPN 的效果"，用户填 `tunIp` 即可，无需单独配置 |

实施结果（P0）：
1. `Constants.kt` 增加 `TUN_IP`、`TUN_PREFIX` Key；`DataStore.kt` 增加 `tunIp`（默认 10.0.0.2）、`tunPrefix`（默认 24）。
2. `global_preferences.xml`「OpenPPP2 隧道参数」新增 2 个 EditTextPreference + `strings.xml` 文案（含 16~30 提示），绑定 reloadListener。
3. `OpenPPP2Instance.buildVpnOptionsJson()` 用 DataStore 值替代硬编码，`tunMaskFor()` 换算点分掩码，prefix 强制 coerceIn(16,30)。
4. `Builder.addAddress` 与 `set_network_interface` 使用同一份值（VpnService.kt 原已接线，值同源）。
5. 修改后需重启 VPN 生效（现有 reloadListener 机制）。

### P1 链路行为参数（原生接口已有或需小改）

| 参数 | 现状 | 计划 |
|---|---|---|
| `--tun-flash`（QoS/TOS） | 原生 JNI 已有 `set_default_flash_type_of_service`（libopenppp2.kt:265 已声明），**UI 未接线** | 加 SwitchPreference `tunFlash`（默认 no），VpnService 启动时调用 setter |
| `--link-restart` | 原生桥无接口 | 新增 JNI setter（包装客户端重连次数），UI EditText 默认 0 |
| `--rt`（实时模式） | ✅ 已处理：默认开启 | JNI_OnLoad 设 `ppp::RT = true`（android/libopenppp2.cpp:221），对齐 CLI 默认 yes；用户确认默认开启、不加开关 |
| `--tun-ssmt` | 原生桥无接口 | 评估后再做；UI 保持 extraArgs 兜底 |
| `--auto-restart` | 由 Exclave 前台服务/GuardedProcessPool 承担 | 不重复实现，文档说明 |

### P2 路由与探测

| 参数 | 计划 |
|---|---|
| `--vbgp`（智能路由分流） | 原生桥无接口；与 bypassMode 关系需先评估，暂缓 |
| `probe.*`（本次新增的多入口探测） | 默认开启，经 profile JSON（ConfigBean.content）下发；UI 暂不提供开关（按此前约定），但需在文档说明默认值/行为 |
| `extraArgs` 透传 | 把 VpnService.kt:321 的解析从"仅 mux 两项"扩展为常用参数白名单（tun-ip/tun-gw/tun-mask/link-restart/tun-flash），其余忽略并提示 |

## 4. 注意事项

- **两处一致**：`Builder.addAddress` 与 `set_network_interface` 的 ip/mask 必须同源，否则原生网关判断（IPAddressIsGatewayServer）错乱。
- **prefix 范围**：原生 `set_network_interface` 强制 prefix 16~30（小于 16 拒绝，大于 30 按 prefix 重算），UI 校验范围保持一致。
- **IP 冲突**：tun IP 与局域网网段冲突（如家用 10.0.0.0/24）会导致路由黑洞，UI 提示用户避开物理网段。
- **重启生效**：隧道参数修改后需重建 VPN（现有 reloadListener 机制即可，服务重启自动重建）。
- **IPv6**：目前固定 `IPV6_BLOCK_ADDRESS`（泄漏阻断 ULA），P0 不做 IPv6 地址自定义；如需再评估 `tun-ip6` 参数。
- **文档同步**：P0/P1 每完成一批，更新本表与 README 安卓说明。

## 5. 后台静默日志采集（✅ 已实现）

目的：断流/卡顿发生时无需一直开着日志页，也能拿到 VPN 运行期间的完整日志。

| 项 | 说明 |
|---|---|
| 触发 | VPN 前台服务启动即采集（`VpnService.startVpn` 中 `LogCollector.start`），停止时关闭（`killProcesses` 中 `LogCollector.stop`），静默运行；**一次 VPN 会话 = 一个日志文件**：启动写 `===== session started <时间> =====` 并新建文件，手动停止写 `===== session stopped <时间> =====` 收尾，异常中断（崩溃/被杀）在下次启动时补写 `===== session stopped (interrupted) =====` |
| 采集范围 | 本 app 进程日志：原生 `ppp`、`libopenppp2`（JNI 桥）、`OpenPPP2VpnService`/`VpnService`（Kotlin 服务层）、`AndroidRuntime`（崩溃）、`LogCollector`、`Exclave`、`ProxyInstance` 等 tag；Android 7.0+ 追加 `--pid`（只采本进程）与 `-T 1`（不重复旧缓冲）；非 root 读不到系统/其他 app 日志 |
| 落盘 | `filesDir/logs/ppp-<时间戳>.log`（每次会话新时间戳即新文件），`logcat -v threadtime` 格式，UTF-8，**逐行 flush**（崩溃/杀进程不丢已采日志） |
| 滚动 | 单文件 20MB 自动换新文件，最多保留 3 个，最旧自动删除（磁盘占用上限约 60MB） |
| 导出 | 日志页「发送日志」：报告先附后台采集文件尾部（最近 512KB），再附当前 `logcat -d` |
| 崩溃保留 | `CrashHandler` 崩溃报告自动附带采集文件内容；即使 logcat 环形缓冲被清空（`logcat -c` / 系统回收），文件历史仍在 |
| 跨进程 | 采集由 `:bg` 进程（VpnService）写入，主进程崩溃时 `LogCollector.snapshot()` 按文件名读取，不依赖内存状态 |
| 管理界面 | 日志页右上角文件夹按钮 → 日志文件列表（`LogFilesFragment`）：查看尾部、分享单个/全部、删除单个/清空全部，不打印 logcat 控制台 |

注意：
- 手动「清空日志」（`logcat -c`）只清系统环形缓冲，不影响采集文件。
- 日志按会话分文件后，可在日志文件列表按 `session started/stopped` 标记精确对应某一次会话；单文件 20MB 上限仍保留（超长会话自动换文件）。
- 崩溃/被杀时采集随进程一起停止，文件停笔在最后一行（逐行 flush，内容不丢）；`run()` 错误退出也会立即收尾写 `session stopped`；硬崩溃（无收尾标记）的旧文件在下次启动时自动补写 `session stopped (interrupted)` 收尾标记。
- 采集文件在应用私有目录：卸载或「清除数据」会删除；崩溃报告与导出经 FileProvider 分享出去。

## 6. 开关 / 配置切换卡死修复（✅ 已实现）

症状：VPN 开关变灰无法开启、切换配置无响应（在原生 SIGSEGV 崩溃循环场景下高频出现）。

根因：
- 停止链路 `stopRunner → killProcesses → libopenppp2.stop()` 在主线程**同步等待**原生 io_context 执行任务（`libopenppp2_invoke_on_run_context` 与 `Invoke` 里的 `Awaitable::Await()` 无限等待）。
- 若 VPN worker 线程已退出 / io_context 已 stop（run() 提前返回、崩溃残留等），posted 任务永不执行 → `Await()` 永久阻塞 → 状态卡在 `Stopping` → `ServiceButton` 禁用（Stopping 非 canStop 且非 Stopped）+ `forceLoad` 切换走 `Illegal state` 分支 → 开关与切换全部失效，只能杀进程恢复。

修复：
1. `Executors::Awaitable` 新增 `Await(int timeout_ms)` 超时重载（`ppp/threading/Executors.h` / `Executors.cpp`）。
2. `libopenppp2_invoke_on_run_context` 与 `libopenppp2_application::Invoke`：post 前检查 `io_context.stopped()` 直接返回；`Await(5000)` 超时返回错误码并打印 `libopenppp2` 错误日志（`android/libopenppp2.cpp`）。
3. `VpnService.killProcesses`：记录 `libopenppp2.stop()` 返回码，非 0 时 `Logs.w`，超时/未运行状态可见。

效果：任何 JNI 桥调用最多阻塞 5 秒即返回，停止链路必然走完 → 状态复位 `Stopped` → 开关可再次开启、配置可切换。

### 6.1 莫名其妙自己关闭（✅ 已修复）

症状：VPN 运行中（用户未操作）自己断掉，无任何提示；通知可能还在但网络已断。

根因：`libopenppp2.run(0)` 在 VPN worker 线程返回（start 失败、原生内部异常、连接层错误等）时，`finally` 只是把状态改为 `Stopped`——**无消息、不 stopSelf**：UI 只看到开关自己关掉，前台服务却以幽灵状态继续存活（通知残留、服务空转）。

修复（`VpnService.kt` vpnThread）：
- `run()` 非零返回 / 抛异常 → 记录 `last_error`，`changeState(Stopped, 具体原因)`（UI snackbar 显示"VPN 意外退出"+ 原因，不再无声无息）。
- `run()` 返回 0 且非用户主动停止 → `changeState(Stopped, "VPN 连接已断开")` + `stopSelf()`，前台服务与通知一并清理。
- 用户主动停止（Stopping/Stopped）路径不受影响（不重复提示、不重复 stopSelf）。

注意：SIGSEGV 崩溃导致的进程死亡（`openppp2-vpn-th` 段错误）不经过此路径，属于原生崩溃问题，另行处理。
