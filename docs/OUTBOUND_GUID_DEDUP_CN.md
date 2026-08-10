# 客户端同 GUID 出口去重与运行时互斥设计文档

> 状态：设计稿（未实现）
> 目标版本：桌面端 `ppp --mode=client`
> 关联代码：`main.cpp`（加载期合并）、`ppp/app/client/VEthernetNetworkSwitcher.*`（运行时互斥）、`ppp/app/server/VirtualEthernetSwitcher.cpp`（服务器端行为参考）
> 前置背景：`docs/MULTI_ENTRY_CN.md`（入口探测优选）、`docs/MULTI_ENTRY_SESSION_EXPORT.md`（会话导出）

## 1. 背景与问题

### 1.1 出口定义与三种重复场景

客户端出口（outbound）由 `main.cpp` 从三个来源组装进 `outbound_configurations_`：

| 来源 | 触发 | tag |
|------|------|-----|
| 主配置 | `--config appsettings.json` | `main` |
| Servers 菜单 | `--server-dir ./servers` | `server:<文件名>`（`server_menu=true`） |
| GEO 出口 | `--config geo.yaml` / `.txt` | geo 声明标签（`route_used=true`） |

当前加载期去重键是**精确字符串对** `guid + server`（`main.cpp:3304`、`3460`）：

| 场景 | guid | server 写法 | 现状 |
|------|------|-------------|------|
| A：geo outbound = main 配置 | 相同 | 逐字符相同 | ✅ `matched_primary` 别名合并（`main.cpp:3463-3484`），永不双开 |
| B：geo outbound = 菜单项 | 相同 | 逐字符相同 | ✅ 菜单项 tag 改名合并（`main.cpp:3476-3482`），连接复用 |
| C：同 guid、server 写法不同 | **相同** | **写法不同** | ❌ **不合并 → 双会话 → 互踢** |

### 1.2 写法差异的具体形态（情况 C）

同一台服务器，两个 JSON 里 `client.server` 写法不同，去重键就不同：

```json
// 配置 X（main）
"server": "wss://1.2.3.4:443/tun"
// 配置 Y（geo outbound "us"）
"server": "wss://1.2.3.4/tun"          // 省略默认端口 443
// 配置 Z（geo outbound "eu"）
"server": "WSS://1.2.3.4:443/tun/"     // 大小写 / 尾斜杠
```

三者 guid 相同 → 同一会话身份，但精确串比较全部不匹配 → 加载期不合并。

### 1.3 服务器端行为（已核实）

`VirtualEthernetSwitcher::Run`（`VirtualEthernetSwitcher.cpp:1479`）：mux 控制连接握手后按 `session_id`（= 客户端 guid 的 Int128）查 exchanger：

```cpp
VirtualEthernetExchanger* exchanger = GetExchanger(session_id).get();
if (NULLPTR != exchanger) {
    if (!managed_server->ShouldReplaceDuplicateGuid(session_id)) {
        // duplicateGuidPolicy = "reject_new"：拒绝新连接
        return STATUS_ERROR;
    }
    return Establish(transmission, session_id, NULLPTR, y);  // 默认：后连者替换先连者
}
```

- `ShouldReplaceDuplicateGuid` 默认 `true`（`VirtualEthernetManagedServer.h:49` `replace_duplicate = true`；JSON 里 `duplicateGuidPolicy != "reject_new"` 即为替换模式，`VirtualEthernetManagedServer.cpp:749`）。
- **非 mux 附加连接走 `Connect`**（`VirtualEthernetSwitcher.cpp:1799`）叠加到既有会话，不产生新 exchanger，不受影响——互踢只发生在**两个独立 exchanger 各自建立 mux 控制连接**时。

### 1.4 危害链

