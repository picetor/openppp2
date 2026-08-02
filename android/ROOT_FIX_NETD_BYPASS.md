# 根因定论与 Root 修复：Chrome 显式绑定 WiFi 绕过 VPN

> 日期：2026-08-02
> 设备：Nokia 9（Android 9 / SDK 28，Magisk root，arm64-v8a）
> 隧道：HKBN IPv6-only 服务器（61.244.242.112）
> 分支：inet6，提交 `ca4d8f0f`

## 现象

- 隧道握手成功（`phase=connected`），UI 显示"已连接"
- 但 Chrome 打不开网页：访问 HKBN 服务器 `61.244.242.112` 时 TCP 卡在
  `SYN_SENT [UNREPLIED]`（conntrack 无响应）
- dmesg 显示 `OUT=wlan0 MARK=0xc3`（流量走了 WiFi 而不是 tun0）

## 根因

Chrome（uid 10190）会**显式绑定部分 socket 到 WiFi 网络**（netId 195 →
fwmark `0xc3`）。Android netd 为绑定网络的 socket 安装高优先级规则：

```
11500: from all fwmark 0xc3/0xffff   iif lo lookup wlan0
11500: from all fwmark 0x100c3/0x1ffff iif lo lookup wlan0
```

VPN 的 tun0 规则在 **12000**：

```
12000: from all fwmark 0x0/0x20000 iif lo uidrange 0-99999 lookup tun0
```

优先级数值小者先匹配 → **11500 的 wlan0 规则先于 12000 的 tun0 规则** →
绑定 WiFi 的 Chrome socket 绕过 VPN 直连物理 WiFi → 当到服务器的物理路径
不可达（IPv6-only 隧道下直连被黑洞）时，TCP 卡死在 `SYN_SENT`。

> 注：qtaguid 显示 Chrome 大头流量其实走 tun0（rx 435MB），因为 Chrome
> 大部分新建连接走默认网络（VPN）；只有预连接/显式绑定 WiFi 的少数
> socket 走 wlan0，访问 61.244.242.112 恰好命中后者。

## A/B 对照实验（同手机、同 VPN 会话）

| 状态 | dmesg | conntrack | 结果 |
|---|---|---|---|
| 规则在 | `OUT=wlan0 MARK=0xc3` | `SYN_SENT [UNREPLIED]` | 卡死 |
| 删除 11500 规则 | `OUT=tun0 SRC=10.0.0.2 MARK=0xc3` | `TIME_WAIT/ESTABLISHED [ASSURED]` | Chrome 收到 Akamai HTTP 响应 |
| 恢复 11500 规则 | `OUT=wlan0 MARK=0xc3` | `SYN_SENT [UNREPLIED]` | 卡死复现 |

验证命令：

```
# 删除绕过规则后，fwmark 0xc3 的路由决策：
ip route get 61.244.242.112 mark 0xc3
# => dev tun0 table tun0 src 10.0.0.2   （走隧道 ✓）

# 端到端：
curl -v http://61.244.242.112/   # Connected + AkamaiGHost 400 响应
```

## 为什么不能"加一条同优先级 tun0 规则"？

**会自环**：隧道传输 socket 经 `VpnService.protect()` 保护，其 fwmark 为
`0x200c3`（bit17 = 0x20000 = NETWORK_FORCE_NO_VPN）。`0x200c3 & 0xffff ==
0xc3`，会命中 `fwmark 0xc3/0xffff lookup tun0` → 隧道流量自己进自己 →
隧道断。**所以必须只删除，不添加。**

## Root 修复实现（PppVpnService.kt）

- `installTun0OverrideRules()`：VPN 建立时（`!proxyOnly`）用 root 删除两条
  `pref 11500` 的 wlan0 绕过规则。删除后 fwmark 0xc3 落到 12000 tun0 规则
  （bit17=0 → 匹配 `0x0/0x20000`）。
- `removeTun0OverrideRules()`：VPN teardown 时恢复两条规则，避免泄漏
  "无绕过"策略给后续会话。
- `isRootAvailable()` / `runRootCommand()`：`su -c` 封装，root 不可用时
  静默跳过（回退到原行为）。

### netd 是否会重新下发？

实测：删除 11500 规则后等待 60s，**netd 未自动恢复**（仅剩优先级 19000
的 `fwmark 0xc3/0x1ffff lookup wlan0`，其优先级低于 12000，不构成绕过）。
netd 只在网络切换/连接事件时才重新下发规则——而这类事件同时会触发
openppp2 网络监控重连 → 重新执行 `installTun0OverrideRules()` → 覆盖
再次生效。因此修复在稳态下有效。

> 真机观察：VPN 网络建立完成（`VPN started with key=0`）的瞬间，netd 可能
> 短暂重新下发 11500 规则（启动后 ~5s 的检查曾见到它出现）；但稳态下
> （连接后 45s+、10 分钟+ 复查）规则保持已删除状态，未再复发。若在
> 生产中发现 netd 在连接瞬间的重发造成窗口期，可加一个轻量 watchdog
> （每 30s 检查 `ip rule show` 中 11500 是否回来，回来即删）。

### 13000 的规则不构成绕过

`13000: from all fwmark 0x100c3/0x1ffff iif lo lookup wlan0` 优先级低于
12000，fwmark 0xc3 流量先命中 12000 tun0 → 不绕过，无需删除。

## 测试/验证清单

- [x] root 可用：`su -c id` → `uid=0(root) context=u:r:magisk:s0`
- [x] 删除规则后 `ip route get ... mark 0xc3` → tun0
- [x] 删除规则后 curl 端到端通（Akamai 响应）
- [x] 60s 内 netd 不恢复规则
- [x] 规则可恢复（add 报 File exists 说明 netd 已恢复，无妨）
- [x] 打包 APK 后真机验证 Chrome 直接可上网（静态模式/默认模式）

## 真机验证结果（2026-08-02，Nokia 9 / Android 9）

APK 2.1.6（versionCode=5，含 `ca4d8f0f` root 修复）安装后连接 HKBN 隧道：

| 检查项 | 结果 |
|---|---|
| vpn.log 删除日志 | `tun0 override deleted 2/2 bypass rules` ✓ |
| 删除后 remaining | 仅 `19000: fwmark 0xc3/0x1ffff lookup wlan0`（不绕过）✓ |
| 稳态 ip rule（45s 间隔 × 2） | 11500 保持已删除，未复发 ✓ |
| 路由决策 | `ip route get 61.244.242.112 mark 0xc3` → `dev tun0 src 10.0.0.2` ✓ |
| dmesg（Chrome 访问） | `TCP80IN OUT=tun0 SRC=10.0.0.2 DST=61.244.242.112 SYN/ACK/PSH/FIN MARK=0xc3` ✓ |
| Chrome 实际上网 | example.com / example.org / NeverSSL 全部加载成功 ✓ |
| DNS | `DNSPKTIN OUT=tun0 DST=8.8.8.8` 走隧道 ✓ |

**结论：修复生效，Chrome 显式绑定 WiFi 的 socket（fwmark 0xc3）不再绕过
VPN，TCP 完整握手 + 数据收发，无 SYN_SENT 卡死。**

> 对比修复前：`OUT=wlan0 MARK=0xc3` + `SYN_SENT [UNREPLIED]` 卡死。

## 相关记忆

- 小端 IP：33554442 = 10.0.0.2
- fwmark 位：bit17 = 0x20000 = NETWORK_FORCE_NO_VPN
- 禁止截图；验证用 uiautomator dump / conntrack / 计数器等文本方式
