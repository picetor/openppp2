# openppp2 服务端与核心协议分析报告

> 分析对象：`D:\github\openppp2`（openppp2 分支，基于 liulilittle/openppp2），仅阅读未修改。

## 1. 服务端接入、鉴权与虚拟 IP 分配

**监听与接入**：`VirtualEthernetSwitcher::CreateAllAcceptors()`（`ppp/app/server/VirtualEthernetSwitcher.cpp:2038`）按 `NetworkAcceptorCategories` 枚举（`VirtualEthernetSwitcher.h:326`）建立 5 类监听：TCP（`tcp.listen.port`）、WebSocket（`websocket.listen.ws`）、WebSocket/TLS（`wss`）以及 CDN1/CDN2 中继（`cdn[0]/cdn[1]`，走 `ppp/net/proxies/sniproxy`）。`Run()`（cpp:1432）为每个 acceptor 启动 `Socket::AcceptLoopbackAsync`，回调中调整 socket 选项后进入 `Accept(context, socket, categories)`（cpp:1584）：CDN 交给 sniproxy；其余按类别构造传输对象（cpp:3090 的 `Accept(int, ...)`：`ITcpipTransmission` / `IWebsocketTransmission` / `ISslWebsocketTransmission`），随后 `YieldContext::Spawn` 运行 `Run(context, transmission, y)`（cpp:1499）。

**握手与鉴权**：`Run` 首先执行 `transmission->HandshakeClient(y, mux)`（传输层握手，见第 2 节），从客户端取得 128 位 `session_id`（即客户端 GUID）与 mux 标志。之后分两条路径：
- 非 mux：`Connect()`（cpp:1864）仅查 exchanger 合并统计，随即 `AddNewConnection` 建立 `VirtualEthernetNetworkTcpipConnection` 透传会话——该路径**不再做任何鉴权**，属于老协议兼容路径。
- mux：若配置了管理面板则走 `VirtualEthernetManagedServer::AuthenticationToManagedServer()`（`VirtualEthernetManagedServer.cpp:104`）：开启 `ManagementEnabled()`（配置了 `server.management.endpoint` 或 node_id+communication_key）时本地按 `AuthorizeByManagementPolicy(session_id)` 黑/白名单判定；否则通过 WebSocket+JSON 协议向面板发 `PACKET_CMD_AUTHENTICATION`（帧格式为 8 位十六进制长度头 + `{Id,Node,Guid,Cmd,Data}` JSON），超时（`PACKET_TIMEOUT_AUTHENTICATION`）回调失败。鉴权回调携带 `VirtualEthernetInformation`（QoS、剩余双向流量、过期时间），随后 `Establish()`（cpp:1776）创建 `VirtualEthernetExchanger` 并下发 INFO 信封，客户端流量配额/过期校验在 `VirtualEthernetInformation::Valid()`（`VirtualEthernetInformation.h:62`：剩余流量>0 且 `ExpiredTime > time(NULL)`）。注意：**未配置管理面板时直接 `Establish`，GUID 即全部信任依据**。

**虚拟 IP 分配**：
- IPv4 新协议：INFO 信封扩展 JSON 中的 `client-ipv4-request`（mode=auto/manual）→ `UpdateIPv4Request()`（cpp:3771）→ `IPv4LeasePool`（`IPv4LeasePool.h`）：`gateway=network+1`、`broadcast=network|~mask`，`AcquireAuto` 从 network+1 顺序扫描到 broadcast-1 并跳过网关；`AcquireManual` 请求地址被占用/是广播时回退 auto 并回 `conflict`。结果经 `client-ipv4` 扩展返回 address/gateway/mask。
- IPv4 老协议：客户端发 LAN 广告帧（ip+mask）→ exchanger `OnLan`→`Arp()`（`VirtualEthernetExchanger.cpp:556`）→ `switcher_->AddNatInformation(exchanger, ip, mask)` 登记 `nats_` 所有权表（IP→exchanger），供入向 NAT 转发寻址。
- IPv6：`UpdateIPv6Request()`（cpp:3708）+ `BuildInformationIPv6Extensions()`（cpp:361）按 nat66/gua 模式分配地址、登记 `ipv6s_` 映射与 `ipv6_leases_` 租约（`TickIPv6Leases` cpp:3508 到期回收）。

