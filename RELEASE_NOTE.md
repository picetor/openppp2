# 🚀 openppp2 — inet6 版本发布说明

> **PPP PRIVATE NETWORK™ 2** — 企业级虚拟以太网 VPN 解决方案
>
> 基于上游 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) 修改版编译。

---

## ✨ 核心新特性

### 🌐 完整 IPv6 支持
- IPv6 分流隧道（`--bypass6` / `--bypass-ngw6` / `--bypass-nic6`）
- 服务器 → 客户端 IPv6 地址自动分配（GUA）
- 拆分默认路由 `::/1` + `8000::/1`，与 IPv4 策略一致
- 双栈并发（PPP/WS/WSS 均支持）
- Linux + Windows 平台完整支持

### 🪟 Windows 原生支持（Fork 独有）
- MSBuild + vcpkg 完整构建系统
- GitHub Actions Release + Debug CI 自动构建
- TAP 驱动（已签名） + PaperAirplane + sysproxy

### ⚡ 优选 IP + WSS 加速
- `client.websocket.host` — WebSocket Host 头，CDN 正确路由
- `client.websocket.sni` — TLS SNI 字段

### 📋 Debug 日志系统
- `PPP_LOG_VERBOSE` 编译开关，12 核心模块详细日志
- `--log-file <path>` 运行时日志重定向
- CI Debug 工作流（自动触发）

---

## 📦 构建产物

基于上游 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) 修改版编译。

### 编译环境

| 平台 | 环境 |
|------|------|
| Linux amd64 | Ubuntu 24.04, GCC 13, CMake 3.28.3, Make |
| Linux aarch64 | 交叉编译 (aarch64-linux-gnu-g++-13), Ubuntu 24.04 |
| macOS | macOS 14 (arm64), Apple Clang, CMake, Make |
| Windows | windows-latest, MSVC (Visual Studio 2022), vcpkg |

### 包含的二进制文件

| 文件名 | 架构 | 特性 |
|--------|------|------|
| `openppp2-linux-amd64` | x86_64 | 基础版 |
| `openppp2-linux-amd64-simd` | x86_64 | SIMD 优化 |
| `openppp2-linux-amd64-io-uring` | x86_64 | io_uring |
| `openppp2-linux-amd64-io-uring-simd` | x86_64 | io_uring + SIMD |
| `openppp2-linux-amd64-tc-simd` | x86_64 | 时间校正 + SIMD |
| `openppp2-linux-amd64-tc-io-uring` | x86_64 | 时间校正 + io_uring |
| `openppp2-linux-amd64-tc-io-uring-simd` | x86_64 | 时间校正 + io_uring + SIMD |
| `openppp2-linux-arm64` | ARM64 | 基础版 |
| `openppp2-linux-arm64-io-uring` | ARM64 | io_uring |
| `openppp2-linux-arm64-tc` | ARM64 | 时间校正 |
| `openppp2-linux-arm64-tc-io-uring` | ARM64 | 时间校正 + io_uring |
| `openppp2-darwin-amd64` | macOS x86_64 | 基础版 |
| `openppp2-darwin-arm64` | macOS ARM64 | 基础版 |
| `openppp2-windows-x64` | Windows x64 | 基础版 |

> 每个 zip 包含：`ppp` 二进制 + `appsettings.json` + `dns-rules.txt` + `firewall-rules.txt` + `ip.txt`

---

## 🔧 快速开始

### 服务器

```json
{
    "tcp": { "listen": { "port": 20000 } },
    "websocket": {
        "host": "your-domain.com",
        "path": "/tun",
        "listen": { "ws": 20080, "wss": 20443 },
        "ssl": {
            "certificate-file": "your-domain.com.pem",
            "certificate-key-file": "your-domain.com.key"
        }
    }
}
```

### 客户端 — PPP 直连

```json
{
    "client": {
        "server": "ppp://your-server.com:20000/"
    }
}
```

### 客户端 — 优选 IP + WSS

```json
{
    "client": {
        "server": "wss://IP:20443/tun",
        "websocket": { "host": "your-domain.com", "sni": "your-domain.com" }
    }
}
```

### 客户端 — IPv6 分流

```bash
# Linux
./ppp --client --bypass6 --bypass-ngw6 fe80::1 --bypass-nic6 eth0

# Windows
ppp.exe --client --bypass6
```

---

## ⚠️ 注意事项

1. **证书**：WSS 需要自行准备 SSL 证书
2. **Windows TAP 驱动**：需安装 `Driver/` 目录下的 tap0901 驱动（已签名）
3. **管理员权限**：`ppp.exe` 需要管理员权限运行
4. **Debug 版本**：仅用于诊断，勿用于生产环境
5. **Windows IPv6 旁路由**：Windows 旁路由模式下 IPv6 可能绕过软路由直接走上级路由器，建议使用客户端模式

---

<p align="center">
  <strong>PPP PRIVATE NETWORK™ 2</strong><br>
  <a href="https://github.com/picetor/openppp2">github.com/picetor/openppp2</a>
</p>
