# <img src="https://img.icons8.com/color/48/000000/vpn.png" width="30" height="30"> PPP PRIVATE NETWORK™ 2  
**企业级虚拟以太网 VPN 解决方案**  
下一代安全网络接入技术，提供高性能虚拟以太网隧道服务  

<div align="right" style="margin-top:-40px;">
  <kbd>
    <a href="README_EN.md">English</a>
  </kbd>
  <kbd style="background:#0366d6;">
    <strong>简体中文</strong>
  </kbd>
</div>

---

## ⚠️ 本分支说明

本仓库 `master` 分支为**修改版**，基于上游 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) 的 `main` 分支。

**核心修改：**
- 完全静态链接，不依赖系统动态库
- GLIBC 兼容层，支持旧系统运行
- 条件编译（ENABLE_IO_URING / ENABLE_TC / __SIMD__）
- 多变体一键构建（amd64 × 8 + arm64 × 4）
- 新增 `client.websocket.host` / `client.websocket.sni` 优选 IP 支持

> 详细编译环境与依赖清单请参见 `编译指南` 及 [`环境需求.md`](环境需求.md) 和 [`WSS修改版环境需求.md`](WSS修改版环境需求.md)

---

## 📋 目录

- [隧道协议总览](#-隧道协议总览)
- [PPP 原生隧道](#-ppp-原生隧道)
- [WebSocket 隧道 (WS)](#-websocket-隧道-ws)
- [WebSocket 安全隧道 (WSS)](#-websocket-安全隧道-wss)
- [优选 IP + WSS 加速](#-优选-ip--wss-加速)
- [编译指南](#-编译指南)
- [附录](#-附录)

---

## 🚇 隧道协议总览

openppp2 支持多种隧道协议，适用于不同场景：

| 协议 | 传输层 | 加密 | CDN 友好 | 适用场景 |
|------|--------|------|----------|----------|
| **PPP** | TCP | AES 加密 | ❌ | 直连服务器，高性能 |
| **WS** | TCP + WebSocket | AES 加密 | ✅ | CDN 转发，WebSocket 封包 |
| **WSS** | TCP + TLS + WebSocket | TLS + AES 双重加密 | ✅ | CDN 转发，TLS 加密传输 |

> 所有隧道均支持 MUX 多路复用、PaperAirplane TCP 加速、虚拟以太网层。

---

## 🔌 PPP 原生隧道

PPP 协议是 openppp2 的原生隧道协议，基于 TCP 直连，性能最佳。

### 服务端

```json
{
    "tcp": {
        "listen": {
            "port": 20000
        }
    }
}
```

### 客户端

```json
{
    "client": {
        "server": "ppp://your-server.com:20000/"
    }
}
```

> **适用场景**：服务器有公网 IP，无需 CDN 中转，追求极致性能。

---

## 🌐 WebSocket 隧道 (WS)

WebSocket 隧道将 PPP 流量封装在 WebSocket 协议中，可被 CDN 识别和转发。

### 服务端

```json
{
    "websocket": {
        "host": "your-domain.com",
        "path": "/tun",
        "listen": {
            "ws": 20080
        }
    }
}
```

### 客户端

```json
{
    "client": {
        "server": "ws://your-domain.com:20080/tun"
    }
}
```

> **适用场景**：服务器通过 CDN 转发，无需 TLS 加密（WS 本身无加密，数据由 PPP 层 AES 加密）。
Cloudflare添加域名开启小黄云代理，再在Origin Rules将域名回源到上方选择的端口20080


---

## 🔒 WebSocket 安全隧道 (WSS)

WSS = WebSocket over TLS，在 WS 基础上增加 TLS 加密层，双重加密更安全，且 CDN 广泛支持。

### 服务端

```json
{
    "websocket": {
        "host": "your-domain.com",
        "path": "/tun",
        "listen": {
            "wss": 20443
        },
        "ssl": {
            "certificate-file": "your-domain.com.pem",
            "certificate-key-file": "your-domain.com.key",
            "certificate-key-password": "",
            "ciphersuites": "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256"
        }
    }
}
```

### 客户端

```json
{
    "client": {
        "server": "wss://your-domain.com:20443/tun"
    }
}
```

> **适用场景**：需要 TLS 加密传输，通过 CDN 转发，兼顾安全与速度。
Cloudflare添加域名开启小黄云代理，使用acme申请证书，SSL开启完全/严格，配置填上证书，再在Origin Rules将域名回源到上方选择的端口20443

---

## ⚡ 优选 IP + WSS 加速

> 这是本修改版的核心新增功能。通过 `client.websocket.host` 和 `client.websocket.sni` 字段，实现优选 IP 连接 + CDN 正确路由。
> `client.websocket.host` 同时适用于 WS 和 WSS 两种隧道。

### 原理

```
客户端 → 优选 IP (CDN 边缘节点) → CDN 内部路由 → 你的源服务器
                │
                ├─ Host 头: your-domain.com  (WebSocket 握手)
                └─ SNI:    your-domain.com  (TLS 握手)
```

连接优选 IP 的同时，通过自定义 Host 和 SNI 字段，让 CDN 将流量正确转发到你的服务器。

### 客户端配置

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
| `server` | 填写优选IP或优选域名，本WSS隧道改动的重点 |
| `websocket.host` | WebSocket Host 头，填写真实域名 |
| `websocket.sni` | TLS SNI 字段，填写真实域名 |

### 场景示例

**场景一：优选 IP + WSS（推荐）**
```json
{
    "client": {
        "server": "wss://优选IP:20443/tun",
        "websocket": { "host": "your-domain.com", "sni": "your-domain.com" }
    }
}
```

**场景二：优选 IP + WS（无 TLS）**
```json
{
    "client": {
        "server": "ws://优选IP:20080/tun",
        "websocket": { "host": "your-domain.com" }
    }
}
```

**场景三：域名直连（无需优选 IP）**
```json
{
    "client": {
        "server": "wss://your-domain.com:20443/tun",
        "websocket": { "host": "", "sni": "" }
    }
}
```
> `host` 和 `sni` 为空时，行为与原版一致，使用 URL 中的主机名。

---

## 编译指南

> 详细的路由配置、旁路由模式、Windows 平台指南请参见原版文档或 [liulilittle/openppp2](https://github.com/liulilittle/openppp2)。

本项目涉及两份编译环境清单，均以 CI 工作流为准，构建目标为 Release（`-O3` 优化）：

| 清单 | 基于代码 | 说明 |
|------|----------|------|
| 📄 [`环境需求.md`](环境需求.md) | 原版 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) | 含相比原版的修改附录 |
| 📄 [`WSS修改版环境需求.md`](WSS修改版环境需求.md) | 本项目 [picetor/openppp2](https://github.com/picetor/openppp2) | 基于本仓库 master 分支 |