```
客户端: 出口 X（main）已建立 mux 控制连接（guid G）
客户端: geo 规则命中出口 Y（同 guid G，server 写法不同）→ EnsureOutbound(Y) → 打开第二个 exchanger
服务器: Y 的控制连接握手 → GetExchanger(G) 已存在 → 替换模式 → Establish(Y) → 顶掉 X 的会话
客户端: X 的 exchanger 断线 → ExchangeToReconnectingState → 重连（probe 后重选入口）
服务器: X 重连的控制连接 → 替换模式 → 顶掉 Y 的会话
客户端: Y 的 exchanger 断线 → 重连 → 再顶 X
        ↑ 互踢循环，两个出口都永远无法稳定建立
```

与 `SwitchPrimaryOutbound` 注释（`VEthernetNetworkSwitcher.cpp:1834`）警告的"重复 session_id 产生 connect → handshake success → read failed 循环"是同一根因，只是那边防的是同 tag 重复选择，这边是**跨 tag 的同 guid 双开**。

## 2. 设计目标与非目标

### 目标

1. **加载期**：消除"同 guid + server 写法差异"导致的漏合并（情况 C）。
2. **运行期**：任何时刻，同一 guid 最多只有一个活跃 exchanger（兜底，防动态变化与未来入口）。
3. 保留用户"同一台服务器的多个入口"的意图（入口并入 `client.servers` 由 probe 优选，而非开多份会话）。
4. 完全向后兼容：不配置多出口时行为不变。

### 非目标

- 不做服务器端修改（`duplicateGuidPolicy` 已能表达拒绝语义，客户端侧自愈是本次范围）。
- 不支持"同 guid 双出口同时活跃"（服务器端 replace 语义下本就不可能成立，不做无意义的放开）。
- 不改变 `--server-dir` 的展示逻辑与 geo 规则语法。

## 3. 方案设计

### 3.1 总体思路：两阶段防护

```
阶段 1（加载期，main.cpp）：去重键升级 = guid + 规范化 server
    - 写法差异 → 合并为同一 outbound 定义（入口并入 servers，probe 优选）
    - 同 guid 不同入口 → 合并为一个定义 + 入口并入 servers 列表
阶段 2（运行期，VEthernetNetworkSwitcher）：打开/切换前同 guid 互斥兜底
    - EnsureOutbound / CompletePendingOutboundSwitch 前扫描同 guid 活跃 exchanger
```

### 3.2 阶段 1：规范化去重键

**复用现成构建块**：`UriAuxiliary::Parse` 的返回值就是规范化 URL（`UriAuxiliary.cpp:236-248`）：

```
输入  "WSS://1.2.3.4:443/tun/"  →  "wss://1.2.3.4:443/tun/"
输入  "wss://1.2.3.4/tun"       →  "wss://1.2.3.4:443/tun"   （协议默认端口补齐）
输入  "tcp://1.2.3.4:443"       →  "tcp://1.2.3.4:443/"
```

- scheme 统一小写、显式端口/协议默认端口归一、path 保留（`/` 与 `/tun` 视为不同——它们本来就是不同端点）。
- 域名与 IP 不合并：`wss://x.com:443/tun` vs `wss://1.2.3.4:443/tun` 键不同。这是**故意的**——域名与优选 IP 是不同入口，应作为 `servers` 入口并存，而不是"写法差异"。

#### 合并算法（`LoadGeoOutboundConfigurations` / `--server-dir` 加载，替换现有 `guid+server` 精确串匹配）

```
key = guid + "|" + NormalizeServer(server)

加载每个定义 D：
    matched = 现有定义中 key 相同者
    if (matched 存在):
        if (D 是 main 来源):           # 主配置优先
            保持 matched 现有 tag/configuration，D 的入口并入 matched 的 servers（去重）
        else:
            D 的 server 并入 matched 的 servers（若 D.server != matched.server 且不在列表）
            D 的 servers 列表并入 matched 的 servers（去重）
            # tag 保留：matched 是非 main 时沿用现有"tag 改名"逻辑
            # （`matched->tag = D.tag; matched->route_used = true;` 语义不变）
    else:
        新建定义
```

要点：

