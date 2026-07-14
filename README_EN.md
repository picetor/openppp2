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
- [CLI Reference (Original)](#-cli-reference-original)
- [CLI Startup Examples](#-cli-startup-examples)
- [Debug Build](#-debug-build)
- [Tunnel Protocol Configuration](#-tunnel-protocol-configuration)
- [Build System](#-build-system)
- [Related Projects](#-related-projects)

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
# Client side — same as IPv4 bypass, no separate mode needed
ppp --mode=client --bypass6=./ipv6.txt

# Server side — broadcast RA to assign ULA prefix to client virtual NICs
ppp --bypass6=./ipv6.txt
```

> IPv6 bypass loads automatically when `./ipv6.txt` exists, even without the `--bypass6` parameter. IPv6 bypass shares the route bypass mechanism with `--bypass`.

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
ppp --mode=client --dns=1.1.1.1,8.8.8.8,2606:4700:4700::1111,2001:4860:4860::8888
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

#### Configuration — original domain

```json
{
    "client": {
        "server": "wss://your-domain.com:20443/tun"
    }
}
```
---
CDN optimized IP scenario — add `client.websocket`:
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

| Parameter | Platform | Description | Default |
|-----------|----------|-------------|:------:|
| `--bypass6=<file1\|file2>` | All | IPv6 bypass list file | `./ipv6.txt` |
| `--bypass-nic6=<interface>` | Linux | Physical NIC for IPv6 bypass | auto-select |
| `--bypass-ngw6=<ip>` | All | IPv6 bypass next-hop gateway | `::` (disabled) |

---

## 📖 CLI Reference (Original)

The complete upstream CLI reference — fully compatible with this fork.

### ⚙️ General Commands

| Command | Description | Format | Default |
|---------|-------------|--------|:------:|
| `--rt` | Real-time mode | `--rt=[yes｜no]` | `yes` |
| `--dns` | Set DNS servers | `--dns <IP list>` | `8.8.8.8,8.8.4.4` |
| `--tun-flash` | Advanced QoS control | `--tun-flash=[yes｜no]` | `no` |
| `--pull-iplist` | Download country IP list | `--pull-iplist [file]/[country]` | `./ip.txt/CN` |
| `--config` | Config file path | `--config <path>` | `./appsettings.json` |
| `--mode` | Run mode | `--mode=[client｜server]` | `server` |

> 🔗 **IP list source**: [APNIC official](http://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest)

---

### 🖥️ Server Commands

| Command | Description | Format | Default |
|---------|-------------|--------|:------:|
| `--firewall-rules` | Firewall rules file | `--firewall-rules <file>` | `./firewall-rules.txt` |

---

### 💻 Client Commands

#### Core Settings
| Command | Description | Format | Default |
|---------|-------------|--------|:------:|
| `--lwip` | Protocol stack selection | `--lwip=[yes｜no]` | Win: `yes`<br>Other: `no` |
| `--vbgp` | Smart route split | `--vbgp=[yes｜no]` | `yes` |
| `--nic` | Specify physical NIC | `--nic <name>` | auto-select |
| `--ngw` | Force gateway address | `--ngw <IP>` | auto-detect |

#### Virtual NIC
| Command | Description | Format | Default |
|---------|-------------|--------|:------:|
| `--tun` | NIC name | `--tun <name>` | platform-specific |
| `--tun-ip` | IP address | `--tun-ip <IP>` | `10.0.0.2` |
| `--tun-gw` | Gateway address | `--tun-gw <IP>` | `10.0.0.1` |
| `--tun-mask` | Subnet mask | `--tun-mask <bits>` | `30` |
| `--tun-host` | Preferred network | `--tun-host=[yes｜no]` | `yes` |

#### Advanced Features
| Command | Description | Format | Default |
|---------|-------------|--------|:------:|
| `--tun-mux` | MUX connections | `--tun-mux <count>` | `0` |
| `--tun-mux-acceleration` | MUX acceleration | `--tun-mux-acceleration <mode>` | `0` |
| `--tun-vnet` | Subnet forwarding | `--tun-vnet=[yes｜no]` | `yes` |
| `--tun-ssmt` | Hyper-threading optimization | `--tun-ssmt=[threads]/[mode]` | `4/st` |
| `--tun-static` | Static tunnel | `--tun-static=[yes｜no]` | `no` |
| `--link-restart` | Link restart count | `--link-restart=[count]` | `0` |
| `--block-quic` | Block QUIC traffic | `--block-quic=[yes\|no]` | `no` |
| `--auto-restart` | Auto-restart program | `--auto-restart=[seconds]` | `0` |

#### Route Settings
| Command | Description | Format | Default |
|---------|-------------|--------|:------:|
| `--bypass` | Bypass list | `--bypass <file1\|file2>` | `./ip.txt` |
| `--bypass-nic` | Bypass list NIC | `--bypass-nic <NIC>` | |
| `--bypass-ngw` | Bypass list gateway | `--bypass-ngw <IP>` | `0.0.0.0` |
| `--virr` | Auto-update & apply | `--virr [file]/[country]` | `./ip.txt/CN` |
| `--dns-rules` | DNS rules | `--dns-rules <file>` | `./dns-rules.txt` |

#### Platform-Specific
| Command | Platform | Description | Format | Default |
|---------|:------:|-------------|--------|:------:|
| `--tun-route` | Linux | Route compatibility | `--tun-route=[yes｜no]` | `no` |
| `--tun-protect` | Linux | Route protection | `--tun-protect=[yes｜no]` | `yes` |
| `--tun-promisc` | macOS / Linux | Promiscuous mode | `--tun-promisc=[yes｜no]` | `yes` |

---

### 🪟 Windows Commands

| Command | Description | Format |
|---------|-------------|--------|
| `--system-network-reset` | Network reset | `--system-network-reset` |
| `--system-network-optimization` | Performance optimization | `--system-network-optimization` |
| `--system-network-preferred-ipv4` | Prefer IPv4 | `--system-network-preferred-ipv4` |
| `--system-network-preferred-ipv6` | Prefer IPv6 | `--system-network-preferred-ipv6` |
| `--tun-driver` | Select adapter driver; use `tap` for layer-2 bridging | `--tun-driver=[auto\|wintun\|tap]` |
| `--no-lsp` | Disable LSP | `--no-lsp` |

---

### 📚 Global Parameters

#### MUX Acceleration Modes
| Value | Mode | Use Case |
|:--:|------|----------|
| 0 | Standard | Normal usage |
| 1 | Server acceleration | Download-intensive |
| 2 | Client acceleration | Upload-intensive |
| 3 | Bidirectional | High performance |

#### Virtual NIC Defaults
| Platform | Default |
|----------|--------|
| Windows | `PPP` |
| Linux | `ppp` |
| macOS | `utun0` |

#### SSMT Optimization Modes
| Mode | Direction |
|------|-----------|
| st | Single-connection high throughput |
| mq | Multi-connection high concurrency |

#### Network Protocol Stacks
| Type | Description |
|:----:|-------------|
| `lwip` | For Windows |
| `ctcp` | For non-Windows |

---

## 📋 CLI Startup Examples

Core settings (tunnel type, server address, encryption keys) go in `appsettings.json`. DNS, gateway, and bypass files have automatic defaults. CLI parameters only override on demand.

### Client Mode

```bash
# Minimal — all settings in appsettings.json
ppp --mode=client
```

A real-world client startup example:

```bash
start ppp.exe --mode=client --config=./config/HKBN.json --tun-mux=0 --tun-host=yes --tun-vnet=yes --tun-gw=192.168.12.0 --tun-ip=192.168.12.25 --tun-flash=yes --tun-mask=24 --link-restart=3 --tun-static=no --block-quic=yes
```

> `--tun-gw`, `--tun-ip`, `--tun-mask` override server-assigned IPs for a fixed intranet address. `--link-restart=3` auto-reconnects up to 3 times on disconnect. Bypass/DNS parameters (`--bypass` / `--bypass6` / `--dns-rules` / `--dns`) use defaults from `appsettings.json` and are normally omitted from the CLI.

### Server Mode

```bash
# mode defaults to server
./ppp --mode=server

# Custom config
./ppp --mode=server --config=./server.json
```

### General Parameters

```bash
# Custom config file
ppp --mode=client --config=./my-config.json

# Auto-restart on crash/disconnect
ppp --mode=client --auto-restart=300

# Show help
ppp --help
```

> **Note**: The `client.server` field in `appsettings.json` supports `ppp://`, `ws://`, and `wss://` protocols. To switch tunnel types, just change this field — no CLI changes needed. For more parameters, see the upstream [CLI reference](https://github.com/liulilittle/openppp2/blob/main/README_CN.md#-%E5%91%BD%E4%BB%A4%E8%A1%8C%E6%8E%A5%E5%8F%A3).

---

## 🔍 Debug Build

The Release build (default) only outputs the TUI dashboard — no debug logs. The **Debug build** enables the `PPP_LOG_VERBOSE` macro, producing detailed `LOG_DEBUG` / `LOG_INFO` output for troubleshooting connections, routing, DNS, etc.

### Release vs Debug

| | Release | Debug |
|---|---|---|
| Compile flags | `-O3` | `-D_DEBUG -DPPP_LOG_VERBOSE -g3` |
| Optimization | Full | None |
| Log output | TUI dashboard only | Dashboard + detail logs |
| Binary size | Smaller | Larger (with debug symbols) |
| Use case | Production | Troubleshooting |

### Getting Debug Builds

GitHub Actions builds both Release and Debug for every platform. In the [Releases](https://github.com/picetor/openppp2/releases) page, files with `debug` in the name are Debug builds:

```
openppp2-windows-x64.zip          ← Release
openppp2-windows-x64-debug.zip    ← Debug
openppp2-linux-amd64.zip          ← Release
openppp2-linux-amd64-debug.zip    ← Debug
...
```

### `--log-file` Usage

The Debug build supports `--log-file` to redirect debug logs to a file (no effect in Release):

```bash
# Debug build: write logs to file
./ppp --mode=client --log-file ./ppp_debug.log

# Live tail
tail -f ./ppp_debug.log
```

> **Note**: The TUI dashboard always prints to the console. `--log-file` only redirects `LOG_DEBUG` / `LOG_INFO` entries. The two are independent.

---

## 🔗 Tunnel Protocol Configuration

openppp2 supports three tunnel transport protocols: **PPP** (raw TCP), **WS** (WebSocket), and **WSS** (WebSocket over TLS). To switch protocols, just change `client.server` in `appsettings.json` — the CLI always uses `--mode=client`.

### PPP (Raw TCP)

Simplest deployment. Best for LAN or direct-connect scenarios.

**appsettings.json**:
```json
{
    "tcp": {
        "listen": { "port": 20000 }
    },
    "client": {
        "server": "ppp://server-ip:20000/"
    }
}
```

### WS (WebSocket, no encryption)

WebSocket tunnel, compatible with CDN / reverse proxies. No TLS.

**appsettings.json** — original domain:
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
Optimized IP:
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

### WSS (WebSocket over TLS)

Production recommended. Encrypted transport with CDN optimized IP support.

**appsettings.json**:
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
CDN optimized IP scenario — add `client.websocket`:
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

> Regardless of protocol, the client command is always `ppp --mode=client`, server is `./ppp`.

### Protocol Comparison

| | PPP | WS | WSS |
|---|---|---|---|
| Encryption | AES app-layer | AES app-layer | TLS + AES dual |
| Port | Custom | 80/custom | 443/custom |
| CDN | ❌ | ✅ | ✅ |
| Disguise | ❌ | HTTP header disguise | HTTPS disguise |
| Recommended | LAN/direct | NAT traversal | Production/public |

> PPP mode doesn't use TLS but data is still encrypted at the application layer (controlled by `key.protocol` / `key.transport`). WSS adds TLS transport encryption on top.

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

---

## 🔗 Related Projects

These projects work alongside this repo, covering rule generation, one-click deployment, and IP data sources.

### DNS Rule Generator — `dns-rules_geosite_generator`

[dns-rules_geosite_generator](https://github.com/picetor/dns-rules_geosite_generator) converts [MetaCubeX/meta-rules-dat](https://github.com/MetaCubeX/meta-rules-dat) geosite classifications into `dns-rules.txt` bypass lists for this project.

```
geosite classifications (MetaCubeX)  ──→  geosite2dns.py  ──→  dns-rules.txt
```

- Supports 3 data sources: GitHub source files, geosite.dat Protobuf, mosdns unpack
- YAML mapping config defines classification → DNS relationships
- Output format fully compatible with openppp2 (83-byte fixed-length records)

```bash
# Recommended: direct GitHub source files (supports @cn sub-classifications)
python geosite2dns.py -m geosite-mapping.yaml -o dns-rules.txt --from-source
```

### One-Click Install — `openppp2_install`

[openppp2_install](https://github.com/picetor/openppp2_install) provides two deployment scripts:

| Script | Purpose |
|--------|---------|
| `ppp_install.sh` | Single mode — server OR client per machine |
| `ppp_dual.sh` | Dual mode — server AND client on the same machine |

Built-in systemd service management, tmux TUI status panel, smart arch/version detection. Run `ppp` after install to enter the management menu.

```bash
# One-click install (single mode)
wget -4 -O ppp_install.sh https://raw.githubusercontent.com/picetor/openppp2_install/main/ppp_install.sh && chmod +x ppp_install.sh && ./ppp_install.sh
```

### IP Bypass List Sources

`ip.txt` (IPv4) and `ipv6.txt` (IPv6) bypass lists are sourced from [mayaxcn/china-ip-list](https://github.com/mayaxcn/china-ip-list):

| File | Source | Purpose |
|------|--------|---------|
| `ip.txt` | [APNIC](http://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest) | IPv4 domestic addresses, `--bypass` / `--pull-iplist` auto-generation |
| `ipv6.txt` | [chnroute_v6.txt](https://github.com/mayaxcn/china-ip-list/blob/master/chnroute_v6.txt) | IPv6 domestic addresses, `--bypass6` |

> To update bypass lists, download from the links above and replace the files in the repo.

### IPv6 Reference — `openppp2_Miaocchi`

Some IPv6 features in this fork draw from [Miaocchi/openppp2](https://github.com/Miaocchi/openppp2):

- **IPv6 Fix Summary**: [`docs/IPV6_FIXES.md`](https://github.com/Miaocchi/openppp2/blob/main/docs/IPV6_FIXES.md) — systematically reviewed all IPv6-related code across `ppp/` core and platform directories (socket creation, address resolution, VNetstack processing, IPv6Auxiliary layer), clarifying the boundary between VPN transport and virtual Ethernet planes
- **Windows IPv6 DNS Leak Prevention** & **Source Address Selection Fix** drew on that fork's analysis of Windows IPv6 behavior
- **IPv6 Lease Management** & **NDP Proxy** server-side design docs provided reference for the IPv6 data plane implementation
