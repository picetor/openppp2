# UCP 诊断部署说明（临时文件，不入库）

> 本文件是临时部署说明，完成后可删除。

## ⚠️ 服务端卡死事件（2026-08-07）

**症状**：部署诊断版后服务端卡死连不上（23.249.25.106:20001）。

**根因**：服务端 1Hz 诊断循环存在 UAF 竞态——
服务端模式下 `IUcpTransmission::connection_` 是裸指针（归 `UcpServer` 所有），
连接关闭后 UcpServer 经 `DeferDestroyEntry` 在后台线程销毁 `UcpConnection`，
而诊断循环在 io_context 线程用裸指针调 `conn->GetDiagnostics()`，
命中窗口即访问已释放对象 → 崩溃/内存损坏/卡死。

**处理**：已移除全部服务端诊断代码，恢复稳定版。

## 当前代码状态（回滚后）

| 仓库 | 分支 | 提交 | 内容 |
|---|---|---|---|
| picetor/openppp2 | `ucp` | （新提交） | **移除**服务端 1Hz 诊断循环 + IUcpTransmission 访问器 |
| picetor/ucp | `main` | `6be6b69` | UcpDatagramNetwork 内置 DoEvents（**保留**，客户端二次连接挂起修复） |
| picetor/ucp | `main` | （新提交） | **移除** FQ/FLUSH 诊断日志（UcpDiagLog helper） |

本机 MSBuild (Debug x64) 编译链接通过（ppp.exe）。

## 服务器恢复操作步骤（紧急）

```bash
# ---------- 1. 同步 ucp 仓库（位置以实际为准） ----------
cd /root/ucp && git pull origin main          # 或 /root/build/ucp

# ---------- 2. 同步 openppp2 仓库 ----------
cd /root/build/openppp2 && git pull origin ucp

# ---------- 3. 重新构建 ----------
cd /root/build/openppp2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cp build/ppp /opt/ppp/ppp

# ---------- 4. 重启服务 ----------
systemctl restart ppp        # 或 ppp-server 等实际服务名

# ---------- 5. 确认 ucp_diag.log 不再增长 ----------
ls -la /opt/ppp/ucp_diag.log 2>/dev/null || echo "no ucp_diag.log (expected)"

# ---------- 6. 验证服务恢复（客户端拨号 20001） ----------
```

## 若服务端无法恢复（进程起不来）

1. 检查服务日志：`journalctl -u ppp -n 200` 或 `/opt/ppp/server.log`
2. 若怀疑 ucp 库新改动问题，可临时回退 ucp：
   `git -C /root/ucp checkout 6167332` 后重新构建（注意 6167332 是旧诊断提交，仅应急用）
  - **recv 停滞 + state 不变** → 服务端没收到客户端数据或状态机卡住。

## 预期结论分支
1. credit-gate 持续触发且 uncapped 未出现 → 连接没进 active 集合 → 查 OnPcbConnected/attached 置位路径。
2. ivv 从未写出（handshake end 不出现/超时）→ 查 WriteAsync → FlushSendQueueAsync 调用链。
3. 无 [FLUSH] 日志 → flush 未执行 → 查驱动线程/UcpServer 的 tick 分发。

---

## 单向流量中断诊断（2026-08-08 最新）

**症状**：握手成功、连接建立，但客户端→服务器方向在握手后 3-5 秒完全中断；
服务器→客户端正常（服务器 keepalive 持续到达客户端）。
服务器 VPN 层 idle timeout（15s）后关会话 → 客户端重连 → 循环。

**关键洞察**：服务器 keepalive 持续发（size 各不相同 = 窗口未满）
⇒ 服务器持续收到客户端 UCP ACK（ACK 独立发送不走 flush）
⇒ 客户端 UDP 发送正常 ⇒ **问题在 FlushSendQueueAsync（Data 走 flush，停滞）**。

**已部署的诊断（构建 31181411330）**：
1. **客户端** `IUcpTransmission::DriveLoop` 每 ~5s 打 LOG_DEBUG：
   `state/flight/rwnd/cwnd/pacing/sentData/retrans/sentAck/sentNak/bytesSent/bytesRecv/rttUs/delivered/dgrams/sndBlock/sndErr/recvErr`
2. **服务器** `VirtualEthernetSwitcher::OnTick` 每 ~5s 打 Info：
   `UCP server diag: dgrams=.. decodeFail=..`（UcpServer 计数器，ucp fb9c8bc）

**⚠️ 安全注意（上次教训）**：服务端诊断只能访问 `ucp_server_`（shared_ptr）的
**计数器**，绝不能访问 `UcpConnection*` 裸指针（UcpServer 异步销毁 → UAF）。

**判别矩阵（新日志）**：
- `sentData` 停滞 + `sndBlock/sndErr` 增长 → UDP 层 would_block 静默丢弃
- `sentData` 停滞 + 计数正常 → flush 停滞（pacing/window/timer）
- `sentData` 增长 + 服务器 `dgrams` 不增 → 网络层问题
- `state != 3` → SendAsync 状态门拒绝（NotConnected 丢弃）
- 服务器 `dgrams` 增 + `decodeFail` 增 → 编解码不匹配

**GitHub Actions 部署**（SSH 不可用，rfchost 控制台）：
1. 构建完成 → `gh run download <run-id> -n openppp2-linux-amd64-debug`
2. 服务器二进制 /usr/local/bin/ppp（替换后重启服务）
3. 客户端二进制替换后重启
4. 采集 ppp_server.log / ppp_client.log 新诊断行