## 2. 传输层抽象：ITransmission 与 TCP/WebSocket

**接口设计**（`ppp/transmissions/ITransmission.h`）：`ITransmission` 继承 `IAsynchronousWriteIoQueue`，纯虚 `DoReadBytes`、`ShiftToScheduler`、`GetRemoteEndPoint`；对外提供 `HandshakeClient/HandshakeServer`、`Read/Write`（协程）与 `Write(cb)`（回调）、`Encrypt/Decrypt`。实现桥接在 `ITransmissionBridge`（`ITransmission.cpp:52`）。

**握手**（`InternalHandshakeClient` cpp:1066 / `InternalHandshakeServer` cpp:1103）：先互发若干随机 NOP 填充包（`Transmission_Handshake_Nop`，次数由 `key.kl/kh` 决定，含随机可打印填充与 4 字节 `kfs` 头 + 按 `key.kf` 逐字节 XOR 的混淆，`Transmission_Handshake_Pack_SessionId` cpp:814），再交换：客户端 GUID（session_id）→ 随机 128 位 `ivv` → 随机 `nmux`（最低位=是否启用 mux）。`ivv` 用于握手后重建会话级密文。整个握手包先经 base94 编码（`handshaked_==false` 时 `Encrypt` 走 base94 路径，cpp:86-104）。

**分帧**：握手前用 base94 长度头——4 字节 simple header（随机 key 字节+填充+base94 长度，`base94_encode_length` cpp:258），首包带 3 字节校验的 extended header，成功后 `frame_tn_/frame_rn_` 切换为 simple 模式；握手后改用二进制 `Transmission_Packet_Encrypt/Decrypt`（cpp:659/715）：3 字节 protocol 密文头（含长度）+ transport 密文负载。

**TCP 与 WebSocket**：`ITcpipTransmission`（`ITcpipTransmission.cpp`）；WS 由模板 `templates::WebSocket<IWebsocket>`（`templates/WebSocket.h`）实例化 `IWebsocketTransmission`/`ISslWebsocketTransmission`（`IWebsocketTransmission.h`），底层是 beast 封装（`ppp/net/asio/websocket.*`），`key.plaintext` 时用文本帧否则二进制帧；Windows 上按 `Socket::IsDefaultFlashTypeOfService` 附加 QoSS（TOS）。读写经 `ITransmissionQoS` 带宽整形（`ITransmissionQoS.h`）。

**超时与保活**：握手超时定时器 `InternalHandshakeTimeoutSet`（cpp:1154），超时值 `tcp.connect.timeout`+随机 jitter（`nexcept`），超时后补发 NOP 并 `Dispose`。链路层 `VirtualEthernetLinklayer::DoKeepAlived`（`VirtualEthernetLinklayer.cpp:814`）：空闲上限 `min(connect.timeout<<1, inactive.timeout)+5s` 判死；在 [1s, max_timeout] 随机间隔发送随机可打印 ASCII 载荷的 `KEEPALIVED(0x7F)` 帧。

## 3. MUX 协议：vmux_net

**帧格式**：`vmux_hdr`（`vmux_net.h:122`）9 字节 packed：`seq(4) + cmd(1) + connection_id(4)`，网络字节序；命令从 `('E'-1)` 起：`cmd_syn/syn_ok/push/fin/keep_alived/acceleration/mux_mode_set/nack/ack/mux_rebuild`。

**多链路聚合**：每个 carrier link 是一条 `VirtualEthernetTcpipConnection`，经 `add_linklayer()`（cpp:2198）加入 `tx_links_/rx_links_`；`handshake()`（cpp:2620）由服务端分配 1..`pool_hard_max` 的 `receive_id` 回给客户端；`forwarding()`（cpp:2719）为每条链路各起一个读协程，读到帧后 post 到 vmux strand 统一 `packet_input_flow`/`packet_input_unorder`。`linklayer_established()`：`opened_connections >= max_connections` 时置 `established_`，此后才允许 `post` 数据。

**compat 序号/重排**：`packet_input_unorder`（cpp:747）维护全局 `rx_ack_`：`seq==rx_ack_` 立即投递并 ++；`seq>rx_ack_` 缓存进 `rx_queue_`（`map<seq,packet>`，`packet_less` 用 int32 差值处理回绕），缺口补齐后重放连续段；`seq<rx_ack_` 视为非法帧。

