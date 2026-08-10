# OpenPPP2 Release Notes — 2.0.202608102330（2026-08-10）

> 本说明只列**新增/增强功能**，不含修复项。
> 相关设计文档：`docs/CONNECTIVITY_TEST_CN.md`、`docs/MULTI_ENTRY_CN.md`、`docs/MULTI_ENTRY_PPP_CN.md`、`docs/ANDROID_TUNNEL_SETTINGS_CN.md`。

---

## 一、客户端核心：连通性探测与多入口优选（桌面/控制台 + 安卓共用）

- **连接前全入口探测选优**：首次连接/重连前对 `client.server` + `client.servers` 全部入口探测，自动选择 RTT 最低的可用入口；默认开启（`client.probe.enabled`，默认 `true`），可在配置中关闭。
- **分层探测**：L1 = TCP connect；L3 = TLS(SNI) + WebSocket 101（镜像真实连接、不校验 CA）；不做 L4/L5 完整链路握手，避免幽灵会话。
- **缓存 / TTL / 滞回 / 惩罚**：
  - 探测结果按入口缓存，默认 TTL 30s；
  - 真实连接失败入口拉黑（penalty）；
  - 当前入口 RTT ≤ 最优 ×1.3 时保持不动，防止抖动横跳；
  - 全部入口不可达时回退旧的主入口逻辑，不影响连接。
- **探测参数可调**：`timeout-ms`、`ttl-seconds`、`parallel`、`stage`、`categories`（`tcp`/`ws`/`wss`/`udp`）。
- **故障切换**：重连时自动重选入口；失败入口拉黑 + 缓存失效，页面与日志保持一致。

## 二、多入口配置（ws/wss 与 ppp 隧道通用）

- `client.servers` 新增入口数组，每个元素支持：
  - `IP:port`；
  - `[IPv6]:port`（IPv6 双栈入口）；
  - `域名:port`（域名备份入口）。
- **域名备份入口**：探测前在协程内解析域名，连接使用解析后的 IP；缓存与拉黑统一按 `域名:port` 为键，DNS 漂移不失效；解析失败只标记该入口不可达并保留显示，不丢弃、不阻塞其它入口。
- **后台 5 秒全配置刷新**：覆盖所有菜单出口 + 主入口；未建连出口只做 L1（TCP connect）探测，已建连出口保持配置 stage，避免对空闲 wss 服务器每 5 秒做完整 TLS 握手。
- **SERVERS 页面实时显示**：每个入口显示 `(RTTms)` / `(unreachable)` / `-> 生效入口`。

配置示例：

```json
"client": {
    "server":  "ppp://kt-a.aursys.cfd:20000",
    "servers": [
        "kt-nat3.aursys.cfd:10005",
        "38.49.57.29:20000",
        "[2400:cb00:2049:1::c629:d7a2]:20000"
    ],
    "probe": {
        "enabled":     true,
        "timeout-ms":  800,
        "ttl-seconds": 30,
        "parallel":    true,
        "stage":       3,
        "categories":  ["tcp", "ws", "wss"]
    }
}
```

## 三、安卓客户端

- **虚拟网卡参数设置**：新增 `tunIp`、`tunPrefix`（16~30）设置项，替换原先的硬编码地址。
- **后台静默日志采集**：日志在后台采集、不打印到控制台；每次会话独立日志文件，带启动/停止标记，单文件上限 20MB。
- **日志文件管理界面**：可查看、分享、管理多个日志文件。
- **延迟测试增强**：HTTP 代理关闭时自动回退到 SOCKS5 通道进行测速。
- **局域网访问开关（allowAccess）**、SOCKS5 回退、日志级别设置（`set_log_level` JNI）。
- **构建产物**：单一 universal APK（不再按 ABI 拆包），固定命名（去掉 `-<sha>`），debug/release 双 APK 由 CI 产出。

## 四、其它

- **peer-local-bridge**：对等本地网桥，把入站隧道 TCP 桥接到本地通告的对等 LAN。
- **界面精简**：移除流量/工具抽屉页与流量统计设置项。
- **规则资产**：geoip/geosite 数据版本更新，规则加载链路完善。