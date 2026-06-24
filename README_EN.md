# <img src="https://img.icons8.com/color/48/000000/vpn.png" width="30" height="30"> PPP PRIVATE NETWORK™ 2  
**Enterprise Virtual Ethernet VPN Solution**  
Next-generation secure network access technology providing high-performance virtual ethernet tunneling  

<div align="right" style="margin-top:-40px;">
  <kbd style="background:#0366d6;">
    <strong>English</strong>
  </kbd>
  <kbd>
    <a href="README.md">简体中文</a>
  </kbd>
</div>

---

## ⚠️ About This Branch

This `master` branch is a **modified fork** based on upstream [liulilittle/openppp2](https://github.com/liulilittle/openppp2) `main` branch.

**Key modifications:**
- Fully static linking, no system dynamic library dependencies
- GLIBC compatibility layer for older systems
- Conditional compilation (ENABLE_IO_URING / ENABLE_TC / __SIMD__)
- One-click multi-variant build (amd64 × 8 + arm64 × 4)
- Added `client.websocket.host` / `client.websocket.sni` for optimized IP support

> For detailed build environment and dependency lists, see [`环境需求.md`](环境需求.md) and [`WSS修改版环境需求.md`](WSS修改版环境需求.md)

---

## 📋 Table of Contents

- [Tunnel Protocols Overview](#-tunnel-protocols-overview)
- [PPP Native Tunnel](#-ppp-native-tunnel)
- [WebSocket Tunnel (WS)](#-websocket-tunnel-ws)
- [WebSocket Secure Tunnel (WSS)](#-websocket-secure-tunnel-wss)
- [Optimized IP + WSS Acceleration](#-optimized-ip--wss-acceleration)
- [Build Guide](#-build-guide)
- [Appendix](#-appendix)

---

## 🚇 Tunnel Protocols Overview

openppp2 supports multiple tunnel protocols for different scenarios:

| Protocol | Transport | Encryption | CDN Friendly | Use Case |
|----------|-----------|------------|--------------|----------|
| **PPP** | TCP | AES | ❌ | Direct server connection, best performance |
| **WS** | TCP + WebSocket | AES | ✅ | CDN proxy, WebSocket encapsulation |
| **WSS** | TCP + TLS + WebSocket | TLS + AES | ✅ | CDN proxy, TLS encrypted transport |

> All tunnels support MUX multiplexing, PaperAirplane TCP acceleration, and virtual ethernet layer.

---

## 🔌 PPP Native Tunnel

PPP is openppp2's native tunnel protocol, based on direct TCP connection for best performance.

### Server

```json
{
    "tcp": {
        "listen": {
            "port": 20000
        }
    }
}
```

### Client

```json
{
    "client": {
        "server": "ppp://your-server.com:20000/"
    }
}
```

> **Use case**: Server has a public IP, no CDN needed,追求 maximum performance.

---

## 🌐 WebSocket Tunnel (WS)

WebSocket tunnel encapsulates PPP traffic in WebSocket protocol, recognizable and forwardable by CDN.

### Server

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

### Client

```json
{
    "client": {
        "server": "ws://your-domain.com:20080/tun"
    }
}
```

> **Use case**: Server behind CDN, no TLS needed (data encrypted by PPP layer AES).
Add your domain to Cloudflare with proxy (orange cloud) enabled, then create an Origin Rule to route traffic back to port 20080.

---

## 🔒 WebSocket Secure Tunnel (WSS)

WSS = WebSocket over TLS, adding TLS encryption on top of WS for double-layer security.

### Server

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

### Client

```json
{
    "client": {
        "server": "wss://your-domain.com:20443/tun"
    }
}
```

> **Use case**: TLS encrypted transport through CDN, balancing security and speed.
Add your domain to Cloudflare with proxy (orange cloud) enabled, obtain a certificate via acme, set SSL to Full (Strict), configure the certificate, then create an Origin Rule to route traffic back to port 20443.

---

## ⚡ Optimized IP + WSS Acceleration

> This is the core new feature of this fork. The `client.websocket.host` and `client.websocket.sni` fields enable optimized IP connections with correct CDN routing.

### How It Works

```
Client → Optimized IP (CDN Edge) → CDN Internal Routing → Your Origin Server
                │
                ├─ Host header: your-domain.com  (WebSocket handshake)
                └─ SNI:         your-domain.com  (TLS handshake)
```

Connect to an optimized IP while using custom Host and SNI fields to make the CDN route traffic correctly to your server.

### Client Configuration

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

| Field | Description |
|-------|-------------|
| `server` | Fill in the optimized IP or domain name, the key change of this WSS tunnel |
| `websocket.host` | WebSocket Host header, set to your real domain |
| `websocket.sni` | TLS SNI field, set to your real domain |

### Usage Scenarios

**Scenario 1: Optimized IP + WSS (Recommended)**
```json
{
    "client": {
        "server": "wss://优选IP:20443/tun",
        "websocket": { "host": "your-domain.com", "sni": "your-domain.com" }
    }
}
```

**Scenario 2: Optimized IP + WS (No TLS)**
```json
{
    "client": {
        "server": "ws://优选IP:20080/tun",
        "websocket": { "host": "your-domain.com" }
    }
}
```

**Scenario 3: Direct Domain Connection (No Optimized IP)**
```json
{
    "client": {
        "server": "wss://your-domain.com:20443/tun",
        "websocket": { "host": "", "sni": "" }
    }
}
```
> When `host` and `sni` are empty, behavior is identical to the original version.

---

## � Build Guide

> For detailed routing configuration,旁路由 mode, and Windows platform guides, refer to the original documentation at [liulilittle/openppp2](https://github.com/liulilittle/openppp2).

This project includes two build environment checklists, both aligned with CI workflows and targeting Release builds (`-O3` optimization):

| Checklist | Based On | Description |
|-----------|----------|-------------|
| 📄 [`环境需求.md`](环境需求.md) | Original [liulilittle/openppp2](https://github.com/liulilittle/openppp2) | Includes appendix of modifications from original |
| 📄 [`WSS修改版环境需求.md`](WSS修改版环境需求.md) | This repo [picetor/openppp2](https://github.com/picetor/openppp2) | Based on this repo's master branch |
