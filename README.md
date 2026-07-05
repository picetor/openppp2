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

- [IPv6 特性总览](#-ipv6-特性总览)
  - [IPv6 分流 (--bypass6)](#-ipv6-分流---bypass6)
  - [VPN 服务器 IPv6 连通性保证](#-vpn-服务器-ipv6-连通性保证)
  - [Windows IPv6 DNS 防泄漏](#-windows-ipv6-dns-防泄漏)
  - [Windows IPv6 源地址选择修复](#-windows-ipv6-源地址选择修复)
  - [服务器端 IPv6 模式](#-服务器端-ipv6-模式)
  - [IPv6 DNS 原生支持](#-ipv6-dns-原生支持)
- [WSS 优选 IP 加速](#-wss-优选-ip-加速)
- [SOCKS5 代理](#-socks5-代理)
- [改进与修复](#-改进与修复)
- [CLI 参数对比](#-cli-参数对比)
- [构建系统](#-构建系统)

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
ppp --mode=client --server=wss://... --bypass-ngw6=fe80::1

# 自定义分流文件 + 网关
ppp --mode=client --server=wss://... \
     --bypass6=./ipv6.txt \
     --bypass-ngw6=fe80::1

# Linux 指定物理网卡
ppp --mode=client --server=wss://... \
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

**问题**：VPN 连接时，物理网卡的 IPv6 DNS 服务器仍然可用，导致 IPv6 DNS 查询绕过
 VPN 隧道，造成 DNS 泄漏。

**修复**：
- VPN **连接时**：自动扫描所有物理网卡的 IPv6 DNS → 临时清除
- VPN **断开时**：自动恢复所有物理网卡的 IPv6 DNS

无需额外配置，自动生效。

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

### IPv6 DNS 原生支持

`--dns=` 参数原生支持 IPv6 DNS 地址：

```bash
ppp --mode=client --server=wss://... --dns=1.1.1.1,8.8.8.8,2606:4700:4700::1111,2001:4860:4860::8888
```

IPv6 DNS 通过 `ApplyIPv6Assignment()` → `ApplyClientDns()` 路径下发到 TAP 适配器，与 IPv4 DNS (`--dns=`) 互不冲突。

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
        "server": "wss://优选IP:20443/tun",
        "websocket": {
            "host": "your-domain.com",
            "sni": "your-domain.com"
        }
    }
}
```

| 字段 | 说明 |
|------|------|
| `server` | 填写优选 IP，而非真实域名 |
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

| 参数 | 支持平台 | 说明 |
|------|----------|------|
| `--bypass6=<file1\|file2>` | 全平台 | IPv6 分流列表 |
| `--bypass-nic6=<interface>` | Linux | IPv6 分流物理网卡 |
| `--bypass-ngw6=<ip>` | 全平台 | IPv6 分流网关 |

---

## 🏗️ 构建系统

本分支的 CI/CD 包含 8 个工作流（4 Release + 4 Debug ），覆盖 Windows / Linux (amd64/aarch64) / macOS (arm64/amd64)。


| 平台 | 架构 | 构建类型 |
|------|------|----------|
| Windows | x64 | Release / Debug |
| Linux | amd64 (7 variants) | Release / Debug |
| Linux | aarch64 (4 variants) | Release / Debug |
| macOS | arm64 + amd64 | Release / Debug |

> 其他配置项（隧道协议、路由策略、MUX 多路复用、PaperAirplane 加速等）与原版一致，请参考上游文档。
