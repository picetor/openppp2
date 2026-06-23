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

> For detailed build environment and dependency清单, see [`环境需求.md`](环境需求.md)

---

## 📋 Table of Contents

- [Tunnel Protocols Overview](#-tunnel-protocols-overview)
- [PPP Native Tunnel](#-ppp-native-tunnel)
- [WebSocket Tunnel (WS)](#-websocket-tunnel-ws)
- [WebSocket Secure Tunnel (WSS)](#-websocket-secure-tunnel-wss)
- [Optimized IP + WSS Acceleration](#-optimized-ip--wss-acceleration)
- [Server Configuration](#-server-configuration)
- [Client Configuration](#-client-configuration)
- [Quick Start](#-quick-start)
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
    "server": {
        "listen": { "ppp": 20000 },
        "ip": "10.0.0.1",
        "netmask": "255.255.255.0"
    }
}
```

### Client

```json
{
    "client": {
        "server": "ppp://your-server.com:20000/",
        "ip": "10.0.0.2",
        "netmask": "255.255.255.0"
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
    "server": {
        "listen": { "ws": 20080 },
        "websocket": {
            "host": "your-domain.com",
            "path": "/tun"
        },
        "ip": "10.0.0.1",
        "netmask": "255.255.255.0"
    }
}
```

### Client

```json
{
    "client": {
        "server": "ws://your-server.com:20080/tun",
        "ip": "10.0.0.2",
        "netmask": "255.255.255.0"
    }
}
```

> **Use case**: Server behind CDN, no TLS needed (data encrypted by PPP layer AES).

---

## 🔒 WebSocket Secure Tunnel (WSS)

WSS = WebSocket over TLS, adding TLS encryption on top of WS for double-layer security.

### Server

```json
{
    "server": {
        "listen": { "wss": 20443 },
        "websocket": {
            "host": "your-domain.com",
            "path": "/tun",
            "ssl": {
                "certificate-file": "your-domain.com.pem",
                "certificate-key-file": "your-domain.com.key",
                "certificate-key-password": "",
                "ciphersuites": "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256"
            }
        },
        "ip": "10.0.0.1",
        "netmask": "255.255.255.0"
    }
}
```

### Client

```json
{
    "client": {
        "server": "wss://your-domain.com:20443/tun",
        "ip": "10.0.0.2",
        "netmask": "255.255.255.0"
    }
}
```

> **Use case**: TLS encrypted transport through CDN, balancing security and speed.

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
        },
        "ip": "10.0.0.2",
        "netmask": "255.255.255.0"
    }
}
```

| Field | Description |
|-------|-------------|
| `server` | Set to the optimized IP address (not domain) |
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

## 🖥️ Server Configuration

### Full Server Configuration Reference

```json
{
    "server": {
        "listen": {
            "ppp": 20000,
            "ws": 20080,
            "wss": 20443
        },
        "ip": "10.0.0.1",
        "netmask": "255.255.255.0",
        "log": "./ppp-server.log",
        "websocket": {
            "host": "your-domain.com",
            "path": "/tun",
            "ssl": {
                "certificate-file": "your-domain.com.pem",
                "certificate-key-file": "your-domain.com.key",
                "certificate-key-password": "",
                "ciphersuites": "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256"
            },
            "verify-peer": false,
            "http": {
                "error": "Status Code: 404; Not Found",
                "request": {
                    "Cache-Control": "no-cache",
                    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
                },
                "response": {
                    "Server": "Kestrel"
                }
            }
        }
    }
}
```

### Start Server

```bash
# Start server (reads appsettings.json by default)
./ppp --server

# Specify config file
./ppp --server --config /path/to/appsettings.json
```

---

## 💻 Client Configuration

### Full Client Configuration Reference

```json
{
    "client": {
        "guid": "{F4569208-BB45-4DEB-B115-0FEA1D91B85B}",
        "server": "ppp://192.168.0.24:20000/",
        "server-proxy": "http://user:pass@192.168.0.18:8080/",
        "bandwidth": 10000,
        "log": "./ppp-client.log",
        "websocket": {
            "host": "",
            "sni": ""
        },
        "reconnections": {
            "timeout": 5
        },
        "paper-airplane": {
            "tcp": true
        },
        "http-proxy": {
            "bind": "192.168.0.24",
            "port": 8080
        },
        "socks-proxy": {
            "bind": "192.168.0.24",
            "port": 1080,
            "username": "test",
            "password": "123456"
        },
        "mappings": [
            {
                "local-ip": "192.168.0.24",
                "local-port": 80,
                "protocol": "tcp",
                "remote-ip": "::",
                "remote-port": 10001
            },
            {
                "local-ip": "192.168.0.24",
                "local-port": 7000,
                "protocol": "udp",
                "remote-ip": "::",
                "remote-port": 10002
            }
        ],
        "routes": [
            {
                "name": "CMNET",
                "nic": "eth1",
                "ngw": "192.168.1.1",
                "path": "./cmcc.txt",
                "vbgp": "https://ispip.clang.cn/cmcc.txt"
            }
        ]
    }
}
```

### Start Client

```bash
# Start client
./ppp --client

# Specify config file
./ppp --client --config /path/to/appsettings.json
```

---

## 🚀 Quick Start

### 1. Download Pre-built Binary

Download from [Releases](https://github.com/picetor/openppp2/releases) or the `builds/` directory.

### 2. Prepare Configuration

```bash
# Download appsettings.json
wget https://raw.githubusercontent.com/picetor/openppp2/master/appsettings.json

# Edit configuration (refer to tunnel sections above)
vim appsettings.json
```

### 3. Run Server

```bash
chmod +x ppp
./ppp --server
```

### 4. Run Client

```bash
./ppp --client
```

> For detailed routing configuration,旁路由 mode, and Windows platform guides, refer to the original documentation at [liulilittle/openppp2](https://github.com/liulilittle/openppp2).

---

## 🔨 Build Guide

### Original Build Checklist [Original Code]

For the complete build environment, third-party dependency building, GLIBC compatibility layer, CMake configuration and build steps of the original code, see:
> 📄 **[`releases环境需求清单.md`](releases环境需求清单.md)** — Original release build environment checklist (debug logging disabled)

### WSS Modified Checklist [Modified Code]

For the build environment checklist of this `master` branch fork, adapted for fully static linking, GLIBC compatibility layer, conditional compilation, multi-variant builds, etc., see:
> 📄 **[`WSS修改版环境需求.md`](WSS修改版环境需求.md)** — WSS modified build environment checklist (debug logging retained)
