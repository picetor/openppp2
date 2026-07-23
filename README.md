# 🔐 PPP PRIVATE NETWORK™ 2 — 分支特性说明

<div align="right" style="margin-top:-40px;">
  <kbd style="background:#0366d6;">
    <strong>简体中文</strong>
  </kbd>
  <kbd>
    <a href="README_EN.md">English</a>
  </kbd>
</div>

---

> 本分支基于上游 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) `main` 分支修改。
> 以下仅列出**与原版不同的特性与用法**。原版已有功能（隧道协议、路由策略、服务器配置等）请参考上游文档。

---

## 📋 目录

- [Geo 分流模式](#-geo-分流模式)
- [IPv6 特性总览](#-ipv6-特性总览)
  - [IPv6 分流 (--bypass6)](#-ipv6-分流---bypass6)
  - [VPN 服务器 IPv6 连通性保证](#-vpn-服务器-ipv6-连通性保证)
  - [Windows IPv6 DNS 防泄漏](#-windows-ipv6-dns-防泄漏)
  - [Windows TUN DNS 防泄漏](#-windows-tun-dns-防泄漏)
  - [Windows IPv6 源地址选择修复](#-windows-ipv6-源地址选择修复)
  - [服务器端 IPv6 模式](#-服务器端-ipv6-模式)
  - [IPv6 DNS 配置与下发](#-ipv6-dns-配置与下发)
- [WSS 优选 IP 加速](#-wss-优选-ip-加速)
- [SOCKS5 代理](#-socks5-代理)
- [改进与修复](#-改进与修复)
- [CLI 参数对比](#-cli-参数对比)
- [命令行接口（原版）](#-命令行接口原版)
- [CLI 启动示例](#-cli-启动示例)
- [Debug 日志版本](#-debug-日志版本)
- [隧道协议配置](#-隧道协议配置)
- [构建系统](#-构建系统)
- [关联项目](#-关联项目)

---

## 🧭 Geo 分流模式

客户端可用 `--bypass-mode` 一键切换分流引擎：

| 模式 | 行为 |
|------|------|
| `ip` | 默认模式，继续读取 `ip.txt`、`ipv6.txt`、`dns-rules.txt` 及 `appsettings.json` 中的原有路由配置 |
| `geo` | 读取 `geo-rules.txt`，并按需加载 Mihomo/V2Ray 格式的 `geosite.dat`、`geoip.dat` |
| `no` | 不读取用户分流规则；VPN 服务器地址等维持隧道所必需的保护路由仍会保留 |

```bash
ppp --mode=client --bypass-mode=geo

ppp --mode=client --bypass-mode=geo \
    --geo-rules=./geo-rules.txt \
    --geosite=./geosite.dat \
    --geoip=./geoip.dat
```

以上是原有的单出口 Geo 用法：`--config` 仍指向 JSON，规则动作使用 `direct` 或 `tunnel`（`tunnel` 等价于 `main`）。JSON 启动和 `--bypass-mode=ip|geo|no` 行为不变。

### 单 TAP 多出口模式

当 `--config` 直接指向 `.txt` 文件时，程序将其严格识别为“多出口 Geo 清单”；指向 `.json` 时仍是传统单配置模式。默认清单路径可放在工作目录，例如：

```bash
ppp --mode=client --config=./geo-rules.txt \
    --geosite=./geosite.dat \
    --geoip=./geoip.dat
```

```ini
# 出口声明：标签=JSON 配置路径
main=./main.json
tunnel1=./tunnel1.json
tunnel2=./tunnel2.json

# 可选；未写时默认 main
final=main
direct_dns=local

# 匹配规则：类型,值,出口标签|direct
geosite,github,main
geosite,openai,tunnel1
geosite,microsoft,direct
geosite,cn,direct
geoip,cn,direct
domain-suffix,example.com,tunnel2
ip-cidr,192.0.2.0/24,tunnel1
ip-cidr6,2001:db8::/32,tunnel2
```

清单规则如下：

- 必须声明 `main=<JSON>`；其他出口标签只能包含小写字母、数字、`_`、`-`，不区分输入大小写。`direct`、`tunnel`、`reject` 为保留字。重复标签、未知动作、缺少文件或无规则均会拒绝启动。
- JSON 路径按程序的当前工作目录解析，与 `geo-rules.txt` 放在哪个目录无关。每个 JSON 都会独立读取自己的 `client.server`、`client.guid`、协议密钥、传输密钥、WebSocket/TLS 参数及 `client.server-proxy`，因此各出口可以使用不同的 key。
- 只创建一个 TAP。`main` 负责 TAP 地址、DNS、系统路由、本地 HTTP/SOCKS 监听等全局状态；各标签拥有独立的远端连接和重连状态。所有出口启动时都会创建，任一出口不可初始化时整体启动失败；选中的出口断线时不会自动泄漏或回退到 `main`。
- TCP 连接在建立时固定出口；原始 TCP/UDP/ICMP 按目标地址维护活动粘滞，持续有流量时不会因 DNS TTL 到期切换到另一组 key，空闲 5 分钟后才重新按规则选择。
- 多出口模式不支持 `--tun-static=yes`，因为旧静态 UDP 回声只有一套全局服务器/聚合器，无法隔离不同出口密钥；使用该组合会明确拒绝启动。传统 JSON 模式不受影响。

### 匹配语法和顺序

规则格式为 `类型,值,动作`，支持 `geosite`（含 `@cn` 等属性）、`geoip`、`domain`/`full`、`domain-suffix`、`domain-keyword`、`domain-regex`/`regexp`、`ip-cidr`、`ip-cidr6`。动作可以是清单中已声明的出口标签或 `direct`；兼容动作 `tunnel` 等价于 `main`。

规则严格从上到下匹配，第一条命中生效。域名规则通过 DNS 应答中的 A/AAAA 记录建立带 TTL 的 IPv4/IPv6 地址策略；操作系统是否同时发起 A 和 AAAA 查询由系统解析器决定，程序不会强制“双查”。同一 CDN IP 被多个域名命中时，优先级更高（更靠前）的规则拥有该 IP 策略。`direct_dns` 只重定向命中 `direct` 的域名查询；其他域名仍使用 TAP/主配置 DNS，解析出的业务连接再按目标 IP 进入对应出口。纯 IP 连接只匹配 `geoip`/CIDR 规则，未命中时使用 `final`（默认 `main`）。

`direct_dns=local` 会在网络接管前，从选中的物理网卡一次性读取原始 IPv4/IPv6 DNS。可以追加显式地址（例如 `direct_dns=local,223.5.5.5,119.29.29.29`），也可以只写地址以完全自定义；重复或无效地址会被移除。直连 DNS 的 IPv4/IPv6 路由固定到物理出口并在退出时删除，接管后不会重新读取网卡 DNS，避免把虚拟 DNS 读回后形成递归。

DoH、DoT、DoQ 不会被解密、重定向或伪造失败，而是作为普通连接按服务端域名/IP 使用同一套 geo 规则：明确命中 `direct` 的国内加密 DNS 端点保持直连，其他或未知端点使用 `final`（默认 `main`）。因此不会破坏证书固定、HTTP/3 或软件自身的安全 DNS逻辑；加密隧道内部的单个查询无法再由 openppp2 二次分流。

传统单出口规则示例：

```ini
direct_dns=local,223.5.5.5,119.29.29.29

geosite,github,tunnel
geosite,microsoft@cn,direct
geoip,cn,direct
domain-suffix,example.com,tunnel
domain,api.example.com,direct
ip-cidr,192.0.2.0/24,tunnel
ip-cidr6,2001:db8::/32,direct
```

Geo 数据文件不会内置到程序中，启用 Geo 模式前需将兼容的 `geosite.dat` 和 `geoip.dat` 放到对应路径。

---

## 🌐 IPv6 特性总览

本分支相比原版增加了完整的 IPv6 分流、DNS 防泄漏、源地址选择修复等能力，覆盖**客户端分流**与 **Windows 平台兼容性**两个维度。

### IPv6 分流 (`--bypass6`)

通过操作系统路由表实现**纯路由级** IPv6 分流，不对 IPv6 数据包做任何深度检测。

```
流量方向:
  国内 IPv6 (ipv6.txt)  ──→ 物理网卡直连 (bypass)
  其余 IPv6 (::/0)      ──→ TUN 隧道 (VPN)
```

#### 用法

```bash
# 基本用法（使用默认 ipv6.txt）
ipv6.txt 在启动目录，可以自动加载，另外可以指定网关，网卡

# 自定义分流文件 + 网关
ppp --mode=client \
     --bypass6=./ipv6.txt \
     --bypass-ngw6=fe80::1

# Linux 指定物理网卡
ppp --mode=client \
     --bypass6=./ipv6.txt \
     --bypass-nic6=eth0 \
     --bypass-ngw6=fe80::1
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--bypass6=<file1\|file2>` | IPv6 分流列表文件 | `./ipv6.txt` |
| `--bypass-nic6=<interface>` | (Linux) 物理网卡名 | auto-select |
| `--bypass-ngw6=<ip>` | IPv6 下一跳网关 | `::` (禁用分流) |

> **注意**: 不指定 `--bypass-ngw6` 时不分流，所有 IPv6 走 TUN 隧道，与原版行为一致。

#### ipv6.txt 格式

```
2400:da00::/32
2401:fa00::/32
# 注释以 # 或 ; 开头
```

#### 平台路由命令

| 平台 | 路由命令 |
|------|----------|
| Windows | `CreateIpForwardEntry2` (IP Helper API) — 无弹窗，非 `system("netsh")` |
| Linux | `ip -6 route add <cidr> via <ngw6> dev <ifname>` |
| macOS | `route -n add -inet6 <cidr> <ngw6>` |

---

### VPN 服务器 IPv6 连通性保证

**问题**：VPN 客户端设置 `::/0` 默认路由经由 TAP 设备后，VPN 服务器本身的 IPv6 地址因路由指向隧道而不可达 → UDP 静态 echo 超时 → 不断重连。

**修复**：在安装 TAP 默认路由前，为 VPN 服务器的 IPv6 地址添加一条 `/128` 精确路由通过物理网卡。`/128` 优先级高于 `::/0`，确保服务器始终可达。

```
安装的路由表:
  ::/0                    → TAP 设备 (隧道)
  <VPN服务器IPv6>/128     → 物理网卡 (pin route)
```

所有平台均已实现（Windows `netsh` / Linux `ip route` / macOS `route add`）。

---

### 🛡️ Windows IPv6 DNS 防泄漏

**范围**：只管理 openppp2 自己的 TUN DNS，不修改物理网卡、ICS、WSL、Hyper-V 或其他虚拟网卡。
 VPN 隧道，造成 DNS 泄漏。

**修复**：
- VPN **连接时**：保存并临时清除 TUN 的 IPv6 DNS，使宿主普通查询进入 IPv4 虚拟 DNS 网关
- VPN **断开时**：恢复 TUN 原始 IPv6 DNS

无需额外配置，自动生效。

---

### 🛡️ Windows TUN DNS 防泄漏

Windows 只把 openppp2 自己的 TUN 网卡 DNS 设置为虚拟网关（例如 `192.168.12.1`）。ICS、WSL、Hyper-V、Docker、移动热点及其他虚拟网卡不会被改绑或清空。openppp2 在 TUN 内接收宿主查询：普通域名使用 TUN 实际下发的主 DNS，命中 `direct` 的域名使用 `direct_dns`。主 DNS 同时通过延迟隔离的 Static Echo UDP 通道和 `main` exchanger 兼容通道查询，首个有效响应立即返回；该 DNS 专用通道在 Windows 本地 DNS 开启时自动建立，不要求启用全局 `--tun-static=yes`，其他 UDP 流量仍保持原模式。普通主 DNS 查询不会自动创建 TCP/53 回退连接；本地直连 DNS 若 UDP 300 ms 未返回，只向同一台直连解析器尝试 TCP/53，不会把国内域名送往主 DNS。应用显式发出的 TCP DNS 查询仍按相同分流策略处理。该方式不监听宿主的 53 端口，避免与 ICS 等服务冲突；退出时恢复 TUN 原始 DNS。

- 无需 DNS 模式开关，也无需额外启动参数。
- Geo/IP 分流均可使用：命中 `direct` 的域名使用 `geo-rules.txt` 的 `direct_dns`；其他域名只使用 `main` DNS 及其同出口备用服务器，失败时不会回退到物理网卡 DNS。
- 只守护 TUN 网卡：运行期间将其 IPv4 DNS 固定到虚拟网关并清空其 IPv6 DNS；其他网卡保持不变。若应用主动绕过系统解析器，严格防泄露还需要可选的 Windows 过滤层，不能仅靠网卡 DNS 配置宣称完全无泄露。
- `udp.dns.prefer_ipv4` 只处理隧道规则的 DNS 响应；命中 `direct` 的 DNS 响应保持原始 A/AAAA 记录。
- `udp.dns.prefer_ipv4=true` 仅在已缓存 A 记录时移除 AAAA；没有 A 缓存时立即保留并返回 AAAA。

```powershell
# 正常启动即可；DNS 自动经 TUN
.\ppp.exe --mode=client

# 查看网卡 DNS 和到上游 DNS 的 TUN 路由
Get-DnsClientServerAddress
route print 1.1.1.1

# 验证系统解析
nslookup example.com
```

---

### 🎯 Windows IPv6 源地址选择修复

**问题**：Windows 优先选择全局单播地址（2409::/...）而非 TAP 的 ULA 地址（fd42::
/...）发起对外连接，导致 IPv6 流量绕过 VPN。

**原因**：Windows 路由表中 `::/0` 优先级 30，而 `fc00::/7`（ULA）优先级仅为 3。

**修复**：VPN 连接时提升 `fd00::/8` 的优先级为 **50**（原为 3），断开时恢复。

---

### 服务器端 IPv6 模式

`appsettings.json` 中支持服务器端 IPv6 数据面配置：

```json
{
    "server": {
        "ipv6": {
            "mode": "nat66"         // "nat66" | "gua" | "" (禁用)
        }
    }
}
```

| 模式 | 说明 | 支持平台 |
|------|------|----------|
| `nat66` | ULA↔GUA 地址转换（类似 IPv4 NAT） | Linux 专用 |
| `gua` | 全局单播地址直通 | Linux 专用 |
| 空/禁用 | 关闭 IPv6 数据面 | 全平台 |

> **Windows 兼容**：原版在 Windows 端遇到 `server.ipv6.mode` 会直接拒绝加载配置。本分支自动检测并禁用 IPv6 数据面，配置可正常加载（客户端不受影响）。

---

### IPv6 DNS 配置与下发

IPv6 DNS 不使用客户端的 `--dns=` 参数，而是在服务端 `appsettings.json` 中配置：

```json
"server": {
    "ipv6": {
        "dns1": "2606:4700:4700::1111",
        "dns2": "2001:4860:4860::8888"
    }
}
```

服务端通过 `AssignedIPv6Dns1/2` 将配置随 IPv6 分配信息下发。Linux/macOS 客户端通过 `ApplyIPv6Assignment()` → `ApplyClientDns()` 应用到 TAP 适配器；Windows 客户端当前会清除 TAP 的 IPv6 DNS，并优先使用由 `--dns=` 配置的 IPv4 DNS。

---

### ⚡ WSS 优选 IP 加速

在连接 CDN 优选 IP 的同时，通过自定义 Host/SNI 字段让 CDN 正确路由到你的源站。

```
客户端 → 优选 IP (CDN 边缘节点) → CDN 内部路由 → 你的源站服务器
               │
               ├─ Host: your-domain.com  (WebSocket 握手)
               └─ SNI:  your-domain.com  (TLS 握手)
```

#### 用法

```json
{
    "client": {
        "server": "wss://优选IP:port/tun",
        "websocket": {
            "host": "your-domain.com",
            "sni": "your-domain.com"
        }
    }
}
```

| 字段 | 说明 |
|------|------|
| `server` | 填写优选IP和端口或域名，而非真实域名 |
| `websocket.host` | WebSocket Host 头，设为真实域名 |
| `websocket.sni` | TLS SNI 字段，设为真实域名 |

`host` 和 `sni` 同时支持 WS (`ws://`) 和 WSS (`wss://`) 隧道。留空时行为与原版完全一致。

---

### 🔌 SOCKS5 代理

内置 SOCKS5 代理服务器，作为 TUN 模式的补充。

#### 用法

无需额外配置，SOCKS5 代理默认与 TUN 同时启动。地址在启动日志中显示：

```
Socks Proxy           : 127.0.0.1:1080/socks
```

#### 认证与 bug 修复

相比原版修复了以下问题：

| 问题 | 原版行为 | 本分支 |
|------|----------|--------|
| **CONNECT 无回复** | 建立隧道后未发送 SOCKS5 成功回复，客户端一直等待 | ✅ 正确发送 `REP=0` 回复 |
| **域名端口损坏** | Null 终止符覆盖了端口高字节 | ✅ 先读端口再 Null 终止 |
| **认证逻辑错误** | `&&` 而非 `\|\|` — 只需用户名或密码之一匹配即通过 | ✅ `\|\|` — 任一不匹配即拒绝 |
## 🔧 改进与修复

| 改进项 | 原版 | 本分支 |
|--------|------|--------|
| **Windows IPv6 路由** | 调用 `system("netsh ...")`，可能弹出 GUI 对话框导致 hang | ✅ 使用 Windows IP Helper API (`CreateIpForwardEntry2`)，无弹窗 |
| **路由已存在处理** | `netsh` 报错"对象已存在"导致重连循环 | ✅ `ERROR_OBJECT_ALREADY_EXISTS` 视为成功 |
| **静态链接** | 依赖系统动态库 | ✅ 全静态链接，单文件部署 |
| **GLIBC 兼容** | 仅支持较新 GLIBC | ✅ 向后兼容旧版 GLIBC |
| **UDP echo socket** | 硬编码 IPv6，无 IPv6 环境下创建失败 | ✅ 自动回退 IPv4 |
| **服务器 IPv6 数据面** | Windows 端加载配置因 `server.ipv6.mode` 卡死 | ✅ 自动禁用并继续加载 |

---

## 📖 CLI 参数对比

新增的命令行参数（相比原版）：

| 参数 | 支持平台 | 说明 | 默认值 |
|------|----------|------|:------:|
| `--bypass6=<file1\|file2>` | 全平台 | IPv6 分流列表 | `./ipv6.txt` |
| `--bypass-nic6=<interface>` | Linux | IPv6 分流物理网卡 | 自动选择 |
| `--bypass-ngw6=<ip>` | 全平台 | IPv6 分流网关 | `::` (禁用分流) |

---

## 📖 命令行接口（原版）

以下为上游原版完整 CLI 参数参考，本分支完全兼容。

### ⚙️ 通用命令

| 命令 | 功能 | 格式 | 默认值 |
|------|------|------|:------:|
| `--rt` | 实时模式 | `--rt=[yes｜no]` | `yes` |
| `--dns` | 设置DNS服务器 | `--dns <IP列表>` | `8.8.8.8,8.8.4.4` |
| `--tun-flash` | 启用高级QoS策略控制 | `--tun-flash=[yes｜no]` | `no` |
| `--pull-iplist` | 下载国家IP列表 | `--pull-iplist [文件]/[国家]` | `./ip.txt/CN` |
| `--config` | 配置文件路径 | `--config <文件路径>` | `./appsettings.json` |
| `--mode` | 运行模式 | `--mode=[client｜server]` | `server` |

> 🔗 **IP列表数据源**: [APNIC 官方列表](http://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest)

---

### 🖥️ 服务器命令

| 命令 | 功能 | 格式 | 默认值 |
|------|------|------|:------:|
| `--firewall-rules` | 防火墙规则文件 | `--firewall-rules <文件>` | `./firewall-rules.txt` |

---

### 💻 客户端命令

#### 核心设置
| 命令 | 功能 | 格式 | 默认值 |
|------|------|------|:------:|
| `--lwip` | 协议栈选择 | `--lwip=[yes｜no]` | Win: `yes`<br>其他: `no` |
| `--vbgp` | 智能路由分流 | `--vbgp=[yes｜no]` | `yes` |
| `--nic` | 指定物理网卡 | `--nic <网卡名>` | 自动选择 |
| `--ngw` | 强制网关地址 | `--ngw <IP>` | 自动获取 |

#### 虚拟网卡
| 命令 | 功能 | 格式 | 默认值 |
|------|------|------|:------:|
| `--tun` | 网卡名称 | `--tun <名称>` | 平台相关 |
| `--tun-ip` | IP地址 | `--tun-ip <IP>` | `10.0.0.2` |
| `--tun-gw` | 网关地址 | `--tun-gw <IP>` | `10.0.0.1` |
| `--tun-mask` | 子网掩码 | `--tun-mask <位数>` | `30` |
| `--tun-host` | 首选网络 | `--tun-host=[yes｜no]` | `yes` |

#### 高级功能
| 命令 | 功能 | 格式 | 默认值 |
|------|------|------|:------:|
| `--tun-mux` | MUX连接数 | `--tun-mux <连接数>` | `0` |
| `--tun-mux-acceleration` | MUX加速 | `--tun-mux-acceleration <模式>` | `0` |
| `--tun-vnet` | 子网转发 | `--tun-vnet=[yes｜no]` | `yes` |
| `--tun-ssmt` | 超线程优化 | `--tun-ssmt=[线程数]/[模式]` | `4/st` |
| `--tun-static` | 静态隧道 | `--tun-static=[yes｜no]` | `no` |
| `--link-restart` | 链路重连次数 | `--link-restart=[重连次数]` | `0` |
| `--block-quic` | 阻止QUIC流量 | `--block-quic=[yes\|no]` | `no` |
| `--auto-restart` | 自动重启程序 | `--auto-restart=[秒]` | `0` |

#### 路由设置
| 命令 | 功能 | 格式 | 默认值 |
|------|------|------|:------:|
| `--bypass-mode` | 选择分流引擎 | `--bypass-mode=ip｜geo｜no` | `ip` |
| `--bypass` | 绕过列表 | `--bypass <文件1\|文件2>` | `./ip.txt` |
| `--bypass-nic` | 指定绕过列表的接口 | `--bypass-nic <网卡>` | |
| `--bypass-ngw` | 指定绕过列表的网关 | `--bypass-ngw <IP>` | `0.0.0.0` |
| `--virr` | 自动更新并生效 | `--virr [文件]/[国家]` | `./ip.txt/CN` |
| `--dns-rules` | DNS规则 | `--dns-rules <文件>` | `./dns-rules.txt` |
| `--geo-rules` | Geo 规则文件 | `--geo-rules <文件>` | `./geo-rules.txt` |
| `--geosite` | geosite 数据文件 | `--geosite <文件>` | `./geosite.dat` |
| `--geoip` | geoip 数据文件 | `--geoip <文件>` | `./geoip.dat` |

#### 平台专用
| 命令 | 平台 | 功能 | 格式 | 默认值 |
|------|:----:|------|------|:------:|
| `--tun-route` | Linux | 路由兼容 | `--tun-route=[yes｜no]` | `no` |
| `--tun-protect` | Linux | 路由保护 | `--tun-protect=[yes｜no]` | `yes` |
| `--tun-promisc` | macOS / Linux | 混杂模式 | `--tun-promisc=[yes｜no]` | `yes` |

---

### 🪟 Windows 命令

| 命令 | 功能 | 格式 |
|------|------|------|
| `--system-network-reset` | 网络重置 | `--system-network-reset` |
| `--system-network-optimization` | 性能优化 | `--system-network-optimization` |
| `--system-network-preferred-ipv4` | 设置IPV4网络优先 | `--system-network-preferred-ipv4` |
| `--system-network-preferred-ipv6` | 设置IPV6网络优先 | `--system-network-preferred-ipv6` |
| `--tun-driver` | 选择虚拟网卡驱动；需要二层桥接时使用 `tap` | `--tun-driver=[auto\|wintun\|tap]` |
| `--no-lsp` | 禁用LSP | `--no-lsp` |

---

### 📚 全局参数

#### MUX 加速模式
| 值 | 模式 | 适用场景 |
|:--:|------|----------|
| 0 | 标准 | 常规使用 |
| 1 | 服务器加速 | 下载密集型 |
| 2 | 客户端加速 | 上传密集型 |
| 3 | 双向加速 | 高性能需求 |

#### MUX 使用说明

MUX 使用多条隧道连接承载客户端流量，默认关闭。客户端通过 `--tun-mux` 指定连接数，服务端会自动完成协商，无需单独设置连接数。

```bash
# 关闭 MUX
ppp --mode=client --config=appsettings.json --tun-mux=0

# 启用 4 条 MUX 连接，使用标准加速模式
ppp --mode=client --config=appsettings.json --tun-mux=4 --tun-mux-acceleration=0
```

`appsettings.json` 配置示例：

```json
"mux": {
    "connect": {
        "timeout": 20
    },
    "inactive": {
        "timeout": 60
    },
    "congestions": 134217728,
    "mode": "compat",
    "turbo": false,
    "keep-alived": [
        5,
        20
    ],
    "flow": {
        "reorder": {
            "bytes": 1048576,
            "timeout": 400
        }
    },
    "tx": {
        "queue": {
            "max": 4096,
            "stall": 8000
        }
    },
    "debug": {
        "key": "",
        "set-mode": ""
    }
}
```

| 配置项 | 功能 | 默认值 |
|--------|------|:------:|
| `connect.timeout` | MUX 连接建立超时，单位为秒 | `20` |
| `inactive.timeout` | MUX 空闲连接超时，单位为秒 | `60` |
| `congestions` | 单连接接收拥塞阈值，单位为字节；`0` 表示不限制 | `134217728` |
| `mode` | MUX 调度模式：`compat`、`flow`、`balance` 或 `stripe` | `compat` |
| `turbo` | flow 模式动态连接池；最多扩展到基础连接数的 3 倍 | `false` |
| `keep-alived` | 心跳间隔随机范围，单位为秒 | `[5, 20]` |
| `flow.reorder.bytes` | 每个业务流的最大乱序缓存，单位为字节 | `1048576` |
| `flow.reorder.timeout` | 等待缺失数据序号的超时，单位为毫秒 | `400` |
| `tx.queue.max` | 数据发送队列高水位，达到后暂停继续读取 | `4096` |
| `tx.queue.stall` | 发送队列持续阻塞后重建 MUX 的时间，单位为毫秒 | `8000` |
| `debug.key` | 远程切换 MUX 模式的共享密钥；空值表示禁用 | 空 |
| `debug.set-mode` | 启动后向对端发送一次模式切换请求 | 空 |

#### MUX 调度模式

| 模式 | 链路选择 | 接收排序 | 适用场景 |
|------|----------|----------|----------|
| `compat` | 空闲链路竞争 | 全局序号 | 与原版或旧版本互通，兼容性最高 |
| `flow` | 空闲链路竞争 | 默认全局序号；启用 turbo 后协商逐流序号 | 链路质量不同，或需要动态扩缩连接池 |
| `balance` | 空闲链路竞争 | 逐流序号，避免不同业务流互相阻塞 | 多连接并发、链路质量不同，推荐优先测试 |
| `stripe` | 按链路轮询分包 | 逐流序号 | 带宽和延迟接近的同质链路 |

客户端和服务端应配置相同的 `mux.mode`。`balance`、`stripe` 以及 `flow + turbo` 会通过 `ordering_caps` 协商 flow-v2；只有两端都支持时才启用逐流排序，否则自动退回全局兼容排序。与原版服务端或客户端互通时应使用：

```json
"mode": "compat",
"turbo": false
```

启用 balance：

```json
"mode": "balance",
"turbo": false
```

启用 flow 动态连接池：

```json
"mode": "flow",
"turbo": true
```

> 建议先从 `--tun-mux=2` 或 `--tun-mux=4` 开始测试。连接数越多，连接建立、心跳和服务器资源开销也越大，不一定能继续提高速度。不需要 MUX 时只需设置 `--tun-mux=0`，无需删除配置文件中的 `mux` 段。

> `turbo` 仅在 `flow` 模式生效，会根据发送队列和链路活跃度动态增加或回收载波连接。该功能涉及运行期连接扩缩，建议先在测试环境验证断线重连、长连接和高并发场景。

#### 虚拟网卡默认值
| 平台 | 默认值 |
|------|--------|
| Windows | `PPP` |
| Linux | `ppp` |
| macOS | `utun0` |

#### SSMT 优化模式
| 模式 | 优化方向 |
|------|----------|
| st | 单连接大流量 |
| mq | 多连接高并发 |

#### 网络协议栈
| 类型 | 说明 |
|:--------:|---------|
| `lwip` | 适用于 Windows |
| `ctcp` | 适用于非 Windows |

---

## 📋 CLI 启动示例

隧道类型、服务器地址、加密密钥等核心配置均写入 `appsettings.json`，DNS、网关、分流文件等也有默认值自动加载。CLI 参数仅用于按需覆盖或微调。

### 客户端模式

```bash
# 最简启动（全部使用 appsettings.json 默认值）
ppp --mode=client
```

一个贴近实际使用的客户端启动示例：

```bash
start ppp.exe --mode=client --config=./config/HKBN.json --tun-mux=0 --tun-host=yes --tun-vnet=yes --tun-gw=192.168.12.0 --tun-ip=192.168.12.25 --tun-flash=yes --tun-mask=24 --link-restart=3 --tun-static=no --block-quic=yes
```

> `--tun-gw`、`--tun-ip`、`--tun-mask` 覆盖配置文件中服务器分配的 IP，实现固定内网 IP。`--link-restart=3` 断开后自动重连 3 次。分流的 `--bypass` / `--bypass6` / `--dns-rules` / `--dns` 如不指定则使用 `appsettings.json` 中的默认值，通常无需在 CLI 重复。

### 服务端模式

```bash
# mode 默认 server
./ppp --mode=server

# 指定配置文件
./ppp --mode=server --config=./server.json
```

### 通用参数

```bash
# 更换配置文件
ppp --mode=client --config=./my-config.json

# 自动重启（崩溃/断开后自动拉起）
ppp --mode=client --auto-restart=300

# 查看帮助
ppp --help
```

> **说明**：`appsettings.json` 中的 `client.server` 支持 `ppp://`、`ws://`、`wss://` 三种协议，切换隧道类型只需修改此字段，无需改动 CLI 命令。更多 CLI 参数参考上游 [命令行接口文档](https://github.com/liulilittle/openppp2/blob/main/README_CN.md#-%E5%91%BD%E4%BB%A4%E8%A1%8C%E6%8E%A5%E5%8F%A3)。

---

## 🔍 Debug 日志版本

Release 构建（默认）只输出 TUI 仪表盘，不输出调试日志。**Debug 构建**额外启用 `PPP_LOG_VERBOSE` 宏，输出详细的 `LOG_DEBUG` / `LOG_INFO` 日志，用于排查连接、路由、DNS 等问题。

### Release vs Debug

| | Release | Debug |
|---|---|---|
| 编译宏 | `-O3` | `-D_DEBUG -DPPP_LOG_VERBOSE -g3` |
| 优化 | 全量优化 | 无优化 |
| 日志输出 | 仅仪表盘 TUI | 仪表盘 + 详细调试日志 |
| 文件体积 | 较小 | 较大（含调试符号） |
| 适用场景 | 生产部署 | 问题排查 |

### 获取 Debug 构建

GitHub Actions 为每个平台同时构建 Release 和 Debug 版本，在 [Releases](https://github.com/picetor/openppp2/releases) 页面中文件名带 `debug` 的即为 Debug 构建：

```
openppp2-windows-x64.zip          ← Release
openppp2-windows-x64-debug.zip    ← Debug
openppp2-linux-amd64.zip          ← Release
openppp2-linux-amd64-debug.zip    ← Debug
...
```

### `--log-file` 使用

Debug 构建支持 `--log-file` 参数将调试日志写入文件（Release 构建此参数无效果）：

```bash
# Debug 构建：日志写入文件
./ppp --mode=client --log-file ./ppp_debug.log

# 实时查看
tail -f ./ppp_debug.log
```

> **注意**：仪表盘 TUI 始终输出到控制台，`--log-file` 只重定向 `LOG_DEBUG` / `LOG_INFO` 等调试日志。两者互不干扰。

---

## 🔗 隧道协议配置

openppp2 支持三种隧道传输协议：**PPP**（原生 TCP）、**WS**（WebSocket）、**WSS**（WebSocket over TLS）。切换协议只需修改 `appsettings.json` 中 `client.server` 的值，CLI 始终用 `--mode=client` 即可。

### PPP（原生 TCP 直连）

最简单的部署方式，适合内网或直连场景。

**appsettings.json**：
```json
{
    "tcp": {
        "listen": { "port": 20000 }
    },
    "client": {
        "server": "ppp://服务器IP:20000/"
    }
}
```

### WS（WebSocket 无加密）

WebSocket 隧道，可配合 CDN / 反向代理，无 TLS 加密。

**appsettings.json**：
原始域名
```json
{
    "websocket": {
        "host": "your-domain.com",
        "path": "/tun",
        "listen": { "ws": 20080 }
    },
    "client": {
        "server": "ws://your-domain.com:20080/tun"
    }
}
```
---
优选域名/IP
```json
{
    "websocket": {
        "host": "your-domain.com",
        "path": "/tun",
        "listen": { "ws": 20080 }
    },
    "client": {
        "server": "ws://IP:port/tun",
        "websocket": {
            "host": "your-domain.com"
        }
    }
}
```
### WSS（WebSocket over TLS）

生产推荐方案，加密传输，支持 CDN 优选 IP 加速。

**appsettings.json**：
```json
{
    "websocket": {
        "host": "your-domain.com",
        "path": "/tun",
        "listen": {
            "ws": 20080,
            "wss": 20443
        },
        "ssl": {
            "certificate-file": "your-domain.com.pem",
            "certificate-key-file": "your-domain.com.key",
            "ciphersuites": "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256"
        }
    },
    "client": {
        "server": "wss://your-domain.com:20443/tun"
    }
}
```
---
CDN 优选 IP 场景需额外配置 `client.websocket`：
```json
{
    "client": {
        "server": "wss://IP:port/tun",
        "websocket": {
            "host": "your-domain.com",
            "sni": "your-domain.com"
        }
    }
}
```

> 无论哪种协议，客户端启动命令统一为 `ppp --mode=client`，服务器端为 `./ppp`。

### 协议对比

| | PPP | WS | WSS |
|---|---|---|---|
| 加密 | AES 应用层加密 | AES 应用层加密 | TLS + AES 双层 |
| 端口 | 自定义 | 80/自定义 | 443/自定义 |
| CDN | ❌ | ✅ | ✅ |
| 伪装 | ❌ | HTTP 头伪装 | HTTPS 伪装 |
| 推荐场景 | 内网/直连 | 内网穿透 | 生产公网 |

> PPP 模式虽然不走 TLS，但数据仍然经过应用层 AES 加密（由 `key.protocol` / `key.transport` 控制）。WSS 模式在此基础上增加了 TLS 传输层加密。

---

## 🏗️ 构建系统

本分支的 CI/CD 包含 8 个工作流（4 Release + 4 Debug ），覆盖 Windows / Linux (amd64/aarch64) / macOS (arm64/amd64)。


| 平台 | 架构 | 构建类型 |
|------|------|----------|
| Windows | x64 | Release / Debug |
| Linux | amd64 (7 variants) | Release / Debug |
| Linux | aarch64 (4 variants) | Release / Debug |
| macOS | arm64 + amd64 | Release / Debug |

> 隧道协议、路由策略和 PaperAirplane 等配置可参考上游文档。MUX 的启用方法和当前可用模式请参考上方“MUX 使用说明”。

---

## 🔗 关联项目

本仓库与以下项目协同工作，覆盖规则生成、一键部署、IP 数据源等环节。

### DNS 规则生成 — `dns-rules_geosite_generator`

[dns-rules_geosite_generator](https://github.com/picetor/dns-rules_geosite_generator) 将 [MetaCubeX/meta-rules-dat](https://github.com/MetaCubeX/meta-rules-dat) 的 geosite 分类数据转换为本项目的 `dns-rules.txt` 绕过列表。

```
geosite 分类 (MetaCubeX)  ──→  geosite2dns.py  ──→  dns-rules.txt
```

- 支持 GitHub 源文件、geosite.dat Protobuf、mosdns 解包三种数据源
- 通过 YAML 映射配置定义分类 → DNS 的对应关系
- 输出格式与 openppp2 完全兼容（83 bytes 定长记录）

```bash
# 推荐方式：直连 GitHub 源文件（支持 @cn 子分类）
python geosite2dns.py -m geosite-mapping.yaml -o dns-rules.txt --from-source
```

### 一键安装 — `openppp2_install`

[openppp2_install](https://github.com/picetor/openppp2_install) 提供两套部署脚本：

| 脚本 | 用途 |
|------|------|
| `ppp_install.sh` | 单模式 — 一台机器只装服务端或只装客户端 |
| `ppp_dual.sh` | 双模式 — 同机同时运行服务端 + 客户端 |

内置 systemd 服务管理、tmux TUI 状态面板、智能架构/版本检测。安装后可执行 `ppp` 进入管理菜单。

```bash
# 一键安装（单模式）
wget -4 -O ppp_install.sh https://raw.githubusercontent.com/picetor/openppp2_install/main/ppp_install.sh && chmod +x ppp_install.sh && ./ppp_install.sh
```

### IP 分流数据来源

`ip.txt`（IPv4 国内地址）和 `ipv6.txt`（IPv6 国内地址）分流列表来源于 [mayaxcn/china-ip-list](https://github.com/mayaxcn/china-ip-list)：

| 文件 | 来源 | 用途 |
|------|------|------|
| `ip.txt` | [APNIC](http://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest) | IPv4 国内地址，`--bypass` 分流 / `--pull-iplist` 自动生成 |
| `ipv6.txt` | [chnroute_v6.txt](https://github.com/mayaxcn/china-ip-list/blob/master/chnroute_v6.txt) | IPv6 国内地址，`--bypass6` 分流 |

> 更新分流列表时，直接从上述链接下载覆盖仓库中的对应文件即可。

### IPv6 参考实现 — `openppp2_Miaocchi`

本分支的部分 IPv6 功能借鉴了 [Miaocchi/openppp2](https://github.com/Miaocchi/openppp2) 的实现与文档：

- **IPv6 修复汇总**：[`docs/IPV6_FIXES.md`](https://github.com/Miaocchi/openppp2/blob/main/docs/IPV6_FIXES.md) — 系统性地审查了 `ppp/` 核心与平台目录中所有 IPv6 相关代码（socket 创建、地址解析、VNetstack 处理、IPv6Auxiliary 层），梳理了 VPN 传输层与虚拟以太网层两层 IPv6 的边界
- **Windows IPv6 DNS 防泄漏** & **源地址选择修复** 等方案的思路参考了该分支对 Windows 平台 IPv6 行为的分析
- **IPv6 租约管理**与**NDP 代理**等服务器端方案的设计文档为该分支的 IPv6 数据面实现提供了参考