- **主配置（main）优先级不变**：`matched_primary` 别名逻辑（`main.cpp:3463-3484`）保留，扩展为"同 guid 即别名"。
- **入口并入不丢失意图**：`wss://x.com:443/tun`（main）+ `wss://1.2.3.4:443/tun`（geo us）合并后 = `server: wss://x.com:443/tun` + `servers: ["1.2.3.4:443"]`——连接时 probe 在两者间优选，符合"同服务器多入口"的用法。
- **日志告警**：每次合并打 `LOG_WARN`（"outbound %s merged into %s by guid"），便于用户发现配置写法不一致。
- guid 为空（`GuidStringToInt128 == 0`）的定义不参与合并（`NewExchanger` 会拒绝，保持现状）。

#### 兼容性检查

| 现有行为 | 合并后 | 影响 |
|----------|--------|------|
| `matched_primary`（guid+server 相同 + main） | 仍走别名路径 | 无 |
| server_menu 与 geo 同 guid+server | tag 改名合并 | 无 |
| 完全不同的服务器（guid 不同） | 各自独立 | 无 |
| 同 guid 不同 server（原情况 C） | **合并 + 入口并入 servers** | 行为修正 ✓ |

### 3.3 阶段 2：运行时同 guid 互斥（兜底）

即使加载期合并，运行期仍可能产生同 guid 双 exchanger（如磁盘 JSON 被外部修改后 `ReloadOutboundConfiguration` 重读、`EnsureOutbound` 按需打开等）。加两道互斥闸：

#### 闸 1：`EnsureOutbound(tag)` 打开前（`VEthernetNetworkSwitcher.cpp:1938`）

```
打开候选 exchanger 前（锁内）：
    guid = GuidStringToInt128(candidate.configuration->client.guid)
    for (other in outbound_exchangers_):
        if (other.tag != tag && 同 guid && !other->IsDisposed()):
            if (other 是 main 或 当前 active):   # TUN 拥有者 / 数据面，不可被杀
                return NULLPTR（拒绝打开，日志 WARN）
            else:
                other->Dispose(); outbound_exchangers_.erase(other.tag)   # 非活跃旧会话替换
```

- **优先级规则**：`main`（TUN 拥有者）> 当前 `active_outbound_` > 其他。低优先级让位，高优先级保护。
- 与服务器端 replace 语义对齐：客户端**主动替换**而不是等服务器踢，避免互踢窗口。

#### 闸 2：`CompletePendingOutboundSwitch` primary 分支（`VEthernetNetworkSwitcher.cpp:1975`）

```
primary_switch 生效时（新 main 已就位，锁内）：
    new_guid = GuidStringToInt128(target->GetConfiguration()->client.guid)
    for (other in outbound_exchangers_):
        if (other.tag != "main" && other.second != target && 同 new_guid && !IsDisposed()):
            replaced = other.second
            outbound_exchangers_.erase(other.tag)
    锁外：replaced->Dispose()
```

- 切换后所有同 guid 的旧出口（geo / 菜单别名）立即让位，新 main 独占会话。
- 注意：**不清理 `outbound_configurations_` 定义**——定义保留（TUI 可见、geo 规则命中时经 `primary_outbound_` 别名映射回 main，`GetExchanger` 已有该路径），只清活跃 exchanger。

### 3.4 定义层别名映射的确认（无需改动，已验证）

- `GetExchanger`：`tag == primary_outbound_ → tag = "main"`（`VEthernetNetworkSwitcher.cpp:2237`）——合并后同 guid 的 geo 标签命中规则时自然走 main，无需新代码。
- `GetOutboundStatuses`：`server_menu` 项 `tag == primary_outbound_` 时查 `"main"` 槽（`VEthernetNetworkSwitcher.cpp:1667`）——菜单显示复用 main 状态，无需新代码。
- 后台探测（`RefreshOutboundProbes`）：按定义列表探测，合并后同一 guid 只探测一次（一个定义），顺带减少探测流量。