**flow-v2（每流独立 DSN）**：`packet_input_flow`（cpp:1077）中控制帧（keep_alived/mux_mode_set/mux_rebuild/syn/syn_ok/acceleration/nack/ack）不参与 DSN 门控、内联投递；push/fin 数据帧按 `flows_[cid]` 的 `flow_rx_next_` 独立排序，`flow_reorder_` 有界缓冲（`flow_reorder_cap_bytes_` 上限，超限先 `flow_force_advance` 驱逐，仅 FIN 允许闭合缺口，数据缺口触发 M1 重建）。发送侧 `post_internal`（cpp:1695）：flow-v2 下数据帧打每连接 DSN（`tx_flow_seq_[cid]` 从 1 起），控制帧 seq=0 进高优先级 `tx_ctrl_queue_` 先发。

**NACK/ACK 重传（M4）**：`flow_send_nack`（cpp:1413）请求 `[flow_rx_next_, dsn_to]`（跨度≤64 帧），指数退避（上限 1600ms）重发至 `flow_nack_max_retries_`，耗尽则 `close_with_notice(close_reason_m4_retransmit_exhausted)`；`packet_input_nack`（cpp:1488）从发送缓存 `tx_flow_cache_` 深拷贝重放；接收方 `flow_maybe_ack` 每 `flow_ack_every_frames_` 帧或超时发聚合 `cmd_ack`（`dsn_ack=flow_rx_next_` 水位），`packet_input_ack` 释放缓存。能力协商在 MUX 帧的 `ordering_caps` 字节（bit0=flow_v2，bit1=nack），见 exchanger `OnMux`（`VirtualEthernetExchanger.cpp:378`），缺省 compat。

**调度模式**（`process_tx_all_packets` cpp:2133 分发）：
- compat：竞争式——空闲链路依次取队首帧，全局序号（`process_tx_compat_packets`）。
- flow：发送侧与 compat 相同（cpp:1901 直接转发），接收侧在 turbo 开启时协商 flow-v2；新连接首包经 `select_turbo_linklayer` 走"最近活跃"链路（以 `last_active_` 近因代替 RTT，仅一次提示，不绑定连接）。
- balance：发送侧仍是竞争（cpp:2059），接收侧**强制** flow-v2 逐流重排——设计注释明确说明这是为避免 per-connection 绑定导致多条重流挤在同一链路退化为单 TCP。
- stripe：发送侧逐包轮询 `select_striped_linklayer`（`stripe_cursor_` 循环，cpp:1972），接收侧 flow-v2。
- `mode_requires_flow_v2`（vmux_net.h:349）：balance/stripe 必须 flow-v2，flow+turbo 亦需。

**会话维护**：`update()`（cpp:590）做 per-socket 空闲/连接超时、MUX 总空闲超时（未建成时给 2×connect 宽限）、心跳 `cmd_keep_alived`、D11 数据队列高水位 stall watchdog、`flow_evict_expired`、turbo 动态链路池（`turbo_controller_tick`/`retire_linklayer_runtime`/`reap_retired_linklayers`，池上限 `pool_hard_max = max_connections × PPP_MUX_TURBO_FACTOR_MAX(=3)`，退役链路等 `inflight_==0` 才回收，避免迟到写完成触碰已退役调度状态）。

## 4. 链路层帧格式（VirtualEthernetLinklayer）

