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
