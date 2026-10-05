# Windows 物理网卡 DNS 的保存、接管和恢复

Windows TUN 客户端按选中的物理出口网卡处理 DNS。有线和无线采用相同逻辑，其他物理网卡不参与此次接管。

| 启动前的设置 | VPN 运行时 | 正常停止或异常退出后再次启动 |
| --- | --- | --- |
| IPv4 自动 DNS | 127.0.0.1（原来存在有效 IPv4 DNS 时） | 恢复自动 DNS，由当前网络重新下发地址 |
| IPv4 手动 DNS | 127.0.0.1 | 恢复原静态服务器及顺序 |
| IPv6 自动 DNS | ::1（原来存在有效 IPv6 DNS 时） | 恢复自动 DNS |
| IPv6 手动 DNS | ::1 | 恢复原静态服务器及顺序 |

有效 DNS 列表只用于确定是否需要 IPv4/IPv6 环回解析服务，不能用来区分自动和手动。DNS 的自动状态也不能从 IP 的 DHCPEnabled 推断。

`DnsLifecycle.cpp` 分别读取 `Tcpip` 与 `Tcpip6` 网卡 GUID 对应的手动 `NameServer` 覆盖值。没有手动覆盖表示自动模式；`DhcpNameServer` 和 WMI 的有效 DNS 列表不作为静态快照。恢复自动模式使用 `netsh interface ipv4/ipv6 set dnsservers source=dhcp`。恢复手动模式使用 `source=static`，依次添加其余服务器，任何一条命令失败都不能当作全部恢复成功。

命令格式见 [Microsoft netsh interface 文档](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/netsh-interface)。

## 持久化记录

修改物理 DNS 前，原模式和静态地址写入 `HKLM\SOFTWARE\OpenPPP2\DnsRecovery\{网卡GUID}-ipv4/ipv6` 并刷新到磁盘。记录包括 `OriginalAutomatic`、`OriginalNameServer`、进程 PID、进程创建时间和 `Ready` 标志。未成功保存记录时不修改物理 DNS。

x86 与 x64 核心统一使用注册表 64 位视图，切换核心架构后仍能读取同一份恢复记录。

同一进程重新连接时保留首份原始快照，避免把 VPN 自己的 127.0.0.1 当成启动前设置。恢复失败保留记录；恢复一组静态 DNS 中途失败时，保存恢复进度，允许重试。

正常恢复与 DNS 守护操作通过全局互斥锁串行化。恢复记录删除后，延迟到达的守护操作不会重新写入环回地址。IPv6 地址配置辅助流程只修改 TUN DNS，物理 DNS 的保存和恢复由这一套记录统一负责。

## 异常退出恢复

下一次 TUN 客户端启动会在读取物理网卡有效 DNS 前检查记录：

1. PID 和创建时间都匹配的存活进程仍拥有记录，不能抢占。
2. 进程已退出时，按网卡 GUID 查找当前接口，允许接口编号变化。
3. 当前 DNS 仍是对应环回地址，且 UDP/TCP 53 端口都没有解析服务时，恢复原模式。
4. 当前 DNS 已被用户或其他程序修改时保留该新设置，撤销旧的恢复记录。
5. 网卡暂时不可用、端口被其他服务占用或恢复失败时保留记录。无法安全恢复的启动会报告错误。

旧版本产生的残留没有原始恢复记录，无法可靠推断当初是自动 DNS 还是手动 DNS。该实现不会仅凭环回地址就覆盖用户设置。历史残留需先由用户确定原模式并恢复一次，之后的新会话才能保存可靠快照。

## 验证

`tests/windows_dns_lifecycle_test.cpp` 编译实际生命周期实现，使用进程隔离的 HKCU 测试记录和模拟网卡/netsh 边界，不修改真实网卡。Windows CI 在 x64 和 x86 核心构建前运行这些测试，覆盖自动/手动、混合协议族、守护刷新、异常退出、PID 重用、接口编号变化、其他 DNS 服务、用户修改、部分恢复失败、保存中断和无效地址。

真实网卡验收需要编译后的核心：分别在有线与无线、自动与手动的初始状态下启动和停止 VPN，检查自动模式仍为自动、静态模式的服务器及顺序不变；强制结束核心后再次启动，检查恢复日志及原模式。单纯比较有效 DNS 地址不足以证明自动模式已恢复。
