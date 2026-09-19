import json
import importlib.util
import ipaddress
import pathlib
import re
import struct
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def load_pcap_analyzer():
    path = ROOT / "tests/tools/analyze_pktmon_pcap.py"
    spec = importlib.util.spec_from_file_location("analyze_pktmon_pcap", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def load_app_probe():
    path = ROOT / "tests/tools/dataplane_app_probe.py"
    spec = importlib.util.spec_from_file_location("dataplane_app_probe", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class DataplaneReliabilitySourceTests(unittest.TestCase):
    def test_pktmon_analyzer_parses_ipv4_and_mss_without_dependencies(self) -> None:
        analyzer = load_pcap_analyzer()
        tcp = bytearray(24)
        struct.pack_into("!HH", tcp, 0, 40000, 443)
        tcp[12] = 6 << 4
        tcp[13] = 0x02
        tcp[20:24] = bytes((2, 4, 5, 80))
        ip = bytearray(20 + len(tcp))
        ip[0] = 0x45
        struct.pack_into("!H", ip, 2, len(ip))
        ip[8] = 64
        ip[9] = 6
        ip[12:16] = bytes((192, 0, 2, 1))
        ip[16:20] = bytes((198, 51, 100, 1))
        checksum = analyzer.checksum_valid

        def folded_checksum(data: bytes) -> int:
            if len(data) & 1:
                data += b"\0"
            total = sum(struct.unpack(f"!{len(data) // 2}H", data))
            while total >> 16:
                total = (total & 0xFFFF) + (total >> 16)
            return (~total) & 0xFFFF

        pseudo = bytes(ip[12:20]) + bytes((0, 6)) + struct.pack("!H", len(tcp))
        struct.pack_into("!H", tcp, 16, folded_checksum(pseudo + tcp))
        ip[20:] = tcp
        struct.pack_into("!H", ip, 10, folded_checksum(ip[:20]))
        self.assertTrue(checksum(bytes(ip[:20])))
        self.assertEqual(analyzer.find_ipv4(b"prefix" + ip), bytes(ip))
        self.assertTrue(analyzer.transport_checksum_valid(bytes(ip), 20, 6))
        self.assertEqual(analyzer.tcp_mss(bytes(tcp)), 1360)
        self.assertEqual(
            analyzer.trace_key(
                {
                    "protocol": "1",
                    "address_family": "4",
                    "src_ip": "1.1.1.1",
                    "dst_ip": "192.168.13.25",
                    "icmp_type": "0",
                    "icmp_identifier": "7",
                    "icmp_sequence": "9",
                }
            ),
            "icmp|4|1|1.1.1.1|192.168.13.25|0|7|9",
        )
        self.assertEqual(
            analyzer.trace_key(
                {
                    "protocol": "17",
                    "address_family": "4",
                    "src_ip": "192.168.13.1",
                    "src_port": "53",
                    "dst_ip": "192.168.13.25",
                    "dst_port": "50000",
                    "dns_transaction_id": "4660",
                }
            ),
            "dns|4|192.168.13.1|53|192.168.13.25|50000|4660",
        )

        source_v6 = ipaddress.ip_address("2001:db8::1").packed
        destination_v6 = ipaddress.ip_address("2001:db8::2").packed
        udp = bytearray(8)
        struct.pack_into("!HHH", udp, 0, 53, 50000, len(udp))
        pseudo_v6 = source_v6 + destination_v6 + struct.pack("!I", len(udp)) + bytes((0, 0, 0, 17))
        struct.pack_into("!H", udp, 6, folded_checksum(pseudo_v6 + udp))
        ipv6 = bytearray(40 + len(udp))
        ipv6[0] = 0x60
        struct.pack_into("!H", ipv6, 4, len(udp))
        ipv6[6] = 17
        ipv6[7] = 64
        ipv6[8:24] = source_v6
        ipv6[24:40] = destination_v6
        ipv6[40:] = udp
        self.assertEqual(analyzer.find_ipv6(b"prefix" + ipv6), bytes(ipv6))
        self.assertEqual(analyzer.locate_ipv6_transport(bytes(ipv6)), (40, 17, True))
        self.assertTrue(analyzer.transport_checksum_valid_v6(bytes(ipv6), 40, 17))

        icmp = bytearray(8)
        icmp[0] = 0
        struct.pack_into("!HH", icmp, 4, 7, 9)
        struct.pack_into("!H", icmp, 2, folded_checksum(icmp))
        reply = bytearray(20 + len(icmp))
        reply[0] = 0x45
        struct.pack_into("!H", reply, 2, len(reply))
        reply[8] = 64
        reply[9] = 1
        reply[12:16] = bytes((1, 1, 1, 1))
        reply[16:20] = bytes((192, 168, 13, 25))
        reply[20:] = icmp
        struct.pack_into("!H", reply, 10, folded_checksum(reply[:20]))

        def block(block_type: int, body: bytes) -> bytes:
            padding = bytes((-len(body)) & 3)
            length = 12 + len(body) + len(padding)
            return struct.pack("<II", block_type, length) + body + padding + struct.pack("<I", length)

        shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))
        idb = block(1, struct.pack("<HHI", 1, 0, 65535))
        epb = block(6, struct.pack("<IIIII", 0, 0, 1, len(reply), len(reply)) + reply)
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "trace.pcapng"
            log = pathlib.Path(directory) / "core.log"
            app_log = pathlib.Path(directory) / "app.jsonl"
            capture.write_bytes(shb + idb + epb)
            log.write_text(
                "DATAPLANE TUN_RX flow_id=42 protocol=1 address_family=4\n"
                "DATAPLANE POLICY_SELECTED flow_id=42 protocol=1 address_family=4\n"
                "DATAPLANE REMOTE_TX flow_id=42 protocol=1 address_family=4\n"
                "DATAPLANE REMOTE_RX flow_id=42 protocol=1 address_family=4\n"
                "DATAPLANE PACKET_BUILT flow_id=42 protocol=1 address_family=4\n"
                "DATAPLANE WINTUN_ALLOCATED flow_id=42 protocol=1 address_family=4\n"
                "DATAPLANE WINTUN_SUBMITTED WINTUN_SUBMIT_OK flow_id=42 protocol=1 "
                "address_family=4 src_ip=1.1.1.1 src_port=0 dst_ip=192.168.13.25 "
                "dst_port=0 dns_transaction_id=-1 icmp_type=0 icmp_code=0 "
                "icmp_identifier=7 icmp_sequence=9\n",
                encoding="utf-8",
            )
            app_log.write_text(
                json.dumps(
                    {
                        "stage": "APP_COMPLETED",
                        "ok": True,
                        "protocol": 1,
                        "address_family": 4,
                        "src_ip": "1.1.1.1",
                        "src_port": 0,
                        "dst_ip": "192.168.13.25",
                        "dst_port": 0,
                        "icmp_type": 0,
                        "icmp_identifier": 7,
                        "icmp_sequence": 9,
                        "dns_transaction_id": -1,
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            result = analyzer.analyze([capture], log, app_log)
            self.assertEqual(result["icmp"]["matched"], 0)
            self.assertEqual(result["icmp"]["unique_replies"], 1)
            self.assertEqual(result["core_os_correlation"]["os_observed_matches"], 1)
            self.assertEqual(result["core_os_correlation"]["missing_flow_ids"], [])
            self.assertEqual(result["core_os_correlation"]["request_correlated_submissions"], 1)
            self.assertEqual(result["core_os_correlation"]["tun_rx_to_os_observed_matches"], 1)
            self.assertEqual(result["core_os_correlation"]["request_path_complete"], 1)
            self.assertEqual(result["core_os_correlation"]["remote_roundtrip_complete"], 1)
            self.assertEqual(result["core_os_correlation"]["remote_os_observed_complete"], 1)
            self.assertTrue(result["core_os_correlation"]["injection_stage_sets_match"])
            self.assertEqual(result["core_os_correlation"]["app_completed_matches"], 1)
            self.assertEqual(result["core_os_correlation"]["fully_observed_matches"], 1)

    def test_app_probe_builds_stable_icmp_and_dns_identifiers(self) -> None:
        probe = load_app_probe()
        packet = bytearray(struct.pack("!BBHHH", 8, 0, 0, 7, 9) + bytes(range(1, 32)))
        struct.pack_into("!H", packet, 2, probe.checksum(bytes(packet)))
        self.assertEqual(probe.checksum(bytes(packet)), 0)
        self.assertEqual(probe.encode_dns_name("example.com"), b"\x07example\x03com\x00")
        self.assertEqual(probe.ptr_query_name("1.2.3.4"), "4.3.2.1.in-addr.arpa")
        source = read("tests/tools/dataplane_app_probe.py")
        self.assertIn('"stage": "APP_COMPLETED" if ok else "APP_TIMEOUT"', source)
        self.assertIn('(1, "A", args.dns_name)', source)
        self.assertIn('(28, "AAAA", args.dns_name)', source)
        self.assertIn('(12, "PTR", ptr_query_name(args.dns_ptr_address))', source)

    def test_default_tun_mtu_and_mss_are_safe(self) -> None:
        settings = json.loads(read("appsettings.json"))
        tun = settings["client"]["tun"]
        self.assertEqual(tun["mtu"], 1400)
        self.assertTrue(tun["mss-clamp"])
        self.assertLessEqual(tun["mss-v4"], tun["mtu"] - 40)
        self.assertLessEqual(tun["mss-v6"], tun["mtu"] - 60)
        mss = read("ppp/app/protocol/VirtualEthernetTcpMss.h")
        self.assertIn("ntohs(iphdr->flags)", mss)
        self.assertIn("ip_hdr::IP_MF", mss)
        self.assertIn("ip_hdr::IP_OFFMASK", mss)
        configuration = read("ppp/configurations/AppConfiguration.cpp")
        self.assertIn("mtu_present && !mss_v4_present", configuration)
        self.assertIn("mtu_present && !mss_v6_present", configuration)
        self.assertIn("config.client.tun.mtu - 40", configuration)
        self.assertIn("config.client.tun.mtu - 60", configuration)

        network_interface = read("windows/ppp/win32/network/NetworkInterface.cpp")
        mtu_reader = re.search(
            r"int GetInterfaceMtu\(int interface_index\) noexcept([\s\S]*?)"
            r"static bool SetInterfaceMtuIpInterfaceEntry",
            network_interface,
        )
        self.assertIsNotNone(mtu_reader)
        self.assertIn("GetIpInterfaceEntry", mtu_reader.group(1))
        self.assertIn("ipInterfaceRow.NlMtu", mtu_reader.group(1))
        self.assertLess(
            mtu_reader.group(1).index("GetIpInterfaceEntry"),
            mtu_reader.group(1).index("GetIfEntry"),
        )
        windows_tap = read("windows/ppp/tap/TapWindows.cpp")
        set_mtu = re.search(
            r"bool TapWindows::SetInterfaceMtu\(int mtu\) noexcept([\s\S]*?)"
            r"void TapWindows::Dispose",
            windows_tap,
        )
        self.assertIsNotNone(set_mtu)
        self.assertIn("network::SetInterfaceMtu(interface_index, mtu)", set_mtu.group(1))
        self.assertNotIn("SetInterfaceMtuIpSubInterface", set_mtu.group(1))

    def test_proxy_direct_policy_uses_explicit_route_action(self) -> None:
        header = read("ppp/net/native/rib.h")
        implementation = read("ppp/net/native/checksum.cpp")
        switcher = read("ppp/app/client/VEthernetNetworkSwitcher.cpp")
        self.assertIn("enum class RouteOrigin", header)
        self.assertIn("enum class RouteAction", header)
        self.assertIn("typedef struct RouteEntry", header)
        self.assertIn("typedef struct RouteEntry6", header)
        self.assertIn("if (prefix == entry.Prefix)", implementation)
        self.assertRegex(
            switcher,
            r"TryGetBestRoute[\s\S]{0,400}RouteAction::Direct",
        )
        self.assertIn('"route-origin-policy": true', read("appsettings.json"))
        linux_tap = read("linux/ppp/tap/TapLinux.cpp")
        self.assertRegex(
            linux_tap,
            r"AddRoute\(ip, prefix_mask, gw,[\s\S]{0,180}RouteOrigin::PhysicalSystem,[\s\S]{0,120}RouteAction::Direct",
        )
        self.assertIn("RouteInformationTable6::AddAllRoutesByIPList", implementation)
        self.assertIn("AddRoute(dst, entry.Prefix, nh, entry.Origin, entry.Action)", switcher)

    def test_windows_best_interface_success_condition_is_not_inverted(self) -> None:
        switcher = read("ppp/app/client/VEthernetNetworkSwitcher.cpp")
        self.assertIn("const DWORD result = ::GetBestInterface", switcher)
        self.assertIn("if (result != NO_ERROR)", switcher)
        self.assertNotIn("if (!::GetBestInterface", switcher)

    def test_windows_direct_sockets_are_protected_before_io_and_can_fallback(self) -> None:
        switcher_header = read("ppp/app/client/VEthernetNetworkSwitcher.h")
        switcher = read("ppp/app/client/VEthernetNetworkSwitcher.cpp")
        exchanger = read("ppp/app/client/VEthernetExchanger.cpp")
        rinetd_header = read("ppp/net/rinetd/RinetdConnection.h")
        rinetd = read("ppp/net/rinetd/RinetdConnection.cpp")
        tcpip_header = read("ppp/app/client/VEthernetNetworkTcpipConnection.h")
        tcpip = read("ppp/app/client/VEthernetNetworkTcpipConnection.cpp")

        self.assertIn("ProtectWindowsSocket(intptr_t socket_handle", switcher_header)
        protect_start = switcher.index("bool VEthernetNetworkSwitcher::ProtectWindowsSocket")
        protect_end = switcher.index("#endif", protect_start)
        protect_body = switcher[protect_start:protect_end]
        for token in ("IP_UNICAST_IF", "IPV6_UNICAST_IF", "GetBestInterfaceEx"):
            self.assertIn(token, protect_body)
        self.assertIn("selected_interface != (DWORD)underlying->Index", protect_body)
        self.assertIn("selected_interface == (DWORD)tap_index", protect_body)

        transmission_start = exchanger.index("VEthernetExchanger::OpenTransmission")
        transmission = exchanger[transmission_start:]
        self.assertLess(
            transmission.index("ProtectWindowsSocket"),
            transmission.index("async_connect(*socket"),
        )
        rinetd_open = rinetd[rinetd.index("bool RinetdConnection::Open"):]
        self.assertLess(rinetd_open.index("ProtectSocket("), rinetd_open.index("async_connect(*socket"))
        self.assertIn("socket_protection_rejected_ = true", rinetd_open)
        self.assertIn("WasSocketProtectionRejected", rinetd_header)
        self.assertIn("WasSocketProtectionRejected() ? 1 : -1", tcpip_header)
        self.assertIn("reason=direct_socket_protection_rejected", tcpip)

        dns_start = switcher.index("bool VEthernetNetworkSwitcher::RedirectDnsServer")
        dns = switcher[dns_start:]
        self.assertLess(dns.index("ProtectWindowsSocket"), dns.index("socket->send_to"))
        ipv6_dns_start = switcher.index("bool VEthernetNetworkSwitcher::OnIPv6UdpPacketInput")
        ipv6_dns_end = switcher.index("bool VEthernetNetworkSwitcher::OnIPv6IcmpPacketInput", ipv6_dns_start)
        ipv6_dns = switcher[ipv6_dns_start:ipv6_dns_end]
        self.assertIn("ProtectWindowsSocket", ipv6_dns)
        self.assertLess(ipv6_dns.index("ProtectWindowsSocket"), ipv6_dns.index("socket->send_to"))

    def test_static_echo_receives_have_operation_local_state(self) -> None:
        exchanger = read("ppp/app/client/VEthernetExchanger.cpp")
        start = exchanger.index("bool VEthernetExchanger::StaticEchoLoopbackSocket")
        end = exchanger.index("bool VEthernetExchanger::StaticEchoAddRemoteEndPoint", start)
        receive_loop = exchanger[start:end]
        self.assertIn("receive_buffer", receive_loop)
        self.assertIn("source_ep", receive_loop)
        self.assertNotIn("static_echo_source_ep_", receive_loop)
        self.assertNotIn("buffer_.get()", receive_loop)

    def test_static_echo_recovery_requires_a_valid_response(self) -> None:
        exchanger = read("ppp/app/client/VEthernetExchanger.cpp")
        header = read("ppp/app/client/VEthernetExchanger.h")
        send_start = exchanger.index(
            "bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const std::shared_ptr<Byte>&"
        )
        send_end = exchanger.index("StaticEchoReadPacket", send_start)
        send_body = exchanger[send_start:send_end]
        self.assertNotIn("static_echo_degraded_until_.store(0", send_body)
        self.assertNotIn("static_echo_consecutive_failures_.store(0", send_body)

        receive_start = exchanger.index("int VEthernetExchanger::StaticEchoYieldReceiveForm")
        receive_end = exchanger.index("bool VEthernetExchanger::Sleep", receive_start)
        receive_body = exchanger[receive_start:receive_end]
        self.assertIn("static_echo_server_ep_set_.find(normalized_source)", receive_body)
        self.assertIn("packet->Id != static_echo_session_id_", receive_body)
        self.assertLess(
            receive_body.index("if (!StaticEchoPacketInput(packet))"),
            receive_body.index("static_echo_receive_packets_.fetch_add"),
        )
        self.assertIn("static_echo_degraded_until_.store(0", receive_body)
        for field in (
            "response_timeouts",
            "source_rejected",
            "session_mismatch",
            "output_failed",
            "tx_queued",
            "tx_completed",
            "tx_failed",
        ):
            self.assertIn(field, read("main.cpp"))
            self.assertIn(field, read("tui/src/rpc/schema.rs"))
        self.assertIn("static_echo_source_rejected_", header)
        self.assertIn("static_echo_response_timeouts_", header)
        clean_start = exchanger.index("void VEthernetExchanger::StaticEchoClean")
        clean_end = exchanger.index("bool VEthernetExchanger::StaticEchoAllocated", clean_start)
        clean_body = exchanger[clean_start:clean_end]
        self.assertIn("static_echo_server_ep_set_.clear()", clean_body)
        self.assertIn("static_echo_server_ep_balances_.clear()", clean_body)
        timeout_start = exchanger.index("bool VEthernetExchanger::StaticEchoSwapAsynchronousSocket")
        timeout_end = exchanger.index("bool VEthernetExchanger::StaticEchoGatewayServer", timeout_start)
        timeout_body = exchanger[timeout_start:timeout_end]
        self.assertIn("static_echo_response_timeouts_.fetch_add", timeout_body)
        self.assertIn("failures >= 3", timeout_body)
        self.assertIn("compare_exchange_strong", timeout_body)
        self.assertIn("configuration->udp.static_.icmp", timeout_body)
        self.assertIn("static_echo_enabled", timeout_body)

    def test_tui_always_forwards_core_log_file(self) -> None:
        gui = read("tui/src/gui.rs")
        terminal = read("tui/src/terminal/mod.rs")
        for source in (gui, terminal):
            self.assertIn('set_command_argument(&mut args, "--log-file", &absolute)', source)
            self.assertIn('remove_command_argument(&mut args, "--log-file")', source)
            self.assertIn("核心日志路径校验失败", source)
        self.assertIn('log_file: "./ppp-core.log".to_string()', read("tui/src/core/settings.rs"))
        settings = read("tui/src/core/settings.rs")
        self.assertIn("pub fn validate_core_log_path", settings)
        self.assertIn("OpenOptions::new()", settings)
        self.assertIn("核心日志已启用，但日志文件路径为空", settings)
        self.assertIn('normalize_log_level(&settings.log_level) == "none"', settings)
        self.assertIn("fs::canonicalize(parent)?.join(file_name)", settings)
        main = read("main.cpp")
        self.assertIn("static bool OpenCoreLogFile", main)
        self.assertGreaterEqual(main.count("if (!OpenCoreLogFile(&log_open_error))"), 2)
        self.assertIn("CoreApiSignalStartup(handle, false, -1, log_open_error.data())", main)
        self.assertIn("CORE_LOG_ROTATE_BYTES = 64 * 1024 * 1024", main)
        self.assertIn("CORE_LOG_MAX_FILES = 4", main)
        self.assertIn("MaintainCoreLogFile(now)", main)
        self.assertIn("ExchangeLogStream(stdout)", main)
        self.assertIn("TransformLogStream", main)
        maintain_start = main.index("static void MaintainCoreLogFile")
        maintain_end = main.index("static void RpcLogSink", maintain_start)
        self.assertNotIn("FlushLogs()", main[maintain_start:maintain_end])
        diagnostics = read("ppp/stdafx.cpp")
        self.assertIn("wait_for(lock, std::chrono::seconds(1)", diagnostics)
        self.assertIn("FILE* ExchangeLogStream(FILE* stream)", diagnostics)
        self.assertIn("FILE* TransformLogStream", diagnostics)

    def test_wintun_submit_has_validation_and_completion_counters(self) -> None:
        tap = read("windows/ppp/tap/TapWindows.cpp")
        self.assertIn("ValidateWintunInjectionPacket", tap)
        self.assertIn("WINTUN_VALIDATE_FAIL", tap)
        self.assertIn("wintun_built_packets_.fetch_add", tap)
        self.assertIn("WINTUN_ALLOCATED", tap)
        self.assertIn("WINTUN_SUBMITTED", tap)
        self.assertIn("IPPROTO_ICMPV6", tap)
        self.assertIn("ppp::ipv6::ComputePseudoChecksum", tap)
        self.assertIn("udp_checksum == 0", tap)
        self.assertIn("LocateWintunIpv6Transport", tap)
        self.assertIn("extension_count > 8", tap)
        self.assertIn("next_header == 44", tap)
        self.assertIn("fragment_seen", tap)
        self.assertIn("fragment_flags & 0x0006U", tap)
        self.assertIn("transport_view.payload + 4", tap)
        self.assertIn("transport_view.payload + 6", tap)
        self.assertIn("udp->chksum == 0 ||", tap)
        self.assertIn("ip_hdr::IP_OFFMASK", tap)
        self.assertIn("if (!transport_view.checksum_verifiable)", tap)
        self.assertIn("WINTUN_SUBMIT_OK", tap)
        self.assertIn("WINTUN_SUBMIT_FAIL", tap)
        self.assertIn("if (GetDataplaneTrace())", tap)
        self.assertIn("static void LogWintunTraceStage", tap)
        for field in (
            "src_ip=%s",
            "src_port=%u",
            "dst_ip=%s",
            "dst_port=%u",
            "ttl=%d",
            "flags=0x%x",
            "ip_checksum=0x%04x",
            "transport_checksum=0x%04x",
            "dns_transaction_id=%d",
            "icmp_type=%d",
            "icmp_code=%d",
            "icmp_identifier=%d",
            "icmp_sequence=%d",
            "interface_luid=%llu",
            "route_origin=%s",
            "route_action=%s",
            "outbound=%d",
            "elapsed_ms=%llu",
        ):
            self.assertIn(field, tap)
        self.assertIn("ntohs(udp->len) == transport_length", tap)
        self.assertIn("inet_chksum_pseudo(transport, IPPROTO_UDP", tap)
        self.assertIn("inet_chksum_pseudo(transport, IPPROTO_TCP", tap)
        self.assertIn("inet_chksum(transport, transport_length) == 0", tap)
        self.assertIn("ip->dest != expected_ipv4", tap)
        self.assertIn("memcmp(bytes + 24, expected.data(), expected.size())", tap)
        self.assertIn('GetCommandArgument("--dataplane-trace", argc, argv, "no")', read("main.cpp"))
        adapter_header = read("windows/ppp/tap/WintunAdapter.h")
        adapter = read("windows/ppp/tap/WintunAdapter.cpp")
        self.assertIn("bool* allocated = nullptr", adapter_header)
        self.assertIn("if (allocated) *allocated = true", adapter)
        self.assertIn("built_packets", read("windows/ppp/tap/TapWindows.h"))
        self.assertIn('"local", "tunnel", 1', tap)
        self.assertIn("wintun_trace_contexts_", read("windows/ppp/tap/TapWindows.h"))
        self.assertIn("TakeWintunTrace", tap)
        self.assertIn('TraceInputStage("POLICY_SELECTED"', read("ppp/app/client/VEthernetNetworkSwitcher.cpp"))
        self.assertIn('TraceInputStage("REMOTE_TX"', read("ppp/app/client/VEthernetNetworkSwitcher.cpp"))
        self.assertIn('TracePacketStage(original_bytes->Buffer.get()', read("ppp/app/client/VEthernetNetworkSwitcher.cpp"))
        self.assertIn('OutputWithTrace(packet, packet_length, "LOCAL_RX")', read("ppp/app/client/VEthernetNetworkSwitcher.cpp"))
        switcher = read("ppp/app/client/VEthernetNetworkSwitcher.cpp")
        switcher_header = read("ppp/app/client/VEthernetNetworkSwitcher.h")
        self.assertIn("bool                                                            upstream_owner = false", switcher_header)
        self.assertIn("local_response || !waiter.upstream_owner", switcher)
        self.assertIn("complete_pending(MakeLocalDnsServFail(*query), true)", switcher)
        self.assertIn("complete_pending(processed, false)", switcher)
        self.assertIn("const auto remote_tx_traced", switcher)
        self.assertIn('trace_stage("REMOTE_TX"', switcher)
        self.assertIn("transferred == request->size()", switcher)
        self.assertIn("static thread_local TapWindowsInputTrace", tap)
        self.assertIn("PeekWintunTrace", tap)

    def test_runtime_snapshot_exposes_dataplane_counters(self) -> None:
        main = read("main.cpp")
        schema = read("tui/src/rpc/schema.rs")
        fixture = json.loads(read("tui/tests/fixtures/desktop-snapshot.json"))
        for name in ("duplicate_syn_count", "static_echo", "mux", "wintun"):
            self.assertIn(name, fixture["dataplane"])
            self.assertIn(name, main)
        self.assertIn("pub dataplane: Dataplane", schema)
        self.assertIn("pub struct StaticEchoDiagnostics", schema)
        self.assertIn("pub struct MuxDiagnostics", schema)
        self.assertIn("pub struct WintunDiagnostics", schema)
        self.assertIn("get_active_stream_count", main)
        self.assertIn("get_tx_queue_bytes", main)
        self.assertIn("get_head_of_line_stall_ms", main)
        self.assertIn("get_stream_open_p95_ms", main)
        self.assertIn("get_stream_open_p50_ms", main)
        self.assertIn("get_stream_open_p99_ms", main)
        mux = read("ppp/app/mux/vmux_net.cpp")
        mux_header = read("ppp/app/mux/vmux_net.h")
        self.assertIn("record_stream_open_latency", mux)
        self.assertIn("const uint64_t stream_open_started = now_tick()", mux)
        self.assertIn("stream_open_le_1000_ms_", mux_header)
        self.assertIn("get_stream_open_percentile_ms", mux)
        self.assertIn("bucket_upper_bound", mux)
        self.assertIn("policy_selected_packets", schema)
        self.assertIn("remote_tx_packets", schema)
        self.assertIn("local_rx_packets", schema)

    def test_diagnostic_types_are_not_duplicated(self) -> None:
        exchanger = read("ppp/app/client/VEthernetExchanger.h")
        self.assertEqual(exchanger.count("struct StaticEchoDiagnostics final"), 1)
        self.assertEqual(exchanger.count("struct MuxDiagnostics final"), 1)
        self.assertEqual(exchanger.count("GetStaticEchoDiagnostics() const noexcept"), 1)
        self.assertEqual(exchanger.count("GetMuxDiagnostics() const noexcept"), 1)
        for member in (
            "mux_channel_opened_ = 0",
            "mux_channel_open_failures_ = 0",
            "mux_generation_resets_ = 0",
            "mux_fallbacks_ = 0",
            "static_echo_response_timeouts_ = 0",
        ):
            self.assertEqual(exchanger.count(member), 1)

    def test_acceptance_harness_preserves_capture_cleanup_and_gates_results(self) -> None:
        script = read("tests/tools/Invoke-DataplaneAcceptance.ps1")
        self.assertIn("[int]$PingCount = 100", script)
        self.assertIn("[int]$DnsCount = 100", script)
        self.assertRegex(
            script,
            r"\[Parameter\(Mandatory = \$true\)\]\s+\[ValidateNotNullOrEmpty\(\)\]\s+\[string\]\$CoreLog",
        )
        self.assertIn("Core log does not exist", script)
        self.assertIn("finally {", script)
        self.assertIn("& pktmon.exe stop", script)
        self.assertIn("$pingReplies -ge $minimumPingReplies", script)
        self.assertIn("$effectiveMtu -eq $ExpectedMtu", script)
        self.assertIn("$logCounters.wintun_submit_ok -gt 0", script)
        self.assertIn("$logCounters.wintun_allocated -eq $logCounters.wintun_submit_ok", script)
        self.assertIn("function Get-CoreLogFlowIds", script)
        self.assertIn("DATAPLANE REMOTE_RX", script)
        self.assertIn("DATAPLANE LOCAL_RX", script)
        self.assertIn("DATAPLANE POLICY_SELECTED", script)
        self.assertIn("DATAPLANE REMOTE_TX", script)
        self.assertIn("DATAPLANE PACKET_BUILT", script)
        self.assertIn("$injectionFlowIds = @($remoteRxFlowIds + $localRxFlowIds)", script)
        self.assertIn("Compare-Object -ReferenceObject $injectionUnique", script)
        self.assertIn("Compare-Object -ReferenceObject $packetBuiltUnique", script)
        self.assertIn("flow_stages_match = $flowStagesMatch", script)
        self.assertIn("request_stage_sets_valid = $requestStageSetsValid", script)
        self.assertIn("dataplane_app_probe.py", script)
        self.assertIn("scan_log_secrets.py", script)
        self.assertIn("--start-byte $baselineCoreLogLength", script)
        self.assertIn("secret_scan_passed = $secretScanPassed", script)
        self.assertNotIn("socks_proxy = $SocksProxy", script)
        self.assertIn("--app-log $appProbePath", script)
        self.assertIn("$fullyObservedMatches -eq $appCompletedRecords", script)
        self.assertIn("gate_scope = if ($FullGate) { 'automated_extended_runtime' } else { 'automated_base_only' }", script)
        self.assertIn("[switch]$FullGate", script)
        self.assertIn("'automated_extended_runtime'", script)
        self.assertIn("Invoke-CurlSeries -Name 'socks-ipv4'", script)
        self.assertIn("Invoke-CurlSeries -Name 'socks-domain'", script)
        self.assertIn("Invoke-CurlSeries -Name 'http-connect'", script)
        self.assertIn("Invoke-CurlSeries -Name 'https-short'", script)
        self.assertIn("Invoke-DownloadSeries -Name 'download-10mb'", script)
        self.assertIn("Invoke-DownloadSeries -Name 'download-100mb'", script)
        self.assertIn("-MinimumBytes 10000000 -Count 20", script)
        self.assertIn("-MinimumBytes 100000000 -Count 5", script)
        self.assertIn("Get-FileHash -LiteralPath $TemporaryPath -Algorithm SHA256", script)
        self.assertIn("manual_gates_remaining", script)
        self.assertIn("$baselineLogCounters = Get-CoreLogCounters", script)
        self.assertIn("[Threading.Thread]::Sleep(1500)", script)
        self.assertIn("$coreLogRotatedDuringRun", script)
        self.assertIn("$coreLogMissingDuringRun", script)
        self.assertIn("$coreLogTruncatedDuringRun", script)
        self.assertIn("$coreLogReplacedDuringRun", script)
        self.assertIn("started_monotonic_ms", script)
        self.assertIn("Output directory must be empty", script)
        self.assertNotIn("Set-NetFirewallProfile", script)
        restart_script = read("tests/tools/Invoke-DataplaneRestartGate.ps1")
        self.assertIn("[int]$Cycles = 20", restart_script)
        self.assertIn("Get-DnsClientServerAddress", restart_script)
        self.assertIn("Compare-Object -ReferenceObject $baselineRoutes", restart_script)
        self.assertIn("$state.ipv6_mtu -eq $ExpectedMtu", restart_script)
        self.assertIn("@($state.route_signature).Count -gt 0", restart_script)
        self.assertIn("$record.cleanup_ok", restart_script)
        self.assertIn("$Cycles -eq 20", restart_script)
        self.assertNotIn("Set-NetFirewallProfile", restart_script)
        tap_interface = read("ppp/tap/ITap.h")
        self.assertIn("OutputWithTrace", tap_interface)
        self.assertIn('OutputWithTrace(packet, packet_length, "REMOTE_RX")', read("ppp/ethernet/VEthernet.cpp"))
        self.assertIn('OutputWithTrace(packet.get(), packet_length, "REMOTE_RX")', read("ppp/ethernet/VNetstack.cpp"))

    def test_log_secret_scanner_redacts_findings_and_honors_offset(self) -> None:
        module_path = ROOT / "tests/tools/scan_log_secrets.py"
        spec = importlib.util.spec_from_file_location("scan_log_secrets", module_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "core.log"
            prefix = b"old token=must-not-be-scanned\n"
            log.write_bytes(prefix + b"safe runtime line\n")
            clean = module.scan(log, len(prefix))
            self.assertTrue(clean["passed"])
            with log.open("ab") as stream:
                stream.write(b"Authorization: Bearer do-not-echo-this\n")
                stream.write(b"adapter=123e4567-e89b-12d3-a456-426614174000\n")
            finding = module.scan(log, len(prefix))
            rendered = json.dumps(finding)
            self.assertFalse(finding["passed"])
            self.assertEqual(finding["finding_count"], 2)
            self.assertNotIn("do-not-echo-this", rendered)
        rpc = read("ppp/app/rpc/LocalRpcServer.cpp")
        self.assertNotIn("token mismatch, got=", rpc)
        self.assertNotIn("token=%s", rpc)
        self.assertIn("authentication mismatch", rpc)
        self.assertIn("authentication=%s", rpc)
        transmission = read("ppp/transmissions/ITransmission.cpp")
        server_exchanger = read("ppp/app/server/VirtualEthernetExchanger.cpp")
        traffic_logger = read("ppp/app/protocol/VirtualEthernetLogger.cpp")
        self.assertNotIn("sid=%s", transmission)
        self.assertNotIn("session_id=%s", transmission)
        self.assertNotIn("session_id=%s", server_exchanger)
        self.assertIn("SESSION-SHA256-", traffic_logger)
        self.assertIn("SHA256(", traffic_logger)

    def test_ai_control_api_is_core_owned_and_read_only_diagnostics(self) -> None:
        core = read("main.cpp")
        header = read("ppp/core/CoreApi.h")
        rust_rpc = read("tui/src/rpc/mod.rs")
        cli = read("tui/src/core/control.rs")
        cli_entry = read("tui/src/bin/ppp-tui-cli.rs")

        self.assertIn('method == "describe_api"', core)
        self.assertIn('method == "get_health"', core)
        self.assertIn('method == "run_diagnostics"', core)
        self.assertIn('result["read_only"] = true', core)
        self.assertIn('result["safety"]["secrets_in_responses"] = false', core)
        self.assertIn('snapshot["network"]["tun"]', core)
        self.assertNotIn("client->GetTapNetworkInterface()", core)
        self.assertIn("ppp_core_api_version(void)", header)
        self.assertIn('Self::DescribeApi => "describe_api"', rust_rpc)
        self.assertIn('Self::GetHealth => "get_health"', rust_rpc)
        self.assertIn('Self::RunDiagnostics { .. } => "run_diagnostics"', rust_rpc)
        self.assertIn('"api"', cli)
        self.assertIn('"health"', cli)
        self.assertIn('"diagnose"', cli)
        self.assertIn("parse_cli_control(&args)", cli_entry)
        self.assertIn("execute_once(&request.address", cli_entry)
        self.assertIn("serde_json::to_string_pretty", cli_entry)

    def test_performance_summary_uses_nearest_rank_and_throughput(self) -> None:
        module_path = ROOT / "tests/tools/summarize_dataplane_performance.py"
        spec = importlib.util.spec_from_file_location("summarize_dataplane_performance", module_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.nearest_rank([1, 2, 3, 4], 50), 2)
        self.assertEqual(module.nearest_rank([1, 2, 3, 4], 95), 4)
        summary = module.summarize_records([
            {"gate": "download-10mb", "ok": True, "elapsed_ms": 1000, "bytes": 10_000_000},
            {"gate": "download-10mb", "ok": False, "elapsed_ms": 30000, "bytes": 0},
        ])
        self.assertEqual(summary["download-10mb"]["failed"], 1)
        self.assertEqual(summary["download-10mb"]["throughput_mbps"]["p50"], 80.0)
        script = read("tests/tools/Invoke-DataplaneAcceptance.ps1")
        self.assertIn("summarize_dataplane_performance.py", script)
        self.assertIn("performance_summary_passed = $performanceSummaryPassed", script)


if __name__ == "__main__":
    unittest.main()