无固定同步头，以 1 字节 action 分派（`VirtualEthernetLinklayer.h:75`）：`INFO=0x7E`、`KEEPALIVED=0x7F`、FRP 系列 `0x20~0x25`（映射端口隧道）、`LAN=0x28`、`NAT=0x29`、TCP 系列 `SYN=0x2A/SYNOK=0x2B/PSH=0x2C/FIN=0x2D`、`SENDTO=0x2E`、`ECHO=0x2F/ECHOACK=0x30`、`STATIC=0x31/STATICACK=0x32`、`MUX=0x35/MUXON=0x36`。
- TCP 流帧：`ACTION(1)+CONNECT_ID(3B 大端)`，ID 由原子计数器 `NewId()`（cpp:411）生成 1..0xFFFFFF，0 保留（`PACKET_ConnectId` cpp:216）。
- 端点编码：`ACTION(1)+ADDR_LEN(1)+HOST(addr_len)+PORT_LEN(1)+PORT_STR(port_len)`（`PACKET_IPEndPoint` cpp:39），域名走异步 DNS（`GetAddressByHostName`），并过防火墙端口/域名/网段过滤。
- INFO：`1 + sizeof(VirtualEthernetInformation)`（packed 28B：BandwidthQoS/Incoming/Outgoing/ExpiredTime，网络序）+ 可选 `[2B 大端 json_len + 扩展 JSON]`，旧客户端可忽略尾部。
- MUX/MUXON：packed 结构 `{il, vlan(2), max_connections(2), acceleration(1), ordering_caps(1)}` 与 `{il, vlan(2), seq(4), ack(4)}`（cpp:466-488）。
- 数据面包（`VirtualEthernetPacket.cpp`）`PACKET_HEADER`（packed 23B）：`mask_id(4,随机)` + `header_length(1,LCG 混淆)` + `session_id(4,符号位区分 UDP/IP 封装)` + `checksum(2,inet_chksum)` + `posedo(12:src_ip/src_port/dst_ip/dst_port)` + 负载，再套 ssea 混淆（delta_encode、shuffle_data、masked_xor_random_next，密钥因子 `kf=random_next(key.kf*mask_id)`）。

## 5. 加密与密钥派生

双密文体系：`Ciphertext`（`Ciphertext.h`）包装 RC4 或 `EVP`（`EVP.cpp`，OpenSSL `EVP_Cipher*` 或硬件 AESNI `aesni::AES`）。`EVP::initKey`（cpp:199）：`EVP_BytesToKey(cipher, EVP_md5(), 无盐, password, 1 轮)` 生成 key，再 `iv=MD5("Ppp@"+method+"."+key+"."+password)`，最后用 RC4 对 iv 二次混淆。

**密钥派生**（`VirtualEthernetPacket::Ciphertext`，`VirtualEthernetPacket.cpp:623`）：`ivv_string = guid(32hex) + "/" + fsid(32hex) + "\\" + id(32) + ";"`；`protocol = Ciphertext(key.protocol, key.protocol_key + ivv_string)`、`transport = Ciphertext(key.transport, key.transport_key + ivv_string)`。传输层在构造时先用裸 `key.protocol/transport` 初始化（`ITransmission.cpp:994`），握手交换随机 `ivv` 后按 `protocol_key + ("+"+ivv)` 重建（cpp:1090/1137）——即每个会话的 IV 材料都带新鲜随机分量。

**用法**：数据包负载用 transport 密文加/解密（`PackBy`/`STATIC_Unpack`），头部 `checksum` 起的字段用 protocol 密文；传输层每帧 `Transmission_Packet_Encrypt` 三层处理（transport 加密负载 → protocol 加密头 → header_kf 派生负载混淆，`safest=握手前`）。开关由 `key.plaintext`/`IsHaveCiphertext` 控制。握手前后帧还叠加 base94 可打印化。

## 6. 服务端 IPv6 数据面（nat66/gua）

核心不在独立文件，而是散在 `VirtualEthernetSwitcher.cpp` 的 IPv6 段落，配平台实现：
- 数据面核心：`OpenIPv6TransitIfNeed`（cpp:2405，创建 TAP `ipv6_transit_tap_`）、`ReceiveIPv6TransitPacket`（cpp:2306，前缀匹配→源地址校验（拒绝 unspecified/multicast/loopback/link-local）→`FindIPv6Exchanger(destination)`→MSS 钳制→`SendIPv6PacketToClient` 封装 `NAT` action）、`SendIPv6TransitPacket`（cpp:2226，Linux 上按 `GetPreferredTunFd` 做多队列亲和）、`AddIPv6Exchanger`（cpp:876）、`AddIPv6NeighborProxy/AddIPv6TransitRoute`（NDP 代理与主机路由，cpp:1343/1192）、`TickIPv6Leases`（cpp:3508）。运行状态机：`GetIPv6RuntimeState()` 0=off/1=nat66/2=gua/3=failed。
- 平台实现：Linux 在 `linux/ppp/ipv6/LINUX_IPv6Auxiliary.cpp`（`ip6tables -t nat POSTROUTING ... MASQUERADE` + FORWARD 放行，sysctl 快照）；macOS 在 `darwin/ppp/ipv6/DARWIN_IPv6Auxiliary.cpp`（pf anchor `openppp2/nat66`）；Windows 在 `windows/ppp/win32/network/WfpNat66.cpp`（签名 WFP 驱动）与 `windows/ppp/ipv6/WIN32_IPv6Auxiliary.cpp`；`ppp/ipv6/IPv6Auxiliary.cpp` 做平台分发。解析/校验工具在 `ppp/ipv6/IPv6Packet.h`（TryParsePacket/PrefixMatch/ComputePseudoChecksum）与 `ppp/app/server/VirtualEthernetIPv6.h`。

