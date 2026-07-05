# 🔐 PPP PRIVATE NETWORK™ 2 — Fork Features

<div align="right" style="margin-top:-40px;">
  <kbd>
    <a href="README.md">简体中文</a>
  </kbd>
  <kbd style="background:#0366d6;">
    <strong>English</strong>
  </kbd>
</div>

---

> This fork is based on the upstream [liulilittle/openppp2](https://github.com/liulilittle/openppp2) `main` branch.
> This document lists **only the features and usage that differ from the original**. For existing features (tunnel protocols, routing policies, server configuration, etc.), please refer to the upstream documentation.

---

## 📋 Table of Contents

- [IPv6 Feature Overview](#-ipv6-feature-overview)
  - [IPv6 Split Tunneling (--bypass6)](#-ipv6-split-tunneling---bypass6)
  - [VPN Server IPv6 Reachability Guarantee](#-vpn-server-ipv6-reachability-guarantee)
  - [Windows IPv6 DNS Leak Prevention](#-windows-ipv6-dns-leak-prevention)
  - [Windows IPv6 Source Address Selection Fix](#-windows-ipv6-source-address-selection-fix)
  - [Server-Side IPv6 Mode](#-server-side-ipv6-mode)
  - [Native IPv6 DNS Support](#-native-ipv6-dns-support)
- [WSS Optimized IP Acceleration](#-wss-optimized-ip-acceleration)
- [SOCKS5 Proxy](#-socks5-proxy)
- [Improvements & Bug Fixes](#-improvements--bug-fixes)
- [CLI Parameters Comparison](#-cli-parameters-comparison)
- [Build System](#-build-system)

---

## 🌐 IPv6 Feature Overview

This fork adds comprehensive IPv6 split tunneling, DNS leak prevention, and source address selection fixes, covering **client-side routing** and **Windows platform compatibility**.

### IPv6 Split Tunneling (`--bypass6`)

Pure **route-level** IPv6 split tunneling via the OS routing table — no packet inspection involved.

```
Traffic flow:
  Domestic IPv6 (ipv6.txt)  ──→ Physical NIC (bypass)
  All other IPv6 (::/0)    ──→ TUN tunnel (VPN)
```

#### Usage

```bash
# Basic (uses default ipv6.txt)
ppp --mode=client --server=wss://... --bypass-ngw6=fe80::1

# Custom bypass list + gateway
ppp --mode=client --server=wss://... \
     --bypass6=./ipv6.txt \
     --bypass-ngw6=fe80::1

# Linux: specify physical NIC
ppp --mode=client --server=wss://... \
     --bypass6=./ipv6.txt \
     --bypass-nic6=eth0 \
     --bypass-ngw6=fe80::1
```

| Parameter | Description | Default |
|-----------|-------------|---------|
| `--bypass6=<file1\|file2>` | IPv6 bypass list file | `./ipv6.txt` |
| `--bypass-nic6=<interface>` | (Linux) Physical NIC name | auto-select |
| `--bypass-ngw6=<ip>` | IPv6 next-hop gateway | `::` (bypass disabled) |

> **Note**: When `--bypass-ngw6` is omitted, no IPv6 bypass is configured — all v6 traffic goes through the TUN tunnel, identical to the original behavior.

#### ipv6.txt format

```
2400:da00::/32
2401:fa00::/32
# Comments start with # or ;
```

#### Platform Route Commands

| Platform | Route Command |
|----------|---------------|
| Windows | `CreateIpForwardEntry2` (IP Helper API) — no popups, not `system("netsh")` |
| Linux | `ip -6 route add <cidr> via <ngw6> dev <ifname>` |
| macOS | `route -n add -inet6 <cidr> <ngw6>` |

---

### VPN Server IPv6 Reachability Guarantee

**Problem**: After the VPN client installs the `::/0` default route via TAP, the VPN server's own IPv6 address becomes unreachable (routed into the tunnel) → UDP static echo timeout → reconnection loop.

**Fix**: Before installing the TAP default route, add a `/128` pin route for the VPN server's IPv6 address through the physical NIC. The `/128` is more specific than `::/0`, ensuring the server always stays reachable.

```
Installed route table:
  ::/0                    → TAP device (tunnel)
  <VPN-server-IPv6>/128   → Physical NIC (pin route)
```

Implemented on all platforms (Windows `netsh` / Linux `ip route` / macOS `route add`).

---

### 🛡️ Windows IPv6 DNS Leak Prevention

**Problem**: When the VPN is connected, physical NICs' IPv6 DNS servers are still reachable, causing IPv6 DNS queries to bypass the VPN tunnel.

**Fix**:
- On VPN **connect**: Automatically enumerates all physical NICs' IPv6 DNS → clears them temporarily
- On VPN **disconnect**: Automatically restores all physical NICs' IPv6 DNS

Works automatically — no extra configuration needed.

---

### 🎯 Windows IPv6 Source Address Selection Fix

**Problem**: Windows prefers global unicast addresses (2409::/...) over TAP's ULA addresses (fd42::/...) for outgoing connections, causing IPv6 traffic to bypass the VPN.

**Root Cause**: Windows routing table assigns `::/0` precedence 30, but `fc00::/7` (ULA) has only precedence 3.

**Fix**: Elevates `fd00::/8` prefix precedence to **50** (originally 3) on VPN connect, restores on disconnect.

---

### Server-Side IPv6 Mode

`appsettings.json` supports server-side IPv6 data plane configuration:

```json
{
    "server": {
        "ipv6": {
            "mode": "nat66"         // "nat66" | "gua" | "" (disabled)
        }
    }
}
```

| Mode | Description | Platform |
|------|-------------|----------|
| `nat66` | ULA↔GUA address translation (similar to IPv4 NAT) | Linux only |
| `gua` | Global unicast address passthrough | Linux only |
| empty/disabled | Disable IPv6 data plane | All |

> **Windows compatibility**: The original version refuses to load config when `server.ipv6.mode` is set on Windows. This fork auto-detects and disables the IPv6 data plane, allowing the config to load normally (client unaffected).

---

### Native IPv6 DNS Support

The `--dns=` parameter natively supports IPv6 DNS addresses:

```bash
ppp --mode=client --server=wss://... --dns=1.1.1.1,8.8.8.8,2606:4700:4700::1111,2001:4860:4860::8888
```

IPv6 DNS is delivered to the TAP adapter via `ApplyIPv6Assignment()` → `ApplyClientDns()`, coexisting with IPv4 DNS (`--dns=`) without conflict.

---

### ⚡ WSS Optimized IP Acceleration

Connect to a CDN optimized IP while preserving correct CDN routing via custom Host/SNI headers.

```
Client → Optimized IP (CDN Edge) → CDN Internal Routing → Your Origin Server
               │
               ├─ Host: your-domain.com  (WebSocket handshake)
               └─ SNI:  your-domain.com  (TLS handshake)
```

#### Usage

```json
{
    "client": {
        "server": "wss://optimized-ip:20443/tun",
        "websocket": {
            "host": "your-domain.com",
            "sni": "your-domain.com"
        }
    }
}
```

| Field | Description |
|-------|-------------|
| `server` | Use an optimized IP instead of your real domain |
| `websocket.host` | WebSocket Host header — set to your real domain |
| `websocket.sni` | TLS SNI field — set to your real domain |

`host` and `sni` work with both WS (`ws://`) and WSS (`wss://`) tunnels. When left empty, the behavior is identical to the original version.

---

### 🔌 SOCKS5 Proxy

Built-in SOCKS5 proxy server, complementing the TUN mode.

#### Usage

No additional configuration needed. The SOCKS5 proxy starts together with TUN by default. Its address appears in the startup log:

```
Socks Proxy           : 127.0.0.1:1080/socks
```

#### Bug Fixes vs Original

| Issue | Original Behavior | This Fork |
|-------|-------------------|-----------|
| **No CONNECT reply** | Tunnel established but no SOCKS5 success reply sent — client hangs | ✅ Correctly sends `REP=0` reply |
| **Domain port corruption** | Null terminator overwrites port high byte | ✅ Read port first, then null-terminate |
| **Auth logic error** | Uses `&&` instead of `\|\|` — only one of username/password needed to pass | ✅ `\|\|` — reject if either credential mismatches |

---

## 🔧 Improvements & Bug Fixes

| Improvement | Original | This Fork |
|-------------|----------|-----------|
| **Windows IPv6 routing** | Uses `system("netsh ...")` — may pop up GUI dialog causing hangs | ✅ Uses Windows IP Helper API (`CreateIpForwardEntry2`), no popups |
| **Route already exists** | `netsh` error "Object already exists" causes reconnect loop | ✅ `ERROR_OBJECT_ALREADY_EXISTS` treated as success |
| **Static linking** | Depends on system shared libraries | ✅ Fully static, single-file deployment |
| **GLIBC compatibility** | Requires modern GLIBC | ✅ Backward compatible with older GLIBC |
| **UDP echo socket** | Hardcoded IPv6, fails on systems without IPv6 | ✅ Auto-fallback to IPv4 |
| **Server IPv6 data plane** | Windows config load hangs on `server.ipv6.mode` | ✅ Auto-disables and continues loading |

---

## 📖 CLI Parameters Comparison

New CLI parameters (compared to the original):

| Parameter | Platform | Description |
|-----------|----------|-------------|
| `--bypass6=<file1\|file2>` | All | IPv6 bypass list file |
| `--bypass-nic6=<interface>` | Linux | Physical NIC for IPv6 bypass |
| `--bypass-ngw6=<ip>` | All | IPv6 bypass next-hop gateway |

---

## 🏗️ Build System

This fork's CI/CD includes 9 workflows (4 Release + 4 Debug + 1 Release packaging), covering Windows / Linux (amd64/aarch64) / macOS (arm64/amd64).

Each build automatically cleans up old workflow runs, keeping only the last 10 per workflow.

| Platform | Architecture | Build Type |
|----------|-------------|------------|
| Windows | x64 | Release / Debug |
| Linux | amd64 (7 variants) | Release / Debug |
| Linux | aarch64 (4 variants) | Release / Debug |
| macOS | arm64 + amd64 | Release / Debug |

> All other features (tunnel protocols, routing policies, MUX multiplexing, PaperAirplane acceleration, etc.) are identical to the original. Please refer to the upstream documentation.