### 3.5 边界情况

| 场景 | 行为 |
|------|------|
| 同 guid，server 相同但大小写/默认端口/尾斜杠不同 | 规范化后合并 ✓（情况 C 主修复） |
| 同 guid，域名 vs IP 入口 | 合并为一个定义，入口并入 `servers`，probe 优选 |
| 同 guid，且都是非 main 出口 | tag 改名合并（现有逻辑），入口并入 |
| `EnsureOutbound` 打开时同 guid main 活跃 | 拒绝打开 + WARN（保护 TUN 拥有者） |
| `EnsureOutbound` 打开时同 guid 非活跃旧会话 | Dispose 旧会话后打开 |
| 磁盘 JSON 运行期被改 guid | 闸 2 兜底：切换时同 guid 旧会话清理 |
| geo 出口与 main 同 guid（原情况 A） | 别名路径不变，无新开销 |
| 三个以上同 guid 定义 | 逐对合并，最终一个定义 + 多个 tag 别名 |

### 3.6 改动清单

| 文件 | 改动 |
|------|------|
| `main.cpp` | `LoadGeoOutboundConfigurations`（~3456）与 `--server-dir` 加载（~3304）的去重键改为 `guid + NormalizeServer(server)`；入口并入逻辑；合并 WARN 日志 |
| `main.cpp`（或 `UriAuxiliary`） | 新增辅助 `NormalizeServerEntry(const ppp::string&) -> ppp::string`（封装 `UriAuxiliary::Parse` 的 normalized 返回；Parse 需要 YieldContext，加载期无协程 → 用 `resolver=false` 变体或纯字符串归一） |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | `EnsureOutbound` 加闸 1；`CompletePendingOutboundSwitch` primary 分支加闸 2 |
| `ppp/app/client/VEthernetNetworkSwitcher.h` | （可选）新增 `DisposeOutboundsByGuid(const Int128&, const ppp::string& except_tag)` 辅助 |

> 注意：`UriAuxiliary::Parse` 的 `resolver` 参数为 false 时不做 DNS 解析（域名保留字面），恰好满足加载期需求；需确认 Parse 在无 YieldContext 时的调用路径（`UriAuxiliary.cpp:88` 已有 `resolver=false` 重载）。

## 4. 验证计划

1. **加载期单测**：
   - 同 guid、server 写法差异（大小写/默认端口/尾斜杠）→ 合并为一个定义；
   - 同 guid、域名 vs IP → 合并，入口出现在 `servers` 列表；
   - 不同 guid → 不合并；
   - 主配置优先：main + geo 同 guid → 别名路径，main 的 server 不变。
2. **端到端（互踢复现修复验证）**：
   - 构造情况 C 配置（main 与 geo us 同 guid 不同 server 写法）；
   - 启动客户端，规则命中 us → 观察：无互踢循环，单会话稳定；
   - 日志出现合并 WARN。
3. **运行时兜底**：运行中修改磁盘 JSON 的 guid 并触发 `SwitchPrimaryOutbound` → 旧同 guid 会话被清理，无双会话。
4. **回归**：不带多出口 / 不带 `--server-dir` 跑现有测试（connect-hang、peer-removed、prefer-ipv4 等既有场景），确认无行为变化。
5. **服务器侧对照**：`duplicateGuidPolicy: "reject_new"` 时，客户端互斥照常工作（客户端不依赖服务器踢人）。

## 5. 开放问题

- [ ] 是否把合并后的入口顺序固定为"先 main 后并入"，还是按 RTT 动态排序（probe 已排序，`servers` 顺序只影响首轮）——倾向后者（现状语义）。
- [ ] `NormalizeServerEntry` 放 `UriAuxiliary`（公共）还是 `main.cpp`（局部）——倾向 `UriAuxiliary`，未来 `--server-dir` 与 geo 加载共用。
- [ ] 是否需要把同 guid 合并的可见性暴露到 TUI（如 tag 行尾标注 `(merged)`）。
