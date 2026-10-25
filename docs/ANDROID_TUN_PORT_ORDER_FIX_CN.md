# 安卓 TUN 端口字节序缺陷：连接永远连错端口（已修复）

## 1. 症状

新构建（含 `bb750e0e` "fix: stabilize TAP Wintun DNS and TUI core startup"）装到手机后，
"本该走隧道"的流量看起来没有进隧道：隧道显示已连接，服务端却看不到任何用户流量，
被代理的站点全部超时；客户端日志里大量 `mux connect_yield failed` 与
`RinetdConnection::Open: async_connect failed ... Connection timed out`，随后回落到直连。

## 2. 关键判据（可复用）

把"内核里的真实四元组"和"客户端解析出的四元组"放在一起对比，一次就能定性：

```powershell
# 手机侧发起一个已知端口的请求
adb shell 'nohup curl -s -m 12 -o /dev/null http://93.184.216.34:8080/ &'
adb shell "cat /proc/net/tcp" | Select-String '1F90'   # 1F90 = 8080
adb logcat -d -s ppp:D | Select-String 'LwIpBeginAccept'
```

实测（2026-10-25，realme RMX3706 / Android 16）：

```
内核:   192.168.12.32:37402  ->  93.184.216.34:1F90 (8080)
客户端: source=10.0.0.2:6802, destination=93.184.216.34:36895
```

`36895 = 0x901F`，正是 `0x1F90` 的字节互换；源端口 `6802` 与 `37402` 同样互为互换。
443 也稳定表现为 `47873`（`0xBB01`），与端口无关的目的地址则完全正确。

## 3. 根因

### 3.1 lwIP 已把 TCP 头转成主机序，代码又转了一次

`common/lwip/core/tcp_in.c:227` 在 `tcp_input()` 开头就地转换：

```c
tcphdr->src  = lwip_ntohs(tcphdr->src);
tcphdr->dest = lwip_ntohs(tcphdr->dest);
```

`before_accept` 在 `tcp_in.c:664` 才被调用，拿到的端口已是主机序。
但 `bb750e0e` 把 `common/libtcpip/netstack.cpp` 里原本正确的

```cpp
endpoint dep(address_v4(dip), h->dest);
```

改成了 `ntohs(h->dest)`，于是每个被接受的连接都换了端口。
服务端按 `<目的IP>:<互换后的端口>` 去连接，必然超时，
客户端再回落到直连（同样错端口），所以"隧道里什么都过不去"。

### 3.2 回环 accept 路径没有把端口转回网络序（第二处）

修好 3.1 后连接能建立（`MuxOrConnect: success`），但 HTTP 仍然 `Empty reply`。
日志显示延迟 SYN 回放后，回环 accept 又建了一条 `port=20480`（= `swap(80)`）的连接：
`lwip::netstack::link()` 返回的是主机序端口（lwIP 的 `pcb->local_port` 即主机序），
而 `LwIpAcceptLink()` 的键和表都按网络序保存（`LwIpBeginAccept` 调用时传的是 `htons(port)`），
两边约定不一致 → 键对不上 → 重复连接、原连接失去数据面。

## 4. 修复

| 文件 | 修改 |
|---|---|
| `common/libtcpip/netstack.cpp` | `dep(..., h->dest)` / `sep(..., h->src)`，去掉二次 `ntohs` |
| `ppp/ethernet/VNetstack.cpp` | 回环 accept 调 `LwIpAcceptLink` 前把 `netstack::link()` 的主机序端口 `htons()` 成网络序，与 `LwIpBeginAccept` 的约定一致 |

## 5. 验证（Android 16，修复后 debug 构建）

```
http://example.com/            -> 200, 0.9 s
https://example.com/           -> 200, 0.8 s
https://ipinfo.io/ip           -> 23.249.25.106      (服务器出口)
https://1.1.1.1/cdn-cgi/trace  -> ip=23.249.25.106 loc=JP colo=NRT
http://www.baidu.com/          -> 200               (geo 分流，直连)
```

日志侧：`BeginAcceptClient ... port=80/443`、`MuxOrConnect: success`，
不再出现 `swap(port)` 的第二条连接。

## 6. 复盘

- 端口类故障先比对 `/proc/net/tcp` 与客户端解析日志，比抓包更快。
- `ntohs`/`htons` 是否必要，取决于"这个缓冲区有没有被转换过"；
  lwIP 的 `before_accept` 属于**已转换**的一侧，表/键属于**网络序**的一侧。
- 只改一侧会让两条路径的键不一致，表现为"连接成功但数据不通"，比彻底连不上更难查。