## 7. 设计亮点与潜在问题

**亮点**：
1. 抗识别能力极强：双层密文 + base94/delta/shuffle/XOR/LCG 头长五重混淆，握手包带随机填充与 dummy NOP，显著提升流量指纹与被动探测难度。
2. MUX 调度演进清晰：从 compat 全局序号，到 flow-v2 每流独立 DSN 消除跨流队头阻塞，再到 M4 NACK/ACK 指数退避重传；M1 语义宁可整体重建也不跳过用户数据，取舍明确。
3. turbo 动态链路池的退役协议很细致：`retiring_` + `inflight_` 计数 + 迟到完成防护 + 池目标由近因质量推导，热增删链路不破坏已建立会话。
4. 锁粒度与生命周期处理成熟：`Open`/`AddNewExchanger` 刻意拆分锁窗口避免锁反转与死锁；vmux strand 上统一调度避免数据竞争；大量防御性边界检查与 teardown guard。
5. 兼容性设计：INFO 尾随扩展 JSON、MUX `ordering_caps` 缺省为 compat、老客户端 LAN/Arp 路径保留，均向后兼容。

**潜在问题**：
1. **信任模型偏弱**：GUID（session_id）即"用户名"，握手仅靠共享静态密钥 XOR 混淆传输，无挑战-应答/签名；未启用管理面板时服务端对 GUID 不做任何额外校验，窃得密钥者可任意伪装 GUID。
2. **混淆与校验依赖保密而非密码学强度**：数据包 checksum 用无密钥 `inet_chksum`，header_length 映射与 shuffle/xor 的强度完全系于 `key.kf`；`EVP_BytesToKey` 采用 MD5 单轮、无盐，方法名可推断时密钥拉伸不足。
3. 非 mux 直连路径（`Connect`）不鉴权即建 `VirtualEthernetNetworkTcpipConnection`，与 mux 路径的鉴权强度不一致，且存在被利用作匿名隧道的面。
4. flow-v2 的发送缓存 `tx_flow_cache_` 与接收缓冲按连接设上限，但缺少**全局**内存预算，大量慢连接并发时可能整体放大内存；NACK/ACK 控制帧本身不可靠，ACK 丢失可造成发送缓存积压（虽有重发兜底）。
5. `DoKeepAlived` 间隔在 [1s, max_timeout] 随机，`max_timeout` 较大时心跳稀疏，运营商 NAT 会话可能先于应用心跳被回收；而 `PacketInput` 对未知 action 一律判死断开，抗畸形帧攻击面偏激进。
6. 平台面割裂且门槛高：Windows NAT66 依赖签名 WFP 驱动，macOS 依赖 pf anchor 挂载，失败仅置 `ipv6_runtime_state_=3`，缺少自动回退 IPv4 的降级路径。

**总结**：openppp2 服务端是一个"混淆优先、性能其次、兼容兜底"的 VPN 实现——传输层五重混淆 + 双密文、MUX 多链路竞争/均衡/分条调度 + flow-v2 每流重排与 NACK 重传、管理面板二次鉴权与配额下发、nat66/gua 双模式 IPv6 数据面，架构分层清晰、工程细节扎实；其安全弱点集中在"共享密钥即一切"的信任模型与基于保密性的混淆强度上，适合作为隐蔽代理/抗封锁场景，而不应被当作强认证、强完整性保障的企业级 VPN。
