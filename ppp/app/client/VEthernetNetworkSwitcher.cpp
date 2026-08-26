#include <ppp/app/client/VEthernetNetworkTcpipStack.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/ConnectivityProbe.h>
#include <ppp/app/client/proxys/VEthernetHttpProxySwitcher.h>
#include <ppp/app/client/proxys/VEthernetHttpProxyConnection.h>
#include <ppp/IDisposable.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/io/File.h>
#include <ppp/threading/Timer.h>
#include <ppp/threading/Executors.h>
#include <ppp/collections/Dictionary.h>
#include <ppp/auxiliary/StringAuxiliary.h>
#include <ppp/auxiliary/UriAuxiliary.h>
#include <ppp/tap/TapStub.h>
#include <ppp/net/packet/IPFrame.h>
#include <ppp/net/packet/UdpFrame.h>
#include <ppp/net/packet/IcmpFrame.h>
#include <ppp/net/native/ip.h>
#include <ppp/net/native/udp.h>
#include <ppp/net/native/icmp.h>
#include <ppp/net/native/checksum.h>
#include <ppp/net/asio/vdns.h>
#include <ppp/net/Socket.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/http/HttpClient.h>
#include <ppp/net/asio/InternetControlMessageProtocol.h>
#include <ppp/ipv6/IPv6Packet.h>
#if defined(_WIN32)
#include <windows/ppp/ipv6/IPv6Auxiliary.h>
#elif defined(_LINUX)
#include <linux/ppp/ipv6/IPv6Auxiliary.h>
#elif defined(_MACOS)
#include <darwin/ppp/ipv6/IPv6Auxiliary.h>
#endif

#if defined(_MACOS) && defined(PPP_LOG_VERBOSE)
#include <dirent.h>
#include <errno.h>
#include <sys/resource.h>
#endif
#if defined(PPP_LOG_VERBOSE)
#include <chrono>
#endif

#if defined(_WIN32)
#include <windows/ppp/tap/TapWindows.h>
#include <windows/ppp/win32/network/Firewall.h>
#include <windows/ppp/win32/network/Router.h>
#include <windows/ppp/net/proxies/HttpProxy.h>
#include <windows/ppp/win32/network/NetworkInterface.h>
#include <objbase.h>
#include <netioapi.h>
#include <ws2tcpip.h>
#else
#include <common/unix/UnixAfx.h>
#if defined(_MACOS)
#include <darwin/ppp/tap/TapDarwin.h>
#include <darwin/ppp/ipv6/IPv6Auxiliary.h>
#include <darwin/ppp/net/proxies/HttpProxy.h>
#else
#include <linux/ppp/tap/TapLinux.h>
#endif
#endif

using ppp::auxiliary::StringAuxiliary;
using ppp::auxiliary::UriAuxiliary;
using ppp::collections::Dictionary;
using ppp::threading::Timer;
using ppp::threading::Executors;
using ppp::net::AddressFamily;
using ppp::net::IPEndPoint;
using ppp::net::Ipep;
using ppp::net::native::ip_hdr;
using ppp::net::native::udp_hdr;
using ppp::net::native::icmp_hdr;
using ppp::net::packet::IPFlags;
using ppp::net::packet::IPFrame;
using ppp::net::packet::UdpFrame;
using ppp::net::packet::IcmpFrame;
using ppp::net::packet::IcmpType;
using ppp::net::packet::BufferSegment;
using ppp::transmissions::ITransmission;

namespace ppp {
    namespace app {
        namespace client {
#if defined(_WIN32)
            static std::shared_ptr<ppp::string> MakeLocalDnsServFail(const ppp::string& query) noexcept {
                auto response = make_shared_object<ppp::string>(query);
                if (NULLPTR == response || response->size() < 12) {
                    return NULLPTR;
                }

                Byte* packet = reinterpret_cast<Byte*>(&(*response)[0]);
                uint16_t flags = static_cast<uint16_t>((packet[2] << 8) | packet[3]);
                flags = static_cast<uint16_t>((flags | 0x8000u) & 0xfff0u);
                flags = static_cast<uint16_t>(flags | 0x0002u); // SERVFAIL
                packet[2] = static_cast<Byte>(flags >> 8);
                packet[3] = static_cast<Byte>(flags);
                memset(packet + 6, 0, 6); // ANCOUNT, NSCOUNT and ARCOUNT
                return response;
            }
#endif
#if defined(_MACOS) && defined(PPP_LOG_VERBOSE)
            static int GetOpenFileDescriptorCount(int& error) noexcept {
                error = 0;
                DIR* directory = opendir("/dev/fd");
                if (!directory) {
                    error = errno;
                    return -1;
                }

                int count = 0;
                while (struct dirent* entry = readdir(directory)) {
                    if (entry->d_name[0] == '.' &&
                        (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
                        continue;
                    }
                    count++;
                }

                closedir(directory);
                return std::max(0, count - 1); // Exclude the descriptor opened by opendir itself.
            }
#endif

            VEthernetNetworkSwitcher::VEthernetNetworkSwitcher(const std::shared_ptr<boost::asio::io_context>& context, bool lwip, bool vnet, bool mta, const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration) noexcept
                : VEthernet(context, lwip, vnet, mta)
                , configuration_(configuration)
                , base_configuration_(configuration)
                , icmppackets_aid_(0) {

#if !defined(_ANDROID) && !defined(_IPHONE)
                route_added_     = false;
#if defined(_LINUX)   
                protect_mode_    = false;
#endif
#endif
                static_mode_     = false;
                block_quic_      = false;
                icmppackets_aid_ = RandomNext();

                prefer_ipv4_ = (NULLPTR != configuration_ && configuration_->udp.dns.prefer_ipv4);

                // The remote IPv6 capability is learned from the server's
                // Information extensions.  The client profile may not contain a
                // server.ipv6 section (and must not be treated as the remote
                // server's capability advertisement).
                ipv6_server_has_dataplane_ = false;

#if defined(PPP_LOG_VERBOSE)
                std::shared_ptr<boost::asio::io_context> debug_context = context;
                try {
                    debug_watchdog_ = std::thread([this, debug_context]() noexcept {
                        uint64_t last_report = 0;
                        while (!debug_watchdog_stop_.load()) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            if (debug_watchdog_stop_.load()) {
                                break;
                            }

                            uint64_t last_tick = debug_last_tick_.load();
                            uint64_t current = ppp::threading::Executors::GetTickCount();
                            if (last_tick == 0 || current - last_tick < 10000 || current - last_report < 5000) {
                                continue;
                            }
                            last_report = current;

#if defined(_MACOS)
                            int fd_error = 0;
                            int fd_count = GetOpenFileDescriptorCount(fd_error);
                            struct rlimit fd_limit = {};
                            int rlimit_status = getrlimit(RLIMIT_NOFILE, &fd_limit);
                            LOG_DEBUG("VEthernetNetworkSwitcher::Watchdog: EVENT_LOOP_STALL, stalled_ms=%llu, io_stopped=%d, fd_count=%d, fd_soft=%llu, fd_hard=%llu, fd_errno=%d, rlimit_status=%d",
                                (unsigned long long)(current - last_tick), debug_context ? (int)debug_context->stopped() : -1,
                                fd_count,
                                rlimit_status == 0 ? (unsigned long long)fd_limit.rlim_cur : 0,
                                rlimit_status == 0 ? (unsigned long long)fd_limit.rlim_max : 0,
                                fd_error, rlimit_status);
                            ::fflush(ppp::g_log_stream);
#else
                            LOG_DEBUG("VEthernetNetworkSwitcher::Watchdog: EVENT_LOOP_STALL, stalled_ms=%llu, io_stopped=%d",
                                (unsigned long long)(current - last_tick), debug_context ? (int)debug_context->stopped() : -1);
#endif
                        }
                    });
                }
                catch (const std::exception& e) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::Watchdog: thread creation failed, exception=%s", e.what());
                }
#endif
            }

            VEthernetNetworkSwitcher::~VEthernetNetworkSwitcher() noexcept {
                Finalize();
            }

            VEthernetNetworkSwitcher::NetworkInterface::NetworkInterface() noexcept
                : Index(-1) {

            }

            std::shared_ptr<ppp::ethernet::VNetstack> VEthernetNetworkSwitcher::NewNetstack() noexcept {
                auto my = shared_from_this();
                auto self = std::dynamic_pointer_cast<VEthernetNetworkSwitcher>(my);
                return make_shared_object<VEthernetNetworkTcpipStack>(self);
            }

            bool VEthernetNetworkSwitcher::OnTick(uint64_t now) noexcept {
#if defined(PPP_LOG_VERBOSE)
                debug_last_tick_ = now;
#endif
                if (!VEthernet::OnTick(now)) {
                    return false;
                }
                CompletePendingOutboundSwitch(now);

#if !defined(_ANDROID) && !defined(_IPHONE)
                OnTickRefreshOutboundProbes(now);

                if (!proxy_only_) {
                    UpdateNetworkTakeover(now);
                }

                if (geo_rules_) {
                    ppp::vector<ppp::app::client::geo::GeoRuleEngine::RouteUpdate> expired;
                    geo_rules_->Update(now, expired);
                    for (const auto& route : expired) {
                        DeleteGeoDynamicRoute(route.address);
                    }
                }
#endif
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    for (auto affinity = outbound_affinities_.begin(); affinity != outbound_affinities_.end();) {
                        if (affinity->second.expires_at <= now) affinity = outbound_affinities_.erase(affinity);
                        else ++affinity;
                    }
                }

#if defined(PPP_LOG_VERBOSE)
                if (now >= debug_diagnostics_next_) {
                    debug_diagnostics_next_ = now + 5000;

                    size_t lan2wan = 0;
                    size_t wan2lan = 0;
                    std::shared_ptr<ppp::ethernet::VNetstack> netstack = GetNetstack();
                    if (netstack) {
                        netstack->GetDebugConnectionCounts(lan2wan, wan2lan);
                    }

                    size_t mappings = 0;
                    size_t datagrams = 0;
                    size_t timers = 0;
                    std::shared_ptr<VEthernetExchanger> debug_exchanger = exchanger_;
                    if (debug_exchanger) {
                        debug_exchanger->GetDebugObjectCounts(mappings, datagrams, timers);
                    }

                    uint64_t incoming = 0;
                    uint64_t outgoing = 0;
                    std::shared_ptr<ppp::transmissions::ITransmissionStatistics> statistics = statistics_;
                    if (statistics) {
                        incoming = statistics->IncomingTraffic.load();
                        outgoing = statistics->OutgoingTraffic.load();
                    }

                    std::shared_ptr<boost::asio::io_context> debug_context = GetContext();
#if defined(_MACOS)
                    int fd_error = 0;
                    int fd_count = GetOpenFileDescriptorCount(fd_error);
                    struct rlimit fd_limit = {};
                    int rlimit_status = getrlimit(RLIMIT_NOFILE, &fd_limit);
                    LOG_DEBUG("VEthernetNetworkSwitcher::Diagnostics: tick=%llu, io_stopped=%d, state=%d, reconnects=%d, tcp_lan2wan=%llu, tcp_wan2lan=%llu, mappings=%llu, datagrams=%llu, timers=%llu, incoming=%llu, outgoing=%llu, fd_count=%d, fd_soft=%llu, fd_hard=%llu, fd_errno=%d, rlimit_status=%d",
                        (unsigned long long)now, debug_context ? (int)debug_context->stopped() : -1,
                        debug_exchanger ? (int)debug_exchanger->GetNetworkState() : -1,
                        debug_exchanger ? debug_exchanger->GetReconnectionCount() : -1,
                        (unsigned long long)lan2wan, (unsigned long long)wan2lan,
                        (unsigned long long)mappings, (unsigned long long)datagrams, (unsigned long long)timers,
                        (unsigned long long)incoming, (unsigned long long)outgoing,
                        fd_count,
                        rlimit_status == 0 ? (unsigned long long)fd_limit.rlim_cur : 0,
                        rlimit_status == 0 ? (unsigned long long)fd_limit.rlim_max : 0,
                        fd_error, rlimit_status);
                    ::fflush(ppp::g_log_stream);
#else
                    LOG_DEBUG("VEthernetNetworkSwitcher::Diagnostics: tick=%llu, io_stopped=%d, state=%d, reconnects=%d, tcp_lan2wan=%llu, tcp_wan2lan=%llu, mappings=%llu, datagrams=%llu, timers=%llu, incoming=%llu, outgoing=%llu",
                        (unsigned long long)now, debug_context ? (int)debug_context->stopped() : -1,
                        debug_exchanger ? (int)debug_exchanger->GetNetworkState() : -1,
                        debug_exchanger ? debug_exchanger->GetReconnectionCount() : -1,
                        (unsigned long long)lan2wan, (unsigned long long)wan2lan,
                        (unsigned long long)mappings, (unsigned long long)datagrams, (unsigned long long)timers,
                        (unsigned long long)incoming, (unsigned long long)outgoing);
#endif
                }
#endif

                // Safety net: flush expired pending AAAA responses even if no DNS traffic triggers it
                FlushExpiredPendingAAAAResponses();

                std::shared_ptr<ppp::transmissions::ITransmissionQoS> qos = qos_; 
                if (NULLPTR != qos) {
                    qos->Update(now);
                }

                if (!outbound_exchangers_.empty()) {
                    ppp::vector<std::shared_ptr<VEthernetExchanger>> updated_exchangers;
                    for (auto& entry : outbound_exchangers_) {
                        if (NULLPTR == entry.second) continue;
                        bool duplicate = false;
                        for (const auto& updated : updated_exchangers) {
                            if (updated == entry.second) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate) {
                            updated_exchangers.emplace_back(entry.second);
                            entry.second->Update();
                        }
                    }
                }
                else {
                    std::shared_ptr<VEthernetExchanger> exchanger = exchanger_;
                    if (NULLPTR != exchanger) exchanger->Update();
                }
                {
                    std::shared_ptr<VEthernetExchanger> pending;
                    {
                        SynchronizedObjectScope scope(GetSynchronizedObject());
                        pending = pending_outbound_exchanger_;
                    }
                    if (NULLPTR != pending) {
                        bool already_updated = false;
                        for (const auto& entry : outbound_exchangers_) {
                            if (entry.second == pending) {
                                already_updated = true;
                                break;
                            }
                        }
                        if (!already_updated) pending->Update();
                    }
                }

                std::shared_ptr<IForwarding> forwarding = forwarding_; 
                if (NULLPTR != forwarding) {
                    forwarding->Update(now);
                }

                ppp::vector<int> releases_icmppackets; 
                for (;;) {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    for (auto&& kv : icmppackets_) {
                        const VEthernetIcmpPacket& icmppacket = kv.second;
                        if (icmppacket.datetime > now) {
                            continue;
                        }

                        releases_icmppackets.emplace_back(kv.first);
                    }

                    for (int ack_id : releases_icmppackets) {
                        ppp::collections::Dictionary::RemoveValueByKey(icmppackets_, ack_id);
                    }

                    break;
                }

                VEthernetTickEventHandler tick_event = TickEvent; 
                if (tick_event) {
                    tick_event(this, now);
                }

                return true;
            }

#if !defined(_ANDROID) && !defined(_IPHONE)
            namespace {
                ConnectivityProbe::ProbeType ProbeTypeFromUriProtocol(UriAuxiliary::ProtocolType protocol_type) noexcept {
                    if (protocol_type == UriAuxiliary::ProtocolType::ProtocolType_Http ||
                        protocol_type == UriAuxiliary::ProtocolType::ProtocolType_WebSocket) {
                        return ConnectivityProbe::ProbeType_WebSocket;
                    }
                    elif(protocol_type == UriAuxiliary::ProtocolType::ProtocolType_HttpSSL ||
                        protocol_type == UriAuxiliary::ProtocolType::ProtocolType_WebSocketSSL) {
                        return ConnectivityProbe::ProbeType_WebSocketSSL;
                    }
                    return ConnectivityProbe::ProbeType_Tcp;
                }


                ppp::string NormalizeProbeEntryString(const ppp::string& address, int port) noexcept {
                    if (address.empty() || port <= IPEndPoint::MinPort || port > IPEndPoint::MaxPort) {
                        return ppp::string();
                    }
                    ppp::string entry = address.find(':') != ppp::string::npos ? "[" + address + "]" : address;
                    entry += ":";
                    entry += stl::to_string<ppp::string>(port);
                    return entry;
                }

                bool ProbeOutboundCandidate(
                    const ConnectivityProbe::TCPEndPoint&                     remoteEP,
                    ConnectivityProbe::ProbeType                              probe_type,
                    const ppp::string&                                        hostname,
                    const ppp::string&                                        path,
                    const ppp::string&                                        ws_host,
                    const ppp::string&                                        ws_sni,
                    int                                                       stage,
                    int                                                       timeout_ms,
                    ppp::coroutines::YieldContext&                            y,
                    int&                                                      rtt_ms,
                    const ConnectivityProbe::ProtectSocketHandler&            protect) noexcept {
                    rtt_ms = 0;
                    if (stage <= 2 || probe_type == ConnectivityProbe::ProbeType_Tcp) {
                        return ConnectivityProbe::ProbeTcp(remoteEP, timeout_ms, y, rtt_ms, protect);
                    }
                    if (probe_type == ConnectivityProbe::ProbeType_WebSocket) {
                        ppp::string host = ws_host.empty() ? hostname : ws_host;
                        return ConnectivityProbe::ProbeWebSocket(remoteEP, host, path, timeout_ms, y, rtt_ms, protect);
                    }
                    if (probe_type == ConnectivityProbe::ProbeType_WebSocketSSL) {
                        ppp::string host = ws_host.empty() ? hostname : ws_host;
                        ppp::string sni = ws_sni.empty() ? host : ws_sni;
                        return ConnectivityProbe::ProbeWebSocketSSL(remoteEP, host, sni, path, timeout_ms, y, rtt_ms, protect);
                    }
                    return ConnectivityProbe::ProbeTcp(remoteEP, timeout_ms, y, rtt_ms, protect);
                }
            }

            void VEthernetNetworkSwitcher::OnTickRefreshOutboundProbes(uint64_t now) noexcept {
                // Claim the refresh slot atomically so two tick paths can never
                // both pass the gate and spawn a duplicate refresh.
                if (outbound_probe_refreshing_.exchange(true)) {
                    return;
                }
                if (now < next_outbound_probe_refresh_) {
                    outbound_probe_refreshing_ = false;
                    return;
                }
                next_outbound_probe_refresh_ = now + 5000;

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context || context->stopped()) {
                    outbound_probe_refreshing_ = false;
                    return;
                }

                auto self = shared_from_this();
                bool spawned = ppp::coroutines::YieldContext::Spawn(NULLPTR, *context,
                    [self, this](ppp::coroutines::YieldContext& y) noexcept {
                        RefreshOutboundProbes(y);
                    });
                if (!spawned) {
                    outbound_probe_refreshing_ = false;
                }
            }
            bool VEthernetNetworkSwitcher::RefreshOutboundProbes(ppp::coroutines::YieldContext& y) noexcept {
                using ProtocolType = UriAuxiliary::ProtocolType;

                struct Candidate final {
                    ppp::string                                                     entry;
                    ConnectivityProbe::ProbeType                                    probe_type = ConnectivityProbe::ProbeType_Tcp;
                    boost::asio::ip::tcp::endpoint                                  remoteEP;
                    ppp::string                                                     hostname;
                    ppp::string                                                     path;
                };
                struct OutboundWork final {
                    ppp::string                                                     tag;
                    std::shared_ptr<VEthernetExchanger>                             exchanger;
                    int                                                             timeout_ms = 800;
                    int                                                             stage = 3;
                    ppp::string                                                     ws_host;
                    ppp::string                                                     ws_sni;
                    ppp::vector<Candidate>                                          candidates;
                };

                // Snapshot the outbound list under the short lock only.  Building
                // the work items below parses each candidate URI, and
                // UriAuxiliary::Parse performs async DNS resolution that can
                // suspend this stackful coroutine.  Holding syncobj_ across such
                // a suspension is fatal: the per-packet data path (GetExchanger)
                // locks the same mutex on the same io thread and deadlocks
                // immediately (_RESOURCE_DEADLOCK_WOULD_OCCUR).
                struct OutboundSnapshot final {
                    ppp::string                                                     tag;
                    std::shared_ptr<ppp::configurations::AppConfiguration>          configuration;
                    std::shared_ptr<VEthernetExchanger>                             exchanger;
                    bool                                                            established = false;
                };
                ppp::vector<OutboundSnapshot> outbounds;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    outbounds.reserve(outbound_configurations_.size());
                    for (const OutboundConfiguration& outbound : outbound_configurations_) {
                        ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag));
                        if (!outbound.server_menu && tag != "main") {
                            continue;
                        }
                        const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration = outbound.configuration;
                        if (NULLPTR == configuration) {
                            continue;
                        }
                        const ppp::string& client_server = configuration->client.server;
                        if (client_server.empty()) {
                            continue;
                        }
                        // Master probe switch: false disables the 5s background
                        // refresh entirely -- no probing, no route pinning, and no
                        // latency suffix on the SERVERS page for this outbound.
                        if (!configuration->client.probe.enabled) {
                            continue;
                        }
                        // An upstream proxy owns the real tunnel path; a direct probe
                        // would measure the wrong hop, so leave those unprobed.
                        if (!configuration->client.server_proxy.empty()) {
                            continue;
                        }

                        // The "main" map slot is reused after a primary
                        // outbound hot switch. Match by configuration rather
                        // than by tag so probe results cannot leak between
                        // outbounds (for example, nubeHK into ggvUS).
                        std::shared_ptr<VEthernetExchanger> exchanger;
                        for (const auto& item : outbound_exchangers_) {
                            if (NULLPTR != item.second &&
                                item.second->GetConfiguration() == configuration) {
                                exchanger = item.second;
                                break;
                            }
                        }
                        bool established = NULLPTR != exchanger &&
                            exchanger->GetNetworkState() ==
                                VEthernetExchanger::NetworkState_Established;

                        outbounds.emplace_back(OutboundSnapshot{
                            std::move(tag), configuration, std::move(exchanger), established });
                    }
                }

                ppp::vector<OutboundWork> works;
                {
                    works.reserve(outbounds.size());
                    for (const OutboundSnapshot& outbound_ref : outbounds) {
                        const ppp::string& tag = outbound_ref.tag;
                        const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration = outbound_ref.configuration;
                        const ppp::string& client_server = configuration->client.server;

                        OutboundWork work;
                        work.tag = tag;
                        work.exchanger = outbound_ref.exchanger;
                        work.timeout_ms = std::max<int>(50, configuration->client.probe.timeout_ms);
                        // Background refresh only needs L1 for outbounds without an
                        // established tunnel; probing idle wss endpoints to L3 every
                        // 5s burns server-side TLS handshakes for no user value.  The
                        // probe depth is built-in fixed (3 established / 1 otherwise).
                        work.stage = outbound_ref.established ? 3 : 1;
                        work.ws_host = configuration->client.websocket.host;
                        work.ws_sni = configuration->client.websocket.sni;

                        ppp::string primary_address;
                        ppp::string primary_path;
                        ProtocolType primary_protocol = ProtocolType::ProtocolType_PPP;
                        {
                            ppp::string entry_hostname;
                            ppp::string entry_address;
                            ppp::string entry_path;
                            int entry_port = IPEndPoint::MinPort;
                            ProtocolType entry_protocol = ProtocolType::ProtocolType_PPP;
                            ppp::string abs_url;
                            ppp::string entry_server = UriAuxiliary::Parse(client_server, entry_hostname, entry_address,
                                entry_path, entry_port, entry_protocol, &abs_url, y);
                            if (!entry_server.empty() && !entry_hostname.empty() && !entry_address.empty() &&
                                entry_port > IPEndPoint::MinPort && entry_port <= IPEndPoint::MaxPort) {
                                if (entry_protocol == ProtocolType::ProtocolType_Socks) {
                                    entry_protocol = ProtocolType::ProtocolType_PPP;
                                }
                                IPEndPoint ipep(entry_address.data(), entry_port);
                                if (!IPEndPoint::IsInvalid(ipep)) {
                                    Candidate candidate;
                                    // Keep the display key consistent with the
                                    // connection-side key (hostname:port).
                                    candidate.entry = NormalizeProbeEntryString(entry_hostname, entry_port);
                                    candidate.hostname = entry_hostname;
                                    candidate.path = entry_path;
                                    candidate.remoteEP = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(ipep);
                                    candidate.probe_type = ProbeTypeFromUriProtocol(entry_protocol);
                                    work.candidates.emplace_back(std::move(candidate));
                                    primary_address = entry_address;
                                    primary_path = entry_path;
                                    primary_protocol = entry_protocol;
                                }
                            }
                        }

                        if (!primary_address.empty()) {
                            for (const ppp::string& entry : configuration->client.servers) {
                                ppp::string host_string;
                                int entry_port = 0;
                                if (!Ipep::ParseEndPoint(entry, host_string, entry_port)) {
                                    continue;
                                }
                                if (entry_port <= IPEndPoint::MinPort || entry_port > IPEndPoint::MaxPort) {
                                    continue;
                                }
                                host_string = LTrim(RTrim(host_string));
                                if (host_string.empty()) {
                                    continue;
                                }
                                boost::asio::ip::address address = Ipep::ToAddress(host_string, false);
                                if (IPEndPoint::IsInvalid(address) && Ipep::IsDomainAddress(host_string)) {
                                    // Hostname backup entry: resolve it so ws/wss and ppp
                                    // tunnels can use domains here as well.  Resolution may
                                    // suspend this coroutine; no lock is held on this path.
                                    boost::asio::ip::udp::endpoint resolved =
                                        ppp::coroutines::asio::GetAddressByHostName<boost::asio::ip::udp>(
                                            host_string.data(), entry_port, y);
                                    address = resolved.address();
                                }
                                if (IPEndPoint::IsInvalid(address)) {
                                    continue; // Unresolvable host; skip this round.
                                }
                                std::string address_string = address.to_string();
                                ppp::string address_string_ppp(address_string.data(), address_string.size());
                                IPEndPoint ipep(address_string_ppp.data(), entry_port);
                                if (IPEndPoint::IsInvalid(ipep)) {
                                    continue;
                                }
                                Candidate candidate;
                                candidate.entry = NormalizeProbeEntryString(host_string, entry_port);
                                candidate.hostname = host_string;
                                candidate.path = primary_path;
                                candidate.remoteEP = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(ipep);
                                candidate.probe_type = ProbeTypeFromUriProtocol(primary_protocol);
                                work.candidates.emplace_back(std::move(candidate));
                            }
                        }

                        if (!work.candidates.empty()) {
                            works.emplace_back(std::move(work));
                        }
                    }
                }

                if (works.empty()) {
                    outbound_probe_refreshing_ = false;
                    return true;
                }

#if defined(_WIN32)
                // Pin every candidate on the physical adapter so probe sockets
                // never enter the TAP (self-loop) while the tunnel is up.
                for (const OutboundWork& work : works) {
                    for (const Candidate& candidate : work.candidates) {
                        const boost::asio::ip::address& probe_ip = candidate.remoteEP.address();
                        if (probe_ip.is_v4()) {
                            EnsureWindowsIPv4ServerRoute(probe_ip);
                        }
                        elif(probe_ip.is_v6()) {
                            EnsureWindowsIPv6ServerRoute(probe_ip);
                        }
                    }
                }
#endif

                ConnectivityProbe::ProtectSocketHandler protector;
#if defined(_LINUX)
                {
                    std::shared_ptr<ppp::net::ProtectorNetwork> protector_network = GetProtectorNetwork();
                    if (NULLPTR != protector_network) {
                        protector = [protector_network](int sockfd) noexcept {
                            return protector_network->Protect(sockfd);
                        };
                    }
                }
#endif

                struct ProbeRefreshState final {
                    std::atomic<int>                            pending = 0;
                    ppp::coroutines::YieldContext*              parent = NULLPTR;
                };
                ProbeRefreshState state;
                state.pending = static_cast<int>(works.size());
                state.parent = &y;

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    outbound_probe_refreshing_ = false;
                    return false;
                }

                auto self = shared_from_this();
                for (int i = 0; i < static_cast<int>(works.size()); i++) {
                    bool spawned = ppp::coroutines::YieldContext::Spawn(NULLPTR, *context,
                        [self, this, i, protector, &works, &state](ppp::coroutines::YieldContext& cy) noexcept {
                            const OutboundWork& work = works[i];
                            int best_rtt = INT_MAX;
                            ppp::string best_entry;
                            bool reachable = false;

                            // Persist per-entry outcomes into the outbound exchanger so
                            // the hot-switch state machine sees fresh per-entry RTTs.
                            // The map slot for "main" can now refer to a
                            // different outbound after a primary hot switch.
                            // Use the exchanger captured with this exact
                            // configuration instead of looking it up by tag.
                            std::shared_ptr<VEthernetExchanger> exchanger = work.exchanger;
                            std::shared_ptr<ppp::configurations::AppConfiguration> probe_configuration =
                                (NULLPTR != exchanger) ? exchanger->GetConfiguration() : NULLPTR;
                            const uint64_t probe_now = Executors::GetTickCount();
                            const uint64_t probe_ttl_ms = (NULLPTR != probe_configuration)
                                ? static_cast<uint64_t>(std::max<int>(1, probe_configuration->client.probe.ttl_seconds)) * 1000ULL : 0ULL;

                            for (const Candidate& candidate : work.candidates) {
                                int rtt = 0;
                                bool ok = ProbeOutboundCandidate(candidate.remoteEP, candidate.probe_type,
                                    candidate.hostname, candidate.path, work.ws_host, work.ws_sni,
                                    work.stage, work.timeout_ms, cy, rtt, protector);
                                if (ok && rtt >= 0 && rtt < best_rtt) {
                                    best_rtt = rtt;
                                    best_entry = candidate.entry;
                                    reachable = true;
                                }

                                if (NULLPTR != exchanger && NULLPTR != probe_configuration && !candidate.entry.empty()) {
                                    bool blacklisted = false;
                                    {
                                        SynchronizedObjectScope scope(exchanger->syncobj_);
                                        auto it = exchanger->probe_results_.find(candidate.entry);
                                        if (it != exchanger->probe_results_.end() && it->second.penalty_until > probe_now) {
                                            blacklisted = true;
                                        }
                                    }
                                    if (!blacklisted) {
                                        // Never clear an active blacklist with a fresh result.
                                        exchanger->StoreProbeResult(candidate.entry, candidate.probe_type,
                                            ok, rtt, work.stage, probe_now, probe_ttl_ms);
                                    }
                                }
                            }
                            {
                                SynchronizedObjectScope scope(GetSynchronizedObject());
                                OutboundProbeStatus& status = outbound_probe_statuses_[work.tag];
                                status.checked = true;
                                status.reachable = reachable;
                                status.rtt_ms = reachable ? best_rtt : -1;
                                status.server = reachable ? best_entry : ppp::string();
                                status.updated_at = Executors::GetTickCount();
                            }
                            if (state.pending.fetch_sub(1) == 1) {
                                state.parent->R();
                            }
                        });
                    if (!spawned) {
                        if (state.pending.fetch_sub(1) == 1) {
                            state.parent->R();
                        }
                    }
                }
                y.Suspend();
                outbound_probe_refreshing_ = false;
                return true;
            }
#endif

            bool VEthernetNetworkSwitcher::OnPacketInput(ppp::net::native::ip_hdr* packet, int packet_length, int header_length, int proto, bool vnet) noexcept {
                if (!vnet) {
                    LOG_DEBUG("DATAPLANE VEthernetNetworkSwitcher::OnPacketInput: !vnet gate, proto=%d, len=%d, src=%u, dest=%u -> fallthrough",
                        proto, packet_length, (unsigned)packet->src, (unsigned)packet->dest);
                    return false;
                }

                if (proto != ppp::net::native::ip_hdr::IP_PROTO_TCP &&
                    proto != ppp::net::native::ip_hdr::IP_PROTO_UDP &&
                    proto != ppp::net::native::ip_hdr::IP_PROTO_ICMP) {
                    return false;
                }

                std::shared_ptr<ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                uint32_t destination = packet->dest;
                if (destination == tap->IPAddress) {
                    return false;
                }
                if (packet->src != tap->IPAddress) {
                    return false;
                }

                uint32_t gw = tap->GatewayServer;
                uint32_t mask = tap->SubmaskAddress;
                if (IPAddressIsGatewayServer(destination, gw, mask)) {
                    return false;
                }

                if (destination != ppp::net::native::ip_hdr::IP_ADDR_BROADCAST_VALUE) {
                    if ((destination & mask) != (gw & mask)) {
                        return false;
                    }
                }

                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(Ipep::ToAddress(destination));
                if (NULLPTR == exchanger) return false;
                exchanger->Nat(packet, packet_length);
                return true;
            }

            bool VEthernetNetworkSwitcher::OnPacketInput(const std::shared_ptr<IPFrame>& packet) noexcept {
                if (packet->ProtocolType == ip_hdr::IP_PROTO_UDP) {
                    return OnUdpPacketInput(packet);
                }
                elif(packet->ProtocolType == ip_hdr::IP_PROTO_ICMP) {
                    return OnIcmpPacketInput(packet);
                }
                else {
                    return false;
                }
            }

            bool VEthernetNetworkSwitcher::OnIPv6PacketInput(Byte* packet, int packet_length) noexcept {
                if (!IsVNet()) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::OnIPv6PacketInput: IsVNet()=false");
#if !defined(_ANDROID)
                    // The desktop client only serves the IPv6 data plane while
                    // vnet is enabled. Android keeps vnet=false (its IPv4 path
                    // runs on the netstack) but still needs the IPv6 plane:
                    // the forwarding logic below (GetExchanger -> Nat) does
                    // not depend on the lwip netstack at all.
                    return false;
#endif
                }
                if (NULLPTR == packet || packet_length < (int)sizeof(ppp::ipv6::PacketHeader)) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::OnIPv6PacketInput: SKIP, invalid packet=%p, len=%d",
                        (void*)packet, packet_length);
                    return false;
                }

                ppp::ipv6::PacketHeader* ipv6_header = reinterpret_cast<ppp::ipv6::PacketHeader*>(packet);

                // Protocol dispatch based on IPv6 Next Header field
                Byte next_header = ipv6_header->NextHeader;

                // TCP (6): forward blindly via NAT (same as existing behavior)
                if (next_header == IPPROTO_TCP) {
                    boost::asio::ip::address_v6::bytes_type destination_bytes;
                    memcpy(destination_bytes.data(), ipv6_header->Destination, destination_bytes.size());
                    std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(boost::asio::ip::address_v6(destination_bytes));
                    if (NULLPTR == exchanger) {
                        return false;
                    }
                    exchanger->Nat(packet, packet_length);
                    return true;
                }

                // UDP (17): check for DNS interception, QUIC blocking, static mode
                if (next_header == IPPROTO_UDP) {
                    return OnIPv6UdpPacketInput(packet, packet_length, ipv6_header);
                }

                // ICMPv6 (58): handle echo request locally for TAP gateway
                if (next_header == IPPROTO_ICMPV6) {
                    return OnIPv6IcmpPacketInput(packet, packet_length, ipv6_header);
                }

                // Other protocols: forward via NAT
                boost::asio::ip::address_v6::bytes_type destination_bytes;
                memcpy(destination_bytes.data(), ipv6_header->Destination, destination_bytes.size());
                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(boost::asio::ip::address_v6(destination_bytes));
                if (NULLPTR == exchanger) {
                    return false;
                }
                exchanger->Nat(packet, packet_length);
                return true;
            }

            // Prefer IPv4 DNS: when a DNS AAAA response arrives, check if the same
            // domain has cached A records (IPv4). If yes, strip AAAA so the client
            // prefers IPv4. If there is no cached A response yet, forward AAAA
            // immediately. Holding it for the DNS timeout delays Happy Eyeballs and
            // can make IPv6-only sites appear unavailable.
            // Controlled by "udp.dns.prefer_ipv4" in appsettings.json.
            // Returns true if the caller should forward the response now.
            // Returns false if the caller should hold the response (AAAA with no A cache yet).
            bool VEthernetNetworkSwitcher::StripAAAADnsResponseIfIPv4Available(::dns::Message& m) noexcept {
                if (!prefer_ipv4_.load()) {
                    return true;
                }
                if (m.questions.empty()) {
                    return true;
                }
                if (m.questions[0].mType != ::dns::RecordType::kAAAA) {
                    return true;
                }

                // Flush any expired pending AAAA entries before processing this one.
                FlushExpiredPendingAAAAResponses();

                const char* domain = m.questions[0].mName.data();
                int original_aaaa_count = 0;
                for (const auto& rr : m.answers) {
                    if (rr.mType == ::dns::RecordType::kAAAA) original_aaaa_count++;
                }

                // Check cache for A records of the same domain
                ::dns::Message a_check;
                a_check.questions = m.questions;
                a_check.questions[0].mType = ::dns::RecordType::kA;
                ppp::string cache_result = ppp::net::asio::vdns::QueryCache2(
                    domain, a_check, ppp::net::asio::vdns::AddressFamily::kA);
                if (cache_result.empty()) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::StripAAAADnsResponseIfIPv4Available: no A cache for %s, keeping %d AAAA records",
                        domain, original_aaaa_count);
                    return true;
                }
                bool hasA = false;
                for (const auto& rr : a_check.answers) {
                    if (rr.mType == ::dns::RecordType::kA) {
                        hasA = true;
                        break;
                    }
                }
                if (!hasA) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::StripAAAADnsResponseIfIPv4Available: cached no A for %s, keeping %d AAAA records",
                        domain, original_aaaa_count);
                    return true;
                }
                // Strip AAAA records from the response
                m.answers.erase(std::remove_if(m.answers.begin(), m.answers.end(),
                    [](const ::dns::ResourceRecord& rr) noexcept { return rr.mType == ::dns::RecordType::kAAAA; }),
                    m.answers.end());
                m.additions.erase(std::remove_if(m.additions.begin(), m.additions.end(),
                    [](const ::dns::ResourceRecord& rr) noexcept { return rr.mType == ::dns::RecordType::kAAAA; }),
                    m.additions.end());

                int remaining_aaaa_count = 0;
                for (const auto& rr : m.answers) {
                    if (rr.mType == ::dns::RecordType::kAAAA) remaining_aaaa_count++;
                }
                LOG_DEBUG("VEthernetNetworkSwitcher::StripAAAADnsResponseIfIPv4Available: STRIPPED %d AAAA for %s, remaining AAAA=%d",
                    original_aaaa_count, domain, remaining_aaaa_count);
                return true;
            }

            // Called after A records are written to the DNS cache (via AddCache)
            // or when a DNS response triggers StripAAAADnsResponseIfIPv4Available.
            // Two flush categories:
            //   - hasA: the domain's A record arrived → strip AAAA, forward (prefer IPv4)
            //   - expired: timeout elapsed, no A will come → forward AAAA as-is (pure IPv6 site)
            void VEthernetNetworkSwitcher::FlushPendingAAAAResponses() noexcept {
                if (pending_aaaa_.empty()) {
                    return;
                }

                uint64_t now = Executors::GetTickCount();

                // Collect domains to flush (avoid modifying map while iterating)
                ppp::vector<ppp::string> domains_strip;   // hasA: strip AAAA
                ppp::vector<ppp::string> domains_expired; // timeout: forward as-is
                for (auto& pair : pending_aaaa_) {
                    const ppp::string& domain = pair.first;
                    const auto& pending = pair.second;

                    bool expired = (pending->expire_time > 0 && now >= pending->expire_time);

                    ::dns::Message a_check;
                    a_check.questions.resize(1);
                    a_check.questions[0].mName = domain;
                    a_check.questions[0].mType = ::dns::RecordType::kA;
                    ppp::string cache_result = ppp::net::asio::vdns::QueryCache2(
                        domain.data(), a_check, ppp::net::asio::vdns::AddressFamily::kA);

                    bool hasA = false;
                    if (!cache_result.empty()) {
                        for (const auto& rr : a_check.answers) {
                            if (rr.mType == ::dns::RecordType::kA) { hasA = true; break; }
                        }
                    }

                    if (hasA) {
                        domains_strip.emplace_back(domain);
                    } else if (expired || !cache_result.empty()) {
                        // A resolved (cache populated) but no A records, or timeout expired.
                        // Either way, forward AAAA as-is (pure IPv6 site).
                        domains_expired.emplace_back(domain);
                    }
                }

                // Flush: strip AAAA (A records available)
                for (const ppp::string& domain : domains_strip) {
                    auto it = pending_aaaa_.find(domain);
                    if (it == pending_aaaa_.end()) continue;

                    auto& pending = it->second;
                    ::dns::Message m;
                    if (m.decode(reinterpret_cast<const uint8_t*>(pending->EncodedPacket.data()),
                        static_cast<int>(pending->EncodedPacket.size())) == ::dns::BufferResult::NoError) {

                        int before = 0;
                        for (const auto& rr : m.answers) {
                            if (rr.mType == ::dns::RecordType::kAAAA) before++;
                        }

                        m.answers.erase(std::remove_if(m.answers.begin(), m.answers.end(),
                            [](const ::dns::ResourceRecord& rr) noexcept { return rr.mType == ::dns::RecordType::kAAAA; }),
                            m.answers.end());
                        m.additions.erase(std::remove_if(m.additions.begin(), m.additions.end(),
                            [](const ::dns::ResourceRecord& rr) noexcept { return rr.mType == ::dns::RecordType::kAAAA; }),
                            m.additions.end());

                        LOG_DEBUG("VEthernetNetworkSwitcher::FlushPendingAAAAResponses: STRIPPED %d AAAA for %s (A records available)",
                            before, domain.data());

                        std::size_t new_sz = 0;
                        char dns_packet[PPP_MAX_DNS_PACKET_BUFFER_SIZE];
                        if (m.encode(dns_packet, PPP_MAX_DNS_PACKET_BUFFER_SIZE, new_sz) == ::dns::BufferResult::NoError && new_sz > 0) {
                            if (pending->IsIPv6) {
                                DatagramOutput(
                                    boost::asio::ip::udp::endpoint(pending->SrcV6, pending->SrcPort),
                                    boost::asio::ip::udp::endpoint(pending->DstV6, pending->DstPort),
                                    dns_packet, static_cast<int>(new_sz), false);
                            } else {
                                DatagramOutput(pending->SourceEP, pending->DestinationEP,
                                    dns_packet, static_cast<int>(new_sz), false);
                            }
                        }
                    }
                    pending_aaaa_.erase(it);
                }

                // Flush: forward as-is (timeout expired, pure IPv6 site)
                for (const ppp::string& domain : domains_expired) {
                    auto it = pending_aaaa_.find(domain);
                    if (it == pending_aaaa_.end()) continue;

                    auto& pending = it->second;
                    LOG_DEBUG("VEthernetNetworkSwitcher::FlushPendingAAAAResponses: forwarding %s as-is (timeout expired, likely IPv6-only)",
                        domain.data());

                    if (pending->IsIPv6) {
                        DatagramOutput(
                            boost::asio::ip::udp::endpoint(pending->SrcV6, pending->SrcPort),
                            boost::asio::ip::udp::endpoint(pending->DstV6, pending->DstPort),
                            const_cast<char*>(pending->EncodedPacket.data()),
                            static_cast<int>(pending->EncodedPacket.size()), false);
                    } else {
                        DatagramOutput(pending->SourceEP, pending->DestinationEP,
                            const_cast<char*>(pending->EncodedPacket.data()),
                            static_cast<int>(pending->EncodedPacket.size()), false);
                    }
                    pending_aaaa_.erase(it);
                }
            }

            // Called from StripAAAADnsResponseIfIPv4Available to clean up expired pending
            // entries on every DNS response. Ensures pure-IPv6 sites' AAAA is forwarded
            // promptly even if no new A records trigger FlushPendingAAAAResponses.
            void VEthernetNetworkSwitcher::FlushExpiredPendingAAAAResponses() noexcept {
                FlushPendingAAAAResponses();
            }

            bool VEthernetNetworkSwitcher::OnIPv6UdpPacketInput(Byte* packet, int packet_length, ppp::ipv6::PacketHeader* ipv6_header) noexcept {
                // Need at least IPv6 header (40) + UDP header (8)
                static constexpr int UDP_HEADER_OFFSET = sizeof(ppp::ipv6::PacketHeader);
                static constexpr int UDP_HEADER_SIZE = 8;
                static constexpr int UDP_PAYLOAD_OFFSET = UDP_HEADER_OFFSET + UDP_HEADER_SIZE;

                if (packet_length < UDP_PAYLOAD_OFFSET) {
                    return false;
                }

                // Parse UDP header from raw bytes
                Byte* udp_start = packet + UDP_HEADER_OFFSET;
                uint16_t src_port = ntohs(*(uint16_t*)(udp_start));
                uint16_t dst_port = ntohs(*(uint16_t*)(udp_start + 2));

                // DNS interception for port 53
                if (dst_port == PPP_DNS_SYS_PORT) {
                    int udp_payload_len = packet_length - UDP_PAYLOAD_OFFSET;
                    if (udp_payload_len > 0) {
                        ::dns::Message m;
                        if (m.decode(reinterpret_cast<uint8_t*>(packet + UDP_PAYLOAD_OFFSET), udp_payload_len) == ::dns::BufferResult::NoError && !m.questions.empty()) {
                            ::dns::QuestionSection& qs = *m.questions.data();

                            const bool address_query = qs.mType == ::dns::RecordType::kA ||
                                qs.mType == ::dns::RecordType::kAAAA;
                            if (address_query && !ppp::net::asio::vdns::QueryCache2(qs.mName.data(), m,
                                qs.mType == ::dns::RecordType::kA ? ppp::net::asio::vdns::AddressFamily::kA : ppp::net::asio::vdns::AddressFamily::kAAAA).empty()) {

                                // Build IPv6 addresses from raw header bytes
                                boost::asio::ip::address_v6::bytes_type src_bytes, dst_bytes;
                                memcpy(src_bytes.data(), ipv6_header->Source, sizeof(src_bytes));
                                memcpy(dst_bytes.data(), ipv6_header->Destination, sizeof(dst_bytes));
                                boost::asio::ip::address_v6 src_v6(src_bytes);
                                boost::asio::ip::address_v6 dst_v6(dst_bytes);

                                // Prefer IPv4: cache is always clean (filtered before AddCache),
                                // so no need to strip here. Forward directly.
                                std::size_t dns_size = 0;
                                char dns_packet[PPP_MAX_DNS_PACKET_BUFFER_SIZE];
                                if (m.encode(dns_packet, PPP_MAX_DNS_PACKET_BUFFER_SIZE, dns_size) == ::dns::BufferResult::NoError && dns_size > 0) {

                                    // Add response to cache if caching is enabled
                                    if (configuration_->udp.dns.cache) {
                                        ppp::net::asio::vdns::AddCache(reinterpret_cast<Byte*>(dns_packet), static_cast<int>(dns_size));
                                    }

                                    // Send DNS response back to the source (swap src/dst)
                                    return DatagramOutput(
                                        boost::asio::ip::udp::endpoint(src_v6, src_port),
                                        boost::asio::ip::udp::endpoint(dst_v6, dst_port),
                                        dns_packet, static_cast<int>(dns_size), false);
                                }
                            }

                            // Check dns-rules.txt for domain-based DNS redirect (mirrors IPv4 RedirectDnsServer logic)
                            // This ensures Chinese domains are resolved via domestic DNS (e.g. AliDNS 223.5.5.5)
                            // instead of the VPN's DNS (Cloudflare), preventing non-China CDN IPs from being returned.
                            if (NULLPTR != exchanger_) {
                                boost::asio::ip::address_v6::bytes_type src_bytes, dst_bytes;
                                memcpy(src_bytes.data(), ipv6_header->Source, sizeof(src_bytes));
                                memcpy(dst_bytes.data(), ipv6_header->Destination, sizeof(dst_bytes));
                                boost::asio::ip::address_v6 src_v6(src_bytes);
                                boost::asio::ip::address_v6 dst_v6(dst_bytes);

                                boost::asio::ip::address redirect_server;
                                bool geo_direct_dns = SelectDirectDnsServer(
                                    stl::transform<ppp::string>(qs.mName), redirect_server);
                                ppp::app::client::dns::Rule::Ptr rulePtr;
                                if (!geo_direct_dns) {
                                    rulePtr = ppp::app::client::dns::Rule::Get(
                                        stl::transform<ppp::string>(qs.mName),
                                        dns_ruless_[0], dns_ruless_[1], dns_ruless_[2]);
                                    if (rulePtr) redirect_server = rulePtr->Server;
                                }
                                if ((geo_direct_dns || NULLPTR != rulePtr) && redirect_server != dst_v6) {
                                    std::shared_ptr<boost::asio::io_context> context = exchanger_->GetContext();
                                    std::shared_ptr<Byte> buffer = exchanger_->GetBuffer();
                                    if (NULLPTR != context && NULLPTR != buffer) {
                                        std::shared_ptr<boost::asio::ip::udp::socket> socket =
                                            make_shared_object<boost::asio::ip::udp::socket>(*context);
                                        if (socket) {
                                            boost::system::error_code ec;
                                            boost::asio::ip::udp::endpoint serverEP(redirect_server, PPP_DNS_SYS_PORT);
                                            socket->open(serverEP.protocol(), ec);
                                            if (!ec) {
                                                int handle = socket->native_handle();
                                                ppp::net::Socket::AdjustDefaultSocketOptional(handle, redirect_server.is_v4());
                                                ppp::net::Socket::SetTypeOfService(handle);
                                                ppp::net::Socket::SetSignalPipeline(handle, false);
                                                ppp::net::Socket::ReuseSocketAddress(handle, true);
#if defined(_LINUX)
                                                // ColorOS also routes IPv6 through the
                                                // VPN (ip -6 rule 13000 -> table 1291
                                                // -> tun0), so a direct DNS socket over
                                                // IPv6 needs the same protect() as its
                                                // IPv4 counterpart to reach the physical
                                                // network. Without it the query loops
                                                // back into the tunnel and times out.
                                                if (!redirect_server.is_loopback()) {
                                                    auto protector_network = GetProtectorNetwork();
                                                    if (NULLPTR != protector_network) {
                                                        protector_network->Protect(handle);
                                                    }
                                                }
#endif

                                                socket->send_to(boost::asio::buffer(packet + UDP_PAYLOAD_OFFSET, udp_payload_len),
                                                    serverEP, 0, ec);
                                                if (!ec) {
                                                    const auto self = shared_from_this();
                                                    const auto cb = make_shared_object<Timer::TimeoutEventHandler>(
                                                        [self, socket](Timer*) noexcept {
                                                            ppp::net::Socket::Closesocket(socket);
                                                        });
                                                    if (NULLPTR != cb) {
                                                        const auto timeout = Timer::Timeout(context,
                                                            (uint64_t)configuration_->udp.dns.timeout * 1000, *cb);
                                                        if (NULLPTR != timeout && EmplaceTimeout(socket.get(), cb)) {
                                                            const auto max_buffer_size = PPP_BUFFER_SIZE - sizeof(serverEP);
                                                            socket->async_receive_from(
                                                                boost::asio::buffer(buffer.get(), max_buffer_size),
                                                                *reinterpret_cast<boost::asio::ip::udp::endpoint*>(buffer.get() + max_buffer_size),
                                                                [self, this, socket, timeout, buffer, src_v6, dst_v6, src_port]
                                                                (boost::system::error_code ec, size_t sz) noexcept {
                                                                    DeleteTimeout(socket.get());
                                                                    if (ec == boost::system::errc::success && sz > 0) {
                                                                        ::dns::Message m;
                                                                        if (m.decode(reinterpret_cast<uint8_t*>(buffer.get()), sz) == ::dns::BufferResult::NoError) {
                                                                            // Prefer IPv4: strip AAAA from upstream responses.
                                                                            // Returns false if AAAA needs deferral (A cache not yet populated).
                                                                            if (!StripAAAADnsResponseIfIPv4Available(m)) {
                                                                                auto pending = make_shared_object<PendingAAAAResponse>();
                                                                                if (pending) {
                                                                                    pending->EncodedPacket.assign(reinterpret_cast<char*>(buffer.get()), sz);
                                                                                    pending->IsIPv6 = true;
                                                                                    pending->SrcV6 = src_v6;
                                                                                    pending->DstV6 = dst_v6;
                                                                                    pending->SrcPort = src_port;
                                                                                    pending->DstPort = PPP_DNS_SYS_PORT;
                                                                                    pending->expire_time = Executors::GetTickCount() + static_cast<uint64_t>(configuration_->udp.dns.timeout) * 1000;
                                                                                    pending_aaaa_[ppp::string(m.questions[0].mName.data())] = pending;
                                                                                }
                                                                            } else {
                                                                                size_t new_sz = 0;
                                                                                if (m.encode(reinterpret_cast<uint8_t*>(buffer.get()), sz, new_sz) == ::dns::BufferResult::NoError && new_sz > 0) {
                                                                                    DatagramOutput(
                                                                                        boost::asio::ip::udp::endpoint(src_v6, src_port),
                                                                                        boost::asio::ip::udp::endpoint(dst_v6, PPP_DNS_SYS_PORT),
                                                                                        buffer.get(), static_cast<int>(new_sz), false);
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                    ppp::net::Socket::Closesocket(socket);
                                                                    if (timeout) {
                                                                        timeout->Stop();
                                                                        timeout->Dispose();
                                                                    }
                                                                });
                                                            return true;
                                                        }
                                                    }
                                                }
                                            }
                                            ppp::net::Socket::Closesocket(socket);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Block QUIC (UDP port 443) if configured
                if (block_quic_ && dst_port == PPP_HTTPS_SYS_PORT) {
                    return false;
                }

                // Default: forward via NAT
                boost::asio::ip::address_v6::bytes_type destination_bytes;
                memcpy(destination_bytes.data(), ipv6_header->Destination, destination_bytes.size());
                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(boost::asio::ip::address_v6(destination_bytes));
                if (NULLPTR == exchanger) {
                    return false;
                }
                exchanger->Nat(packet, packet_length);
                return true;
            }

            bool VEthernetNetworkSwitcher::OnIPv6IcmpPacketInput(Byte* packet, int packet_length, ppp::ipv6::PacketHeader* ipv6_header) noexcept {
                // Need at least IPv6 header (40) + ICMPv6 header (4)
                static constexpr int ICMP_HEADER_OFFSET = sizeof(ppp::ipv6::PacketHeader);
                static constexpr int ICMP_HEADER_MIN_SIZE = 4;
                static constexpr uint8_t ICMPV6_ECHO_REQUEST = 128;
                static constexpr uint8_t ICMPV6_ECHO_REPLY = 129;

                if (packet_length < ICMP_HEADER_OFFSET + ICMP_HEADER_MIN_SIZE) {
                    return false;
                }

                Byte* icmp_start = packet + ICMP_HEADER_OFFSET;
                uint8_t icmp_type = icmp_start[0];
                uint8_t icmp_code = icmp_start[1];

                // Handle Echo Request (type 128, code 0)
                if (icmp_type == ICMPV6_ECHO_REQUEST && icmp_code == 0) {
                    auto tap = GetTap();
                    if (NULLPTR == tap) {
                        return false;
                    }

                    // Extract destination address from packet
                    boost::asio::ip::address_v6::bytes_type dst_bytes;
                    memcpy(dst_bytes.data(), ipv6_header->Destination, sizeof(dst_bytes));
                    boost::asio::ip::address_v6 dst_v6(dst_bytes);

                    boost::asio::ip::address tap_v6 = tap->IPv6Address;
                    boost::asio::ip::address tap_gw6 = tap->IPv6GatewayServer;

                    // Only respond if the echo request is addressed to our TAP interface
                    if (dst_v6 != tap_v6 && (!tap_gw6.is_v6() || dst_v6 != tap_gw6.to_v6())) {
                        // Not for us - forward via NAT
                        std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(dst_v6);
                        if (NULLPTR == exchanger) {
                            return false;
                        }
                        exchanger->Nat(packet, packet_length);
                        return true;
                    }

                    // Swap source and destination addresses in-place
                    Byte temp_addr[16];
                    memcpy(temp_addr, ipv6_header->Source, 16);
                    memcpy(ipv6_header->Source, ipv6_header->Destination, 16);
                    memcpy(ipv6_header->Destination, temp_addr, 16);

                    // Change ICMPv6 type from Echo Request (128) to Echo Reply (129)
                    icmp_start[0] = ICMPV6_ECHO_REPLY;

                    // Reset hop limit to default
                    ipv6_header->HopLimit = ppp::ipv6::IPv6_DEFAULT_HOP_LIMIT;

                    // Recompute ICMPv6 checksum
                    // Zero out old checksum first
                    icmp_start[2] = 0;
                    icmp_start[3] = 0;

                    int icmp_len = packet_length - ICMP_HEADER_OFFSET;
                    boost::asio::ip::address_v6 new_src_v6(dst_bytes); // Was dest, now source
                    boost::asio::ip::address_v6::bytes_type new_dst_bytes;
                    memcpy(new_dst_bytes.data(), temp_addr, 16); // Was source, now dest
                    boost::asio::ip::address_v6 new_dst_v6(new_dst_bytes);

                    uint16_t cksum = ppp::ipv6::ComputePseudoChecksum(
                        icmp_start, static_cast<unsigned int>(icmp_len),
                        new_src_v6, new_dst_v6,
                        IPPROTO_ICMPV6);

                    // ComputePseudoChecksum returns value in host byte order.
                    // Convert to network byte order for wire format using htons().
                    uint16_t cksum_be = htons(cksum);
                    icmp_start[2] = static_cast<Byte>((cksum_be >> 8) & 0xFF);
                    icmp_start[3] = static_cast<Byte>(cksum_be & 0xFF);

                    // Output the modified echo reply back to TAP
                    return Output(packet, packet_length);
                }

                // Other ICMPv6 types: forward via NAT
                boost::asio::ip::address_v6::bytes_type destination_bytes;
                memcpy(destination_bytes.data(), ipv6_header->Destination, destination_bytes.size());
                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(boost::asio::ip::address_v6(destination_bytes));
                if (NULLPTR == exchanger) {
                    return false;
                }
                exchanger->Nat(packet, packet_length);
                return true;
            }

            bool VEthernetNetworkSwitcher::OnUdpPacketInput(const std::shared_ptr<IPFrame>& packet) noexcept {
                std::shared_ptr<UdpFrame> frame = UdpFrame::Parse(packet.get());
                if (NULLPTR == frame) {
                    return false;
                }

                const std::shared_ptr<BufferSegment>& messages = frame->Payload;
                if (NULLPTR == messages) {
                    return false;
                }

                boost::asio::ip::udp::endpoint destinationEP = IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Destination);
                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger(destinationEP.address());
                if (NULLPTR == exchanger) {
                    return false;
                }

                // Check whether dns resolution packets need to be redirected.
                int destinationPort = frame->Destination.Port;
                if (destinationPort == PPP_DNS_SYS_PORT) {
                    if (RedirectDnsServer(exchanger, packet, frame, messages)) {
                        return true;
                    }
                }

                // If the current need to prohibit the transfer of QUIC IETF control protocol traffic, 
                // then the outgoing traffic sent to the 443 two ports through the UDP protocol can be directly discarded, 
                // simple and rough processing, if the remote sensing of all UDP port traffic, 
                // it will produce unnecessary burden and overhead on the performance of the program itself.
                if (block_quic_ && destinationPort == PPP_HTTPS_SYS_PORT) {
                    return false;
                }

                // If the VPN uses static transmission mode, ensure that the link is link ready.
                if (static_mode_) {
                    auto& static_ = configuration_->udp.static_;
                    if (static_.quic && destinationPort == PPP_HTTPS_SYS_PORT) {
                        if (exchanger->StaticEchoAllocated()) {
                            return exchanger->StaticEchoPacketToRemoteExchanger(frame);
                        }
                    }
                    elif(static_.dns && destinationPort == PPP_DNS_SYS_PORT) {
                        if (exchanger->StaticEchoAllocated()) {
                            return exchanger->StaticEchoPacketToRemoteExchanger(frame);
                        }
                    }
                    elif(exchanger->StaticEchoAllocated()) {
                        return exchanger->StaticEchoPacketToRemoteExchanger(frame);
                    }
                }

                boost::asio::ip::udp::endpoint sourceEP = IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Source);
                return exchanger->SendTo(sourceEP, destinationEP, messages->Buffer.get(), messages->Length);
            }

            bool VEthernetNetworkSwitcher::ER(const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<IcmpFrame>& frame, int ttl, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept {
                std::shared_ptr<IPFrame> reply = ppp::net::asio::InternetControlMessageProtocol::ER(packet, frame, ttl, allocator);
                if (NULLPTR == reply) {
                    return false;
                }
                else {
                    return Output(reply.get());
                }
            }

            bool VEthernetNetworkSwitcher::TE(const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<IcmpFrame>& frame, UInt32 source, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept {
                std::shared_ptr<IPFrame> reply = ppp::net::asio::InternetControlMessageProtocol::TE(packet, frame, source, allocator);
                if (NULLPTR == reply) {
                    return false;
                }
                else {
                    return Output(reply.get());
                }
            }

            bool VEthernetNetworkSwitcher::ERORTE(int ack_id) noexcept {
                std::shared_ptr<IPFrame> packet;
                if (ack_id != 0) {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    bool ok = Dictionary::RemoveValueByKey(icmppackets_, ack_id, packet,
                        [](VEthernetIcmpPacket& value) noexcept {
                            return value.packet;
                        });
                    if (!ok) {
                        return false;
                    }
                }

                if (NULLPTR == packet) {
                    return false;
                }

                std::shared_ptr<ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                std::shared_ptr<IcmpFrame> frame = IcmpFrame::Parse(packet.get());
                if (NULLPTR == frame) {
                    return false;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = GetBufferAllocator();
                if (IPAddressIsGatewayServer(frame->Destination, tap->GatewayServer, tap->SubmaskAddress)) {
                    int ttl = static_cast<int>(IPFrame::DefaultTtl);
                    LOG_DEBUG("VEthernetNetworkSwitcher::ERORTE: local IPv4 echo reply via TAP gateway, request_ttl=%u, reply_ttl=%d, src=%u, dst=%u",
                        static_cast<unsigned int>(frame->Ttl), ttl, frame->Source, frame->Destination);
                    return ER(packet, frame, ttl, allocator);
                }
                else {
                    return TE(packet, frame, tap->GatewayServer, allocator);
                }
            }

            bool VEthernetNetworkSwitcher::OnIcmpPacketInput(const std::shared_ptr<IPFrame>& packet) noexcept {
                std::shared_ptr<ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = GetBufferAllocator();
                std::shared_ptr<IcmpFrame> frame = IcmpFrame::Parse(packet.get());
                if (NULLPTR == frame || frame->Ttl == 0) {
                    return false;
                }
                elif(IPAddressIsGatewayServer(frame->Destination, tap->GatewayServer, tap->SubmaskAddress)) {
                    return EchoGatewayServer(exchanger_, packet, allocator);
                }
                elif(frame->Ttl == 1) {
                    return EchoGatewayServer(exchanger_, packet, allocator);
                }
                else {
                    int ttl = std::max<int>(0, static_cast<int>(packet->Ttl) - 1);
                    if (packet->Ttl < 1) {
                        return false;
                    }

                    frame->Ttl = ttl;
                    packet->Ttl = ttl;

                    return EchoOtherServer(GetExchanger(Ipep::ToAddress(frame->Destination)), packet, allocator);
                }
            }

            bool VEthernetNetworkSwitcher::EchoOtherServer(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept {
                if (NULLPTR == exchanger) {
                    return false;
                }

                if (IsDisposed()) {
                    return false;
                }

                std::shared_ptr<BufferSegment> messages = IPFrame::ToArray(allocator, packet.get());
                if (NULLPTR == messages) {
                    return false;
                }

                auto& static_ = configuration_->udp.static_;
                if ((static_mode_ && static_.icmp) && exchanger->StaticEchoAllocated()) {
                    return exchanger->StaticEchoPacketToRemoteExchanger(packet.get());
                }

                return exchanger->Echo(messages->Buffer.get(), messages->Length);
            }

            bool VEthernetNetworkSwitcher::EchoGatewayServer(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept {
                static constexpr int max_icmp_packets_aid = (1 << 24) - 1;
                
                if (NULLPTR == exchanger) {
                    return false;
                }

                int ack_id = 0;
                for (SynchronizedObjectScope scope(GetSynchronizedObject());;) {
                    if (IsDisposed()) {
                        return false;
                    }

                    VEthernetIcmpPacket e = { Executors::GetTickCount() + ppp::net::asio::InternetControlMessageProtocol::MAX_ICMP_TIMEOUT, packet };
                    bool static_exchange = false;

                    for (int i = 0; i < UINT16_MAX; i++) {
                        ack_id = ++icmppackets_aid_;
                        if (ack_id < 1) {
                            icmppackets_aid_ = 0;
                            continue;
                        }

                        if (ack_id > max_icmp_packets_aid) {
                            icmppackets_aid_ = 0;
                            continue;
                        }

                        if (ppp::collections::Dictionary::ContainsKey(icmppackets_, ack_id)) {
                            continue;
                        }

                        if (!ppp::collections::Dictionary::TryAdd(icmppackets_, ack_id, e)) {
                            return false;
                        }

                        auto& static_ = configuration_->udp.static_;
                        if ((static_mode_ && static_.icmp) && exchanger->StaticEchoAllocated()) {
                            static_exchange = true;
                            break;
                        }
                        elif(exchanger->Echo(ack_id)) {
                            return true;
                        }

                        ppp::collections::Dictionary::TryRemove(icmppackets_, ack_id);
                        return false;
                    }

                    if (static_exchange) {
                        break;
                    }

                    return false;
                }

                if (exchanger->StaticEchoGatewayServer(ack_id)) {
                    return true;
                }
                else {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    ppp::collections::Dictionary::TryRemove(icmppackets_, ack_id);
                    return false;
                }
            }

            void VEthernetNetworkSwitcher::Dispose() noexcept {
#if !defined(_ANDROID) && !defined(_IPHONE)
                network_takeover_stopping_.store(true);
                // Restore host routing and DNS before posting asynchronous object
                // teardown. The application may be terminated shortly after Dispose,
                // so critical OS state must never depend on queued cleanup completing.
                {
                    SynchronizedObjectScope scope(prdr_);
                    RestoreNetworkState();
                }
#endif
                LOG_DEBUG("VEthernetNetworkSwitcher::Dispose: posting Finalize");
                auto self = shared_from_this();
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                boost::asio::dispatch(*context, 
                    [self, this, context]() noexcept {
                        Finalize();
                    });
                VEthernet::Dispose();
            }

            void VEthernetNetworkSwitcher::Finalize() noexcept {
                network_takeover_stopping_.store(true);
                LOG_DEBUG("VEthernetNetworkSwitcher::Finalize: releasing all objects");
#if defined(PPP_LOG_VERBOSE)
                StopDebugWatchdog();
#endif
                IDisposable::Dispose(logger_);
                ReleaseAllObjects();
                ReleaseAllPackets();
                ReleaseAllTimeouts();
                LOG_INFO("VEthernetNetworkSwitcher::Finalize: cleanup completed");
            }

#if defined(PPP_LOG_VERBOSE)
            void VEthernetNetworkSwitcher::StopDebugWatchdog() noexcept {
                debug_watchdog_stop_ = true;
                if (debug_watchdog_.joinable() && debug_watchdog_.get_id() != std::this_thread::get_id()) {
                    debug_watchdog_.join();
                }
            }
#endif

            bool VEthernetNetworkSwitcher::OpenLogger() noexcept {
                ppp::string& log = configuration_->client.log;
                if (log.empty()) {
                    return false;
                }

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                VirtualEthernetLoggerPtr logger = make_shared_object<VirtualEthernetLogger>(context, log);
                if (NULLPTR == logger) {
                    return false;
                }

                if (logger->Valid()) {
                    logger_ = std::move(logger);
                    return true;
                }

                IDisposable::Dispose(logger);
                return false;
            }

            void VEthernetNetworkSwitcher::ReleaseAllPackets() noexcept {
                // Clear all ICMP packet container.
                SynchronizedObjectScope scope(GetSynchronizedObject());
                icmppackets_.clear();
            }

            void VEthernetNetworkSwitcher::ReleaseAllTimeouts() noexcept {
                TimeoutEventHandlerTable timeouts; {
                    // Clear all ICMP packet container.
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    timeouts = std::move(timeouts_);
                    timeouts_.clear();
                }

                // Release all timeout callbacks.
                Timer::ReleaseAllTimeouts(timeouts);
            }

#if defined(_ANDROID) || defined(_IPHONE)
            void VEthernetNetworkSwitcher::SetBypassIpList(ppp::string&& bypass_ip_list) noexcept {
                bypass_ip_list_ = std::move(bypass_ip_list);
            }
#endif

            std::shared_ptr<ppp::transmissions::ITransmissionQoS> VEthernetNetworkSwitcher::NewQoS() noexcept {
                int64_t bandwidth = std::max<int64_t>(0, configuration_->client.bandwidth);
                if (bandwidth < 0) {
                    bandwidth *= (1024 >> 3); /* Kbps. */
                }

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                return make_shared_object<ppp::transmissions::ITransmissionQoS>(context, bandwidth);
            }

            bool VEthernetNetworkSwitcher::SetOutboundConfigurations(const OutboundConfigurationList& configurations) noexcept {
                if (configurations.empty()) {
                    outbound_configurations_.clear();
                    return true;
                }

                bool has_main = false;
                ppp::unordered_set<ppp::string> tags;
                for (const OutboundConfiguration& outbound : configurations) {
                    ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag));
                    if (tag.empty() || NULLPTR == outbound.configuration || !tags.emplace(tag).second) {
                        return false;
                    }
                    if (tag == "main") {
                        has_main = true;
                        if (outbound.configuration != base_configuration_) return false;
                    }
                }
                if (!has_main) return false;
                outbound_configurations_ = configurations;
                primary_outbound_ = "main";
                for (const OutboundConfiguration& outbound : configurations) {
                    ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag));
                    if (tag != "main" && outbound.route_used &&
                        outbound.configuration == base_configuration_) {
                        primary_outbound_ = tag;
                        break;
                    }
                }
                return true;
            }

            VEthernetNetworkSwitcher::OutboundStatusList
                VEthernetNetworkSwitcher::GetOutboundStatuses() noexcept {
                OutboundStatusList statuses;
                SynchronizedObjectScope scope(GetSynchronizedObject());
                statuses.reserve(outbound_exchangers_.size());
                for (const OutboundConfiguration& outbound : outbound_configurations_) {
                    ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag));
                    ppp::string exchanger_tag = tag;
                    if (outbound.server_menu) {
                        if (tag == primary_outbound_) exchanger_tag = "main";
                        elif(!outbound.route_used || tag == "main") exchanger_tag.clear();
                    }
                    auto current = exchanger_tag.empty() ? outbound_exchangers_.end() :
                        outbound_exchangers_.find(exchanger_tag);
                    OutboundStatus status;
                    status.tag = tag;
                    status.display_name = outbound.display_name.empty() ? tag : outbound.display_name;
                    status.server = NULLPTR != outbound.configuration ?
                        outbound.configuration->client.server : ppp::string();
                    status.state = current != outbound_exchangers_.end() && NULLPTR != current->second ?
                        static_cast<int>(current->second->GetNetworkState()) : -1;
                    status.reconnects = current != outbound_exchangers_.end() && NULLPTR != current->second ?
                        current->second->GetReconnectionCount() : 0;
                    status.active = outbound.server_menu ?
                        tag == primary_outbound_ : tag == active_outbound_;
                    status.server_menu = outbound.server_menu;
                    status.route_used = outbound.route_used;
                    status.multiple_entries = NULLPTR != outbound.configuration &&
                        !outbound.configuration->client.servers.empty();
                    status.probe_enabled = NULLPTR != outbound.configuration && outbound.configuration->client.probe.enabled;
                    if (current != outbound_exchangers_.end() && NULLPTR != current->second) {
                        status.probe_checked = current->second->GetProbeChecked();
                        status.probe_reachable = current->second->GetProbeReachable();
                        status.probe_rtt_ms = current->second->GetProbeRtt();
                        status.current_entry = current->second->GetCurrentEntry();
                        status.ranked_first_entry = current->second->GetRankedFirstEntry();
                    }
#if !defined(_ANDROID) && !defined(_IPHONE)
                    // Background probe refresh covers every menu entry every 5s.
                    // It may refresh the displayed health metrics, but must not
                    // replace current_entry: that field is the live entry
                    // currently used by the exchanger/MUX.  Replacing it
                    // with the best probe result makes the UI look as if a
                    // healthy connection switched entries when it did not.
                    {
                        auto probe = outbound_probe_statuses_.find(tag);
                        if (probe != outbound_probe_statuses_.end() && probe->second.checked) {
                            status.probe_checked = true;
                            status.probe_reachable = probe->second.reachable;
                            status.probe_rtt_ms = probe->second.rtt_ms;
                            status.probe_entry = probe->second.server;
                        }
                    }
#endif
                    statuses.emplace_back(std::move(status));
                }
                if (statuses.empty() && NULLPTR != exchanger_) {
                    OutboundStatus status;
                    status.tag = "main";
                    status.display_name = "main";
                    status.server = configuration_->client.server;
                    status.state = static_cast<int>(exchanger_->GetNetworkState());
                    status.reconnects = exchanger_->GetReconnectionCount();
                    status.active = true;
                    status.multiple_entries = NULLPTR != configuration_ &&
                        !configuration_->client.servers.empty();
                    status.probe_enabled = NULLPTR != configuration_ && configuration_->client.probe.enabled;
                    status.probe_checked = exchanger_->GetProbeChecked();
                    status.probe_reachable = exchanger_->GetProbeReachable();
                    status.probe_rtt_ms = exchanger_->GetProbeRtt();
                    status.current_entry = exchanger_->GetCurrentEntry();
                    status.ranked_first_entry = exchanger_->GetRankedFirstEntry();
#if !defined(_ANDROID) && !defined(_IPHONE)
                    {
                        auto probe = outbound_probe_statuses_.find("main");
                        if (probe != outbound_probe_statuses_.end() && probe->second.checked) {
                            status.probe_checked = true;
                            status.probe_reachable = probe->second.reachable;
                            status.probe_rtt_ms = probe->second.rtt_ms;
                            status.probe_entry = probe->second.server;
                        }
                    }
#endif
                    statuses.emplace_back(std::move(status));
                }
                return statuses;
            }

            ppp::string VEthernetNetworkSwitcher::GetActiveOutbound() noexcept {
                SynchronizedObjectScope scope(GetSynchronizedObject());
                return active_outbound_.empty() ? ppp::string("main") : active_outbound_;
            }

            bool VEthernetNetworkSwitcher::IsRouteOutbound(
                const ppp::string& value) const noexcept {
                ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(value));
                for (const OutboundConfiguration& outbound : outbound_configurations_) {
                    if (outbound.route_used &&
                        ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag)) == tag) {
                        return true;
                    }
                }
                return false;
            }

            ppp::string VEthernetNetworkSwitcher::GetActiveOutboundSourcePath() noexcept {
                ppp::string active;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    active = active_outbound_.empty() ? ppp::string("main") : active_outbound_;
                    if (active == "main") active = primary_outbound_;
                }
                for (const OutboundConfiguration& outbound : outbound_configurations_) {
                    if (ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag)) == active) {
                        return outbound.source_path;
                    }
                }
                return ppp::string();
            }

            bool VEthernetNetworkSwitcher::SwitchOutbound(const ppp::string& value) noexcept {
                ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(value));
                if (tag.empty() || tag == "direct") {
                    return false;
                }

                OutboundConfiguration* definition = NULLPTR;
                for (OutboundConfiguration& outbound : outbound_configurations_) {
                    if (ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag)) == tag) {
                        definition = &outbound;
                        break;
                    }
                }
                if (NULLPTR == definition) return false;

                std::shared_ptr<VEthernetExchanger> target;
                if (tag == "main") {
                    target = exchanger_;
                }
                else {
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration =
                        ReloadOutboundConfiguration(*definition);
                    if (NULLPTR == configuration) return false;
                    target = NewExchanger(configuration, tag, false);
                    if (NULLPTR == target || !target->Open()) return false;
                }

                std::shared_ptr<VEthernetExchanger> abandoned;
                bool abandoned_referenced = false;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    abandoned = std::move(pending_outbound_exchanger_);
                    pending_outbound_ = tag;
                    pending_outbound_exchanger_ = target;
                    pending_primary_switch_ = false;
                    pending_outbound_deadline_ =
                        ppp::threading::Executors::GetTickCount() + 2000;
                    if (NULLPTR != abandoned) {
                        for (const auto& item : outbound_exchangers_) {
                            if (item.second == abandoned) {
                                abandoned_referenced = true;
                                break;
                            }
                        }
                    }
                }
                if (NULLPTR != abandoned && !abandoned_referenced && abandoned != target &&
                    abandoned != exchanger_) abandoned->Dispose();
                LOG_INFO("VEthernetNetworkSwitcher::SwitchOutbound: target=%s, configuration_reloaded=%d, activation_delay_ms=2000, state=%d",
                    tag.data(), tag != "main",
                    (int)target->GetNetworkState());
                return true;
            }

            bool VEthernetNetworkSwitcher::SwitchPrimaryOutbound(const ppp::string& value) noexcept {
                ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(value));
                if (tag.empty() || tag == "direct") return false;

                // Already the active primary outbound (and no switch in flight):
                // nothing to do.  This matters for "main", which is now always
                // present in the hot-switch menu.  The check MUST run before
                // ReloadOutboundConfiguration/NewExchanger/Open below: re-selecting
                // the same tag would otherwise leak an orphan exchanger whose
                // Loopback keeps reconnecting to the same server (the server
                // rejects the duplicate session_id, producing an endless
                // connect -> handshake success -> read failed loop).
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    if (tag == primary_outbound_ && pending_outbound_.empty()) {
                        return true;
                    }
                    // Treat repeated Enter presses during the two-second warm
                    // activation window as the same request.  Once activation
                    // completes, Enter on the active row becomes the explicit
                    // "switch to Rank #1" action instead.
                    if (tag == pending_outbound_) {
                        return true;
                    }
                }

                // "main" is the primary outbound itself (the configuration that
                // owns the TUN). It is only selectable from the server menu when
                // --server-dir contains a JSON with the same GUID/server, so it
                // is normally NOT switchable back after a hot switch. Accept it
                // explicitly so the hot-switch menu can always return to the
                // primary configuration.
                OutboundConfiguration* definition = NULLPTR;
                for (OutboundConfiguration& outbound : outbound_configurations_) {
                    if (ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag)) == tag &&
                        (outbound.server_menu || outbound.route_used || tag == "main")) {
                        definition = &outbound;
                        break;
                    }
                }
                if (NULLPTR == definition) return false;

                // Reload the selected definition before looking for an
                // existing exchanger.  A server-directory tag such as
                // "server:zgo" and a GEO tag such as "us" can refer to the
                // same JSON, but their tags and configuration pointers are
                // different.  Match the session identity as well, otherwise
                // the hot switch opens a second control/MUX session.
                std::shared_ptr<ppp::configurations::AppConfiguration> configuration =
                    ReloadOutboundConfiguration(*definition);
                if (NULLPTR == configuration) return false;

                // A primary configuration switch deliberately creates a new
                // exchanger/MUX.  The old primary may contain state belonging
                // to a different server (IPv6 lease, proxy, mappings, DNS,
                // entry ranking), so promoting a warm split exchanger here can
                // leave stale state behind.  GEO split outbounds still use the
                // separate EnsureOutbound() reuse path below.
                std::shared_ptr<VEthernetExchanger> target =
                    NewExchanger(configuration, "main", true);
                if (NULLPTR == target || !target->Open()) return false;

                std::shared_ptr<VEthernetExchanger> abandoned;
                bool abandoned_referenced = false;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    abandoned = std::move(pending_outbound_exchanger_);
                    pending_outbound_ = tag;
                    pending_outbound_exchanger_ = target;
                    pending_primary_switch_ = true;
                    pending_outbound_deadline_ =
                        ppp::threading::Executors::GetTickCount() + 2000;
                    if (NULLPTR != abandoned) {
                        for (const auto& item : outbound_exchangers_) {
                            if (item.second == abandoned) {
                                abandoned_referenced = true;
                                break;
                            }
                        }
                    }
                }
                if (NULLPTR != abandoned && !abandoned_referenced && abandoned != target &&
                    abandoned != exchanger_) abandoned->Dispose();
                LOG_INFO("VEthernetNetworkSwitcher::SwitchPrimaryOutbound: target=%s, activation_delay_ms=2000, state=%d",
                    tag.data(), (int)target->GetNetworkState());
                return true;
            }

            bool VEthernetNetworkSwitcher::SwitchPrimaryOutboundToRankedFirst(const ppp::string& value) noexcept {
                const ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(value));
                std::shared_ptr<VEthernetExchanger> target;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    if (tag.empty() || tag != primary_outbound_ || !pending_outbound_.empty()) {
                        return false;
                    }
                    target = exchanger_;
                }
                return NULLPTR != target && target->SwitchToRankedFirstEntry();
            }

            std::shared_ptr<ppp::configurations::AppConfiguration>
                VEthernetNetworkSwitcher::ReloadOutboundConfiguration(
                    OutboundConfiguration& outbound) noexcept {
                if (outbound.source_path.empty()) return outbound.configuration;
                std::shared_ptr<ppp::configurations::AppConfiguration> configuration =
                    make_shared_object<ppp::configurations::AppConfiguration>();
                if (NULLPTR == configuration || !configuration->Load(outbound.source_path)) {
                    LOG_ERROR("VEthernetNetworkSwitcher::ReloadOutboundConfiguration: failed, outbound=%s, path=%s",
                        outbound.tag.data(), outbound.source_path.data());
                    return NULLPTR;
                }
#if defined(_WIN32)
                bool initialize_allocator = configuration->vmem.size > 0;
#else
                bool initialize_allocator = configuration->vmem.path.size() > 0 &&
                    configuration->vmem.size > 0;
#endif
                if (initialize_allocator) {
                    std::shared_ptr<ppp::threading::BufferswapAllocator> allocator =
                        make_shared_object<ppp::threading::BufferswapAllocator>(
                            configuration->vmem.path,
                            std::max<int64_t>((int64_t)1LL << (int64_t)25LL,
                                (int64_t)configuration->vmem.size << (int64_t)20LL));
                    if (NULLPTR != allocator && allocator->IsVaild()) {
                        configuration->SetBufferAllocator(allocator);
                    }
                    else {
                        LOG_ERROR("VEthernetNetworkSwitcher::ReloadOutboundConfiguration: buffer allocator unavailable, outbound=%s, path=%s",
                            outbound.tag.data(), outbound.source_path.data());
                    }
                }
                if (NULLPTR == configuration->GetBufferAllocator()) {
                    // The reloaded configuration declares no vmem pool (or the
                    // pool could not be created). A BufferswapAllocator is a
                    // server-independent packet memory pool, so inherit the
                    // previous outbound allocator (falling back to the switcher
                    // primary allocator) rather than leaving bridges with a null
                    // GetBufferAllocator(), which crashed on the first packet.
                    std::shared_ptr<ppp::threading::BufferswapAllocator> previous =
                        NULLPTR != outbound.configuration ?
                            outbound.configuration->GetBufferAllocator() : NULLPTR;
                    if (NULLPTR == previous) {
                        previous = configuration_->GetBufferAllocator();
                    }
                    if (NULLPTR != previous) {
                        configuration->SetBufferAllocator(previous);
                    }
                }
                outbound.configuration = configuration;
                LOG_INFO("VEthernetNetworkSwitcher::ReloadOutboundConfiguration: loaded, outbound=%s, path=%s, server=%s",
                    outbound.tag.data(), outbound.source_path.data(),
                    configuration->client.server.data());
                return configuration;
            }

            bool VEthernetNetworkSwitcher::ApplyPrimaryClientConfiguration(
                const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration) noexcept {
                if (NULLPTR == configuration) {
                    return false;
                }

                // The primary transport uses the switcher-owned forwarding
                // object. Rebuild it from the newly promoted profile so a
                // server-proxy setting cannot remain inherited from the old
                // main connection.
                IForwardingPtr forwarding;
                if (!configuration->client.server_proxy.empty()) {
                    forwarding = make_shared_object<IForwarding>(GetContext(), configuration);
                    if (NULLPTR == forwarding || !forwarding->Open()) {
                        if (NULLPTR != forwarding) {
                            forwarding->Dispose();
                        }
                        LOG_ERROR("VEthernetNetworkSwitcher::ApplyPrimaryClientConfiguration: cannot open primary server proxy, server=%s",
                            configuration->client.server.data());
                        return false;
                    }
#if defined(_LINUX)
                    forwarding->ProtectorNetwork = GetProtectorNetwork();
#endif
                }

                IForwardingPtr obsolete_forwarding;
                std::shared_ptr<ppp::configurations::AppConfiguration> previous_configuration;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    previous_configuration = configuration_;
                    configuration_ = configuration;
                    obsolete_forwarding = std::move(forwarding_);
                    forwarding_ = std::move(forwarding);
                }

                if (NULLPTR != qos_) {
                    int64_t bandwidth = std::max<int64_t>(0, configuration->client.bandwidth);
                    qos_->SetBandwidth(bandwidth);
                }

                // The client log file is a property of the active profile.
                // Reopen it only when the path changes; ordinary switches keep
                // the existing logger without interrupting log writes.
                const ppp::string previous_log = NULLPTR != previous_configuration
                    ? previous_configuration->client.log : ppp::string();
                if (previous_log != configuration->client.log) {
                    VirtualEthernetLoggerPtr obsolete_logger = std::move(logger_);
                    OpenLogger();
                    if (NULLPTR != obsolete_logger) {
                        IDisposable::Dispose(obsolete_logger);
                    }
                }

                if (NULLPTR != obsolete_forwarding) {
                    obsolete_forwarding->Dispose();
                }

                LOG_INFO("VEthernetNetworkSwitcher::ApplyPrimaryClientConfiguration: active client profile changed, server=%s, server_proxy=%s, mappings=%llu",
                    configuration->client.server.data(),
                    configuration->client.server_proxy.empty() ? "direct" : "configured",
                    (unsigned long long)configuration->client.mappings.size());
                return true;
            }

            std::shared_ptr<VEthernetExchanger> VEthernetNetworkSwitcher::EnsureOutbound(
                const ppp::string& value) noexcept {
                ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(value));
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    auto current = outbound_exchangers_.find(tag);
                    if (current != outbound_exchangers_.end()) return current->second;
                }

                OutboundConfiguration* definition = NULLPTR;
                for (OutboundConfiguration& outbound : outbound_configurations_) {
                    if (ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag)) == tag) {
                        definition = &outbound;
                        break;
                    }
                }
                if (NULLPTR == definition) return NULLPTR;
                std::shared_ptr<ppp::configurations::AppConfiguration> configuration =
                    ReloadOutboundConfiguration(*definition);
                if (NULLPTR == configuration) return NULLPTR;

                // The same JSON may already be active under the primary
                // server-menu tag.  Reuse it under the GEO tag instead of
                // opening a second session when the split rule is first hit.
                std::shared_ptr<VEthernetExchanger> reused;
                ppp::string reused_tag;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    for (const auto& item : outbound_exchangers_) {
                        if (NULLPTR == item.second) continue;
                        std::shared_ptr<ppp::configurations::AppConfiguration> current_configuration =
                            item.second->GetConfiguration();
                        if (NULLPTR == current_configuration ||
                            current_configuration->client.guid != configuration->client.guid ||
                            current_configuration->client.server != configuration->client.server) {
                            continue;
                        }
                        reused = item.second;
                        reused_tag = item.first;
                        break;
                    }
                }
                if (NULLPTR != reused) {
                    {
                        SynchronizedObjectScope scope(GetSynchronizedObject());
                        outbound_exchangers_[tag] = reused;
                    }
                    LOG_INFO("VEthernetNetworkSwitcher::EnsureOutbound: outbound '%s' reused existing tag '%s' by GUID/server identity",
                        tag.data(), reused_tag.data());
                    return reused;
                }

                std::shared_ptr<VEthernetExchanger> candidate =
                    NewExchanger(configuration, tag, tag == "main");
                if (NULLPTR == candidate || !candidate->Open()) return NULLPTR;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    auto result = outbound_exchangers_.emplace(tag, candidate);
                    if (!result.second) {
                        candidate->Dispose();
                        return result.first->second;
                    }
                }
                LOG_INFO("VEthernetNetworkSwitcher::EnsureOutbound: outbound '%s' opened on demand",
                    tag.data());
                return candidate;
            }

            void VEthernetNetworkSwitcher::CompletePendingOutboundSwitch(uint64_t now) noexcept {
                ppp::string target_tag;
                ppp::string previous_tag;
                std::shared_ptr<VEthernetExchanger> previous;
                std::shared_ptr<VEthernetExchanger> replaced;
                std::shared_ptr<VEthernetExchanger> target;
                bool preserve_previous = false;
                bool primary_switch = false;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    if (pending_outbound_.empty() || now < pending_outbound_deadline_) return;
                    target_tag = pending_outbound_;
                    previous_tag = active_outbound_.empty() ? ppp::string("main") : active_outbound_;
                    preserve_previous = IsRouteOutbound(previous_tag);
                    target = std::move(pending_outbound_exchanger_);
                    primary_switch = pending_primary_switch_;
                    if (NULLPTR == target) {
                        pending_outbound_.clear();
                        pending_outbound_deadline_ = 0;
                        pending_primary_switch_ = false;
                        return;
                    }
                    // Proxy-only/TUN is a process-level topology choice. The
                    // listener and OS route ownership cannot be converted
                    // safely while traffic is running, so refuse a profile
                    // whose client.proxy-only value is incompatible with the
                    // topology selected at startup.
                    {
                        const auto target_configuration = target->GetConfiguration();
                        const bool target_proxy_only = NULLPTR != target_configuration &&
                            (target_configuration->client.proxy_only || proxy_only_forced_);
                        if (primary_switch && NULLPTR != target_configuration &&
                            target_proxy_only != proxy_only_) {
                            pending_outbound_.clear();
                            pending_outbound_deadline_ = 0;
                            pending_primary_switch_ = false;
                            LOG_ERROR("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: target=%s has incompatible client.proxy-only=%d (runtime=%d)",
                                target_tag.data(), (int)target_configuration->client.proxy_only,
                                (int)proxy_only_);
                            return;
                        }
                    }
                    if (primary_switch) {
                        // Keep the old primary available under its original
                        // split tag when that configuration is also used by
                        // geo rules.  "main" remains only the current role.
                        previous_tag = primary_outbound_.empty() ?
                            ppp::string("main") : primary_outbound_;
                        previous = exchanger_;
                        // Manual primary switching is a full configuration
                        // replacement.  Do not keep the old primary under a
                        // private/base or GEO alias; its MUX and all per-peer
                        // state must be destroyed below.  Non-primary GEO
                        // switches retain their existing warm split behavior.
                        preserve_previous = false;
                        // Release the old primary's mapping listeners before
                        // promoting the target.  Otherwise equal mapping
                        // ports can make the target registration fail while
                        // the old exchanger is still alive.
                        if (NULLPTR != previous && previous != target && previous->IsPrimaryOutbound() &&
                            !previous->SetPrimaryOutbound(false)) {
                            pending_outbound_.clear();
                            pending_outbound_deadline_ = 0;
                            pending_primary_switch_ = false;
                            LOG_ERROR("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: previous=%s cannot be demoted",
                                previous_tag.data());
                            return;
                        }

                        // Existing TCP/UDP connections are bound to the old
                        // exchanger and cannot migrate to the new primary.
                        // Drop them before the old exchanger is disposed so
                        // subsequent packets establish on the new target.
                        if (NULLPTR != previous && previous != target) {
                            previous->ResetDataChannels();
                        }

                        // The target is always a newly opened primary
                        // exchanger for this path; it does not inherit the
                        // old primary's control/MUX session.
                        if (!target->SetPrimaryOutbound(true)) {
                            if (NULLPTR != previous && previous != target && !previous->IsPrimaryOutbound()) {
                                previous->SetPrimaryOutbound(true);
                            }
                            pending_outbound_.clear();
                            pending_outbound_deadline_ = 0;
                            pending_primary_switch_ = false;
                            LOG_ERROR("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: target=%s cannot be promoted to primary",
                                target_tag.data());
                            return;
                        }

                        auto target_old = outbound_exchangers_.find(target_tag);
                        if (target_old != outbound_exchangers_.end()) {
                            if (target_old->second != target && target_old->second != previous) {
                                replaced = target_old->second;
                            }
                            target_old->second = target;
                        }
                        else if (target_tag != "main") {
                            // Retain the route alias while the exchanger is
                            // serving as main so a later switch can promote
                            // it back without opening a new session.
                            outbound_exchangers_.emplace(target_tag, target);
                        }
                        auto main = outbound_exchangers_.find("main");
                        if (main != outbound_exchangers_.end()) main->second = target;
                        else outbound_exchangers_.emplace("main", target);

                        if (previous_tag != "main" && previous_tag != target_tag) {
                            auto old_alias = outbound_exchangers_.find(previous_tag);
                            if (old_alias != outbound_exchangers_.end() && old_alias->second == previous) {
                                outbound_exchangers_.erase(old_alias);
                            }
                        }

                        exchanger_ = target;
                        primary_outbound_ = target_tag;
                        active_outbound_ = "main";
                    }
                    else {
                        auto old = outbound_exchangers_.find(previous_tag);
                        if (old != outbound_exchangers_.end()) previous = old->second;
                        auto target_old = outbound_exchangers_.find(target_tag);
                        if (target_old != outbound_exchangers_.end()) {
                            replaced = target_old->second;
                            target_old->second = target;
                        }
                        else {
                            outbound_exchangers_.emplace(target_tag, target);
                        }
                        active_outbound_ = target_tag;
                        if (previous_tag != "main" && previous_tag != target_tag &&
                            !preserve_previous) {
                            outbound_exchangers_.erase(previous_tag);
                        }
                    }
                    pending_outbound_.clear();
                    pending_outbound_deadline_ = 0;
                    pending_primary_switch_ = false;
                    outbound_affinities_.clear();
                    // Cache the prefer-IPv4 flag of the now-active outbound so DNS
                    // hot paths (StripAAAADnsResponseIfIPv4Available and the
                    // DispatchLocalDnsQuery completion) can read it without locking.
                    // The write happens only under GetSynchronizedObject() here and in
                    // the constructor, so the atomic read never races a shared_ptr
                    // copy of exchanger_.
                    bool active_prefer_ipv4 = false;
                    if (NULLPTR != target) {
                        std::shared_ptr<ppp::configurations::AppConfiguration> active_configuration = target->GetConfiguration();
                        if (NULLPTR != active_configuration) {
                            active_prefer_ipv4 = active_configuration->udp.dns.prefer_ipv4;
                        }
                    }
                    prefer_ipv4_.store(active_prefer_ipv4);
                    // The outbound has just been switched (hot swap). Flush the DNS
                    // cache so answers learned through the previous outbound (e.g. a
                    // Japan exit) cannot be served to the new configuration (e.g. a
                    // Hong Kong exit). Stale answers would otherwise only be bounded
                    // by their TTL. Mirrors the Android set_app_configuration path.
                    ppp::net::asio::vdns::ClearCache();
                }

                if (primary_switch) {
                    if (!ApplyPrimaryClientConfiguration(target->GetConfiguration())) {
                        LOG_ERROR("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: primary client configuration was only partially applied, target=%s",
                            target_tag.data());
                    }
                    auto reconfigure_http_proxy = [this]() noexcept {
                        if (NULLPTR != http_proxy_) {
                            return http_proxy_->ReconfigureExchanger(exchanger_);
                        }
                        const auto configuration = GetConfiguration();
                        if (NULLPTR == configuration ||
                            configuration->client.http_proxy.port <= IPEndPoint::MinPort ||
                            configuration->client.http_proxy.port > IPEndPoint::MaxPort) {
                            return true;
                        }
                        auto proxy = NewHttpProxy(exchanger_);
                        if (NULLPTR == proxy || !proxy->Open()) {
                            if (NULLPTR != proxy) proxy->Dispose();
                            return false;
                        }
                        http_proxy_ = std::move(proxy);
                        return true;
                    };
                    auto reconfigure_socks_proxy = [this]() noexcept {
                        if (NULLPTR != socks_proxy_) {
                            return socks_proxy_->ReconfigureExchanger(exchanger_);
                        }
                        const auto configuration = GetConfiguration();
                        if (NULLPTR == configuration ||
                            configuration->client.socks_proxy.port <= IPEndPoint::MinPort ||
                            configuration->client.socks_proxy.port > IPEndPoint::MaxPort) {
                            return true;
                        }
                        auto proxy = NewSocksProxy(exchanger_);
                        if (NULLPTR == proxy || !proxy->Open()) {
                            if (NULLPTR != proxy) proxy->Dispose();
                            return false;
                        }
                        socks_proxy_ = std::move(proxy);
                        return true;
                    };
                    if (!reconfigure_http_proxy()) {
                        LOG_ERROR("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: HTTP proxy configuration could not be applied");
                    }
                    if (!reconfigure_socks_proxy()) {
                        LOG_ERROR("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: SOCKS proxy configuration could not be applied");
                    }
#if defined(_WIN32) || defined(_MACOS)
                    // Refresh the OS proxy only when it was enabled by the
                    // user at startup.  Reconfiguration may have changed the
                    // HTTP bind address or port, so the old system endpoint
                    // must not be left behind.
                    if (system_proxy_applied_) {
                        SetHttpProxyToSystemEnv();
                    }
#endif
                    if (NULLPTR != previous && previous != exchanger_) previous->Dispose();
#if defined(_WIN32)
                    // Rebind every tunnel DNS upstream to the new primary
                    // exchanger and re-register its datagram handler on that
                    // exchanger after the old primary is disposed.
                    RebindTunnelDnsUpstreams();
#endif
                }
                else if (NULLPTR != previous && previous_tag != target_tag) {
                    if (previous_tag == "main") previous->ResetDataChannels();
                    else if (!preserve_previous) previous->Dispose();
                }
                if (NULLPTR != replaced && replaced != exchanger_ && replaced != previous) replaced->Dispose();
                UpdateRemoteUri();
                if (primary_switch && NULLPTR != target) {
                    // Hot-switch replay: server extensions are normally received by
                    // the pending exchanger BEFORE this switch completes, and the
                    // first ApplyIPv6Assignment pass may have consulted the OLD
                    // outbound configuration (GetExchanger() still returned the
                    // previous primary) and taken the no-data-plane branch.  Now
                    // that exchanger_ points at the new primary, re-apply the
                    // cached assignment so the TUN always carries the new server's
                    // IPv6 route -- even if that server never re-sends extensions.
                    // The call is idempotent: identical state is skipped inside.
                    ApplyIPv6Assignment(target->GetInformationExtensions(), target);
                }
                LOG_INFO("VEthernetNetworkSwitcher::CompletePendingOutboundSwitch: previous=%s, active=%s, new_exchanger=1, forced=1",
                    previous_tag.data(), target_tag.data());
            }

            std::shared_ptr<VEthernetExchanger> VEthernetNetworkSwitcher::GetExchanger(
                const boost::asio::ip::address& destination) noexcept {
                auto get_active = [this]() noexcept -> std::shared_ptr<VEthernetExchanger> {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    ppp::string tag = active_outbound_.empty() ?
                        ppp::string("main") : active_outbound_;
                    auto selected = outbound_exchangers_.find(tag);
                    return selected != outbound_exchangers_.end() ?
                        selected->second : exchanger_;
                };
                if (!geo_rules_) {
#if defined(_ANDROID) || defined(_IPHONE)
                    if (IsBypassIpAddress(destination) || IsBypassIpAddress6(destination)) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, action=direct, selected_outbound=carrier, reason=ip_bypass_without_geo",
                            ppp::net::Ipep::ToAddressString<ppp::string>(destination).data());
                        return exchanger_;
                    }
#endif
                    std::shared_ptr<VEthernetExchanger> active = get_active();
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, selected_outbound=%s, reason=geo_rules_unavailable",
                        ppp::net::Ipep::ToAddressString<ppp::string>(destination).data(),
                        NULLPTR != active ? active->GetOutboundTag().data() : "none");
                    return active;
                }
                // A direct Geo rule must also work when there is only a main
                // exchanger.  The proxy-only path has no TUN route table to
                // perform this decision for us, so do it before the old
                // no-outbound fast path.
                if (ppp::net::IPEndPoint::IsInvalid(destination)) {
                    return get_active();
                }

                static constexpr uint64_t affinity_timeout = 300000;
                uint64_t now = ppp::threading::Executors::GetTickCount();
                ppp::string address_key = ppp::net::Ipep::ToAddressString<ppp::string>(destination);
                ppp::string tag;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    tag = final_outbound_.empty() ? ppp::string("main") : final_outbound_;
                }
                if (tag == "@active") {
                    std::shared_ptr<VEthernetExchanger> active = get_active();
                    if (NULLPTR != active) tag = active->GetOutboundTag();
                }
                auto decision = geo_rules_->MatchAddress(destination, now);
                if (decision.Matched()) {
                    if (decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct) {
                        // A current rule must override stale tunnel affinity. Existing
                        // connections already retain their exchanger, so this only
                        // affects new traffic for the destination.
                        LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, action=direct, selected_outbound=direct, rule=1, rule_priority=%llu",
                            address_key.data(), (unsigned long long)decision.priority);
#if defined(_ANDROID)
                        // Android's TUN captures the packet before the host routing
                        // table can bypass it. Keep the primary exchanger as a
                        // carrier so TCP/UDP reaches the existing Rinetd/direct UDP
                        // code, where IsBypassIpAddress() selects a protected
                        // physical-network socket. Returning null here drops the
                        // packet before that direct path can run.
                        return exchanger_;
#else
                        return NULLPTR;
#endif
                    }
                    if (!decision.outbound.empty()) {
                        tag = decision.outbound;
                        if (tag == "@active") {
                            std::shared_ptr<VEthernetExchanger> active = get_active();
                            if (NULLPTR != active) tag = active->GetOutboundTag();
                        }
                    }
                }
                else if (!outbound_configurations_.empty()) {
                    // Affinity is only a fallback after a learned DNS policy expires.
                    // It must never mask a newer domain or GeoIP decision.
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    auto affinity = outbound_affinities_.find(address_key);
                    if (affinity != outbound_affinities_.end()) {
                        if (affinity->second.expires_at > now) {
                            affinity->second.expires_at = now + affinity_timeout;
                            tag = affinity->second.tag;
                            if (tag == primary_outbound_) tag = "main";
                            auto selected = outbound_exchangers_.find(tag);
                            if (selected == outbound_exchangers_.end()) {
                                LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, requested_outbound=%s, selected_outbound=none, affinity=1, reason=outbound_not_found",
                                    address_key.data(), tag.data());
                                return NULLPTR;
                            }
                            LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, selected_outbound=%s, affinity=1",
                                address_key.data(), selected->second->GetOutboundTag().data());
                            return selected->second;
                        }
                        outbound_affinities_.erase(affinity);
                    }
                }
                if (tag == "direct") {
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, action=direct, selected_outbound=direct, rule=%d, rule_priority=%llu",
                        address_key.data(), (int)decision.Matched(), (unsigned long long)decision.priority);
                    return NULLPTR;
                }

                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    if (tag == primary_outbound_) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, requested_outbound=%s, selected_outbound=main, reason=primary_outbound_alias",
                            address_key.data(), tag.data());
                        tag = "main";
                    }
                }

                // A named GEO outbound may not have been opened yet. Start it
                // when the first matching flow arrives instead of preconnecting
                // every configuration at process startup.
                if (outbound_exchangers_.find(tag) == outbound_exchangers_.end()) {
                    EnsureOutbound(tag);
                }
                auto selected = outbound_exchangers_.find(tag);
                if (selected == outbound_exchangers_.end()) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, requested_outbound=%s, selected_outbound=none, rule=%d, reason=outbound_not_found",
                        address_key.data(), tag.data(), (int)decision.Matched());
                    return NULLPTR;
                }
                if (!outbound_configurations_.empty()) {
                    // Keep active raw TCP/UDP/ICMP traffic on the same keyed
                    // tunnel even if a learned DNS policy reaches its TTL.
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    outbound_affinities_[address_key] = OutboundAffinity{ tag, now + affinity_timeout };
                }
                if (decision.Matched()) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, action=tunnel, requested_outbound=%s, selected_outbound=%s, rule=1, rule_priority=%llu",
                        address_key.data(), tag.data(), selected->second->GetOutboundTag().data(), (unsigned long long)decision.priority);
                }
                else {
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: destination=%s, action=final, requested_outbound=%s, selected_outbound=%s, rule=0",
                        address_key.data(), tag.data(), selected->second->GetOutboundTag().data());
                }
                return selected->second;
            }

            std::shared_ptr<VEthernetExchanger> VEthernetNetworkSwitcher::GetExchanger(
                const ppp::string& hostname) noexcept {
                auto get_active = [this]() noexcept -> std::shared_ptr<VEthernetExchanger> {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    ppp::string tag = active_outbound_.empty() ?
                        ppp::string("main") : active_outbound_;
                    auto selected = outbound_exchangers_.find(tag);
                    return selected != outbound_exchangers_.end() ?
                        selected->second : exchanger_;
                };

                if (hostname.empty() || !geo_rules_) {
                    return get_active();
                }

                ppp::string tag;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    tag = final_outbound_.empty() ? ppp::string("main") : final_outbound_;
                }
                if (tag == "@active") {
                    auto active = get_active();
                    if (active) tag = active->GetOutboundTag();
                }

                const auto decision = geo_rules_->MatchDomain(hostname);
                if (decision.Matched() &&
                    decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: hostname=%s, action=direct, selected_outbound=direct, rule_priority=%llu",
                        hostname.data(), (unsigned long long)decision.priority);
                    return NULLPTR;
                }

                if (decision.Matched() && !decision.outbound.empty()) {
                    tag = decision.outbound;
                    if (tag == "@active") {
                        auto active = get_active();
                        if (active) tag = active->GetOutboundTag();
                    }
                }

                if (tag == "direct") {
                    return NULLPTR;
                }
                if (tag == primary_outbound_) tag = "main";
                if (outbound_exchangers_.empty() && tag == "main") {
                    return get_active();
                }
                EnsureOutbound(tag);
                auto selected = outbound_exchangers_.find(tag);
                if (selected != outbound_exchangers_.end()) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::GetExchanger: hostname=%s, action=tunnel, requested_outbound=%s, selected_outbound=%s, rule=%d, rule_priority=%llu",
                        hostname.data(), tag.data(), selected->second->GetOutboundTag().data(),
                        (int)decision.Matched(), (unsigned long long)decision.priority);
                    return selected->second;
                }
                return get_active();
            }

            bool VEthernetNetworkSwitcher::IsDirectProxyHost(const ppp::string& hostname) noexcept {
                if (hostname.empty() || !geo_rules_) {
                    return false;
                }
                auto decision = geo_rules_->MatchDomain(hostname);
                if (decision.Matched()) {
                    return decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                }
                SynchronizedObjectScope scope(GetSynchronizedObject());
                return final_outbound_ == "direct";
            }

            bool VEthernetNetworkSwitcher::IsDirectProxyAddress(
                const boost::asio::ip::address& address) noexcept {
                if (ppp::net::IPEndPoint::IsInvalid(address)) {
                    return false;
                }
                if (!geo_rules_) {
                    if (address.is_v4() && rib_) {
                        const uint32_t nip = htonl(address.to_v4().to_uint());
                        return ppp::net::native::ForwardInformationTable::GetNextHop(
                            nip, rib_->GetAllRoutes()) != ppp::net::IPEndPoint::NoneAddress;
                    }
                    if (address.is_v6() && rib6_) {
                        const auto target = address.to_v6().to_bytes();
                        for (const auto& route : *rib6_) {
                            if (route.Prefix < 0 || route.Prefix > 128) {
                                continue;
                            }
                            const auto network = route.Network.to_bytes();
                            const int whole_bytes = route.Prefix / 8;
                            const int remaining_bits = route.Prefix % 8;
                            if (!std::equal(target.begin(), target.begin() + whole_bytes, network.begin())) {
                                continue;
                            }
                            if (remaining_bits > 0) {
                                const uint8_t mask = static_cast<uint8_t>(0xffu << (8 - remaining_bits));
                                if ((target[whole_bytes] & mask) != (network[whole_bytes] & mask)) {
                                    continue;
                                }
                            }
                            return true;
                        }
                    }
                    return false;
                }
                auto decision = geo_rules_->MatchAddress(address,
                    ppp::threading::Executors::GetTickCount());
                if (decision.Matched()) {
                    return decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                }
                SynchronizedObjectScope scope(GetSynchronizedObject());
                return final_outbound_ == "direct";
            }

            std::shared_ptr<VEthernetExchanger> VEthernetNetworkSwitcher::NewExchanger() noexcept {
                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                return NewExchanger(configuration, "main", true);
            }

            std::shared_ptr<VEthernetExchanger> VEthernetNetworkSwitcher::NewExchanger(
                const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration,
                const ppp::string& tag, bool primary) noexcept {
                if (NULLPTR == configuration) {
                    return NULLPTR;
                }
                auto guid = StringAuxiliary::GuidStringToInt128(configuration->client.guid);
                if (guid == 0) {
                    return NULLPTR;
                }

                auto my = shared_from_this();
                auto self = std::dynamic_pointer_cast<VEthernetNetworkSwitcher>(my);
                return make_shared_object<VEthernetExchanger>(self, configuration, GetContext(), guid, tag, primary);
            }

            VEthernetNetworkSwitcher::VEthernetHttpProxySwitcherPtr VEthernetNetworkSwitcher::NewHttpProxy(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept {
                if (NULLPTR == exchanger) {
                    return NULLPTR;
                }
                else {
                    return make_shared_object<VEthernetHttpProxySwitcher>(exchanger);
                }
            }

            VEthernetNetworkSwitcher::VEthernetSocksProxySwitcherPtr VEthernetNetworkSwitcher::NewSocksProxy(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept {
                if (NULLPTR == exchanger) {
                    return NULLPTR;
                }
                else {
                    return make_shared_object<VEthernetSocksProxySwitcher>(exchanger);
                }
            }

            std::shared_ptr<ppp::threading::BufferswapAllocator> VEthernetNetworkSwitcher::GetBufferAllocator() noexcept {
                return configuration_->GetBufferAllocator();
            }

            bool VEthernetNetworkSwitcher::DatagramOutput(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, void* packet, int packet_size, bool caching) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (IsDisposed()) {
                    return false;
                }

                if (destinationEP.port() == PPP_DNS_SYS_PORT) {
                    ObserveGeoDnsResponse(packet, packet_size);
                }

                boost::asio::ip::udp::endpoint remoteEP = Ipep::V6ToV4(destinationEP);
                boost::asio::ip::address address = remoteEP.address();
                if (address.is_v4()) {
                    std::shared_ptr<BufferSegment> messages = make_shared_object<BufferSegment>();
                    if (NULLPTR == messages) {
                        return false;
                    }

                    messages->Buffer = wrap_shared_pointer(reinterpret_cast<Byte*>(packet));
                    messages->Length = packet_size;

                    std::shared_ptr<UdpFrame> frame = make_shared_object<UdpFrame>();
                    if (NULLPTR == frame) {
                        return false;
                    }

                    frame->AddressesFamily = AddressFamily::InterNetwork;
                    frame->Source = IPEndPoint::ToEndPoint(remoteEP);
                    frame->Destination = IPEndPoint::ToEndPoint(sourceEP);
                    frame->Payload = messages;

                    if (caching && configuration_->udp.dns.cache) {
                        int destinationPort = destinationEP.port();
                        if (destinationPort == PPP_DNS_SYS_PORT) {
                            // Prefer IPv4: filter AAAA before caching to keep cache clean.
                            // Returns false if AAAA needs deferral (A cache not yet populated).
                            ::dns::Message m;
                            if (m.decode(reinterpret_cast<uint8_t*>(packet), packet_size) == ::dns::BufferResult::NoError) {
                                if (!StripAAAADnsResponseIfIPv4Available(m)) {
                                    // Defer: AAAA arrived before A. Store and don't forward yet.
                                    auto pending = make_shared_object<PendingAAAAResponse>();
                                    if (pending) {
                                        pending->EncodedPacket.assign(reinterpret_cast<char*>(packet), packet_size);
                                        pending->IsIPv6 = false;
                                        pending->SourceEP = sourceEP;
                                        pending->DestinationEP = destinationEP;
                                        pending->expire_time = Executors::GetTickCount() + static_cast<uint64_t>(configuration_->udp.dns.timeout) * 1000;
                                        pending_aaaa_[ppp::string(m.questions[0].mName.data())] = pending;
                                    }
                                    return true; // Held, don't forward to client yet
                                }
                                // Re-encode (AAAA may have been stripped); new size <= original
                                size_t new_sz = 0;
                                if (m.encode(reinterpret_cast<uint8_t*>(packet), packet_size, new_sz) == ::dns::BufferResult::NoError && new_sz > 0) {
                                    packet_size = static_cast<int>(new_sz);
                                    messages->Length = packet_size;
                                }
                            }
                            ppp::net::asio::vdns::AddCache((Byte*)packet, packet_size);
                            FlushPendingAAAAResponses();
                        }
                    }

                    std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = GetBufferAllocator();
                    std::shared_ptr<IPFrame> ip = UdpFrame::ToIp(allocator, frame.get());
                    return Output(ip.get());
                }
                elif (address.is_v6()) {
                    boost::asio::ip::address_v6 dst_v6 = sourceEP.address().to_v6();
                    if (dst_v6.is_unspecified()) {
                        return false;
                    }

                    std::shared_ptr<UdpFrame> frame = make_shared_object<UdpFrame>();
                    if (NULLPTR == frame) {
                        return false;
                    }

                    frame->AddressesFamily = AddressFamily::InterNetworkV6;
                    frame->Source = IPEndPoint::ToEndPoint(remoteEP);
                    frame->Destination = IPEndPoint::ToEndPoint(sourceEP);

                    std::shared_ptr<BufferSegment> messages = make_shared_object<BufferSegment>();
                    if (NULLPTR == messages) {
                        return false;
                    }

                    messages->Buffer = wrap_shared_pointer(reinterpret_cast<Byte*>(packet));
                    messages->Length = packet_size;
                    frame->Payload = messages;

                    if (caching && configuration_->udp.dns.cache) {
                        int destinationPort = destinationEP.port();
                        if (destinationPort == PPP_DNS_SYS_PORT) {
                            // Prefer IPv4: filter AAAA before caching to keep cache clean.
                            // Returns false if AAAA needs deferral (A cache not yet populated).
                            ::dns::Message m;
                            if (m.decode(reinterpret_cast<uint8_t*>(packet), packet_size) == ::dns::BufferResult::NoError) {
                                if (!StripAAAADnsResponseIfIPv4Available(m)) {
                                    // Defer: AAAA arrived before A. Store and don't forward yet.
                                    auto pending = make_shared_object<PendingAAAAResponse>();
                                    if (pending) {
                                        pending->EncodedPacket.assign(reinterpret_cast<char*>(packet), packet_size);
                                        pending->IsIPv6 = true;
                                        pending->SrcV6 = sourceEP.address().to_v6();
                                        pending->DstV6 = dst_v6;
                                        pending->SrcPort = sourceEP.port();
                                        pending->DstPort = destinationEP.port();
                                        pending->expire_time = Executors::GetTickCount() + static_cast<uint64_t>(configuration_->udp.dns.timeout) * 1000;
                                        pending_aaaa_[ppp::string(m.questions[0].mName.data())] = pending;
                                    }
                                    return true; // Held, don't forward to client yet
                                }
                                // Re-encode (AAAA may have been stripped); new size <= original
                                size_t new_sz = 0;
                                if (m.encode(reinterpret_cast<uint8_t*>(packet), packet_size, new_sz) == ::dns::BufferResult::NoError && new_sz > 0) {
                                    packet_size = static_cast<int>(new_sz);
                                    messages->Length = packet_size;
                                }
                            }
                            ppp::net::asio::vdns::AddCache((Byte*)packet, packet_size);
                            FlushPendingAAAAResponses();
                        }
                    }

                    std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = GetBufferAllocator();
                    std::shared_ptr<BufferSegment> ip6 = UdpFrame::ToIp6(allocator, frame.get());
                    return Output(ip6->Buffer.get(), ip6->Length);
                }

                return false;
            }

            bool VEthernetNetworkSwitcher::OnInformation(const std::shared_ptr<VirtualEthernetInformation>& info) noexcept {
                std::shared_ptr<VEthernetExchanger> exchanger = exchanger_;
                if (NULLPTR == exchanger) {
                    return false;
                }

                std::shared_ptr<ppp::transmissions::ITransmissionQoS> qos = qos_;
                if (NULLPTR != qos) {
                    int64_t bandwidth = static_cast<int64_t>(info->BandwidthQoS) * (1024 >> 3); /* Kbps. */
                    qos->SetBandwidth(bandwidth);
                }

                // If the user still has the remaining incoming/outgoing traffic and the expiration time is not reached, 
                // The VPN link is regarded as successful. Otherwise, the VPN link needs to be disconnected.
                bool valid = info->Valid();
                LOG_DEBUG("VEthernetNetworkSwitcher::OnInformation: Valid=%d, IncomingTraffic=%llu, OutgoingTraffic=%llu, ExpiredTime=%u, now=%u, BandwidthQoS=%lld",
                    (int)valid,
                    (unsigned long long)info->IncomingTraffic,
                    (unsigned long long)info->OutgoingTraffic,
                    info->ExpiredTime,
                    (UInt32)time(NULLPTR),
                    (long long)info->BandwidthQoS);
                if (valid) {
                    return true;
                }

                // If the VPN link needs to be disconnected, the client requires the active end, and the server forcibly disconnects. 
                // This prevents you from bypassing the disconnection problem by modifying the code of the client switch.
                LOG_DEBUG("VEthernetNetworkSwitcher::OnInformation: Valid() returned false, disposing transmission!");
                std::shared_ptr<ppp::transmissions::ITransmission> transmission = exchanger->GetTransmission(); 
                if (NULLPTR != transmission) {
                    transmission->Dispose();
                }
                
                return false;
            }

            void VEthernetNetworkSwitcher::ApplyIPv6Assignment(const VirtualEthernetInformationExtensions& extensions, const std::shared_ptr<VEthernetExchanger>& source) noexcept {
                LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: entered, AssignedIPv6Mode=%d, AssignedIPv6Address=%s, AssignedIPv6Gateway=%s, AssignedIPv6Dns1=%s, AssignedIPv6Dns2=%s",
                    (int)extensions.AssignedIPv6Mode,
                    extensions.AssignedIPv6Address.is_v6() ? extensions.AssignedIPv6Address.to_string().c_str() : "(none)",
                    extensions.AssignedIPv6Gateway.is_v6() ? extensions.AssignedIPv6Gateway.to_string().c_str() : "(none)",
                    extensions.AssignedIPv6Dns1.is_v6() ? extensions.AssignedIPv6Dns1.to_string().c_str() : "(none)",
                    extensions.AssignedIPv6Dns2.is_v6() ? extensions.AssignedIPv6Dns2.to_string().c_str() : "(none)");

                // Proxy-only mode has no kernel TAP adapter, so IPv6 address
                // assignment and route setup (::/1 + 8000::/1) are meaningless.
                // The IPv6 server route (/128) is handled separately by
                // EnsureWindowsIPv6ServerRoute in OpenTransmission.
                if (IsProxyOnly()) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: proxy-only mode, skipping IPv6 assignment");
                    return;
                }

                // The server's Information extension is the only authoritative
                // capability signal.  A client profile can omit server.ipv6 even
                // when the remote server provisions IPv6.
                const bool has_ipv6_dataplane =
                    extensions.AssignedIPv6Mode == VirtualEthernetInformationExtensions::IPv6Mode_Nat66 ||
                    extensions.AssignedIPv6Mode == VirtualEthernetInformationExtensions::IPv6Mode_Gua;
                ipv6_server_has_dataplane_ = has_ipv6_dataplane;

                if (!has_ipv6_dataplane) {
                    // A reconnect can explicitly withdraw a previously assigned IPv6
                    // data plane.  Leaving the old TAP /1 routes or trusting the
                    // cached TAP gateway in that case lets Windows select the
                    // physical NIC's global IPv6 address again.
                    //
                    // No remote IPv6 data plane: remove stale managed state and
                    // keep the physical IPv6 default fail-closed.  Otherwise the
                    // host can bypass the tunnel as soon as an AAAA connection is
                    // opened.  Direct/bypass IPv6 prefixes remain handled by the
                    // normal geo route policy.
                    LOG_INFO("VEthernetNetworkSwitcher::ApplyIPv6Assignment: server has no IPv6 data plane, restoring leak block");
#if !defined(_ANDROID) && !defined(_IPHONE)
                    RestoreIPv6Assignment();
#endif
#if defined(_WIN32)
                    // Keep direct IPv6 decisions: after the physical ::/0 is
                    // suppressed, these more-specific routes are the intentional
                    // domestic/direct allow-list. Remove only stale tunnel-side
                    // decisions left by a previous managed IPv6 assignment.
                    ppp::vector<boost::asio::ip::address> dynamic_addresses;
                    dynamic_addresses.reserve(geo_dynamic_routes6_.size());
                    for (const auto& entry : geo_dynamic_routes6_) {
                        if (entry.second.interface_index !=
                            (underlying_ni_ ? underlying_ni_->Index : -1)) {
                            boost::system::error_code ec;
                            boost::asio::ip::address address = StringToAddress(entry.first.data(), ec);
                            if (!ec && address.is_v6()) dynamic_addresses.emplace_back(std::move(address));
                        }
                    }
                    for (const boost::asio::ip::address& address : dynamic_addresses) {
                        DeleteGeoDynamicRoute(address);
                    }
                    ApplyWindowsIPv6LeakBlockRoutes();
#endif
                    return;
                }

                std::shared_ptr<ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: GetTap() returned null, returning");
                    return;
                }

                // Build the client context using TAP interface info.
                ppp::ipv6::auxiliary::ClientContext ctx;
                ctx.Tap = tap.get();
                ctx.InterfaceIndex = tap->GetInterfaceIndex();

                // Obtain interface name -- prefer tun_ni_ Name if available.
                std::shared_ptr<NetworkInterface> tun_ni = GetTapNetworkInterface();
                if (NULLPTR != tun_ni && !tun_ni->Name.empty()) {
                    ctx.InterfaceName = tun_ni->Name;
                }
#if defined(_LINUX)
                else {
                    int dev_handle = (int)reinterpret_cast<std::intptr_t>(tap->GetHandle());
                    if (dev_handle != -1) {
                        ppp::tap::TapLinux::GetInterfaceName(dev_handle, ctx.InterfaceName);
                    }
                }
#elif defined(_MACOS)
                else {
                    int dev_handle = (int)reinterpret_cast<std::intptr_t>(tap->GetHandle());
                    if (dev_handle != -1) {
                        ppp::darwin::tun::utun_get_if_name(dev_handle, ctx.InterfaceName);
                    }
                }
#endif
                // Determine nat_mode from the extensions (server-assigned), not from local config.
                // The client config may not have a server.ipv6 section at all.
                bool nat_mode = (extensions.AssignedIPv6Mode == VirtualEthernetInformationExtensions::IPv6Mode_Nat66);
                int address_prefix_length = extensions.AssignedIPv6AddressPrefixLength > 0
                    ? extensions.AssignedIPv6AddressPrefixLength
                    : 64;

                // Capture the original host state exactly once. A stack-local snapshot
                // cannot be used during disconnect and was the reason managed IPv6
                // addresses, split default routes and DNS changes survived shutdown.
#if !defined(_ANDROID) && !defined(_IPHONE)
                bool assignment_changed = ipv6_client_state_captured_ &&
                    (ipv6_client_address_ != extensions.AssignedIPv6Address ||
                        ipv6_client_gateway_ != extensions.AssignedIPv6Gateway ||
                        ipv6_client_route_prefix_ != extensions.AssignedIPv6RoutePrefix ||
                        ipv6_client_prefix_length_ != address_prefix_length ||
                        ipv6_client_route_prefix_length_ != extensions.AssignedIPv6RoutePrefixLength ||
                        ipv6_client_nat_mode_ != nat_mode);
                if (assignment_changed) {
                    RestoreIPv6Assignment();
                }
                if (!ipv6_client_state_captured_) {
                    ipv6_client_state_.Clear();
                    ppp::ipv6::auxiliary::CaptureClientOriginalState(ctx, nat_mode, ipv6_client_state_);
                    ipv6_client_address_ = extensions.AssignedIPv6Address;
                    ipv6_client_gateway_ = extensions.AssignedIPv6Gateway;
                    ipv6_client_route_prefix_ = extensions.AssignedIPv6RoutePrefix;
                    ipv6_client_prefix_length_ = address_prefix_length;
                    ipv6_client_route_prefix_length_ = extensions.AssignedIPv6RoutePrefixLength;
                    ipv6_client_nat_mode_ = nat_mode;
                    ipv6_client_state_captured_ = true;
                }
                ppp::ipv6::auxiliary::ClientState& state = ipv6_client_state_;
#else
                ppp::ipv6::auxiliary::ClientState transient_state;
                ppp::ipv6::auxiliary::CaptureClientOriginalState(ctx, nat_mode, transient_state);
                ppp::ipv6::auxiliary::ClientState& state = transient_state;
#endif

                // 1. Apply the assigned IPv6 address.
                if (extensions.AssignedIPv6Address.is_v6() && !state.AddressApplied) {
                    bool gua_mode = (extensions.AssignedIPv6Mode == VirtualEthernetInformationExtensions::IPv6Mode_Gua);
                    LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: applying address=%s/%d gua=%d",
                        extensions.AssignedIPv6Address.to_string().c_str(),
                        address_prefix_length,
                        (int)gua_mode);
                    const bool applied = ppp::ipv6::auxiliary::ApplyClientAddress(
                        ctx,
                        extensions.AssignedIPv6Address,
                        address_prefix_length,
                        gua_mode,
                        state);

                    if (applied) {
                        // Update the tap object so the console display can show IPv6 info.
                        tap->IPv6Address = extensions.AssignedIPv6Address;
                    }
                    else {
                        // The kernel-side address could not be applied (e.g. an
                        // Android TUN whose addresses are fixed by
                        // VpnService.Builder and cannot be changed from native
                        // code without root).  Keep tap->IPv6Address pointing at
                        // the TUN's real configured address so
                        // VEthernetExchanger::TranslateIPv6Packet can translate
                        // between that address and the server-assigned lease.
                        LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: address not applied (rc=0), keeping tap->IPv6Address=%s",
                            tap->IPv6Address.is_v6() ? tap->IPv6Address.to_string().c_str() : "(none)");
                    }

                    // Pin server IPv6 route to the underlying physical NIC (avoid routing loop).
                    // OpenTransmission normally installs this before the first
                    // connection. Keep this call for compatibility with an already
                    // established transport created by an older startup path.
                    boost::asio::ip::address server_address;
                    std::shared_ptr<VEthernetExchanger> route_exchanger =
                        NULLPTR != source ? source : exchanger_;
                    if (route_exchanger) {
                        server_address = route_exchanger->server_url_.remoteEP.address();
                    }
                    if (server_address.is_v6() && !server_address.is_unspecified()) {
#if defined(_WIN32)
                        EnsureWindowsIPv6ServerRoute(server_address);
#else
                        std::shared_ptr<NetworkInterface> underlying_ni = GetUnderlyingNetworkInterface();
                        if (NULLPTR == underlying_ni) {
                            LOG_WARN("VEthernetNetworkSwitcher::ApplyIPv6Configuration: cannot pin server IPv6 route, GetUnderlyingNetworkInterface() returned null");
                        }
                        elif(underlying_ni->Name.empty()) {
                            LOG_WARN("VEthernetNetworkSwitcher::ApplyIPv6Configuration: cannot pin server IPv6 route, underlying_ni->Name is empty");
                        }
                        else {
                            std::string server_ip_str = server_address.to_string();
                            std::string gw6_str = underlying_ni->IPv6GatewayServer.to_string();
                            std::string ni_name_str = underlying_ni->Name.c_str();
                            if (!gw6_str.empty() && underlying_ni->IPv6GatewayServer.is_v6()) {
#if defined(_LINUX)
                                std::string cmd = "ip -6 route replace " + server_ip_str + "/128 via " + gw6_str + " dev " + ni_name_str;
                                int rc = system(cmd.c_str());
                                if (rc != 0) {
                                    LOG_ERROR("VEthernetNetworkSwitcher::ApplyIPv6Configuration: failed to pin server IPv6 route, cmd=\"%s\", rc=%d", cmd.c_str(), rc);
                                }
#elif defined(_MACOS)
                                std::string cmd = "route -n add -inet6 " + server_ip_str + "/128 " + gw6_str;
                                int rc = system(cmd.c_str());
                                if (rc != 0) {
                                    LOG_ERROR("VEthernetNetworkSwitcher::ApplyIPv6Configuration: failed to pin server IPv6 route, cmd=\"%s\", rc=%d", cmd.c_str(), rc);
                                }
#endif
                            }
                            else {
                                LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Configuration: skip pinning server IPv6 route, underlying interface has no IPv6 gateway, gw6_str=\"%s\", is_v6=%d",
                                    gw6_str.c_str(), (int)underlying_ni->IPv6GatewayServer.is_v6());
                            }
                        }
#endif
                    }
                }

                // 2. Apply split default IPv6 routes (::/1 + 8000::/1) via the assigned gateway.
                //    Same approach as IPv4's 0.0.0.0/1 + 128.0.0.0/1 — avoids overwriting
                //    any existing ::/0 on the physical NIC.
                if (extensions.AssignedIPv6Gateway.is_v6() && !state.DefaultRouteApplied) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: applying default route via gateway=%s nat_mode=%d",
                        extensions.AssignedIPv6Gateway.to_string().c_str(), (int)nat_mode);

#if defined(_WIN32)
                    bool had_block_routes = ipv6_block_routes_added_;
                    if (had_block_routes) {
                        RemoveWindowsIPv6LeakBlock();
                    }
#endif

                    bool route_applied = ppp::ipv6::auxiliary::ApplyClientDefaultRoute(
                        ctx, extensions.AssignedIPv6Gateway, nat_mode, state);

#if defined(_WIN32)
                    if (!route_applied && had_block_routes) {
                        const bool restored = ApplyWindowsIPv6LeakBlockRoutes();
                        LOG_ERROR("VEthernetNetworkSwitcher::ApplyIPv6Assignment: managed IPv6 route failed, leak block restored=%d",
                            static_cast<int>(restored));
                    }
#endif

                    // Update the tap object so the console display can show the IPv6 gateway.
                    if (route_applied) {
                        tap->IPv6GatewayServer = extensions.AssignedIPv6Gateway;
                    }
                }

                // 3. Apply an optional routed subnet prefix.
                if (extensions.AssignedIPv6RoutePrefix.is_v6() && extensions.AssignedIPv6RoutePrefixLength > 0 && !state.SubnetRouteApplied) {
                    ppp::ipv6::auxiliary::ApplyClientSubnetRoute(
                        ctx,
                        extensions.AssignedIPv6RoutePrefix,
                        extensions.AssignedIPv6RoutePrefixLength,
                        extensions.AssignedIPv6Gateway.is_v6()
                            ? extensions.AssignedIPv6Gateway
                            : boost::asio::ip::address(),
                        nat_mode,
                        state);
                }

                // 4. Remove all IPv6 addresses on the TAP interface except the assigned one.
                //    Windows generates temporary addresses (RFC 4941) for /64 prefixes, and
                //    stale addresses from previous sessions may persist with infinite lifetime.
                //    Any extra address causes source address mismatch with the server's NAT66
                //    /128 route, breaking return traffic (Echo Reply, DNS responses, etc.)
                //    because the server's FindIPv6Exchanger() lookup fails for unregistered addresses.
#if defined(_WIN32)
                ppp::win32::ipv6::auxiliary::DisableClientTemporaryAddress(ctx, extensions.AssignedIPv6Address);
#endif

                // 5. DNS: Windows RFC 6724 prefers IPv6 DNS over IPv4 DNS, which forces all
                //    queries through the IPv6 UDP path (OnIPv6UdpPacketInput). The IPv4
                //    path has mature dns-rules.txt redirection via RedirectDnsServer.
                //    To prioritize IPv4 DNS, we skip IPv6 DNS assignment on TAP so the
                //    system falls back to IPv4 DNS (set via DHCP / SetDnsAddresses).
                //    We still clear non-TAP NICs' IPv6 DNS to prevent DNS leaks from
                //    physical adapter IPv6 DNS servers (e.g., ISP RA/DHCPv6).
#if defined(_WIN32)
                if (!state.DnsApplied) {
                    ppp::vector<ppp::string> dns_servers; // intentionally empty - skip IPv6 DNS on TAP
                    // Clear IPv6 DNS on the TAP interface itself (in case a previous
                    // ApplyClientDns call left stale IPv6 DNS servers behind).
                    ppp::win32::network::ClearDnsAddressesV6(ctx.InterfaceIndex);
                    // Clear IPv6 DNS on all non-TAP NICs to prevent DNS leaks from
                    // physical adapter IPv6 DNS servers (e.g., ISP RA/DHCPv6).
                    for (auto& [if_index, servers] : state.OriginalAllDnsServers) {
                        if (if_index != ctx.InterfaceIndex && !servers.empty()) {
                            ppp::win32::network::ClearDnsAddressesV6(if_index);
                        }
                    }
                    state.DnsApplied = true;
                    state.DnsServers = std::move(dns_servers);
                    ppp::tap::TapWindows::DnsFlushResolverCache();
                    LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: cleared TAP + %d non-TAP NICs IPv6 DNS (prefer IPv4)",
                        (int)state.OriginalAllDnsServers.size());
                }
#else
                if (!state.DnsApplied) {
                    ppp::vector<ppp::string> dns_servers;
                    if (extensions.AssignedIPv6Dns1.is_v6()) {
                        dns_servers.emplace_back(extensions.AssignedIPv6Dns1.to_string());
                    }
                    if (extensions.AssignedIPv6Dns2.is_v6()) {
                        dns_servers.emplace_back(extensions.AssignedIPv6Dns2.to_string());
                    }
                    if (!dns_servers.empty()) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::ApplyIPv6Assignment: applying %d DNS servers", (int)dns_servers.size());
                        ppp::ipv6::auxiliary::ApplyClientDns(ctx, dns_servers, state);
                    }
                }
#endif
            }

#if !defined(_ANDROID) && !defined(_IPHONE)
            void VEthernetNetworkSwitcher::RestoreIPv6Assignment() noexcept {
                if (!ipv6_client_state_captured_) {
                    return;
                }

                std::shared_ptr<ITap> tap = GetTap();
                if (NULLPTR != tap) {
                    ppp::ipv6::auxiliary::ClientContext ctx;
                    ctx.Tap = tap.get();
                    ctx.InterfaceIndex = tap->GetInterfaceIndex();

                    std::shared_ptr<NetworkInterface> tun_ni = GetTapNetworkInterface();
                    if (NULLPTR != tun_ni && !tun_ni->Name.empty()) {
                        ctx.InterfaceName = tun_ni->Name;
                    }
#if defined(_LINUX)
                    else {
                        int dev_handle = (int)reinterpret_cast<std::intptr_t>(tap->GetHandle());
                        if (dev_handle != -1) {
                            ppp::tap::TapLinux::GetInterfaceName(dev_handle, ctx.InterfaceName);
                        }
                    }
#elif defined(_MACOS)
                    else {
                        int dev_handle = (int)reinterpret_cast<std::intptr_t>(tap->GetHandle());
                        if (dev_handle != -1) {
                            ppp::darwin::tun::utun_get_if_name(dev_handle, ctx.InterfaceName);
                        }
                    }
#endif

                    LOG_INFO("VEthernetNetworkSwitcher::RestoreIPv6Assignment: restoring interface=%d, address=%s/%d",
                        ctx.InterfaceIndex,
                        ipv6_client_address_.is_v6() ? ipv6_client_address_.to_string().c_str() : "(none)",
                        ipv6_client_prefix_length_);
                    ppp::ipv6::auxiliary::RestoreClientConfiguration(
                        ctx,
                        ipv6_client_address_,
                        ipv6_client_prefix_length_,
                        ipv6_client_nat_mode_,
                        ipv6_client_state_);

                    tap->IPv6Address = boost::asio::ip::address();
                    tap->IPv6GatewayServer = boost::asio::ip::address();
                }

                ipv6_client_state_.Clear();
                ipv6_client_address_ = boost::asio::ip::address();
                ipv6_client_gateway_ = boost::asio::ip::address();
                ipv6_client_route_prefix_ = boost::asio::ip::address();
                ipv6_client_prefix_length_ = 0;
                ipv6_client_route_prefix_length_ = 0;
                ipv6_client_nat_mode_ = false;
                ipv6_client_state_captured_ = false;
            }
#endif

#if defined(_WIN32)
            VEthernetNetworkSwitcher::PaperAirplaneControllerPtr VEthernetNetworkSwitcher::NewPaperAirplaneController() noexcept {
                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger();
                if (NULLPTR == exchanger) {
                    return NULLPTR;
                }
                else {
                    return make_shared_object<PaperAirplaneController>(exchanger);
                }
            }
#elif defined(_LINUX)
            VEthernetNetworkSwitcher::ProtectorNetworkPtr VEthernetNetworkSwitcher::NewProtectorNetwork() noexcept {
#if defined(_ANDROID)
                // Embedding the so framework into the Android platform does not use sendfd/recvfd unix to share fd across processes, 
                // So you cannot pass in network cards or unix path names.
                ppp::string dev;
                return make_shared_object<ProtectorNetwork>(dev);
#else
                std::shared_ptr<NetworkInterface> ni = GetUnderlyingNetworkInterface();
                if (NULLPTR == ni) {
                    return NULLPTR;
                }

                return make_shared_object<ProtectorNetwork>(ni->Name);
#endif
            }
#endif

            std::shared_ptr<VEthernetNetworkSwitcher::VirtualEthernetInformation> VEthernetNetworkSwitcher::GetInformation() noexcept {
                std::shared_ptr<VEthernetExchanger> exchanger = exchanger_;
                if (NULLPTR == exchanger) {
                    return NULLPTR;
                }

                return exchanger->GetInformation();
            }
            
            VEthernetNetworkSwitcher::ITransmissionStatisticsPtr VEthernetNetworkSwitcher::NewStatistics() noexcept {
                return make_shared_object<ITransmissionStatistics>();
            }

#if defined(_WIN32)
            static std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> Windows_GetNetworkInterface(const ppp::win32::network::AdapterInterfacePtr& ai, const ppp::win32::network::NetworkInterfacePtr& ni) noexcept {
                if (NULLPTR == ai || NULLPTR == ni) {
                    return NULLPTR;
                }

                std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> result = make_shared_object<VEthernetNetworkSwitcher::NetworkInterface>();
                if (NULLPTR == result) {
                    return NULLPTR;
                }

                boost::system::error_code ec;
                result->Id = ni->Guid;
                result->Index = ai->IfIndex;
                result->Name = ni->ConnectionId;
                result->Description = ni->Description;
                Ipep::StringsTransformToAddresses(ni->DnsAddresses, result->DnsAddresses);

                result->IPAddress = StringToAddress(ai->Address.data(), ec);
                result->SubmaskAddress = StringToAddress(ai->Mask.data(), ec);
                result->GatewayServer = StringToAddress(ai->GatewayServer.data(), ec);
                return result;
            }

            static std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> Windows_GetNetworkInterface(const ppp::win32::network::AdapterInterfacePtr& ai) noexcept {
                if (NULLPTR == ai) {
                    return NULLPTR;
                }

                auto ni = ppp::win32::network::GetNetworkInterfaceByInterfaceIndex(ai->IfIndex);
                return Windows_GetNetworkInterface(ai, ni);
            }

            static std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> Windows_GetTapNetworkInterface(const std::shared_ptr<VEthernetNetworkSwitcher::ITap>& tap) noexcept {
                int interface_index = tap->GetInterfaceIndex();
                if (interface_index == -1) {
                    return NULLPTR;
                }

                ppp::vector<ppp::win32::network::AdapterInterfacePtr> interfaces;
                if (ppp::win32::network::GetAllAdapterInterfaces(interfaces)) {
                    for (auto&& ai : interfaces) {
                        if (ai->IfIndex == interface_index) {
                            return Windows_GetNetworkInterface(ai);
                        }
                    }
                }

                return NULLPTR;
            }

            static std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> Windows_GetUnderlyingNetowrkInterface(const std::shared_ptr<VEthernetNetworkSwitcher::ITap>& tap, const ppp::string& nic) noexcept {
                auto [ai, ni] = ppp::win32::network::GetUnderlyingNetowrkInterface2(tap->GetId(), nic);
                auto result = Windows_GetNetworkInterface(ai, ni);
                if (NULLPTR != result) {
                    // Detect IPv6 default gateway
                    boost::asio::ip::address gw6;
                    int ifindex = -1;
                    if (ppp::win32::network::GetIPv6DefaultGateway(gw6, ifindex)) {
                        result->IPv6GatewayServer = gw6;
                    }
                }
                return result;
            }
#elif !defined(_ANDROID) && !defined(_IPHONE)
            class UnixNetworkInterface final : public VEthernetNetworkSwitcher::NetworkInterface {
            public:
                ppp::string DnsResolveConfiguration;

            public:
                static bool SetDnsResolveConfiguration(const std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface>& underlying_ni) noexcept {
                    if (NULLPTR == underlying_ni) {
                        return false;
                    }

                    UnixNetworkInterface* ni = dynamic_cast<UnixNetworkInterface*>(underlying_ni.get());
                    if (NULLPTR == ni) {
                        return false;
                    }

                    return ppp::unix__::UnixAfx::SetDnsResolveConfiguration(ni->DnsResolveConfiguration);
                }
            };

#if defined(_LINUX)
            static ppp::function<ppp::string(ppp::net::native::RouteEntry&)> Linux_GetNetworkInterfaceName(
                const std::shared_ptr<ppp::tap::ITap>&                              tap_if,
                const std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface>&  tap_ni,
                const std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface>&  underlying_ni,
                ppp::unordered_map<uint32_t, ppp::string>&                          nics) noexcept {

                auto f = 
                    [tap_if, tap_ni, underlying_ni, &nics](ppp::net::native::RouteEntry& entry) noexcept {
                        if (entry.NextHop == tap_if->GatewayServer) {
                            return tap_ni->Name;
                        }
                        
                        ppp::string nic;
                        if (Dictionary::TryGetValue(nics, entry.NextHop, nic)) {
                            if (!nic.empty()) {
                                return nic;
                            }
                        }

                        return underlying_ni->Name;
                    };
                return f;
            }
#endif

            static std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> Unix_GetTapNetworkInterface(const std::shared_ptr<VEthernetNetworkSwitcher::ITap>& tap) noexcept {
                int interface_index = tap->GetInterfaceIndex();
                if (interface_index == -1) {
                    return NULLPTR;
                }

                int dev_handle = (int)reinterpret_cast<std::intptr_t>(tap->GetHandle());
                if (dev_handle == -1) {
                    return NULLPTR;
                }

                ppp::string interface_name;
#if defined(_MACOS)
                if (!ppp::darwin::tun::utun_get_if_name(dev_handle, interface_name)) {
                    return NULLPTR;
                }
#else
                if (!ppp::tap::TapLinux::GetInterfaceName(dev_handle, interface_name)) {
                    return NULLPTR;
                }
#endif

                std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> ni = make_shared_object<VEthernetNetworkSwitcher::NetworkInterface>();
                if (NULLPTR == ni) {
                    return NULLPTR;
                }

                ni->Index = interface_index;
                ni->Name = interface_name;
                ni->GatewayServer = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(tap->GatewayServer, IPEndPoint::MinPort)).address();
                ni->IPAddress = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(tap->IPAddress, IPEndPoint::MinPort)).address();
                ni->SubmaskAddress = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(tap->SubmaskAddress, IPEndPoint::MinPort)).address();

#if defined(_MACOS)
                ppp::tap::TapDarwin* darwin_tap = dynamic_cast<ppp::tap::TapDarwin*>(tap.get()); 
                if (NULLPTR != darwin_tap) {
                    ni->DnsAddresses = darwin_tap->GetDnsAddresses();
                }
#else
                ppp::tap::TapLinux* linux_tap = dynamic_cast<ppp::tap::TapLinux*>(tap.get()); 
                ni->Id = ppp::tap::TapLinux::GetDeviceId(interface_name);

                if (NULLPTR != linux_tap) {
                    ni->DnsAddresses = linux_tap->GetDnsAddresses();
                }
#endif
                return ni;
            }

            static std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> Unix_GetUnderlyingNetowrkInterface(const std::shared_ptr<VEthernetNetworkSwitcher::ITap>& tap, const ppp::string& nic) noexcept {
                std::shared_ptr<UnixNetworkInterface> ni = make_shared_object<UnixNetworkInterface>();
                if (NULLPTR == ni) {
                    return NULLPTR;
                }

#if defined(_MACOS)
                using NetworkInterface = ppp::tap::TapDarwin::NetworkInterface;

                ppp::vector<NetworkInterface::Ptr> network_interfaces;
                if (!ppp::tap::TapDarwin::GetAllNetworkInterfaces(network_interfaces)) {
                    return NULLPTR;
                }

                NetworkInterface::Ptr network_interface = ppp::tap::TapDarwin::GetPreferredNetworkInterface2(network_interfaces, nic);
                if (NULLPTR == network_interface) {
                    return NULLPTR;
                }

                ni->Index = network_interface->Index;
                ni->Name = network_interface->Name;

                struct {
                    boost::asio::ip::address* address;
                    ppp::string* address_string;
                } addresses[] = {{&ni->GatewayServer, &network_interface->GatewayServer},
                    {&ni->IPAddress, &network_interface->IPAddress}, {&ni->SubmaskAddress, &network_interface->SubnetmaskAddress}};

                for (int i = 0; i < arraysizeof(addresses); i++) {
                    auto& r = addresses[i];
                    ppp::string* address_string = r.address_string;
                    if (address_string->empty()) {
                        continue;
                    }

                    boost::system::error_code ec;
                    *r.address = StringToAddress(address_string->data(), ec);
                    if (ec) {
                        return NULLPTR;
                    }
                }

                ni->DefaultRoutes = std::move(network_interface->GatewayAddresses);

                // Detect IPv6 default gateway
                {
                    ppp::string ifname6, gw6_str;
                    ppp::darwin::ipv6::auxiliary::ReadPrimaryDefaultRoute(ifname6, gw6_str);
                    if (!gw6_str.empty()) {
                        boost::system::error_code ec;
                        boost::asio::ip::address gw6 = StringToAddress(gw6_str.data(), ec);
                        if (!ec && gw6.is_v6()) {
                            ni->IPv6GatewayServer = gw6;
                        }
                    }
                }
#else
                ppp::string interface_name;
                ppp::UInt32 ip, gw, mask;
                if (!ppp::tap::TapLinux::GetPreferredNetworkInterface(interface_name, ip, mask, gw, nic)) {
                    return NULLPTR;
                }

                ni->Id = ppp::tap::TapLinux::GetDeviceId(interface_name);
                ni->Index = ppp::tap::TapLinux::GetInterfaceIndex(interface_name);
                ni->Name = interface_name;
                ni->GatewayServer = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(gw, IPEndPoint::MinPort)).address();
                ni->IPAddress = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(ip, IPEndPoint::MinPort)).address();
                ni->SubmaskAddress = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(mask, IPEndPoint::MinPort)).address();

                // Detect IPv6 default gateway
                {
                    ppp::string ifname6;
                    boost::asio::ip::address gw6;
                    if (ppp::tap::TapLinux::GetDefaultGateway6(ifname6, gw6)) {
                        ni->IPv6GatewayServer = gw6;
                    }
                }
#endif

                ni->DnsResolveConfiguration = ppp::unix__::UnixAfx::GetDnsResolveConfiguration();
                ppp::unix__::UnixAfx::GetDnsAddresses(ni->DnsResolveConfiguration, ni->DnsAddresses);
                return ni;
            }
#endif

            bool VEthernetNetworkSwitcher::BlockQUIC(bool value) noexcept {
                // Set the status of the current VPN client switcher that needs to block QUIC traffic flags.
                block_quic_ = value;
                return true;
            }

            bool VEthernetNetworkSwitcher::ProxyOnly(bool* value) noexcept {
                bool previous = proxy_only_;
                if (NULLPTR != value) {
                    proxy_only_forced_ = *value !=
                        (NULLPTR != base_configuration_ && base_configuration_->client.proxy_only);
                    proxy_only_ = *value;
                }
                return previous;
            }

            bool VEthernetNetworkSwitcher::ProxyIpRules(bool* value) noexcept {
                bool previous = proxy_ip_rules_;
                if (NULLPTR != value) {
                    proxy_ip_rules_ = *value;
                }
                return previous;
            }

#if defined(_WIN32)
            bool VEthernetNetworkSwitcher::LocalDns(bool* value) noexcept {
                bool previous = local_dns_enabled_;
                if (NULLPTR != value) {
                    local_dns_enabled_ = *value;
                }
                return previous;
            }

            boost::asio::ip::address VEthernetNetworkSwitcher::ResolveProxyDomainThroughTunnel(
                const ppp::string& hostname, ppp::coroutines::YieldContext& y) noexcept {
                if (!proxy_only_ || hostname.empty() || !y) {
                    return boost::asio::ip::address();
                }

                ::dns::Message message;
                static std::atomic<uint16_t> query_id{ 0 };
                uint16_t id = ++query_id;
                if (id == 0) {
                    id = ++query_id;
                }
                message.mRD = 1;
                message.mId = id;
                message.questions.emplace_back(::dns::QuestionSection(
                    stl::transform<std::string>(hostname),
                    ::dns::RecordType::kA,
                    ::dns::RecordClass::kIN));

                auto query = make_shared_object<ppp::string>();
                if (NULLPTR == query) {
                    return boost::asio::ip::address();
                }
                query->resize(PPP_MAX_DNS_PACKET_BUFFER_SIZE);
                std::size_t query_size = 0;
                if (message.encode(&(*query)[0], query->size(), query_size) !=
                    ::dns::BufferResult::NoError || query_size == 0) {
                    return boost::asio::ip::address();
                }
                query->resize(query_size);

                struct ResolveState final {
                    std::atomic<bool> completed{ false };
                    boost::asio::ip::address address;
                };
                auto state = make_shared_object<ResolveState>();
                if (NULLPTR == state) {
                    return boost::asio::ip::address();
                }

                DispatchLocalDnsQuery(query, false,
                    [state, &y, hostname](const std::shared_ptr<ppp::string>& response) noexcept {
                        boost::asio::ip::address address;
                        if (response && !response->empty()) {
                            ::dns::Message answer;
                            if (answer.decode(
                                    reinterpret_cast<const uint8_t*>(response->data()),
                                    response->size()) == ::dns::BufferResult::NoError &&
                                answer.mQr == 1 &&
                                answer.mRCode == static_cast<uint16_t>(::dns::ResponseCode::kNOERROR)) {
                                for (::dns::ResourceRecord& record : answer.answers) {
                                    if (record.mClass != ::dns::RecordClass::kIN ||
                                        record.mType != ::dns::RecordType::kA) {
                                        continue;
                                    }
                                    auto rdata = record.getRData<::dns::RDataA>();
                                    if (NULLPTR == rdata) {
                                        continue;
                                    }
                                    IPEndPoint endpoint(
                                        AddressFamily::InterNetwork,
                                        rdata->getAddress(), 4,
                                        IPEndPoint::MinPort);
                                    address = IPEndPoint::ToEndPoint<boost::asio::ip::udp>(endpoint).address();
                                    if (address.is_v4() && !IPEndPoint::IsInvalid(address)) {
                                        break;
                                    }
                                    address = boost::asio::ip::address();
                                }
                            }
                        }

                        if (!state->completed.exchange(true)) {
                            state->address = address;
                            LOG_DEBUG("VEthernetNetworkSwitcher::ResolveProxyDomainThroughTunnel: host=%s, address=%s",
                                hostname.data(),
                                address.is_unspecified() ? "(none)" : address.to_string().data());
                            y.R();
                        }
                    });

                y.Suspend();
                return state->address;
            }
#endif

#if defined(_WIN32) || defined(_MACOS)
            bool VEthernetNetworkSwitcher::SetHttpProxyToSystemEnv() noexcept {
                auto http_proxy = GetHttpProxy();
                if (NULLPTR == http_proxy) {
                    return ClearHttpProxyToSystemEnv();
                }

                boost::asio::ip::tcp::endpoint localEP = http_proxy->GetLocalEndPoint();
                int localPort = localEP.port();
                if (localPort <= IPEndPoint::MinPort || localPort > IPEndPoint::MaxPort) {
                    return ClearHttpProxyToSystemEnv();
                }

                boost::asio::ip::address localIP = localEP.address();
                if (IPEndPoint::IsInvalid(localIP) || localIP.is_unspecified()) {
                    localIP = boost::asio::ip::address_v4::loopback();
                }

                // Use the actual listener address.  The configured proxy can
                // legitimately bind to a LAN address (or IPv6 loopback), so a
                // hard-coded 127.0.0.1 would make the system proxy unusable.
                ppp::string host = ppp::net::Ipep::ToAddressString<ppp::string>(localIP);
                ppp::string server = localIP.is_v6() ? "[" + host + "]:" : host + ":";
                server += stl::to_string<ppp::string>(localPort);
                if (system_proxy_applied_ && system_proxy_server_ == server) {
                    return true;
                }
                ppp::string pac;
                bool bok = ppp::net::proxies::HttpProxy::SetSystemProxy(server, pac, true) &&
                    ppp::net::proxies::HttpProxy::SetSystemProxy(server) &&
                    ppp::net::proxies::HttpProxy::RefreshSystemProxy();
                if (!bok) {
                    ClearHttpProxyToSystemEnv();
                    return false;
                }

                system_proxy_server_ = server;
                system_proxy_applied_ = true;
                return bok;
            }

            bool VEthernetNetworkSwitcher::ClearHttpProxyToSystemEnv() noexcept {
                ppp::string server;
                ppp::string pac;
                bool result = ppp::net::proxies::HttpProxy::SetSystemProxy(server, pac, false);
                system_proxy_server_.clear();
                system_proxy_applied_ = false;
                return result;
            }

#endif

#if defined(_WIN32)
            ppp::vector<boost::asio::ip::address> VEthernetNetworkSwitcher::SelectLocalDnsServers(const void* packet, int packet_size) noexcept {
                ppp::vector<boost::asio::ip::address> result;
                ::dns::Message message;
                if (NULLPTR == packet || packet_size < 1 ||
                    message.decode(reinterpret_cast<const uint8_t*>(packet), packet_size) != ::dns::BufferResult::NoError ||
                    message.questions.empty()) {
                    return result;
                }

                const ppp::string hostname = stl::transform<ppp::string>(message.questions[0].mName);
                if (geo_rules_) {
                    const auto decision = geo_rules_->MatchDomain(hostname);
                    if (decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct) {
                        for (const auto& server : direct_dns_servers_) {
                            if (!IPEndPoint::IsInvalid(server) && !server.is_loopback() &&
                                std::find(result.begin(), result.end(), server) == result.end()) {
                                result.emplace_back(server);
                            }
                        }
                        if (!result.empty()) {
                            LOG_DEBUG("VEthernetNetworkSwitcher::SelectLocalDnsServers: geo direct DNS, host=%s, servers=%llu",
                                hostname.data(), (unsigned long long)result.size());
                            return result;
                        }
                    }
                }

                DNSRulePtr rule = ppp::app::client::dns::Rule::Get(hostname,
                    dns_ruless_[0], dns_ruless_[1], dns_ruless_[2]);
                if (rule && !IPEndPoint::IsInvalid(rule->Server) && !rule->Server.is_loopback()) {
                    result.emplace_back(rule->Server);
                    return result;
                }

                if (auto tun = tun_ni_; NULLPTR != tun) {
                    for (const boost::asio::ip::address& address : tun->DnsAddresses) {
                        if (!IPEndPoint::IsInvalid(address) && !address.is_loopback() &&
                            std::find(result.begin(), result.end(), address) == result.end()) {
                            result.emplace_back(address);
                        }
                    }
                }
                else if (proxy_only_) {
                    auto stub = std::dynamic_pointer_cast<ppp::tap::TapStub>(GetTap());
                    if (NULLPTR != stub) {
                        for (const boost::asio::ip::address& address : stub->GetDnsAddresses()) {
                            if (!IPEndPoint::IsInvalid(address) && !address.is_loopback() &&
                                std::find(result.begin(), result.end(), address) == result.end()) {
                                result.emplace_back(address);
                            }
                        }
                    }
                }
                return result;
            }

            bool VEthernetNetworkSwitcher::RegisterTunnelDnsHandler(
                const std::shared_ptr<LocalDnsUpstream>& upstream) noexcept {
                if (NULLPTR == upstream || NULLPTR == upstream->exchanger) {
                    return false;
                }
                auto weak_self = std::weak_ptr<VEthernetNetworkSwitcher>(
                    std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this()));
                auto weak_upstream = std::weak_ptr<LocalDnsUpstream>(upstream);
                return upstream->exchanger->RegisterDatagramHandler(upstream->tunnel_source,
                    [weak_self, weak_upstream](const boost::asio::ip::udp::endpoint&,
                        const boost::asio::ip::udp::endpoint& destination, void* packet, int packet_length) noexcept {
                        auto self = weak_self.lock();
                        auto current = weak_upstream.lock();
                        if (NULLPTR == self || NULLPTR == current) {
                            return true;
                        }
                        if (NULLPTR == packet || packet_length < 2 ||
                            destination.address() != current->server ||
                            destination.port() != PPP_DNS_SYS_PORT) {
                            return true;
                        }
                        const Byte* bytes = static_cast<const Byte*>(packet);
                        const uint16_t id = static_cast<uint16_t>(
                            (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
                        ppp::function<void(const std::shared_ptr<ppp::string>&)> response_callback;
                        {
                            SynchronizedObjectScope response_scope(self->GetSynchronizedObject());
                            auto request = current->requests.find(id);
                            if (request != current->requests.end()) {
                                response_callback = std::move(request->second);
                                current->requests.erase(request);
                            }
                        }
                        if (response_callback) {
                            response_callback(make_shared_object<ppp::string>(
                                reinterpret_cast<const char*>(packet), packet_length));
                        }
                        return true;
                    });
            }

            void VEthernetNetworkSwitcher::RebindTunnelDnsUpstreams() noexcept {
                auto current = exchanger_;
                if (NULLPTR == current) {
                    return;
                }
                ppp::vector<std::shared_ptr<LocalDnsUpstream>> upstreams;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    for (const auto& [key, upstream] : local_dns_upstreams_) {
                        (void)key;
                        if (NULLPTR == upstream || !upstream->through_tunnel) {
                            continue;
                        }
                        if (upstream->exchanger == current) {
                            continue;
                        }
                        upstreams.emplace_back(upstream);
                    }
                }
                for (const auto& upstream : upstreams) {
                    if (NULLPTR == upstream) continue;
                    auto old = upstream->exchanger;
                    if (NULLPTR != old && old != current) {
                        old->ReleaseDatagramHandler(upstream->tunnel_source);
                    }
                    upstream->exchanger = current;
                    bool registered = RegisterTunnelDnsHandler(upstream);
                    if (!registered) {
                        // Port collision on the fresh exchanger is unlikely, but
                        // fall back to allocating another synthetic source port.
                        auto tap = GetTap();
                        if (NULLPTR != tap) {
                            const boost::asio::ip::address source_address = Ipep::ToAddress(tap->GatewayServer);
                            for (int i = 0; i < 16384 && !registered; ++i) {
                                if (++local_dns_tunnel_port_ < 20000 || local_dns_tunnel_port_ > 29999) {
                                    local_dns_tunnel_port_ = 20000;
                                }
                                upstream->tunnel_source = boost::asio::ip::udp::endpoint(
                                    source_address, local_dns_tunnel_port_);
                                registered = RegisterTunnelDnsHandler(upstream);
                            }
                        }
                    }
                    if (!registered) {
                        LOG_WARN("VEthernetNetworkSwitcher::RebindTunnelDnsUpstreams: cannot rebind tunnel DNS handler, server=%s",
                            upstream->server.to_string().data());
                    }
                }
            }

            bool VEthernetNetworkSwitcher::SendLocalDnsUdp(
                const boost::asio::ip::address& server,
                const std::shared_ptr<ppp::string>& query,
                const ppp::function<void(const std::shared_ptr<ppp::string>&)>& callback,
                bool through_tunnel, ppp::string& upstream_key, uint16_t& upstream_id) noexcept {
                if (NULLPTR == query || query->size() < 2 || !callback ||
                    IPEndPoint::IsInvalid(server) || server.is_loopback()) {
                    return false;
                }

                auto context = GetContext();
                if (NULLPTR == context) return false;
                upstream_key = through_tunnel ? "tunnel:" : "direct:";
                upstream_key += server.to_string();
                std::shared_ptr<LocalDnsUpstream> upstream;
                bool start_receive = false;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    auto existing = local_dns_upstreams_.find(upstream_key);
                    if (existing != local_dns_upstreams_.end()) {
                        upstream = existing->second;
                    }
                    else {
                        upstream = make_shared_object<LocalDnsUpstream>();
                        upstream->server = server;
                        upstream->through_tunnel = through_tunnel;
                        if (through_tunnel) {
                            // Main DNS must enter the selected VPN exchanger directly.
                            // Sending an OS UDP socket to the public resolver makes the
                            // packet loop through Wintun and was the source of long-lived
                            // resolver loss on Windows.
                            if (!server.is_v4() || NULLPTR == exchanger_) {
                                return false;
                            }
                            auto tap = GetTap();
                            if (NULLPTR == tap) {
                                return false;
                            }
                            upstream->exchanger = exchanger_;
                            // Use the virtual gateway as a synthetic source.  No host
                            // application can own this address, so the exchanger's
                            // handler cannot collide with an ordinary Windows UDP flow.
                            const boost::asio::ip::address source_address = Ipep::ToAddress(tap->GatewayServer);
                            bool registered = false;
                            for (int i = 0; i < 16384 && !registered; ++i) {
                                if (++local_dns_tunnel_port_ < 20000 || local_dns_tunnel_port_ > 29999) {
                                    local_dns_tunnel_port_ = 20000;
                                }
                                upstream->tunnel_source = boost::asio::ip::udp::endpoint(
                                    source_address, local_dns_tunnel_port_);
                                registered = RegisterTunnelDnsHandler(upstream);
                            }
                            if (!registered) {
                                LOG_WARN("VEthernetNetworkSwitcher::SendLocalDnsUdp: cannot allocate tunnel DNS source, server=%s",
                                    server.to_string().data());
                                return false;
                            }
                        }
                        else {
                            upstream->socket = make_shared_object<boost::asio::ip::udp::socket>(*context);
                            boost::system::error_code ec;
                            upstream->socket->open(server.is_v4() ? boost::asio::ip::udp::v4() : boost::asio::ip::udp::v6(), ec);
#if defined(_ANDROID)
                            // VpnService routes 0.0.0.0/0 through the TUN. A
                            // direct DNS socket that is not protect()ed sends
                            // its query back into the tunnel where the native
                            // layer re-dispatches it as yet another DNS query,
                            // so the domestic resolver (e.g. 223.5.5.5) never
                            // sees it and the query eventually times out.
                            //
                            // VpnService.protect() must run BEFORE connect():
                            // connect() performs the route lookup and pins the
                            // socket to the tun0 route, so a protect() issued
                            // after connect() only toggles a mark that is never
                            // re-applied - the query keeps looping into the
                            // tunnel. Protect right after open() so the route
                            // decision in connect() picks the physical network.
                            if (!ec) {
                                auto protector_network = GetProtectorNetwork();
                                if (NULLPTR != protector_network) {
                                    protector_network->Protect(upstream->socket->native_handle());
                                }
                            }
#endif
                            if (!ec) upstream->socket->connect({ server, PPP_DNS_SYS_PORT }, ec);
                            if (ec) return false;
                        }
                        local_dns_upstreams_[upstream_key] = upstream;
                    }

                    for (int i = 0; i < UINT16_MAX; ++i) {
                        uint16_t candidate = ++upstream->next_id;
                        if (candidate == 0) candidate = ++upstream->next_id;
                        if (upstream->requests.find(candidate) == upstream->requests.end()) {
                            upstream_id = candidate;
                            upstream->requests[candidate] = callback;
                            break;
                        }
                    }
                    if (upstream_id == 0) return false;
                    if (!through_tunnel && !upstream->receiving) {
                        upstream->receiving = true;
                        start_receive = true;
                    }
                }

                if (start_receive) ReceiveLocalDnsUpstream(upstream);
                auto request = make_shared_object<ppp::string>(*query);
                (*request)[0] = static_cast<char>(upstream_id >> 8);
                (*request)[1] = static_cast<char>(upstream_id & 0xff);
                if (through_tunnel) {
                    const boost::asio::ip::udp::endpoint destination(server, PPP_DNS_SYS_PORT);
                    // A successful static-echo enqueue does not prove that the peer
                    // has a working response path. Keep DNS on the proven main UDP
                    // transport and let the caller use TCP as a delayed fallback.
                    const bool static_sent = false;
                    const bool main_sent = upstream->exchanger && upstream->exchanger->SendTo(
                        upstream->tunnel_source, destination,
                        request->data(), static_cast<int>(request->size()));
                    const bool sent = main_sent;
                    LOG_DEBUG("VEthernetNetworkSwitcher::SendLocalDnsUdp: server=%s, static=%d, main=%d",
                        server.to_string().data(), (int)static_sent, (int)main_sent);
                    if (!sent) {
                        SynchronizedObjectScope scope(GetSynchronizedObject());
                        upstream->requests.erase(upstream_id);
                        LOG_WARN("VEthernetNetworkSwitcher::SendLocalDnsUdp: tunnel send failed, server=%s, outbound=main",
                            server.to_string().data());
                    }
                    return sent;
                }
                upstream->socket->async_send(boost::asio::buffer(*request),
                    [this, upstream_key, upstream_id, request](boost::system::error_code ec, std::size_t) noexcept {
                        if (!ec) return;
                        SynchronizedObjectScope scope(GetSynchronizedObject());
                        auto current = local_dns_upstreams_.find(upstream_key);
                        if (current != local_dns_upstreams_.end()) {
                            current->second->requests.erase(upstream_id);
                        }
                        LOG_WARN("VEthernetNetworkSwitcher::SendLocalDnsUdp: direct send failed, upstream=%s, error=%s",
                            upstream_key.data(), ec.message().data());
                    });
                return true;
            }

            void VEthernetNetworkSwitcher::ReceiveLocalDnsUpstream(const std::shared_ptr<LocalDnsUpstream>& upstream) noexcept {
                if (NULLPTR == upstream || NULLPTR == upstream->socket || !upstream->socket->is_open()) return;
                auto response = make_shared_object<ppp::vector<Byte>>();
                response->resize(65536);
                auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                upstream->socket->async_receive(boost::asio::buffer(*response),
                    [self, upstream, response](boost::system::error_code ec, std::size_t size) noexcept {
                        ppp::function<void(const std::shared_ptr<ppp::string>&)> callback;
                        bool rearm = false;
                        if (!ec && size >= 2) {
                            uint16_t id = static_cast<uint16_t>((response->at(0) << 8) | response->at(1));
                            {
                                SynchronizedObjectScope scope(self->GetSynchronizedObject());
                                auto request = upstream->requests.find(id);
                                if (request != upstream->requests.end()) {
                                    callback = std::move(request->second);
                                    upstream->requests.erase(request);
                                }
                                rearm = upstream->socket->is_open();
                            }
                            if (callback) {
                                callback(make_shared_object<ppp::string>(
                                    reinterpret_cast<const char*>(response->data()), size));
                            }
                        }
                        else {
                            SynchronizedObjectScope scope(self->GetSynchronizedObject());
                            upstream->receiving = false;
                            const std::string server_text = upstream->server.to_string();
                            ppp::string upstream_key("direct:");
                            upstream_key.append(server_text.data(), server_text.size());
                            auto current = self->local_dns_upstreams_.find(upstream_key);
                            if (current != self->local_dns_upstreams_.end() && current->second == upstream) {
                                self->local_dns_upstreams_.erase(current);
                            }
                        }
                        if (rearm) self->ReceiveLocalDnsUpstream(upstream);
                    });
            }

            void VEthernetNetworkSwitcher::CancelLocalDnsUdp(
                const ppp::vector<std::pair<ppp::string, uint16_t>>& requests) noexcept {
                SynchronizedObjectScope scope(GetSynchronizedObject());
                for (const auto& request : requests) {
                    auto upstream = local_dns_upstreams_.find(request.first);
                    if (upstream != local_dns_upstreams_.end()) {
                        upstream->second->requests.erase(request.second);
                    }
                }
            }

            void VEthernetNetworkSwitcher::DispatchLocalDnsQuery(
                const std::shared_ptr<ppp::string>& query, bool tcp,
                const ppp::function<void(const std::shared_ptr<ppp::string>&)>& callback) noexcept {
                if (NULLPTR == query || query->empty() || !callback) {
                    return;
                }

                ::dns::Message cached;
                bool policy_direct_query = false;
                if (cached.decode(reinterpret_cast<const uint8_t*>(query->data()), query->size()) == ::dns::BufferResult::NoError &&
                    !cached.questions.empty()) {
                    const ::dns::QuestionSection& question = cached.questions[0];
                    const ppp::string hostname = stl::transform<ppp::string>(question.mName);
                    const bool address_query = question.mType == ::dns::RecordType::kA ||
                        question.mType == ::dns::RecordType::kAAAA;
                    if (geo_rules_) {
                        const auto decision = geo_rules_->MatchDomain(hostname);
                        policy_direct_query =
                            decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                    }
                    else {
                        DNSRulePtr rule = ppp::app::client::dns::Rule::Get(hostname,
                            dns_ruless_[0], dns_ruless_[1], dns_ruless_[2]);
                        policy_direct_query = NULLPTR != rule && rule->Nic;
                    }
                    // A shared cache entry may have been learned through the tunnel
                    // before GeoSite classified this domain as direct. Never let that
                    // stale answer bypass the domestic resolver and the DNS-to-IP
                    // policy observation below.
                    if (address_query && !policy_direct_query &&
                        !ppp::net::asio::vdns::QueryCache2(question.mName.data(), cached,
                        question.mType == ::dns::RecordType::kA ? ppp::net::asio::vdns::AddressFamily::kA :
                        ppp::net::asio::vdns::AddressFamily::kAAAA).empty()) {
                        auto response = make_shared_object<ppp::string>();
                        response->resize(PPP_MAX_DNS_PACKET_BUFFER_SIZE);
                        std::size_t length = 0;
                        if (cached.encode(&(*response)[0], response->size(), length) == ::dns::BufferResult::NoError && length > 0) {
                            response->resize(length);
                            callback(response);
                            return;
                        }
                    }
                }

                // Windows may issue the same DNS question concurrently through
                // several adapters and through both 127.0.0.1 and ::1. Coalesce
                // identical wire queries (the transaction ID is the only ignored
                // field) so one browser lookup creates only one tunnel datagram.
                const uint16_t transaction_id = query->size() >= 2 ?
                    static_cast<uint16_t>((static_cast<Byte>((*query)[0]) << 8) |
                        static_cast<Byte>((*query)[1])) : 0;
                // Preserve flags, class and EDNS/DNSSEC options when coalescing.
                // Only the transaction ID is irrelevant to request semantics.
                ppp::string pending_key = *query;
                if (pending_key.size() >= 2) {
                    pending_key[0] = 0;
                    pending_key[1] = 0;
                }
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    auto pending = local_dns_pending_.find(pending_key);
                    if (pending != local_dns_pending_.end()) {
                        pending->second.emplace_back(LocalDnsWaiter{ transaction_id, callback });
                        return;
                    }
                    local_dns_pending_[pending_key].emplace_back(LocalDnsWaiter{ transaction_id, callback });
                }

                auto complete_pending = [this, pending_key](const std::shared_ptr<ppp::string>& response) noexcept {
                    ppp::vector<LocalDnsWaiter> waiters;
                    {
                        SynchronizedObjectScope scope(GetSynchronizedObject());
                        auto pending = local_dns_pending_.find(pending_key);
                        if (pending == local_dns_pending_.end()) {
                            return;
                        }
                        waiters = std::move(pending->second);
                        local_dns_pending_.erase(pending);
                    }

                    for (LocalDnsWaiter& waiter : waiters) {
                        if (!waiter.callback) {
                            continue;
                        }
                        if (response && response->size() >= 2) {
                            auto individualized = make_shared_object<ppp::string>(*response);
                            (*individualized)[0] = static_cast<char>(waiter.transaction_id >> 8);
                            (*individualized)[1] = static_cast<char>(waiter.transaction_id & 0xff);
                            waiter.callback(individualized);
                        }
                        else {
                            waiter.callback(response);
                        }
                    }
                };

                ppp::vector<boost::asio::ip::address> servers =
                    SelectLocalDnsServers(query->data(), static_cast<int>(query->size()));
                bool direct_query = policy_direct_query;
                ::dns::Message query_message;
                const bool query_decoded =
                    query_message.decode(reinterpret_cast<const uint8_t*>(query->data()), query->size()) ==
                    ::dns::BufferResult::NoError && !query_message.questions.empty();
                if (geo_rules_ && query_decoded) {
                    const auto decision = geo_rules_->MatchDomain(
                        stl::transform<ppp::string>(query_message.questions[0].mName));
                    direct_query = decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                }
                auto context = GetContext();
                if (NULLPTR == context || servers.empty()) {
                    complete_pending(MakeLocalDnsServFail(*query));
                    return;
                }

                const auto completed = make_shared_object<std::atomic<bool>>(false);
                const uint64_t query_started = Executors::GetTickCount();
                const ppp::string query_host = !query_message.questions.empty()
                    ? stl::transform<ppp::string>(query_message.questions[0].mName)
                    : ppp::string();
                const auto query_type = !query_message.questions.empty()
                    ? query_message.questions[0].mType : ::dns::RecordType::kNone;
                const auto query_class = !query_message.questions.empty()
                    ? query_message.questions[0].mClass : ::dns::RecordClass::kNone;
                const ppp::string query_host_lower = ToLower<ppp::string>(query_host);
                const auto valid_response = [query_host_lower, query_type, query_class](
                    const std::shared_ptr<ppp::string>& response, bool udp) noexcept {
                    if (!response || response->size() < 12) return false;
                    ::dns::Message message;
                    if (message.decode(reinterpret_cast<const uint8_t*>(response->data()), response->size()) !=
                        ::dns::BufferResult::NoError || message.mQr != 1 || message.questions.empty()) {
                        return false;
                    }
                    const auto& question = message.questions[0];
                    if (ToLower<ppp::string>(stl::transform<ppp::string>(question.mName)) != query_host_lower ||
                        question.mType != query_type || question.mClass != query_class) {
                        return false;
                    }
                    if (udp && message.mTC != 0) return false;
                    return message.mRCode == static_cast<uint16_t>(::dns::ResponseCode::kNOERROR) ||
                        message.mRCode == static_cast<uint16_t>(::dns::ResponseCode::kNXDOMAIN);
                };
                const auto timer = make_shared_object<boost::asio::steady_timer>(*context);
                const auto close_upstreams = make_shared_object<ppp::vector<ppp::function<void()>>>();
                const auto close_upstreams_lock = make_shared_object<SynchronizedObject>();
                const auto udp_requests = make_shared_object<ppp::vector<std::pair<ppp::string, uint16_t>>>();
                const auto udp_requests_lock = make_shared_object<SynchronizedObject>();
                timer->expires_after(std::chrono::seconds(std::max(1, configuration_->udp.dns.timeout)));

                auto finish = [this, query, query_host, query_started, direct_query, complete_pending, completed, timer, close_upstreams, close_upstreams_lock,
                    udp_requests, udp_requests_lock](const std::shared_ptr<ppp::string>& response) noexcept {
                    if (completed->exchange(true)) {
                        return;
                    }
                    timer->cancel();
                    {
                        SynchronizedObjectScope scope(*close_upstreams_lock);
                        for (auto& close : *close_upstreams) {
                            if (close) close();
                        }
                        close_upstreams->clear();
                    }
                    ppp::vector<std::pair<ppp::string, uint16_t>> requests_to_cancel;
                    {
                        SynchronizedObjectScope scope(*udp_requests_lock);
                        requests_to_cancel = std::move(*udp_requests);
                    }
                    CancelLocalDnsUdp(requests_to_cancel);
                    if (response && !response->empty()) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::DispatchLocalDnsQuery: completed host=%s, direct=%d, elapsed_ms=%llu",
                            query_host.data(), (int)direct_query,
                            (unsigned long long)(Executors::GetTickCount() - query_started));
                        std::shared_ptr<ppp::string> processed = response;
                        if (!direct_query && prefer_ipv4_.load()) {
                            ::dns::Message message;
                            if (message.decode(reinterpret_cast<const uint8_t*>(response->data()), response->size()) == ::dns::BufferResult::NoError &&
                                StripAAAADnsResponseIfIPv4Available(message)) {
                                auto encoded = make_shared_object<ppp::string>();
                                encoded->resize(PPP_MAX_DNS_PACKET_BUFFER_SIZE);
                                std::size_t length = 0;
                                if (message.encode(&(*encoded)[0], encoded->size(), length) == ::dns::BufferResult::NoError && length > 0) {
                                    encoded->resize(length);
                                    processed = encoded;
                                }
                            }
                        }
                        ObserveGeoDnsResponse(processed->data(), static_cast<int>(processed->size()));
                        if (configuration_->udp.dns.cache) {
                            ppp::net::asio::vdns::AddCache(reinterpret_cast<Byte*>(const_cast<char*>(processed->data())),
                                static_cast<int>(processed->size()));
                        }
                        complete_pending(processed);
                    }
                    else {
                        LOG_WARN("VEthernetNetworkSwitcher::DispatchLocalDnsQuery: timeout host=%s, direct=%d, elapsed_ms=%llu",
                            query_host.data(), (int)direct_query,
                            (unsigned long long)(Executors::GetTickCount() - query_started));
                        complete_pending(MakeLocalDnsServFail(*query));
                    }
                };

                // The client's transport does not dictate the upstream transport.
                // A Windows TCP retry must still use the shared upstream UDP path;
                // otherwise every local retry creates new VPN transmissions.
                (void)tcp;
                {
                    auto send_udp_server = [this, query, query_host, query_started, finish, completed, udp_requests,
                        udp_requests_lock, direct_query, valid_response](const boost::asio::ip::address& server) noexcept {
                        ppp::string upstream_key;
                        uint16_t upstream_id = 0;
                        auto upstream_finish = [finish, query_host, query_started, server, direct_query, valid_response](const std::shared_ptr<ppp::string>& response) noexcept {
                            if (!valid_response(response, true)) {
                                LOG_DEBUG("VEthernetNetworkSwitcher::DispatchLocalDnsQuery: ignored invalid UDP response host=%s, server=%s",
                                    query_host.data(), server.to_string().data());
                                return;
                            }
                            LOG_DEBUG("VEthernetNetworkSwitcher::DispatchLocalDnsQuery: response host=%s, server=%s, transport=%s, elapsed_ms=%llu",
                                query_host.data(), server.to_string().data(), direct_query ? "direct-udp" : "tunnel-udp",
                                (unsigned long long)(Executors::GetTickCount() - query_started));
                            finish(response);
                        };
                        if (!SendLocalDnsUdp(server, query, upstream_finish, !direct_query, upstream_key, upstream_id)) {
                            return false;
                        }
                        bool cancel_immediately = false;
                        {
                            SynchronizedObjectScope scope(*udp_requests_lock);
                            if (completed->load()) {
                                cancel_immediately = true;
                            }
                            else {
                                udp_requests->emplace_back(upstream_key, upstream_id);
                            }
                        }
                        if (cancel_immediately) {
                            CancelLocalDnsUdp({ std::make_pair(std::move(upstream_key), upstream_id) });
                        }
                        return true;
                    };

                    // DNS packets are tiny. Race all selected resolvers immediately
                    // and return the first valid response. Delaying the backup made
                    // every query inherit a slow or unavailable primary resolver.
                    bool udp_started = false;
                    for (const auto& server : servers) {
                        udp_started = send_udp_server(server) || udp_started;
                    }
                    if (!udp_started) {
                        finish(NULLPTR);
                        return;
                    }
                    // Reuse the established datagram channel for bounded retries.
                    // Direct/router DNS gets a slightly earlier retry; tunneled DNS
                    // retains more grace for the normal VPN round trip.
                    const auto retry_server = servers.front();
                    const int retry_delays[2] = {
                        direct_query ? 200 : 300,
                        direct_query ? 600 : 900
                    };
                    for (int retry_delay : retry_delays) {
                        auto retry_timer = make_shared_object<boost::asio::steady_timer>(*context);
                        retry_timer->expires_after(std::chrono::milliseconds(retry_delay));
                        {
                            SynchronizedObjectScope scope(*close_upstreams_lock);
                            close_upstreams->emplace_back([retry_timer]() noexcept {
                                retry_timer->cancel();
                            });
                        }
                        retry_timer->async_wait([retry_timer, retry_server, completed,
                            send_udp_server](boost::system::error_code ec) noexcept {
                                if (ec || completed->load()) return;
                                send_udp_server(retry_server);
                            });
                    }
                    timer->async_wait([finish](boost::system::error_code ec) noexcept {
                        if (!ec) {
                            finish(NULLPTR);
                        }
                    });
                    return;
                }

            }

            void VEthernetNetworkSwitcher::ReceiveLocalDnsUdp(const std::shared_ptr<boost::asio::ip::udp::socket>& socket) noexcept {
                if (NULLPTR == socket || !socket->is_open()) {
                    return;
                }
                auto packet = make_shared_object<ppp::vector<Byte>>();
                packet->resize(65536);
                auto source = make_shared_object<boost::asio::ip::udp::endpoint>();
                auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                socket->async_receive_from(boost::asio::buffer(*packet), *source,
                    [self, socket, packet, source](boost::system::error_code ec, std::size_t size) noexcept {
                        if (!ec && size > 0) {
                            auto query = make_shared_object<ppp::string>(reinterpret_cast<const char*>(packet->data()), size);
                            self->DispatchLocalDnsQuery(query, false,
                                [socket, source](const std::shared_ptr<ppp::string>& response) noexcept {
                                    if (response && socket->is_open()) {
                                        socket->async_send_to(boost::asio::buffer(*response), *source,
                                            [response](boost::system::error_code, std::size_t) noexcept {});
                                    }
                                });
                        }
                        if (socket->is_open()) {
                            self->ReceiveLocalDnsUdp(socket);
                        }
                    });
            }

            void VEthernetNetworkSwitcher::ReadLocalDnsTcp(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept {
                auto header = make_shared_object<std::array<Byte, 2>>();
                auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                boost::asio::async_read(*socket, boost::asio::buffer(*header),
                    [self, socket, header](boost::system::error_code ec, std::size_t) noexcept {
                        if (ec) { return; }
                        std::size_t size = (static_cast<std::size_t>((*header)[0]) << 8) | (*header)[1];
                        if (size < 1) { return; }
                        auto query = make_shared_object<ppp::string>();
                        query->resize(size);
                        boost::asio::async_read(*socket, boost::asio::buffer(&(*query)[0], size),
                            [self, socket, query](boost::system::error_code ec, std::size_t) noexcept {
                                if (ec) { return; }
                                self->DispatchLocalDnsQuery(query, true,
                                    [self, socket](const std::shared_ptr<ppp::string>& response) noexcept {
                                        if (!response || !socket->is_open()) { return; }
                                        auto framed = make_shared_object<ppp::string>();
                                        framed->resize(response->size() + 2);
                                        (*framed)[0] = static_cast<char>((response->size() >> 8) & 0xff);
                                        (*framed)[1] = static_cast<char>(response->size() & 0xff);
                                        memcpy(&(*framed)[2], response->data(), response->size());
                                        boost::asio::async_write(*socket, boost::asio::buffer(*framed),
                                            [self, socket, framed](boost::system::error_code ec, std::size_t) noexcept {
                                                if (!ec) self->ReadLocalDnsTcp(socket);
                                            });
                                    });
                            });
                    });
            }

            void VEthernetNetworkSwitcher::AcceptLocalDnsTcp(const std::shared_ptr<boost::asio::ip::tcp::acceptor>& acceptor) noexcept {
                if (NULLPTR == acceptor || !acceptor->is_open()) return;
                auto socket = make_shared_object<boost::asio::ip::tcp::socket>(*GetContext());
                auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                acceptor->async_accept(*socket, [self, acceptor, socket](boost::system::error_code ec) noexcept {
                    if (!ec) self->ReadLocalDnsTcp(socket);
                    if (acceptor->is_open()) self->AcceptLocalDnsTcp(acceptor);
                });
            }

            bool VEthernetNetworkSwitcher::StartLocalDnsProxy() noexcept {
                if (!local_dns_enabled_) return true;
                if (local_dns_udp4_ || local_dns_udp6_ || local_dns_tcp4_ || local_dns_tcp6_) return true;
                auto context = GetContext();
                if (NULLPTR == context) return false;

                boost::system::error_code ec;
                auto udp4 = make_shared_object<boost::asio::ip::udp::socket>(*context);
                udp4->open(boost::asio::ip::udp::v4(), ec);
                if (!ec) udp4->bind({ boost::asio::ip::address_v4::loopback(), PPP_DNS_SYS_PORT }, ec);
                if (ec) { LOG_ERROR("Local DNS: cannot bind UDP 127.0.0.1:53, error=%s", ec.message().data()); return false; }

                auto tcp4 = make_shared_object<boost::asio::ip::tcp::acceptor>(*context);
                tcp4->open(boost::asio::ip::tcp::v4(), ec);
                if (!ec) tcp4->bind({ boost::asio::ip::address_v4::loopback(), PPP_DNS_SYS_PORT }, ec);
                if (!ec) tcp4->listen(boost::asio::socket_base::max_listen_connections, ec);
                if (ec) { LOG_ERROR("Local DNS: cannot bind TCP 127.0.0.1:53, error=%s", ec.message().data()); return false; }

                auto udp6 = make_shared_object<boost::asio::ip::udp::socket>(*context);
                udp6->open(boost::asio::ip::udp::v6(), ec);
                if (!ec) udp6->set_option(boost::asio::ip::v6_only(true), ec);
                if (!ec) udp6->bind({ boost::asio::ip::address_v6::loopback(), PPP_DNS_SYS_PORT }, ec);
                if (ec) { LOG_ERROR("Local DNS: cannot bind UDP [::1]:53, error=%s", ec.message().data()); return false; }

                auto tcp6 = make_shared_object<boost::asio::ip::tcp::acceptor>(*context);
                tcp6->open(boost::asio::ip::tcp::v6(), ec);
                if (!ec) tcp6->set_option(boost::asio::ip::v6_only(true), ec);
                if (!ec) tcp6->bind({ boost::asio::ip::address_v6::loopback(), PPP_DNS_SYS_PORT }, ec);
                if (!ec) tcp6->listen(boost::asio::socket_base::max_listen_connections, ec);
                if (ec) { LOG_ERROR("Local DNS: cannot bind TCP [::1]:53, error=%s", ec.message().data()); return false; }

                local_dns_udp4_ = udp4; local_dns_udp6_ = udp6;
                local_dns_tcp4_ = tcp4; local_dns_tcp6_ = tcp6;
                ReceiveLocalDnsUdp(udp4); ReceiveLocalDnsUdp(udp6);
                AcceptLocalDnsTcp(tcp4); AcceptLocalDnsTcp(tcp6);
                LOG_INFO("Local DNS: listening on UDP/TCP 127.0.0.1:53 and [::1]:53");
                return true;
            }

            void VEthernetNetworkSwitcher::StopLocalDnsProxy() noexcept {
                boost::system::error_code ignored;
                if (auto socket = std::move(local_dns_udp4_); socket) socket->close(ignored);
                if (auto socket = std::move(local_dns_udp6_); socket) socket->close(ignored);
                if (auto acceptor = std::move(local_dns_tcp4_); acceptor) acceptor->close(ignored);
                if (auto acceptor = std::move(local_dns_tcp6_); acceptor) acceptor->close(ignored);
                ppp::vector<std::shared_ptr<LocalDnsUpstream>> upstreams;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    upstreams.reserve(local_dns_upstreams_.size());
                    for (auto& upstream : local_dns_upstreams_) {
                        upstreams.emplace_back(std::move(upstream.second));
                    }
                    local_dns_upstreams_.clear();
                    local_dns_pending_.clear();
                }
                for (const auto& upstream : upstreams) {
                    if (!upstream) continue;
                    if (upstream->socket) upstream->socket->close(ignored);
                    if (upstream->through_tunnel && upstream->exchanger) {
                        upstream->exchanger->ReleaseDatagramHandler(upstream->tunnel_source);
                    }
                }
            }
#endif

#if defined(_ANDROID) || defined(_IPHONE)
            bool VEthernetNetworkSwitcher::AddAllRoute(const std::shared_ptr<ITap>& tap) noexcept {
                RouteInformationTablePtr rib = make_shared_object<RouteInformationTable>();
                if (NULLPTR == rib)  {
                    return false;
                }

                // Android requires the VPN to manage the routing table itself because it is a default gateway hybrid architecture.
                rib_ = rib;

                // Set up VPN subnet ip route.
                uint32_t cidr = ntohl(tap->SubmaskAddress);
                cidr = cidr & ntohl(tap->IPAddress);
                cidr = htonl(cidr);
                rib->AddRoute(cidr, IPEndPoint::NetmaskToPrefix(tap->SubmaskAddress), tap->GatewayServer);

                // Why does Android/APPLE-IOS load routing table information? 
                // This is to implement the IP diversion function of the HTTP proxy to prevent all traffic from going to the VPN server, 
                // Because there are some scenarios that do not want to go through the VPN server.
                if (ppp::string bypass_ip_list = std::move(bypass_ip_list_); bypass_ip_list.size() > 0) {
                    // IP address of the virtual network card is used here to make it inconsistent with the condition of determining
                    // The next hop gateway of the route in the IsBypassIpAddress function.
                    rib->AddAllRoutes(bypass_ip_list, IPEndPoint::LoopbackAddress);

                    // AddAllRoutes() rejects IPv6 lines (AddRoute() only accepts
                    // v4), so parse the IPv6 bypass entries manually into rib6_.
                    // Without this IsBypassIpAddress6() can never match on Android.
                    IPv6RouteTablePtr rib6 = make_shared_object<IPv6RouteTable>();
                    if (NULLPTR != rib6) {
                        // A loopback next hop distinguishes bypass routes from the
                        // TAP v6 gateway in IsBypassIpAddress6().
                        boost::asio::ip::address_v6 ngw6 = boost::asio::ip::address_v6::loopback();
                        ppp::vector<ppp::string> lines;
                        if (Tokenize<ppp::string>(bypass_ip_list, lines, "\r\n") < 1) {
                            Tokenize<ppp::string>(bypass_ip_list, lines, "\n");
                        }

                        for (const ppp::string& line : lines) {
                            ppp::string cidr = ppp::LTrim(ppp::RTrim(line));
                            if (cidr.empty()) {
                                continue;
                            }
                            if (cidr[0] == '#' || cidr[0] == ';') {
                                continue;
                            }

                            std::string host;
                            int prefix = -1;
                            std::size_t i = cidr.find('/');
                            if (i == ppp::string::npos) {
                                host = cidr;
                            }
                            else {
                                if (i == 0) {
                                    continue;
                                }
                                host = cidr.substr(0, i);
                                prefix = atoi(cidr.data() + (i + 1));
                            }

                            boost::system::error_code ec;
                            boost::asio::ip::address ip = ppp::StringToAddress(host, ec);
                            if (ec) {
                                continue;
                            }
                            if (!ip.is_v6()) {
                                continue;
                            }
                            if (prefix < 0) {
                                prefix = 128;
                            }
                            elif (prefix > 128) {
                                continue;
                            }

                            IPv6RouteEntry entry;
                            entry.Network = ip.to_v6();
                            entry.Prefix = prefix;
                            entry.NextHop = ngw6;
                            rib6->emplace_back(entry);
                        }

                        if (!rib6->empty()) {
                            rib6_ = rib6;
                        }
                    }
                }

                // Add dns route set rules.
                uint32_t gws[] = {tap->GatewayServer, IPEndPoint::LoopbackAddress};
                ppp::unordered_set<uint32_t> dns_serverss_[2];
                for (auto&& dns_rules : dns_ruless_) {
                    for (auto& [_, r] : dns_rules) {
                        boost::asio::ip::address server = r->Server;
                        if (!server.is_v4()) {
                            continue;
                        }

                        uint32_t ip = htonl(server.to_v4().to_uint());
                        if (r->Nic) {
                            dns_serverss_[1].emplace(ip);
                        }
                        else {
                            dns_serverss_[0].emplace(ip);
                        }
                    }
                }

                // Compare two lists and remove duplicate ip addresses that appear in both lists.
                ppp::collections::Dictionary::DeduplicationList(dns_serverss_[1], dns_serverss_[0]);
                for (int i = 0; i < arraysizeof(gws); i++) {
                    uint32_t gw = gws[i];
                    for (auto& ip : dns_serverss_[i]) {
                        rib->AddRoute(ip, 32, gw);
                    }
                }

                // Add VPN remote server to IPList bypass route table iplist.
                return AddRemoteEndPointToIPList(Ipep::ToAddress(IPEndPoint::LoopbackAddress));
            }
#endif

            bool VEthernetNetworkSwitcher::PreparedAggregator() noexcept {
                std::shared_ptr<boost::asio::io_context> context = ppp::threading::Executors::GetDefault();
                if (NULLPTR == context) {
                    return false;
                }

                std::shared_ptr<Byte> buffer = ppp::threading::Executors::GetCachedBuffer(context);
                if (NULLPTR == buffer) {
                    return false;
                }

                std::shared_ptr<aggligator::aggligator> aggligator = 
                    make_shared_object<aggligator::aggligator>(*context, buffer, PPP_BUFFER_SIZE, PPP_AGGLIGATOR_CONGESTIONS);
                if (NULLPTR == aggligator) {
                    return false;
                }

                aggligator_ = aggligator;
#if defined(_LINUX)
                aggligator->ProtectorNetwork = GetProtectorNetwork();
#endif
                aggligator->AppConfiguration = configuration_;
                aggligator->BufferswapAllocator = configuration_->GetBufferAllocator();
                return true;
            }

            bool VEthernetNetworkSwitcher::Open(const std::shared_ptr<ITap>& tap) noexcept {
                LOG_DEBUG("VEthernetNetworkSwitcher::Open: starting");
#if defined(_ANDROID) || defined(_IPHONE)
                // Mobile platforms never snapshot the physical NIC
                // (underlying_ni_ is only populated on desktop), so
                // direct_dns=local can contribute no servers here. The
                // explicitly configured direct DNS servers from geo-rules.yaml
                // (e.g. 223.5.5.5, 119.29.29.29) are still loaded so that
                // domain rules (domain-suffix/geosite) can steer domestic DNS
                // lookups to a direct resolver instead of the tunnel DNS, and
                // the DNS response observation path can pin those domains'
                // resolved IPs to direct. Without this the direct DNS list
                // stays empty on Android/iOS and SelectDirectDnsServer()
                // always fails, so domain-based geo rules never take effect.
                RefreshDirectDnsServers();
#else
                // Get and retrieve the current underlying Ethernet interface information!
                // This is needed in both TUN and proxy-only modes: proxy-only still
                // requires the physical NIC info for IPv6 server route pinning.
#if defined(_WIN32)
                underlying_ni_ = Windows_GetUnderlyingNetowrkInterface(tap, preferred_nic_);
#else
                underlying_ni_ = Unix_GetUnderlyingNetowrkInterface(tap, preferred_nic_);
#endif

                // The physical hosting network interface required for the VPN overlap network is not allowed to construct and turn on the VPN service.
                if (auto underlying_ni = underlying_ni_; NULLPTR != underlying_ni) {
                    boost::asio::ip::address& ngw = preferred_ngw_;
                    if (!IPEndPoint::IsInvalid(ngw)) {
                        underlying_ni->GatewayServer = ngw;
                    }
                }
                else {
                    LOG_DEBUG("VEthernetNetworkSwitcher::Open: underlying network interface not found");
                    // The GUI's no-listener proxy control plane deliberately
                    // has no TUN takeover.  It still needs the in-memory
                    // client switcher so the server-directory entries can be
                    // probed before a real client is selected.  Physical NIC
                    // metadata is optional for proxy-only probing; retain the
                    // strict failure for a real TUN client.
                    if (!proxy_only_) {
                        return false;
                    }
                    LOG_INFO("VEthernetNetworkSwitcher::Open: continuing proxy-only without underlying NIC");
                }

                if (!proxy_only_) {
                // Resolve direct_dns=local before any Windows DNS takeover.  This
                // preserves the DHCP/static DNS snapshot of the selected physical
                // interface and prevents reading our own virtual DNS later.
                if (!RefreshDirectDnsServers()) {
                    LOG_ERROR("VEthernetNetworkSwitcher::Open: no usable direct DNS server");
                    return false;
                }

                // Compatibility by all means try to check and fix the gateway route of the physical network card once, 
                // Otherwise there will be no network with all kinds of chain problems!
                FixUnderlyingNgw();
                }
#endif
                // Construction of VEtherent virtual Ethernet switcher processing framework.
                if (!VEthernet::Open(tap)) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::Open: VEthernet::Open failed");
                    return false;
                }
                LOG_DEBUG("VEthernetNetworkSwitcher::Open: VEthernet::Open succeeded");

#if !defined(_ANDROID) && !defined(_IPHONE)
                if (!proxy_only_) {
#if defined(_WIN32)
                // Get network interface information for TAP-Windows virtual Ethernet devices!
                tun_ni_ = Windows_GetTapNetworkInterface(tap);
#else
                // Get network interface information for Linux tun/tap virtual Ethernet devices!
                tun_ni_ = Unix_GetTapNetworkInterface(tap);
#endif

                // The vEthernet network switcher cannot be opened when the virtual network adapter device interface for the VPN startup link cannot be found!
                if (NULLPTR == tun_ni_) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::Open: TAP network interface not found");
                    return false;
                }
                LOG_DEBUG("VEthernetNetworkSwitcher::Open: TAP network interface found");
#if defined(_WIN32)
                // The peer has not advertised an IPv6 data plane yet. Fail
                // closed until its Information extension is received; a client
                // profile's server.ipv6 section is not a remote capability signal.
                ipv6_server_has_dataplane_ = false;
                if (!ApplyWindowsIPv6LeakBlockRoutes()) {
                    LOG_ERROR("VEthernetNetworkSwitcher::Open: initial IPv6 leak block failed");
                    return false;
                }
#endif
                }
#endif

                // Open client-side logger
                OpenLogger();

                // Log module startup
                VirtualEthernetLoggerPtr logger = logger_;
                if (NULLPTR != logger) {
                    ppp::string remote_uri = GetRemoteUri();
                    logger->ModuleStart("VEthernetNetworkSwitcher", BOOST_BEAST_VERSION_STRING, "URI:" + remote_uri);
                }

                // Initial a new network statistics.
                statistics_ = NewStatistics();

                // Instantiate the local QoS throughput speed control module!
                std::shared_ptr<ppp::transmissions::ITransmissionQoS> qos = NewQoS();
                if (NULLPTR == qos) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::Open: NewQoS failed");
                    return false;
                }

#if defined(_LINUX)
                // This section describes how to instantiate the physical network instance protector required by ppp to 
                // Prevent VPN virtual switcher crashes caused by IP route loopback.
                ProtectorNetworkPtr protector_network;
#if defined(_ANDROID)
                protector_network = NewProtectorNetwork();
                if (NULLPTR == protector_network) {
                    return false;
                }
#else
                if (protect_mode_) {
                    protector_network = NewProtectorNetwork();
                }
#endif
#endif
                // Exchangers and their per-outbound proxy forwarders need the
                // protector before their asynchronous connection loops start.
#if defined(_LINUX)
                protect_network_ = protector_network;
#endif
                // Load all configuration metadata, but open only the primary.
                // Menu/fixed GEO outbounds are created on demand.
                OutboundConfigurationList outbound_configurations = outbound_configurations_;
                if (outbound_configurations.empty()) {
                    outbound_configurations.emplace_back(OutboundConfiguration{ "main", configuration_ });
                }
                // The GUI control plane has a synthetic empty primary only to
                // satisfy the outbound table contract.  It must not attempt
                // a VPN handshake before the user selects a server; the
                // server-directory entries remain available to the probe
                // scheduler below.
                const bool metadata_only = proxy_only_ &&
                    configuration_ != NULLPTR && configuration_->client.server.empty();
                if (outbound_configurations.size() > 1 && static_mode_) {
                    // Static UDP echo owns one global server/aggregator set in the
                    // legacy design; sharing it would mix independently keyed
                    // outbounds. Reject it instead of routing through the wrong key.
                    LOG_ERROR("VEthernetNetworkSwitcher::Open: multi-outbound mode does not support --tun-static=yes");
                    IDisposable::Dispose(qos);
                    return false;
                }

                OutboundExchangerTable opened_outbounds;
                std::shared_ptr<VEthernetExchanger> exchanger;
                for (const OutboundConfiguration& outbound : outbound_configurations) {
                    ppp::string tag = ToLower<ppp::string>(ATrim<ppp::string>(outbound.tag));
                    bool primary = tag == "main";
                    if (!primary) continue;
                    if (metadata_only) {
                        LOG_INFO("VEthernetNetworkSwitcher::Open: metadata-only control plane; skipping empty primary outbound");
                        continue;
                    }
                    std::shared_ptr<VEthernetExchanger> candidate = NewExchanger(outbound.configuration, tag, primary);
                    if (NULLPTR == candidate || !candidate->Open()) {
                        LOG_ERROR("VEthernetNetworkSwitcher::Open: failed to open outbound '%s'", tag.data());
                        for (auto& entry : opened_outbounds) {
                            if (entry.second) entry.second->Dispose();
                        }
                        IDisposable::Dispose(qos);
                        return false;
                    }
                    if (primary) exchanger = candidate;
                    opened_outbounds.emplace(tag, std::move(candidate));
                    LOG_INFO("VEthernetNetworkSwitcher::Open: outbound '%s' opened", tag.data());
                }
                if (!metadata_only && NULLPTR == exchanger) {
                    LOG_ERROR("VEthernetNetworkSwitcher::Open: main outbound is missing");
                    for (auto& entry : opened_outbounds) {
                        if (entry.second) entry.second->Dispose();
                    }
                    IDisposable::Dispose(qos);
                    return false;
                }

                VEthernetHttpProxySwitcherPtr http_proxy;
                VEthernetSocksProxySwitcherPtr socks_proxy;
                if (!metadata_only) {
                    // Enable the local HTTP PROXY server middleware to provide proxy services directly by the VPN.
                    http_proxy = NewHttpProxy(exchanger);
                    if (NULLPTR == http_proxy) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: NewHttpProxy failed");
                        return false;
                    }
                    elif(http_proxy->Open()) {
                        http_proxy_ = std::move(http_proxy);
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: HTTP proxy opened");
                    }
                    else {
                        http_proxy->Dispose();
                        http_proxy.reset();
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: HTTP proxy not available (non-fatal)");
                    }

                    // Enable the local SOCKS PROXY server middleware to provide proxy services directly by the VPN.
                    socks_proxy = NewSocksProxy(exchanger);
                    if (NULLPTR == socks_proxy) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: NewSocksProxy failed");
                        return false;
                    }
                    elif(socks_proxy->Open()) {
                        socks_proxy_ = std::move(socks_proxy);
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: SOCKS proxy opened");
                    }
                    else {
                        socks_proxy->Dispose();
                        socks_proxy.reset();
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: SOCKS proxy not available (non-fatal)");
                    }
                }

                // Mounts the various service objects created and opened by the current constructor.
                qos_             = std::move(qos);
                exchanger_       = exchanger;
                outbound_exchangers_ = std::move(opened_outbounds);

                if (proxy_only_) {
                    // Proxy-only has no kernel route installation, but the
                    // in-memory RIB is still needed for --bypass-mode=ip.
                    LoadAllIPListWithFilePaths(boost::asio::ip::address_v4::any());
                    LoadAllIPListWithFilePaths6(boost::asio::ip::address_v6::any());
                    if (!metadata_only && !UpdateRemoteUri()) {
                        LOG_WARN("VEthernetNetworkSwitcher::Open: cannot determine proxy-only remote URI");
                    }

                    LOG_INFO("VEthernetNetworkSwitcher::Open: proxy-only connected; kernel routes/DNS disabled, proxy geo policy enabled%s",
                        NULLPTR == http_proxy_ && NULLPTR == socks_proxy_ ? "; no local listener" : "");
                    return true;
                }

                // New the beast network bandwidth aggregator.
                if (static_mode_ && configuration_->udp.static_.aggligator > 0) {
                    if (!PreparedAggregator()) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::Open: PreparedAggregator failed");
                        return false;
                    }
                }

#if defined(_ANDROID) || defined(_IPHONE)
                if (!AddAllRoute(tap)) {
                    IDisposable::DisposeReferences(qos, exchanger, http_proxy);
                    return false;
                }
#else
                // Load all IPList route table configuration files that need to be loaded.
                if (auto underlying_ni = underlying_ni_; NULLPTR != underlying_ni) {
                    LoadAllIPListWithFilePaths(underlying_ni->GatewayServer);

                    // Use preferred_ngw6_ (--bypass-ngw6) if specified; otherwise fall back to auto-detected IPv6 gateway.
                    {
                        boost::asio::ip::address gw6 = preferred_ngw6_;
                        if (!gw6.is_v6() || gw6.is_unspecified() || IPEndPoint::IsInvalid(gw6)) {
                            gw6 = underlying_ni->IPv6GatewayServer;
                        }
                        LoadAllIPListWithFilePaths6(gw6);
                    }

                    if (geo_rules_ && !ApplyGeoStaticRoutes()) {
                        LOG_ERROR("VEthernetNetworkSwitcher::Open: failed to apply geo static routes");
                        return false;
                    }

                    // Add VPN remote server to IPList bypass route table iplist.
                    if (!AddRemoteEndPointToIPList(underlying_ni->GatewayServer)) {
                        // A hostname may be temporarily unresolved while the
                        // client is opening. The first transport connection
                        // resolves it again and pins the exact endpoint before
                        // dialing, so this optional startup route must not
                        // prevent the core/RPC service from coming up.
                        LOG_WARN("VEthernetNetworkSwitcher::Open: AddRemoteEndPointToIPList deferred; transport will resolve the server endpoint on connect");
                    }
                }
#endif

                // Attempt to load the routing table configuration if the routing table is configured correctly.
                if (RouteInformationTablePtr rib = rib_; NULLPTR != rib) {
                    ForwardInformationTablePtr fib = make_shared_object<ForwardInformationTable>();
                    if (NULLPTR != fib) {
                        fib->Fill(*rib);

                        if (fib->IsAvailable()) {
                            fib_ = fib;
                        }
                    }
                }

                // Build IPv6 FIB from the loaded bypass route table entries.
                if (IPv6RouteTablePtr rib6 = rib6_; NULLPTR != rib6 && !rib6->empty()) {
                    RouteInformationTable6Ptr rib6_fib = make_shared_object<RouteInformationTable6>();
                    if (NULLPTR != rib6_fib) {
                        for (const IPv6RouteEntry& entry : *rib6) {
                            boost::asio::ip::address dst(entry.Network);
                            boost::asio::ip::address nh(entry.NextHop);
                            rib6_fib->AddRoute(dst, entry.Prefix, nh);
                        }

                        ForwardInformationTable6Ptr fib6 = make_shared_object<ForwardInformationTable6>();
                        if (NULLPTR != fib6) {
                            fib6->Fill(*rib6_fib);
                            if (fib6->IsAvailable()) {
                                fib6_ = fib6;
                            }
                        }
                    }
                }

#if !defined(_ANDROID) && !defined(_IPHONE)
                // Hosted clients must not replace the host's routes or DNS until the
                // primary tunnel has completed its handshake. OnTick performs this
                // transition asynchronously so blocking Windows route/WMI calls never
                // prevent the exchanger's connection coroutine from running.
                main_outbound_unavailable_since_.store(0);
#endif
                LOG_DEBUG("VEthernetNetworkSwitcher::Open: completed successfully");
                return true;
            }

#if defined(_WIN32)
            bool VEthernetNetworkSwitcher::UsePaperAirplaneController() noexcept {
                // Open the [PaperAirplane NSP/LSP] paper airplane server controller, 
                // Depending on the configuration and whether it is a CLI command line hosted network flag.
                if (configuration_->client.paper_airplane.tcp) {
                    PaperAirplaneControllerPtr controller = NewPaperAirplaneController();
                    if (NULLPTR == controller) {
                        return false;
                    }

                    // Clean up resources constructed by the current function when opening the server side of the paper plane fails.
                    auto tun_ni = tun_ni_; 
                    if (NULLPTR != tun_ni) {
                        auto tap = GetTap(); 
                        if (NULLPTR != tap) {
                            if (!controller->Open(tun_ni->Index, tap->IPAddress, tap->SubmaskAddress)) {
                                IDisposable::DisposeReferences(controller);
                                return false;
                            }
                        }
                    }

                    // Open the paper plane successfully when you move the created instance on the local variable to 
                    // The virtual ethernet switch hosted fields.
                    paper_airplane_ctrl_ = std::move(controller);
                }
                return true;
            }
#endif

#if !defined(_ANDROID) && !defined(_IPHONE)
            bool VEthernetNetworkSwitcher::FixUnderlyingNgw() noexcept {
                auto ni = underlying_ni_;
                if (NULLPTR == ni) {
                    return false;
                }

                auto gw = ni->GatewayServer; 
                if (gw.is_v4() && !IPEndPoint::IsInvalid(gw) && !gw.is_loopback()) {
                    uint32_t next_hop = htonl(gw.to_v4().to_uint());
#if defined(_WIN32)
                    // Repair physical ethernet route table information on windows platform!
                    ppp::win32::network::Router::Add(IPEndPoint::AnyAddress, IPEndPoint::AnyAddress, next_hop, 1);
#elif defined(_MACOS)
                    ppp::darwin::tun::utun_add_route2(IPEndPoint::AnyAddress, IPEndPoint::AnyAddress, next_hop);
#else
                    // Repair physical ethernet route table information on linux platform!
                    ppp::tap::TapLinux::AddRoute(ni->Name, IPEndPoint::AnyAddress, IPEndPoint::AnyAddress, next_hop);
#endif
                    return true;
                }

                return false;
            }

            void VEthernetNetworkSwitcher::UpdateNetworkTakeover(uint64_t now) noexcept {
                std::shared_ptr<ITap> tap = GetTap();
                std::shared_ptr<VEthernetExchanger> main = exchanger_;
                if (NULLPTR == tap || !tap->IsHostedNetwork() || NULLPTR == main || network_takeover_stopping_.load()) {
                    return;
                }

                if (main->GetNetworkState() == VEthernetExchanger::NetworkState_Established) {
                    main_outbound_unavailable_since_.store(0);
                    if (!route_added_.load()) {
                        QueueNetworkTakeover(true);
                    }
                    return;
                }

                if (!route_added_.load()) {
                    main_outbound_unavailable_since_.store(0);
                    return;
                }

                uint64_t unavailable_since = main_outbound_unavailable_since_.load();
                if (unavailable_since == 0) {
                    main_outbound_unavailable_since_.store(now);
                }
                elif(now >= unavailable_since && now - unavailable_since >= 10000) {
                    QueueNetworkTakeover(false);
                }
            }

            void VEthernetNetworkSwitcher::QueueNetworkTakeover(bool activate) noexcept {
                if (network_takeover_stopping_.load() || network_takeover_worker_.exchange(true)) {
                    return;
                }

                auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                try {
                    std::thread([self, activate]() noexcept {
#if defined(_WIN32)
                        HRESULT hr = CoInitializeEx(NULLPTR, COINIT_MULTITHREADED);
#endif
                        {
                            SynchronizedObjectScope scope(self->prdr_);
                            if (!self->network_takeover_stopping_.load()) {
                                if (activate) {
                                    std::shared_ptr<VEthernetExchanger> main = self->exchanger_;
                                    if (NULLPTR != main && main->GetNetworkState() == VEthernetExchanger::NetworkState_Established) {
                                        self->ApplyNetworkTakeover();
                                    }
                                }
                                else {
                                    std::shared_ptr<VEthernetExchanger> main = self->exchanger_;
                                    if (NULLPTR == main || main->GetNetworkState() != VEthernetExchanger::NetworkState_Established) {
                                        self->RestoreNetworkTakeover(true);
                                    }
                                }
                            }
                        }
#if defined(_WIN32)
                        if (SUCCEEDED(hr)) {
                            CoUninitialize();
                        }
#endif
                        self->network_takeover_worker_.store(false);
                    }).detach();
                }
                catch (const std::exception& e) {
                    network_takeover_worker_.store(false);
                    LOG_ERROR("VEthernetNetworkSwitcher::QueueNetworkTakeover: worker creation failed, exception=%s", e.what());
                }
            }

            bool VEthernetNetworkSwitcher::ApplyNetworkTakeover() noexcept {
                if (network_takeover_stopping_.load() || route_added_.exchange(true)) {
                    return false;
                }

#if defined(_WIN32)
                if (NULLPTR == paper_airplane_ctrl_ && !UsePaperAirplaneController()) {
                    route_added_.store(false);
                    LOG_ERROR("VEthernetNetworkSwitcher::ApplyNetworkTakeover: Paper-Airplane controller failed");
                    return false;
                }
#endif

                AddRoute();

#if defined(_WIN32)
                // Configure only the openppp2 TUN.  ICS, WSL, Hyper-V, Docker and
                // every other adapter keep their own DNS configuration.
                auto tun_ni = tun_ni_;
                if (NULLPTR != tun_ni) {
                    // Use the real IPv4 resolvers as Windows DNS endpoints. Their
                    // /32 routes were installed by AddRouteWithDnsServers(), so DNS
                    // enters TUN directly and cannot collide with ICS/WSL services
                    // that own the host's UDP 0.0.0.0:53 socket.
                    ppp::vector<boost::asio::ip::address> tunnel_dns;
                    ppp::vector<ppp::string> tunnel_dns_strings;
                    for (const auto& address : tun_ni->DnsAddresses) {
                        if (address.is_v4() && !address.is_unspecified() &&
                            !address.is_loopback() && !IPEndPoint::IsInvalid(address)) {
                            tunnel_dns.emplace_back(address);
                            tunnel_dns_strings.emplace_back(address.to_string());
                        }
                    }
                    if (tunnel_dns.empty()) {
                        DeleteRoute();
                        route_added_.store(false);
                        LOG_ERROR("VEthernetNetworkSwitcher::ApplyNetworkTakeover: no usable IPv4 TUN DNS server");
                        return false;
                    }
                    int tap_if_index = tun_ni->Index;
                    int dns_if_index = tap_if_index;
                    // Snapshot the original IPv4 DNS of every CONNECTED NIC so
                    // shutdown can restore them.  Only adapters with OperStatus Up
                    // are captured (disconnected adapters keep their own stale DNS
                    // and are never touched).  The TUN entry is kept authoritative
                    // for the tunnel itself.
                    ppp::unordered_map<int, ppp::vector<ppp::string>> all_dns_v4;
                    ppp::win32::network::GetAllNicsDnsAddresses(all_dns_v4);
                    ni_dns_servers_.clear();
                    for (auto&& [if_index, servers] : all_dns_v4) {
                        ppp::vector<boost::asio::ip::address> addrs;
                        for (const auto& s : servers) {
                            boost::system::error_code ec;
                            boost::asio::ip::address ip = StringToAddress(s.data(), ec);
                            if (!ec) {
                                addrs.emplace_back(ip);
                            }
                        }
                        if (!addrs.empty()) {
                            ni_dns_servers_[if_index] = std::move(addrs);
                        }
                    }
                    ni_dns_servers_[tap_if_index] = tun_ni->DnsAddresses;
                    ppp::vector<ppp::string> system_dns_strings;
                    system_dns_strings.emplace_back(tun_ni->GatewayServer.to_string());
                    ppp::win32::network::ClearDnsAddresses(dns_if_index);
                    if (!ppp::win32::network::SetDnsAddresses(dns_if_index, system_dns_strings)) {
                        auto restore_dns = ni_dns_servers_;
                        ppp::win32::network::SetAllNicsDnsAddresses(restore_dns);
                        ni_dns_servers_.clear();
                        DeleteRoute();
                        route_added_.store(false);
                        LOG_ERROR("VEthernetNetworkSwitcher::ApplyNetworkTakeover: cannot set TUN DNS");
                        return false;
                    }
                    // Pin every other connected NIC's IPv4 resolver to the local
                    // loopback proxy 127.0.0.1.  The Windows DNS Client queries
                    // every NIC's resolvers in parallel and accepts the first
                    // answer; a physical NIC still pointing at the ISP resolver
                    // would answer instantly with GFW-polluted records and steal
                    // the answer from the tunnel DNS.  Disconnected adapters are
                    // already excluded by the snapshot above and are not touched.
                    ppp::vector<int> non_tap_v4_indexes;
                    for (auto&& [if_index, servers] : all_dns_v4) {
                        if (if_index != tap_if_index && !servers.empty()) {
                            ppp::win32::network::SetDnsAddresses(if_index, { "127.0.0.1" });
                            non_tap_v4_indexes.emplace_back(if_index);
                        }
                    }

                    ni_dns_servers_v6_.clear();
                    ni_router_discovery_disabled_v6_.clear();
                    ppp::unordered_map<int, ppp::vector<ppp::string>> all_dns_v6;
                    ppp::vector<int> non_tap_v6_indexes;
                    // Bring up the loopback DNS proxy first so every NIC can
                    // point at 127.0.0.1/[::1] and its queries are funneled into
                    // the tunnel instead of racing the ISP resolver.
                    StartLocalDnsProxy();
                    if (ppp::win32::network::GetAllNicsDnsAddressesV6(all_dns_v6)) {
                        // Snapshot the original IPv6 DNS of every NIC so shutdown can
                        // restore them.  The Windows DNS Client queries every NIC's
                        // resolvers in parallel and accepts the first answer.  A
                        // physical NIC (e.g. the Hyper-V vSwitch carrying the host
                        // LAN) whose IPv6 DNS still points at the ISP resolver
                        // answers instantly with GFW-polluted records and steals the
                        // answer from the tunnel DNS.  Instead of clearing (which RA
                        // RDNSS may re-inject) we pin every non-TAP NIC to the local
                        // proxy ::1, a static value RA never overwrites.
                        ni_dns_servers_v6_ = all_dns_v6;
                        for (const auto& [if_index, servers] : all_dns_v6) {
                            if (if_index != tap_if_index && !servers.empty()) {
                                // Pin the resolver to the local proxy ::1 only.  Do NOT
                                // disable IPv6 router discovery (RA) on the underlying
                                // NICs: RA is what keeps SLAAC public IPv6 addresses
                                // alive on the host (e.g. vEthernet (Debian)).  Killing
                                // it silently removes the host's public IPv6 and breaks
                                // IPv6-only server connectivity.  RDNSS re-injection of
                                // the ISP resolver is instead defeated by re-pinning
                                // ::1 every 30s via the DNS guard timer below.
                                ppp::win32::network::SetDnsAddressesV6(if_index, { "::1" });
                                non_tap_v6_indexes.emplace_back(if_index);
                            }
                        }
                    }
                    ppp::win32::network::ClearDnsAddressesV6(tap_if_index);

                    auto context = GetContext();
                    auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                    dns_guard_timer_ = make_shared_object<ppp::threading::Timer>(context);
                    if (NULLPTR != dns_guard_timer_) {
                        dns_guard_active_.store(true);
                        dns_guard_timer_->TickEvent =
                            [self, tap_if_index, dns_if_index, system_dns_strings, non_tap_v4_indexes, non_tap_v6_indexes](ppp::threading::Timer*, ppp::threading::Timer::TickEventArgs&) noexcept {
                                if (!self->dns_guard_active_.load()) {
                                    return;
                                }

                                self->dns_guard_workers_.fetch_add(1);
                                try {
                                    std::thread([self, tap_if_index, dns_if_index, system_dns_strings, non_tap_v4_indexes, non_tap_v6_indexes]() noexcept {
                                        if (!self->dns_guard_active_.load()) {
                                            self->dns_guard_workers_.fetch_sub(1);
                                            return;
                                        }

                                        HRESULT hr = CoInitializeEx(NULLPTR, COINIT_MULTITHREADED);
                                        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
                                            // DHCP may re-inject ISP IPv4 DNS after
                                            // takeover; keep every connected non-TAP
                                            // NIC pinned to the local proxy so the
                                            // tunnel resolver always wins the race.
                                            for (int if_index : non_tap_v4_indexes) {
                                                ppp::win32::network::SetDnsAddresses(if_index, { "127.0.0.1" });
                                            }
                                            // DHCPv6/RA may re-inject ISP IPv6 DNS after
                                            // takeover; keep every non-TAP NIC pinned to
                                            // the local proxy so the tunnel resolver
                                            // always wins the parallel race.
                                            for (int if_index : non_tap_v6_indexes) {
                                                ppp::win32::network::SetDnsAddressesV6(if_index, { "::1" });
                                            }
                                            ppp::win32::network::ClearDnsAddressesV6(tap_if_index);
                                            if (self->dns_guard_active_.load()) {
                                                ppp::win32::network::ClearDnsAddresses(dns_if_index);
                                                ppp::win32::network::SetDnsAddresses(dns_if_index, system_dns_strings);
                                            }
                                            if (SUCCEEDED(hr)) {
                                                CoUninitialize();
                                            }
                                        }
                                        self->dns_guard_workers_.fetch_sub(1);
                                    }).detach();
                                }
                                catch (const std::exception& e) {
                                    self->dns_guard_workers_.fetch_sub(1);
                                    LOG_ERROR("VEthernetNetworkSwitcher::ApplyNetworkTakeover: DNS guard worker creation failed, exception=%s", e.what());
                                }
                            };
                        dns_guard_timer_->SetInterval(30000);
                        dns_guard_timer_->Start();
                    }
                    LOG_INFO("Windows DNS: virtual gateway %s on TUN ifIndex=%d, upstream=%s",
                        system_dns_strings.front().data(), tap_if_index, tunnel_dns_strings.front().data());
                }

                ppp::tap::TapWindows::DnsFlushResolverCache();
                if (auto underlying_ni = underlying_ni_; NULLPTR != underlying_ni) {
                    ppp::win32::network::DeleteAllDefaultGatewayRoutes(underlying_ni->GatewayServer);
                }
#else
                if (auto tun_ni = tun_ni_; NULLPTR != tun_ni) {
                    ppp::unix__::UnixAfx::SetDnsAddresses(tun_ni->DnsAddresses);
                }
#endif

                ProtectDefaultRoute();
                LOG_INFO("VEthernetNetworkSwitcher::ApplyNetworkTakeover: primary outbound established, routes and DNS applied");
                return true;
            }

            void VEthernetNetworkSwitcher::AddRoute() noexcept {
#if defined(_WIN32)
                std::shared_ptr<ITap> tap = GetTap();

                // Find and delete all default route information!
                if (NULLPTR != tap) {
                    ppp::win32::network::DeleteAllDefaultGatewayRoutes(default_routes_, { tap->GatewayServer });
                }

                // Adds the loaded route table to the operating system.
                if (NULLPTR != tap) {
                    if (std::shared_ptr<NetworkInterface> underlying = underlying_ni_;
                        NULLPTR != underlying && underlying->Index >= 0) {
                        ppp::unordered_map<uint32_t, int> gateway_interfaces;
                        if (underlying->GatewayServer.is_v4() && !underlying->GatewayServer.is_unspecified()) {
                            gateway_interfaces[htonl(underlying->GatewayServer.to_v4().to_uint())] = underlying->Index;
                        }
                        gateway_interfaces[tap->GatewayServer] = tap->GetInterfaceIndex();

                        const auto statistics = ppp::win32::network::AddAllRoutes(rib_, gateway_interfaces);
                        LOG_INFO("Windows IPv4 routes: total=%llu, succeeded=%llu, failed=%llu, physical_gateway=%s, physical_ifindex=%d, tap_ifindex=%d",
                            (unsigned long long)statistics.Total,
                            (unsigned long long)statistics.Succeeded,
                            (unsigned long long)statistics.Failed,
                            underlying->GatewayServer.to_string().c_str(), underlying->Index,
                            tap->GetInterfaceIndex());
                        for (const auto& [error, count] : statistics.Errors) {
                            LOG_ERROR("Windows IPv4 route failures: error=%lu, count=%llu",
                                (unsigned long)error, (unsigned long long)count);
                        }
                    }
                    else {
                        LOG_ERROR("Windows IPv4 routes: underlying interface unavailable");
                    }
                }

                // Capture all IPv6 traffic on the VPN interface until the server
                // supplies managed IPv6 routes. These /1 routes outrank an ISP ::/0
                // even when Router Advertisement recreates it. More-specific China
                // bypass routes still use the underlying interface.
                // A TAP gateway is not proof that the server can carry IPv6: it
                // can be stale after reconnect, while a server without IPv6 sends
                // no IPv6 extension at all.  Only a successfully applied managed
                // IPv6 default route authorizes IPv6 egress.
                if (NULLPTR != tap &&
                    (!ipv6_client_state_captured_ || !ipv6_client_state_.DefaultRouteApplied)) {
                    // Remove active-store routes left by an older build or an
                    // unclean shutdown. Any prefix longer than /1 would otherwise
                    // override the leak block and expose the physical IPv6 address.
                    DeleteWindowsIPv6BypassRoutes();
                    ApplyWindowsIPv6LeakBlockRoutes();
                }
#elif defined(_MACOS)
                // Delete all found default gateway routes.
                if (auto underlying_ni = GetUnderlyingNetworkInterface(); NULLPTR != underlying_ni) {
                    if (auto tap = GetTap(); NULLPTR != tap) {
                        ppp::tap::TapDarwin* darwin_tap = dynamic_cast<ppp::tap::TapDarwin*>(tap.get());
                        if (NULLPTR != darwin_tap && !darwin_tap->IsPromisc()) {
                            if (UnixNetworkInterface* ni = dynamic_cast<UnixNetworkInterface*>(underlying_ni.get()); NULLPTR != ni) {
                                for (auto&& [ip, gw] : ni->DefaultRoutes) {
                                    ppp::darwin::tun::utun_del_route(ip, gw);
                                }
                            }
                        }
                    }

                    // Adds the loaded route table to the operating system.
                    ppp::tap::TapDarwin::AddAllRoutes(rib_);
                }
#else
                // Adds the loaded route table to the operating system.
                if (auto underlying_ni = GetUnderlyingNetworkInterface(); NULLPTR != underlying_ni) {
                    if (auto tap_ni = GetTapNetworkInterface(); NULLPTR != tap_ni) {
                        // Find and delete all default route information.
                        if (auto tap = GetTap(); NULLPTR != tap) {
                            // Find all default gateway routing lists and remove them, but only in non-promiscuous mode.
                            ppp::tap::TapLinux* linux_tap = dynamic_cast<ppp::tap::TapLinux*>(tap.get());
                            if (NULLPTR != linux_tap && !linux_tap->IsPromisc()) {
                                RouteInformationTablePtr default_routes = ppp::tap::TapLinux::FindAllDefaultGatewayRoutes({ tap->GatewayServer });
                                default_routes_ = default_routes;

                                // Delete all default route table information found.
                                if (NULLPTR != default_routes) {
                                    ppp::tap::TapLinux::DeleteAllRoutes(Linux_GetNetworkInterfaceName(tap, tap_ni, underlying_ni, nics_), default_routes);
                                }
                            }

                            // Add all routes configured in VPN/RIB to the operating system.
                            ppp::tap::TapLinux::AddAllRoutes(Linux_GetNetworkInterfaceName(tap, tap_ni, underlying_ni, nics_), rib_);
                        }
                    }
                }
#endif
                // IPv6 direct/bypass prefixes remain usable even when the server
                // is IPv4-only. The physical default route is suppressed, so only
                // these explicit more-specific routes can leave via the NIC.
                AddIPv6Route();

                // Configure the DNS servers used by the virtual network adapter to route to the operating system.
                AddRouteWithDnsServers();
            }

            bool VEthernetNetworkSwitcher::DeleteAllDefaultRoute() noexcept {
                if (auto tap = GetTap(); NULLPTR != tap) {
#if defined(_WIN32)
                    // Find and delete all disallowed windows gateway routes.
                    ppp::vector<MIB_IPFORWARDROW> default_routes;
                    ppp::win32::network::DeleteAllDefaultGatewayRoutes(default_routes, { tap->GatewayServer });
                    return true;
#else
#if defined(_MACOS)
                    auto unix_tap = dynamic_cast<ppp::tap::TapDarwin*>(tap.get());
#else
                    auto unix_tap = dynamic_cast<ppp::tap::TapLinux*>(tap.get());
#endif
                    if (NULLPTR != unix_tap && !unix_tap->IsPromisc()) {
#if defined(_MACOS)
                        // Find and delete all disallowed macos gateway routes.
                        auto rib = ppp::tap::TapDarwin::FindAllDefaultGatewayRoutes({ tap->GatewayServer }); 
                        if (NULLPTR != rib) {
                            for (auto&& [ip, gw] : *rib) {
                                ppp::darwin::tun::utun_del_route(ip, gw);
                            }
                        }
#else
                        // Find and delete all disallowed linux gateway routes.
                        auto rib = ppp::tap::TapLinux::FindAllDefaultGatewayRoutes({ tap->GatewayServer }); 
                        if (NULLPTR != rib) {
                            ppp::tap::TapLinux::DeleteAllRoutes2(rib);
                        }
#endif
                        return true;
                    }
#endif
                }
                return false;
            }

            void VEthernetNetworkSwitcher::DeleteRoute() noexcept {
                if (!geo_dynamic_routes_.empty()) {
                    ppp::vector<boost::asio::ip::address> addresses;
                    addresses.reserve(geo_dynamic_routes_.size());
                    for (const auto& entry : geo_dynamic_routes_) {
                        addresses.emplace_back(boost::asio::ip::address_v4(ntohl(entry.first)));
                    }
                    for (const boost::asio::ip::address& address : addresses) {
                        DeleteGeoDynamicRoute(address);
                    }
                }
                if (!geo_dynamic_routes6_.empty()) {
                    ppp::vector<boost::asio::ip::address> addresses;
                    addresses.reserve(geo_dynamic_routes6_.size());
                    for (const auto& entry : geo_dynamic_routes6_) {
                        boost::system::error_code ec;
                        boost::asio::ip::address address = StringToAddress(entry.first.data(), ec);
                        if (!ec && address.is_v6()) addresses.emplace_back(std::move(address));
                    }
                    for (const boost::asio::ip::address& address : addresses) {
                        DeleteGeoDynamicRoute(address);
                    }
                }
#if defined(_WIN32)
                DeleteWindowsIPv6BypassRoutes();
                RemoveWindowsIPv6LeakBlock();

                // Delete the loaded route table from the windows operating system.
                ppp::win32::network::DeleteAllRoutes(rib_);

                // Add and delete all windows default route information!
                ppp::win32::network::AddAllRoutes(default_routes_);

                // Force to set the network card gateway server, not just manually add the routing table, 
                // In the previous system can add routes, 
                // The system will automatically set the network card, but the latest WIN11 can not.
                if (std::shared_ptr<NetworkInterface> ni = underlying_ni_; NULLPTR != ni) {
                    ppp::win32::network::SetDefaultIPGateway(ni->Index, { ni->GatewayServer });
                }
#elif defined(_MACOS)
                // Delete the loaded route table from the osx operating system.
                if (auto underlying_ni = GetUnderlyingNetworkInterface(); NULLPTR != underlying_ni) {
                    // Delete all rib route table information found.
                    ppp::tap::TapDarwin::DeleteAllRoutes(rib_);

                    // Add and delete all os-x default route information!
                    if (auto tap = GetTap(); NULLPTR != tap) {
                        ppp::tap::TapDarwin* darwin_tap = dynamic_cast<ppp::tap::TapDarwin*>(tap.get());
                        if (NULLPTR != darwin_tap && !darwin_tap->IsPromisc()) {
                            if (UnixNetworkInterface* ni = dynamic_cast<UnixNetworkInterface*>(underlying_ni.get()); NULLPTR != ni) {
                                for (auto&& [ip, gw] : ni->DefaultRoutes) {
                                    ppp::darwin::tun::utun_add_route(ip, gw);
                                }
                            }
                        }
                    }
                }
#else
                // Delete the loaded route table from the linux operating system.
                if (auto underlying_ni = GetUnderlyingNetworkInterface(); NULLPTR != underlying_ni) {
                    if (auto tap_ni = GetTapNetworkInterface(); NULLPTR != tap_ni) {
                        if (auto tap = GetTap(); NULLPTR != tap) {
                            // Delete all rib route table information found.
                            ppp::tap::TapLinux::DeleteAllRoutes(Linux_GetNetworkInterfaceName(tap, tap_ni, underlying_ni, nics_), rib_);

                            // Add and delete all linux-t default route information!
                            if (auto default_routes = default_routes_; NULLPTR != default_routes) {
                                ppp::tap::TapLinux::AddAllRoutes(Linux_GetNetworkInterfaceName(tap, tap_ni, underlying_ni, nics_), default_routes);
                            }
                        }
                    }
                }
#endif

                // Fix and restore physical nic next hop route settings.
                FixUnderlyingNgw();

                // Delete all vpn dns server routes from the operating system.
                DeleteRouteWithDnsServers();
            }

            ppp::string VEthernetNetworkSwitcher::GetRemoteUri() noexcept {
                return server_ru_;
            }

            std::size_t VEthernetNetworkSwitcher::GetIPListCount() noexcept {
                LoadIPListFileVectorPtr ribs = ribs_;
                return NULLPTR != ribs ? ribs->size() : ip_list_count_;
            }

            std::size_t VEthernetNetworkSwitcher::GetIPList6Count() noexcept {
                LoadIPv6ListFileVectorPtr ribs6 = ribs6_;
                return NULLPTR != ribs6 ? ribs6->size() : ip_list6_count_;
            }

            void VEthernetNetworkSwitcher::PreferredNic(const ppp::string& nic) noexcept {
                preferred_nic_ = nic;
            }

            void VEthernetNetworkSwitcher::PreferredNgw(const boost::asio::ip::address& gw) noexcept {
                preferred_ngw_ = gw;
            }

            void VEthernetNetworkSwitcher::PreferredNgw6(const boost::asio::ip::address& gw6) noexcept {
                preferred_ngw6_ = gw6;
            }

            bool VEthernetNetworkSwitcher::AddLoadIPList(
                const ppp::string&                                              path, 
#if defined(_LINUX) 
                const ppp::string&                                              nic,
#endif  
                const boost::asio::ip::address&                                 gw,
                const ppp::string&                                              url) noexcept {

                using File = ppp::io::File;

                if (path.empty()) {
                    return false;
                }

                ppp::string fullpath = File::RewritePath(path.data());
                if (fullpath.empty()) {
                    return false;
                }

                fullpath = File::GetFullPath(path.data());
                if (fullpath.empty()) {
                    return false;
                }

                if (!File::Exists(fullpath.data())) {
                    return false;
                }
                
                uint32_t ngw = IPEndPoint::AnyAddress;
                if (
#if defined(_LINUX) 
                    !nic.empty() && 
#endif
                    gw.is_v4() && !IPEndPoint::IsInvalid(gw)) {
                    ngw = htonl(gw.to_v4().to_uint());
                }

                LoadIPListFileVectorPtr ribs = ribs_;
                if (NULLPTR == ribs) {
                    ribs = make_shared_object<LoadIPListFileVector>();
                    ribs_ = ribs;
                }

                if (NULLPTR == ribs) {
                    return false;
                }
                else {
                    auto tail = std::find_if(ribs->begin(), ribs->end(),
                        [&fullpath](const std::pair<ppp::string, uint32_t>& i) noexcept {
                            return i.first == fullpath;
                        });
                    if (tail != ribs->end()) {
                        return false;
                    }
                }

#if defined(_LINUX) 
                if (ngw != IPEndPoint::AnyAddress) {
                    nics_.emplace(std::make_pair(ngw, nic));
                }
#endif
                
                ribs->emplace_back(std::make_pair(fullpath, ngw));
                ip_list_count_ = ribs->size();
                return true;
            }

            bool VEthernetNetworkSwitcher::AddLoadIPList6(
                const ppp::string&                                              path, 
#if defined(_LINUX) 
                const ppp::string&                                              nic,
#endif  
                const boost::asio::ip::address&                                 gw6,
                const ppp::string&                                              url) noexcept {

                using File = ppp::io::File;

                if (path.empty()) {
                    return false;
                }

                ppp::string fullpath = File::RewritePath(path.data());
                if (fullpath.empty()) {
                    return false;
                }

                fullpath = File::GetFullPath(path.data());
                if (fullpath.empty()) {
                    return false;
                }

                if (!File::Exists(fullpath.data())) {
                    return false;
                }

                boost::asio::ip::address ngw6 = boost::asio::ip::address_v6::any();
                if (gw6.is_v6() && !IPEndPoint::IsInvalid(gw6)) {
                    ngw6 = gw6;
                }

                LoadIPv6ListFileVectorPtr ribs6 = ribs6_;
                if (NULLPTR == ribs6) {
                    ribs6 = make_shared_object<LoadIPv6ListFileVector>();
                    ribs6_ = ribs6;
                }

                if (NULLPTR == ribs6) {
                    return false;
                }
                else {
                    auto tail = std::find_if(ribs6->begin(), ribs6->end(),
                        [&fullpath](const std::pair<ppp::string, boost::asio::ip::address>& i) noexcept {
                            return i.first == fullpath;
                        });
                    if (tail != ribs6->end()) {
                        return false;
                    }
                }

#if defined(_LINUX) 
                if (!ngw6.is_unspecified()) {
                    nics6_.emplace(std::make_pair(ngw6.to_string(), nic));
                }
#endif
                
                ribs6->emplace_back(std::make_pair(fullpath, ngw6));
                ip_list6_count_ = ribs6->size();
                return true;
            }

            bool VEthernetNetworkSwitcher::LoadAllIPListWithFilePaths(const boost::asio::ip::address& gw) noexcept {
                rib_ = NULLPTR;
                fib_ = NULLPTR;

                // Load all the route table iplist configuration files that need to be loaded.
                bool any = false;
                if (gw.is_v4()) {
                    // Obtain the numerical address of the next hop in the IP route table, which is a function implementation of the bypass-iplist.
                    boost::asio::ip::address_v4 in = gw.to_v4();
                    if (uint32_t next_hop = htonl(in.to_uint()); !IPEndPoint::IsInvalid(in)) {
                        if (LoadIPListFileVectorPtr ribs = std::move(ribs_); NULLPTR != ribs) {
                            // Loop in all iplist route table configuration files.
                            RouteInformationTablePtr rib = make_shared_object<RouteInformationTable>();
                            if (NULLPTR != rib) {
                                for (auto&& kv : *ribs) {
                                    const ppp::string& path = kv.first;
                                    const uint32_t ngw = kv.second != IPEndPoint::AnyAddress ? kv.second : next_hop;
                                    any |= rib->AddAllRoutesByIPList(path, ngw);
                                }

                                // Loading is considered valid only if any route is added.
                                if (any) {
                                    rib_ = rib;
                                }
                            }
                        }
                    }
                }

                // A value filled once can only be used once and then reset.
                ribs_.reset();
                return any;
            }

            bool VEthernetNetworkSwitcher::LoadAllIPListWithFilePaths6(const boost::asio::ip::address& gw6) noexcept {
                rib6_ = NULLPTR;

                bool any = false;
                // Use gw6 as a fallback next-hop when an entry's gateway is unspecified.
                boost::asio::ip::address_v6 next_hop6;
                if (gw6.is_v6()) {
                    next_hop6 = gw6.to_v6();
                }

                if (LoadIPv6ListFileVectorPtr ribs6 = std::move(ribs6_); NULLPTR != ribs6) {
                    IPv6RouteTablePtr rib6 = make_shared_object<IPv6RouteTable>();
                    if (NULLPTR != rib6) {
                        for (auto&& kv : *ribs6) {
                            const ppp::string& path = kv.first;
                            const boost::asio::ip::address& ngw6_addr = kv.second;

                            // Use the entry's own gateway if specified; fall back to gw6 parameter.
                            boost::asio::ip::address_v6 ngw6;
                            if (ngw6_addr.is_v6() && !IPEndPoint::IsInvalid(ngw6_addr)) {
                                ngw6 = ngw6_addr.to_v6();
                            }
                            else if (!next_hop6.is_unspecified()) {
                                ngw6 = next_hop6;
                            }
                            else {
                                ngw6 = boost::asio::ip::address_v6::any();
                            }

                            // Read and parse the IPv6 CIDR file
                            ppp::string text = ppp::io::File::ReadAllText(path.c_str());
                            if (text.empty()) {
                                continue;
                            }

                            // Tokenize by newlines
                            ppp::vector<ppp::string> lines;
                            ppp::Tokenize<ppp::string>(text, lines, "\r\n");
                            if (lines.empty()) {
                                ppp::Tokenize<ppp::string>(text, lines, "\n");
                            }

                            for (const ppp::string& line : lines) {
                                ppp::string cidr = ppp::LTrim(ppp::RTrim(line));
                                if (cidr.empty()) {
                                    continue;
                                }

                                // Skip comments
                                if (cidr[0] == '#' || cidr[0] == ';') {
                                    continue;
                                }

                                std::string host;
                                int prefix = -1;

                                std::size_t i = cidr.find('/');
                                if (i == ppp::string::npos) {
                                    host = cidr;
                                }
                                else {
                                    if (i == 0) {
                                        continue;
                                    }

                                    host = cidr.substr(0, i);
                                    prefix = atoi(cidr.data() + (i + 1));
                                }

                                boost::system::error_code ec;
                                boost::asio::ip::address ip = ppp::StringToAddress(host, ec);
                                if (ec) {
                                    continue;
                                }

                                if (!ip.is_v6()) {
                                    continue;
                                }

                                if (prefix < 0) {
                                    prefix = 128;
                                }
                                elif (prefix > 128) {
                                    continue;
                                }

                                IPv6RouteEntry entry;
                                entry.Network = ip.to_v6();
                                entry.Prefix = prefix;
                                entry.NextHop = ngw6;
                                rib6->emplace_back(entry);
                                any = true;
                            }
                        }

                        if (any) {
                            rib6_ = rib6;
                        }
                    }
                }

                ribs6_.reset();
                return any;
            }

            void VEthernetNetworkSwitcher::AddIPv6Route() noexcept {
                if (IPv6RouteTablePtr rib6 = rib6_; NULLPTR == rib6) {
                    return;
                }

#if !defined(_ANDROID) && !defined(_IPHONE)
                for (const IPv6RouteEntry& entry : *rib6_) {
                    // Resolve the next-hop: use entry's own gateway; if unspecified, try current underlying NIC's IPv6 gateway.
                    boost::asio::ip::address_v6 next_hop6 = entry.NextHop;
                    if (next_hop6.is_unspecified()) {
                        if (auto underlying_ni = underlying_ni_; NULLPTR != underlying_ni) {
                            boost::asio::ip::address fallback6 = underlying_ni->IPv6GatewayServer;
                            if (fallback6.is_v6() && !fallback6.is_unspecified() && !IPEndPoint::IsInvalid(fallback6)) {
                                next_hop6 = fallback6.to_v6();
                            }
                        }
                    }

                    // Skip entries whose next-hop could not be resolved.
                    if (next_hop6.is_unspecified()) {
                        continue;
                    }

                    std::string cidr = entry.Network.to_string() + "/" + std::to_string(entry.Prefix);
                    std::string ngw6_str = next_hop6.to_string();

#if defined(_WIN32)
                    // Windows: use IP Helper API (CreateIpv6ForwardEntry) to add IPv6 route silently
                    {
                        (void)cidr;
                        (void)ngw6_str;
                        int interface_index = -1;
                        if (auto underlying_ni = GetUnderlyingNetworkInterface(); NULLPTR != underlying_ni) {
                            interface_index = underlying_ni->Index;
                        }
                        if (interface_index >= 0) {
                            ppp::win32::network::Router::AddIPv6RouteEntry(entry.Network, entry.Prefix, next_hop6, interface_index);
                        }
                    }
#elif defined(_MACOS)
                    // macOS: use route -n add -inet6
                    std::string cmd = "route -n add -inet6 " + cidr + " " + ngw6_str + " 2>/dev/null";
                    system(cmd.c_str());
#else
                    // Linux: use ip -6 route add via with dev
                    ppp::string ifname6;
                    if (auto underlying_ni = GetUnderlyingNetworkInterface(); NULLPTR != underlying_ni) {
                        // Look up the interface from nics6_ using the ngw6 string, fall back to underlying NIC name
                        ppp::string mapped_nic;
                        if (Dictionary::TryGetValue(nics6_, ppp::string(ngw6_str.c_str()), mapped_nic) && !mapped_nic.empty()) {
                            ifname6 = mapped_nic;
                        }
                        else {
                            ifname6 = underlying_ni->Name;
                        }
                    }
                    std::string cmd = "ip -6 route add " + cidr + " via " + ngw6_str;
                    if (!ifname6.empty()) {
                        cmd += " dev " + ifname6;
                    }
                    cmd += " 2>/dev/null";
                    system(cmd.c_str());
#endif
                }
#endif
            }

            bool VEthernetNetworkSwitcher::RefreshDirectDnsServers() noexcept {
                direct_dns_servers_.clear();
                direct_dns_server_index_.store(0);
                if (!geo_rules_) {
                    return true;
                }

                auto append = [this](boost::asio::ip::address address) noexcept {
                    if (IPEndPoint::IsInvalid(address) || address.is_unspecified() ||
                        address.is_multicast() || address.is_loopback()) {
                        return;
                    }
#if defined(_WIN32)
                    if (address.is_v6() && address.to_v6().is_link_local() &&
                        address.to_v6().scope_id() == 0 && underlying_ni_ && underlying_ni_->Index >= 0) {
                        auto scoped = address.to_v6();
                        scoped.scope_id(static_cast<unsigned long>(underlying_ni_->Index));
                        address = scoped;
                    }
#endif
                    if (std::find(direct_dns_servers_.begin(), direct_dns_servers_.end(), address) == direct_dns_servers_.end()) {
                        direct_dns_servers_.emplace_back(std::move(address));
                    }
                };

                if (geo_rules_->UsesLocalDirectDns()) {
                    if (underlying_ni_) {
                        for (const auto& address : underlying_ni_->DnsAddresses) {
                            append(address);
                        }
                    }
                }
                for (const auto& address : geo_rules_->GetDirectDnsServers()) {
                    append(address);
                }

                for (const auto& address : direct_dns_servers_) {
                    LOG_INFO("Direct DNS: server=%s, source=%s",
                        address.to_string().data(),
                        geo_rules_->UsesLocalDirectDns() && underlying_ni_ &&
                            std::find(underlying_ni_->DnsAddresses.begin(), underlying_ni_->DnsAddresses.end(), address) != underlying_ni_->DnsAddresses.end()
                            ? "local" : "configured");
                }
                if (geo_rules_->UsesLocalDirectDns() && direct_dns_servers_.empty()) {
                    LOG_ERROR("Direct DNS: direct_dns=local but the selected physical interface has no usable IPv4/IPv6 DNS server");
                    return false;
                }
                return true;
            }

            bool VEthernetNetworkSwitcher::SelectDirectDnsServer(
                const ppp::string& host, boost::asio::ip::address& server) noexcept {
                if (!geo_rules_ || direct_dns_servers_.empty()) {
                    return false;
                }
                const auto decision = geo_rules_->MatchDomain(host);
                if (decision.action != ppp::app::client::geo::GeoRuleEngine::Action::Direct) {
                    return false;
                }
                const size_t index = direct_dns_server_index_.fetch_add(1) % direct_dns_servers_.size();
                server = direct_dns_servers_[index];
                return true;
            }

            bool VEthernetNetworkSwitcher::ApplyGeoStaticRoutes() noexcept {
                if (!geo_rules_) {
                    return true;
                }

                std::shared_ptr<ITap> tap = GetTap();
                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (!tap || !underlying || !underlying->GatewayServer.is_v4()) {
                    return false;
                }

                if (!rib_) {
                    rib_ = make_shared_object<RouteInformationTable>();
                }
                if (!rib_) {
                    return false;
                }

                uint32_t direct_gw = htonl(underlying->GatewayServer.to_v4().to_uint());
                uint32_t tunnel_gw = tap->GatewayServer;
                ppp::unordered_set<ppp::string> installed;
                size_t applied = 0;
                for (const auto& network : geo_rules_->GetStaticNetworks()) {
                    ppp::string key = ppp::net::Ipep::ToAddressString<ppp::string>(network.address) +
                        "/" + stl::to_string<ppp::string>(network.prefix);
                    if (!installed.emplace(key).second) {
                        continue; // First rule owns an identical prefix.
                    }

                    if (network.address.is_v4()) {
                        uint32_t ip = htonl(network.address.to_v4().to_uint());
                        uint32_t gateway = network.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct ?
                            direct_gw : tunnel_gw;
                        if (rib_->AddRoute(ip, network.prefix, gateway)) applied++;
                    }
                    else if (network.address.is_v6()) {
                        boost::asio::ip::address_v6 gateway;
                        if (network.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct) {
                            if (underlying->IPv6GatewayServer.is_v6() && !underlying->IPv6GatewayServer.is_unspecified()) {
                                gateway = underlying->IPv6GatewayServer.to_v6();
                            }
                        }
                        else if (tap->IPv6GatewayServer.is_v6() && !tap->IPv6GatewayServer.is_unspecified()) {
                            gateway = tap->IPv6GatewayServer.to_v6();
                        }

                        if (!gateway.is_unspecified()) {
                            if (!rib6_) rib6_ = make_shared_object<IPv6RouteTable>();
                            if (rib6_) {
                                IPv6RouteEntry entry;
                                entry.Network = network.address.to_v6();
                                entry.Prefix = network.prefix;
                                entry.NextHop = gateway;
                                rib6_->emplace_back(std::move(entry));
                                applied++;
                            }
                        }
                    }
                }

                LOG_INFO("VEthernetNetworkSwitcher::ApplyGeoStaticRoutes: networks=%llu, applied=%llu",
                    (unsigned long long)geo_rules_->GetStaticNetworks().size(), (unsigned long long)applied);
                return true;
            }

            void VEthernetNetworkSwitcher::AddGeoDynamicRoute(
                const ppp::app::client::geo::GeoRuleEngine::RouteUpdate& update) noexcept {
                if (update.address.is_v6()) {
                    // Install a /128 for both actions so a higher-priority domain
                    // rule can override a broader static GeoIP route.
                    bool direct = update.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                    std::shared_ptr<NetworkInterface> target = direct ?
                        GetUnderlyingNetworkInterface() : GetTapNetworkInterface();
                    if (!target) return;
                    ppp::string address = ppp::net::Ipep::ToAddressString<ppp::string>(update.address);
#if defined(_WIN32)
                    if (!ipv6_client_state_.DefaultRouteApplied && !direct) {
                        // Without a managed server IPv6 dataplane, only explicit
                        // direct decisions may create physical /128 routes.
                        if (geo_dynamic_routes6_.find(address) != geo_dynamic_routes6_.end()) {
                            DeleteGeoDynamicRoute(update.address);
                        }
                        LOG_DEBUG("VEthernetNetworkSwitcher::AddGeoDynamicRoute: suppressing tunnel IPv6 address=%s; server IPv6 is unavailable",
                            address.data());
                        return;
                    }
#endif
                    ppp::string gateway;
                    if (direct) {
                        if (!target->IPv6GatewayServer.is_v6() || target->IPv6GatewayServer.is_unspecified()) return;
                        gateway = target->IPv6GatewayServer.to_string();
                    }
                    else if (target->IPv6GatewayServer.is_v6() && !target->IPv6GatewayServer.is_unspecified()) {
                        gateway = target->IPv6GatewayServer.to_string();
                    }

                    GeoDynamicRoute6 state;
                    state.gateway = gateway;
                    state.interface_index = target->Index;
#if !defined(_WIN32)
                    state.interface_name = target->Name;
#endif
                    auto existing = geo_dynamic_routes6_.find(address);
                    if (existing != geo_dynamic_routes6_.end()) {
                        if (existing->second.gateway == state.gateway &&
                            existing->second.interface_index == state.interface_index &&
                            existing->second.interface_name == state.interface_name) return;
                        DeleteGeoDynamicRoute(update.address);
                    }

                    bool added = false;
#if defined(_WIN32)
                    if (state.interface_index >= 0) {
                        ppp::win32::network::DeleteIPv6Route(
                            state.interface_index, address, 128, gateway);
                        added = ppp::win32::network::AddIPv6Route(
                            state.interface_index, address, 128, gateway, 0);
                    }
#elif defined(_MACOS)
                    std::string delete_command = "route -n delete -inet6 " + std::string(address.data()) + "/128 ";
                    if (!gateway.empty()) delete_command += std::string(gateway.data());
                    else delete_command += "-interface " + std::string(state.interface_name.data());
                    delete_command += " >/dev/null 2>&1";
                    system(delete_command.c_str());
                    std::string command = "route -n add -inet6 " + std::string(address.data()) + "/128 ";
                    if (!gateway.empty()) command += std::string(gateway.data());
                    else command += "-interface " + std::string(state.interface_name.data());
                    command += " >/dev/null 2>&1";
                    added = system(command.c_str()) == 0;
#else
                    added = ppp::tap::TapLinux::AddRoute6(state.interface_name, address, 128, gateway);
#endif
                    if (added) {
                        geo_dynamic_routes6_[address] = std::move(state);
                        LOG_DEBUG("VEthernetNetworkSwitcher::AddGeoDynamicRoute: IPv6 address=%s, action=%s, outbound=%s, priority=%llu",
                            address.data(), direct ? "direct" : "tunnel", update.outbound.data(),
                            (unsigned long long)update.priority);
                    }
                    return;
                }
                if (!update.address.is_v4()) {
                    return;
                }

                std::shared_ptr<ITap> tap = GetTap();
                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (!tap || !underlying || !underlying->GatewayServer.is_v4()) {
                    return;
                }

                uint32_t ip = htonl(update.address.to_v4().to_uint());
                uint32_t gateway = update.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct ?
                    htonl(underlying->GatewayServer.to_v4().to_uint()) : tap->GatewayServer;
                auto existing = geo_dynamic_routes_.find(ip);
                if (existing != geo_dynamic_routes_.end()) {
                    if (existing->second == gateway) {
                        return;
                    }
                    DeleteGeoDynamicRoute(update.address);
                }
                if (AddRoute(ip, gateway, 32)) {
                    geo_dynamic_routes_[ip] = gateway;
                    LOG_DEBUG("VEthernetNetworkSwitcher::AddGeoDynamicRoute: address=%s, action=%s, outbound=%s, priority=%llu",
                        update.address.to_string().data(),
                        update.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct ? "direct" : "tunnel",
                        update.outbound.data(),
                        (unsigned long long)update.priority);
                }
            }

#if defined(_WIN32)
            bool VEthernetNetworkSwitcher::EnsureWindowsIPv4ServerRoute(
                const boost::asio::ip::address& address) noexcept {
                if (!address.is_v4() || address.is_unspecified() || address.is_loopback()) {
                    return false;
                }

                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (NULLPTR == underlying || underlying->Index < 0 ||
                    !underlying->GatewayServer.is_v4() || underlying->GatewayServer.is_unspecified()) {
                    LOG_ERROR("VEthernetNetworkSwitcher::EnsureWindowsIPv4ServerRoute: physical IPv4 gateway unavailable, remote=%s",
                        address.to_string().c_str());
                    return false;
                }

                const uint32_t remote = htonl(address.to_v4().to_uint());
                const uint32_t gateway = htonl(underlying->GatewayServer.to_v4().to_uint());
                const uint32_t mask = IPEndPoint::PrefixToNetmask(32);

                SynchronizedObjectScope scope(GetSynchronizedObject());
                for (const IPv4ServerRoute& route : ipv4_server_routes_) {
                    if (route.address == remote && route.gateway == gateway &&
                        route.interface_index == underlying->Index) {
                        return true;
                    }
                }

                if (auto table = ppp::win32::network::Router::GetIpForwardTable(); NULLPTR != table) {
                    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                        const MIB_IPFORWARDROW& route = table->table[i];
                        if (route.dwForwardDest == remote && route.dwForwardMask == mask &&
                            route.dwForwardNextHop == gateway &&
                            static_cast<int>(route.dwForwardIfIndex) == underlying->Index) {
                            ipv4_server_routes_.emplace_back(IPv4ServerRoute{
                                remote, gateway, underlying->Index, false });
                            return true;
                        }
                    }
                }

                if (!ppp::win32::network::Router::Add(
                        remote, mask, gateway, 1, underlying->Index)) {
                    LOG_ERROR("VEthernetNetworkSwitcher::EnsureWindowsIPv4ServerRoute: add /32 failed, remote=%s, gateway=%s, ifindex=%d",
                        address.to_string().c_str(), underlying->GatewayServer.to_string().c_str(),
                        underlying->Index);
                    return false;
                }

                ipv4_server_routes_.emplace_back(IPv4ServerRoute{
                    remote, gateway, underlying->Index, true });
                LOG_INFO("VEthernetNetworkSwitcher::EnsureWindowsIPv4ServerRoute: pinned remote=%s/32, gateway=%s, ifindex=%d, created=1",
                    address.to_string().c_str(), underlying->GatewayServer.to_string().c_str(),
                    underlying->Index);
                return true;
            }

            void VEthernetNetworkSwitcher::RemoveWindowsIPv4ServerRoutes() noexcept {
                ppp::vector<IPv4ServerRoute> routes;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    routes = std::move(ipv4_server_routes_);
                    ipv4_server_routes_.clear();
                }

                auto table = ppp::win32::network::Router::GetIpForwardTable();
                if (NULLPTR == table) {
                    return;
                }
                const uint32_t mask = IPEndPoint::PrefixToNetmask(32);
                for (const IPv4ServerRoute& route : routes) {
                    if (route.owned && route.interface_index >= 0) {
                        ppp::win32::network::Router::Delete(
                            table, route.address, mask, route.gateway, route.interface_index);
                    }
                }
            }

            bool VEthernetNetworkSwitcher::EnsureWindowsIPv6ServerRoute(
                const boost::asio::ip::address& address) noexcept {
                if (!address.is_v6() || address.is_unspecified() ||
                    address.is_loopback() || address.to_v6().is_link_local()) {
                    return false;
                }

                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (NULLPTR == underlying || underlying->Index < 0 ||
                    !underlying->IPv6GatewayServer.is_v6() ||
                    underlying->IPv6GatewayServer.is_unspecified()) {
                    LOG_ERROR("VEthernetNetworkSwitcher::EnsureWindowsIPv6ServerRoute: physical IPv6 gateway unavailable, remote=%s",
                        address.to_string().c_str());
                    return false;
                }

                SynchronizedObjectScope scope(GetSynchronizedObject());
                for (const IPv6ServerRoute& route : ipv6_server_routes_) {
                    if (route.address == address &&
                        route.interface_index == underlying->Index &&
                        route.gateway == underlying->IPv6GatewayServer) {
                        return true;
                    }
                }

                bool created = false;
                if (!ppp::win32::network::Router::AddIPv6RouteEntry(
                        address.to_v6(), 128, underlying->IPv6GatewayServer.to_v6(),
                        underlying->Index, &created)) {
                    LOG_ERROR("VEthernetNetworkSwitcher::EnsureWindowsIPv6ServerRoute: add /128 failed, remote=%s, gateway=%s, ifindex=%d",
                        address.to_string().c_str(),
                        underlying->IPv6GatewayServer.to_string().c_str(),
                        underlying->Index);
                    return false;
                }

                ipv6_server_routes_.emplace_back(IPv6ServerRoute{
                    address, underlying->IPv6GatewayServer, underlying->Index, created });
                LOG_INFO("VEthernetNetworkSwitcher::EnsureWindowsIPv6ServerRoute: pinned remote=%s/128, gateway=%s, ifindex=%d, created=%d",
                    address.to_string().c_str(),
                    underlying->IPv6GatewayServer.to_string().c_str(),
                    underlying->Index, static_cast<int>(created));
                return true;
            }

            void VEthernetNetworkSwitcher::RemoveWindowsIPv6ServerRoutes() noexcept {
                ppp::vector<IPv6ServerRoute> routes;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    routes = std::move(ipv6_server_routes_);
                    ipv6_server_routes_.clear();
                }
                for (const IPv6ServerRoute& route : routes) {
                    if (!route.owned || route.interface_index < 0 || !route.address.is_v6()) {
                        continue;
                    }
                    ppp::win32::network::DeleteIPv6Route(
                        route.interface_index,
                        route.address.to_string().c_str(),
                        128,
                        route.gateway.is_v6()
                            ? ppp::string(route.gateway.to_string().c_str())
                            : ppp::string());
                }
            }

            void VEthernetNetworkSwitcher::RemoveWindowsIPv6LeakBlock() noexcept {
                // Delete unconditionally so a stale active-store route from an
                // abnormal earlier process does not depend on an in-memory flag.
                if (std::shared_ptr<ITap> tap = GetTap(); NULLPTR != tap) {
                    const int interface_index = tap->GetInterfaceIndex();
                    ppp::win32::network::DeleteIPv6Route(interface_index, "::", 1, ppp::string());
                    ppp::win32::network::DeleteIPv6Route(interface_index, "8000::", 1, ppp::string());
                }
                ipv6_block_routes_added_ = false;
                if (ipv6_block_prefix_policy_applied_) {
                    ppp::win32::network::RestoreIPv6PrefixPolicyULA();
                    ipv6_block_prefix_policy_applied_ = false;
                }
                // Remove compatibility filters from earlier policy revisions.
                // The selective policy below is enforced by suppressing only the
                // physical default route, so direct geo prefixes remain usable.
                ppp::win32::network::Fw::SetIPv6LeakBlock("openppp2 IPv6 Leak Block", NULLPTR, false);

                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (NULLPTR != underlying && underlying->Index >= 0 &&
                    ipv6_ignore_default_routes_captured_) {
                    ppp::win32::network::Router::SetIPv6IgnoreDefaultRoutes(
                        underlying->Index, ipv6_original_ignore_default_routes_);
                }
                if (!ipv6_original_ignore_default_routes_) {
                    const int restored = ppp::win32::network::Router::RestoreIPv6Routes(
                        ipv6_physical_default_routes_);
                    if (restored > 0) {
                        LOG_INFO("VEthernetNetworkSwitcher::RemoveWindowsIPv6LeakBlock: restored physical IPv6 defaults=%d",
                            restored);
                    }
                }
                ipv6_physical_default_routes_.clear();
                ipv6_ignore_default_routes_captured_ = false;
                ipv6_original_ignore_default_routes_ = false;
                ipv6_physical_default_block_applied_ = false;
                if (network_takeover_stopping_.load()) {
                    RemoveWindowsIPv4ServerRoutes();
                    RemoveWindowsIPv6ServerRoutes();
                }
            }

            int VEthernetNetworkSwitcher::DeleteWindowsIPv6BypassRoutes() noexcept {
                IPv6RouteTablePtr rib6 = rib6_;
                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (NULLPTR == rib6 || rib6->empty() || NULLPTR == underlying || underlying->Index < 0) {
                    return 0;
                }

                ppp::vector<std::pair<boost::asio::ip::address_v6, int>> routes;
                routes.reserve(rib6->size());
                for (const IPv6RouteEntry& entry : *rib6) {
                    routes.emplace_back(entry.Network, entry.Prefix);
                }

                const int deleted = ppp::win32::network::Router::DeleteIPv6RouteEntries(routes, underlying->Index);
                if (deleted < 0) {
                    LOG_ERROR("VEthernetNetworkSwitcher::DeleteWindowsIPv6BypassRoutes: route table query failed, ifindex=%d",
                        underlying->Index);
                }
                elif(deleted > 0) {
                    LOG_INFO("VEthernetNetworkSwitcher::DeleteWindowsIPv6BypassRoutes: removed=%d, ifindex=%d",
                        deleted, underlying->Index);
                }
                return deleted;
            }

            bool VEthernetNetworkSwitcher::ApplyWindowsIPv6LeakBlockRoutes() noexcept {
                std::shared_ptr<ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                const int interface_index = tap->GetInterfaceIndex();
                std::shared_ptr<NetworkInterface> underlying = GetUnderlyingNetworkInterface();
                if (interface_index < 0 || NULLPTR == underlying || underlying->Index < 0) {
                    return false;
                }

                // Two /1 routes are more specific than any physical ::/0.  They
                // deliberately terminate on the TUN while no server IPv6 data
                // plane exists, so Windows cannot fall back to a global address on
                // the physical NIC.  A successful IPv6 assignment removes them
                // before installing its own routes.
                ppp::win32::network::DeleteIPv6Route(interface_index, "::", 1, ppp::string());
                ppp::win32::network::DeleteIPv6Route(interface_index, "8000::", 1, ppp::string());
                const bool left = ppp::win32::network::AddIPv6Route(interface_index, "::", 1, ppp::string(), 0);
                const bool right = ppp::win32::network::AddIPv6Route(interface_index, "8000::", 1, ppp::string(), 0);
                ipv6_block_routes_added_ = left && right;

                // A /1 sink alone is insufficient: Windows may select the physical
                // GUA source and then its physical ::/0. Preserve direct IPv6 by
                // keeping all more-specific geo routes, while disabling only the
                // selected physical interface's default-route acquisition.
                if (!ipv6_ignore_default_routes_captured_) {
                    bool original = false;
                    if (!ppp::win32::network::Router::GetIPv6IgnoreDefaultRoutes(
                            underlying->Index, original)) {
                        LOG_ERROR("VEthernetNetworkSwitcher::ApplyWindowsIPv6LeakBlockRoutes: cannot read IgnoreDefaultRoutes, ifindex=%d",
                            underlying->Index);
                        return false;
                    }
                    ipv6_original_ignore_default_routes_ = original;
                    ipv6_ignore_default_routes_captured_ = true;
                }
                const bool ignore_ok = ppp::win32::network::Router::SetIPv6IgnoreDefaultRoutes(
                    underlying->Index, true);
                const int removed_defaults =
                    ppp::win32::network::Router::CaptureAndDeleteIPv6DefaultRoutes(
                        underlying->Index, ipv6_physical_default_routes_);
                ipv6_physical_default_block_applied_ = ignore_ok && removed_defaults >= 0;

                // Remove the old all-public-IPv6 rule if upgrading/reapplying in
                // the same process. It would also block intentional domestic routes.
                ppp::win32::network::Fw::SetIPv6LeakBlock(
                    "openppp2 IPv6 Leak Block", NULLPTR, false);
                if (!ipv6_block_routes_added_) {
                    ppp::win32::network::DeleteIPv6Route(interface_index, "::", 1, ppp::string());
                    ppp::win32::network::DeleteIPv6Route(interface_index, "8000::", 1, ppp::string());
                    LOG_ERROR("VEthernetNetworkSwitcher::ApplyWindowsIPv6LeakBlockRoutes: failed, ifindex=%d", interface_index);
                }
                elif(!ipv6_physical_default_block_applied_) {
                    LOG_ERROR("VEthernetNetworkSwitcher::ApplyWindowsIPv6LeakBlockRoutes: physical default suppression failed, ifindex=%d",
                        underlying->Index);
                }
                else {
                    LOG_INFO("VEthernetNetworkSwitcher::ApplyWindowsIPv6LeakBlockRoutes: installed selective block on tun=%d, physical=%s, removed_defaults=%d, direct_ipv6=allowed",
                        interface_index, underlying->Name.data(), removed_defaults);
                }
                return ipv6_block_routes_added_ && ipv6_physical_default_block_applied_;
            }

#endif

            void VEthernetNetworkSwitcher::DeleteGeoDynamicRoute(const boost::asio::ip::address& address) noexcept {
                if (address.is_v6()) {
                    ppp::string key = ppp::net::Ipep::ToAddressString<ppp::string>(address);
                    auto existing6 = geo_dynamic_routes6_.find(key);
                    if (existing6 == geo_dynamic_routes6_.end()) return;

                    GeoDynamicRoute6 state = existing6->second;
#if defined(_WIN32)
                    if (state.interface_index >= 0) {
                        ppp::win32::network::DeleteIPv6Route(state.interface_index, key, 128, state.gateway);
                    }
#elif defined(_MACOS)
                    std::string command = "route -n delete -inet6 " + std::string(key.data()) + "/128 ";
                    if (!state.gateway.empty()) command += std::string(state.gateway.data());
                    else command += "-interface " + std::string(state.interface_name.data());
                    command += " >/dev/null 2>&1";
                    system(command.c_str());
#else
                    ppp::tap::TapLinux::DeleteRoute6(state.interface_name, key, 128, state.gateway);
#endif
                    geo_dynamic_routes6_.erase(existing6);
                    return;
                }
                if (!address.is_v4()) {
                    return;
                }
                uint32_t ip = htonl(address.to_v4().to_uint());
                auto existing = geo_dynamic_routes_.find(ip);
                if (existing == geo_dynamic_routes_.end()) {
                    return;
                }
                uint32_t gateway = existing->second;
#if defined(_WIN32)
                MIB_IPFORWARDROW route;
                if (ppp::win32::network::Router::GetBestRoute(ip, route) &&
                    route.dwForwardDest == ip && route.dwForwardMask == 0xffffffffu && route.dwForwardNextHop == gateway) {
                    ppp::win32::network::Router::Delete(route);
                }
#else
                DeleteRoute(ip, gateway, 32);
#endif
                geo_dynamic_routes_.erase(existing);
            }

            void VEthernetNetworkSwitcher::ObserveGeoDnsResponse(const void* packet, int packet_size) noexcept {
                if (!geo_rules_) {
                    return;
                }
                ppp::vector<ppp::app::client::geo::GeoRuleEngine::RouteUpdate> updates;
                if (geo_rules_->ObserveDnsResponse(packet, packet_size,
                    ppp::threading::Executors::GetTickCount(), updates)) {
                    LOG_INFO("GeoSite DNS policy learned: routes=%llu",
                        (unsigned long long)updates.size());
                    for (const auto& update : updates) {
                        AddGeoDynamicRoute(update);
                    }
                }
            }

            void VEthernetNetworkSwitcher::AddRouteWithDnsServers() noexcept {
                // Clear the current cached dns server ip address list.
                for (auto& dns_servers : dns_serverss_) {
                    dns_servers.clear();
                }

                // Obtain the IP address list of the DNS server configured on the current physical bearer NIC and VPN virtual network adapter.
                // DNS servers that belong to the TAP virtual subnet (such as the virtual gateway)
                // must never be pinned through the physical gateway; doing so would install a
                // /32 host route that hijacks virtual gateway traffic away from the TAP adapter.
                uint32_t tap_virtual_network = IPEndPoint::AnyAddress;
                uint32_t tap_virtual_mask = IPEndPoint::AnyAddress;
                if (std::shared_ptr<ITap> tap = GetTap(); NULLPTR != tap) {
                    tap_virtual_mask = ntohl(tap->SubmaskAddress);
                    tap_virtual_network = ntohl(tap->IPAddress) & tap_virtual_mask;
                }

                auto add_dns_server_to_dns_servers =
                    [&](const std::shared_ptr<NetworkInterface>& ni, ppp::unordered_set<uint32_t>& dns_servers) noexcept {
                        if (NULLPTR == ni) {
                            return false;
                        }

                        uint32_t ips[2] = { IPEndPoint::AnyAddress, IPEndPoint::AnyAddress };
                        boost::asio::ip::address nips[] = { ni->IPAddress, ni->SubmaskAddress };
                        for (int i = 0; i < arraysizeof(nips); i++) {
                            boost::asio::ip::address& ip = nips[i];
                            if (ip.is_v4()) {
                                ips[i] = ip.to_v4().to_uint();
                            }
                        }

                        uint32_t rip = ips[0] & ips[1];
                        for (boost::asio::ip::address& ip : ni->DnsAddresses) {
                            if (ip.is_v6()) {
                                continue;
                            }

                            if (!ip.is_v4()) {
                                continue;
                            }

                            if (ip.is_multicast()) {
                                continue;
                            }

                            if (ip.is_loopback()) {
                                continue;
                            }

                            if (ip.is_unspecified()) {
                                continue;
                            }

                            if (IPEndPoint::IsInvalid(ip)) {
                                continue;
                            }

                            uint32_t dip = ip.to_v4().to_uint();
                            uint32_t tip = (dip & ips[1]);
                            if (tip == rip) {
                                continue;
                            }

                            // Skip DNS servers inside the TAP virtual subnet. They are reachable
                            // on-link through the virtual gateway, and pinning them via the
                            // physical gateway would shadow the virtual gateway /32 route.
                            // dip is in network byte order (to_uint), while tap_virtual_mask
                            // and tap_virtual_network are host byte order (ntohl); convert
                            // dip before comparing or the skip never matches and the virtual
                            // gateway DNS gets pinned through the physical gateway.
                            if (tap_virtual_mask != IPEndPoint::AnyAddress &&
                                (ntohl(dip) & tap_virtual_mask) == tap_virtual_network) {
                                continue;
                            }

                            dip = htonl(dip);
                            dns_servers.emplace(dip);
                        }
                        return true;
                    };

                add_dns_server_to_dns_servers(tun_ni_, dns_serverss_[0]);
                add_dns_server_to_dns_servers(underlying_ni_, dns_serverss_[1]);

                // Add dns route set rules.
                for (auto&& dns_rules : dns_ruless_) {
                    for (auto& [_, r] : dns_rules) {
                        boost::asio::ip::address server = r->Server;
                        if (!server.is_v4()) {
                            continue;
                        }

                        uint32_t ip = htonl(server.to_v4().to_uint());
                        if (r->Nic) {
                            dns_serverss_[1].emplace(ip);
                        }
                        else {
                            dns_serverss_[0].emplace(ip);
                        }
                    }
                }

                if (geo_rules_) {
                    for (const boost::asio::ip::address& server : direct_dns_servers_) {
                        if (server.is_v4()) {
                            dns_serverss_[1].emplace(htonl(server.to_v4().to_uint()));
                        }
                    }
                }

                // Compare two lists and remove duplicate ip addresses that appear in both lists.
                ppp::collections::Dictionary::DeduplicationList(dns_serverss_[1], dns_serverss_[0]);

                // Add the routing gateway of these DNS as the vpn server, mainly to solve the problem of interference.
                if (std::shared_ptr<ITap> tap = GetTap(); NULLPTR != tap) {
                    for (uint32_t ip : dns_serverss_[0]) {
                        AddRoute(ip, tap->GatewayServer, 32);
                    }

                    // Remove any stale /32 host route that pins an address inside the
                    // TAP virtual subnet (e.g. the virtual gateway) through the physical
                    // gateway. Previous builds installed such routes when a physical NIC
                    // had the virtual gateway configured as a DNS server; they shadow the
                    // TAP on-link route and must be cleaned up even if the DNS is no longer
                    // added to the direct list.
                    uint32_t tap_virtual_network = ntohl(tap->IPAddress) & ntohl(tap->SubmaskAddress);
                    uint32_t tap_virtual_mask = ntohl(tap->SubmaskAddress);
#if defined(_WIN32)
                    if (tap_virtual_mask != IPEndPoint::AnyAddress) {
                        if (auto mib = ppp::win32::network::Router::GetIpForwardTable(); NULLPTR != mib) {
                            for (DWORD i = 0; i < mib->dwNumEntries; i++) {
                                MIB_IPFORWARDROW row = mib->table[i];
                                if (row.dwForwardMask != 0xffffffffu) {
                                    continue;
                                }
                                // GetIpForwardTable returns addresses in network byte order,
                                // while tap_virtual_network/mask are host byte order (ntohl).
                                // Compare in host byte order or the stale-route cleanup
                                // never matches and the hijacked /32 route survives forever.
                                uint32_t dest = ntohl(row.dwForwardDest);
                                if ((dest & tap_virtual_mask) == tap_virtual_network &&
                                    ntohl(row.dwForwardNextHop) != ntohl(tap->GatewayServer)) {
                                    ppp::win32::network::Router::Delete(row);
                                }
                            }
                        }
                    }
#endif
                }

                // Add the dns route table to the loopback settings of the physical nic.
                if (std::shared_ptr<NetworkInterface> ni = underlying_ni_; NULLPTR != ni) {
                    boost::asio::ip::address gw = ni->GatewayServer;
                    if (gw.is_v4()) {
                        uint32_t next_hop = htonl(gw.to_v4().to_uint());
                        for (uint32_t ip : dns_serverss_[1]) {
                            AddRoute(ip, next_hop, 32);
                        }
                    }
#if defined(_WIN32)
                    direct_dns_routes6_.clear();
                    for (const auto& server : direct_dns_servers_) {
                        if (!server.is_v6()) {
                            continue;
                        }
                        auto network = server.to_v6();
                        network.scope_id(0);
                        const std::string address_text = network.to_string();
                        const ppp::string address(address_text.data(), address_text.size());
                        ppp::string gateway;
                        if (!network.is_link_local() && ni->IPv6GatewayServer.is_v6() &&
                            !ni->IPv6GatewayServer.is_unspecified()) {
                            auto next_hop = ni->IPv6GatewayServer.to_v6();
                            next_hop.scope_id(0);
                            const std::string gateway_text = next_hop.to_string();
                            gateway.assign(gateway_text.data(), gateway_text.size());
                        }
                        ppp::win32::network::DeleteIPv6Route(ni->Index, address, 128, gateway);
                        if (ppp::win32::network::AddIPv6Route(ni->Index, address, 128, gateway, 1)) {
                            GeoDynamicRoute6 state;
                            state.gateway = gateway;
                            state.interface_index = ni->Index;
                            direct_dns_routes6_[address] = std::move(state);
                        }
                        else {
                            LOG_WARN("Direct DNS: cannot pin IPv6 server=%s to physical ifIndex=%d",
                                address.data(), ni->Index);
                        }
                    }
#endif
                }
            }

            bool VEthernetNetworkSwitcher::AddRoute(uint32_t ip, uint32_t gw, int prefix) noexcept {
#if defined(_WIN32)
                MIB_IPFORWARDROW route;
                if (ppp::win32::network::Router::GetBestRoute(ip, route)) {
                    if (route.dwForwardDest == ip) {
                        uint32_t mask = IPEndPoint::PrefixToNetmask(prefix);
                        if (route.dwForwardMask == mask && route.dwForwardNextHop == gw) {
                            // The route already exists in the system table
                            // (e.g. a leftover from a previous process). Re-adding it
                            // would fail with ERROR_OBJECT_ALREADY_EXISTS, so the
                            // OS route table still carries it. Treat it as installed.
                            return true;
                        }
                        ppp::win32::network::Router::Delete(route);
                    }
                }

                // Add dns server list IP routing to the windows operating system.
                uint32_t mask = IPEndPoint::PrefixToNetmask(prefix);
                return ppp::win32::network::Router::Add(ip, mask, gw, 1);
#elif defined(_MACOS)
                // Add dns server list IP routing to the macos operating system.
                return ppp::darwin::tun::utun_add_route(ip, prefix, gw);
#else
                // If gateway is of a physical network card, it means that this is the NS route for physical network card.
                if (std::shared_ptr<NetworkInterface> ni = underlying_ni_; NULLPTR != ni) {
                    boost::asio::ip::address next_hop = ni->GatewayServer;
                    if (next_hop.is_v4() && htonl(next_hop.to_v4().to_uint()) == gw) {
                        return ppp::tap::TapLinux::AddRoute(ni->Name, ip, 32, gw);
                    }
                }

                // Add dns server list IP routing to the linux operating system.
                std::shared_ptr<ppp::tap::ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                ppp::tap::TapLinux* linux_tap = dynamic_cast<ppp::tap::TapLinux*>(tap.get());
                if (NULLPTR == linux_tap) {
                    return false;
                }

                return linux_tap->AddRoute(ip, prefix, gw);
#endif
            }

#if defined(_WIN32)
            bool VEthernetNetworkSwitcher::DeleteRoute(const std::shared_ptr<MIB_IPFORWARDTABLE>& mib, uint32_t ip, uint32_t gw, int prefix) noexcept {
                // Delete the IP route for the dns server list added for the windows operating system.
                if (NULLPTR == mib) {
                    return false;
                }

                uint32_t mask = IPEndPoint::PrefixToNetmask(prefix);
                return ppp::win32::network::Router::Delete(mib, ip, mask, gw);
            }
#else
            bool VEthernetNetworkSwitcher::DeleteRoute(uint32_t ip, uint32_t gw, int prefix) noexcept {
#if defined(_MACOS)
                // Delete the IP route for the dns server list added for the macos operating system.
                return ppp::darwin::tun::utun_del_route(ip, prefix, gw);
#else
                // // If gateway is of a physical network card, it means that this is the NS route for physical network card.
                if (std::shared_ptr<NetworkInterface> ni = underlying_ni_; NULLPTR != ni) {
                    boost::asio::ip::address next_hop = ni->GatewayServer;
                    if (next_hop.is_v4() && htonl(next_hop.to_v4().to_uint()) == gw) {
                        return ppp::tap::TapLinux::DeleteRoute(ni->Name, ip, 32, gw);
                    }
                }

                // Delete the IP route for the dns server list added for the linux operating system.
                std::shared_ptr<ppp::tap::ITap> tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                ppp::tap::TapLinux* linux_tap = dynamic_cast<ppp::tap::TapLinux*>(tap.get());
                if (NULLPTR == linux_tap) {
                    return false;
                }

                return linux_tap->DeleteRoute(ip, prefix, gw);
#endif
            }
#endif

            void VEthernetNetworkSwitcher::DeleteRouteWithDnsServers() noexcept {
#if defined(_WIN32)
                for (const auto& entry : direct_dns_routes6_) {
                    ppp::win32::network::DeleteIPv6Route(entry.second.interface_index,
                        entry.first, 128, entry.second.gateway);
                }
                direct_dns_routes6_.clear();
#endif
                // Delete all vpn dns server routes from the operating system.
                if (std::shared_ptr<ppp::tap::ITap> tap = GetTap(); NULLPTR != tap) {
#if defined(_WIN32)
                    // Delete the IP route for the dns server list added for the windows operating system.
                    if (auto mib = ppp::win32::network::Router::GetIpForwardTable(); NULLPTR != mib) {
                        for (uint32_t ip : dns_serverss_[0]) {
                            DeleteRoute(mib, ip, tap->GatewayServer, 32);
                        }
                    }
#else
                    // Delete the IP route for the dns server list added for the macos operating system.
                    for (uint32_t ip : dns_serverss_[0]) {
                        DeleteRoute(ip, tap->GatewayServer, 32);
                    }
#endif
                }

                if (std::shared_ptr<NetworkInterface> ni = underlying_ni_; NULLPTR != ni) {
                    boost::asio::ip::address gw = ni->GatewayServer;
                    if (gw.is_v4()) {
                        uint32_t next_hop = htonl(gw.to_v4().to_uint());
#if defined(_WIN32)
                        // Delete the IP route for the dns server list added for the windows operating system.
                        if (auto mib = ppp::win32::network::Router::GetIpForwardTable(); NULLPTR != mib) {
                            for (uint32_t ip : dns_serverss_[1]) {
                                DeleteRoute(mib, ip, next_hop, 32);
                            }
                        }
#else
                        // Delete the IP route for the dns server list added for the macos operating system.
                        for (uint32_t ip : dns_serverss_[1]) {
                            DeleteRoute(ip, next_hop, 32);
                        }
#endif
                    }
                }

                // Clear the current cached dns server ip address list.
                for (auto& dns_servers : dns_serverss_) {
                    dns_servers.clear();
                }
            }

            // Routes need to be protected on Windows to prevent third - party programs(such as network card drivers) 
            // From silently modifying the current gateway route and forcing out the VPN virtual gateway route.According to our observation, 
            // In some PC and network production environments, third - party programs will destroy VPN deployment routing table information 
            // At certain times.In PPP PRIVATE NETWORK™ 1, this NETWORK route protector exists by default, but PPP PRIVATE Network ™ 2 does 
            // Not currently exist, so a new implementation of this section is needed.
            bool VEthernetNetworkSwitcher::ProtectDefaultRoute() noexcept {
                auto tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

#if !defined(_WIN32)
#if defined(_MACOS)
                auto unix_tap = dynamic_cast<ppp::tap::TapDarwin*>(tap.get());
#else
                auto unix_tap = dynamic_cast<ppp::tap::TapLinux*>(tap.get());
#endif
                if (NULLPTR == unix_tap || unix_tap->IsPromisc()) {
                    return false;
                }
#endif

                // Keep exactly one protector across temporary tunnel outages. It
                // exits after route_added_ becomes false and a later activation can
                // start a replacement.
                if (route_protector_running_.exchange(true)) {
                    return true;
                }

                // Create a new network protection backend subthread.
                auto self = shared_from_this();
                try {
                    std::thread([self, this]() noexcept {
                        auto prepare = [self, this]() noexcept {
                        // If the current VEthernet framework object instance is released, the process is break.
                        if (IsDisposed()) {
                            return false;
                        }

                        // If the route is not added to the system, the route pops out without setting the flag.
                        if (!route_added_.load()) {
                            return false;
                        }

                        // Check whether the physical nic interface information still exists.
                        std::shared_ptr<NetworkInterface> underlying_ni = underlying_ni_;
                        if (NULLPTR == underlying_ni) {
                            return false;
                        }

                        // If the physical network adapter gateway server is not IPV4, the process is displayed.
                        boost::asio::ip::address gw = underlying_ni->GatewayServer;
                        if (!gw.is_v4()) {
                            return false;
                        }

                        return true;
                        };

                        ppp::SetThreadName("protector");
                        for (;;) {
                        // Gets the current process processing start time.
                        uint64_t start = ppp::GetTickCount();

                        // If the pre-preparation check processing fails, just jump out of the loop because the object is being released.
                        bool ok = prepare();
                        if (!ok) {
                            break;
                        }

                        // Try to get the lock, if you can't get the lock, do not deal with it and wait for the next execution.
                        if (prdr_.try_lock()) {
                            ok = prepare();
                            if (ok) {
                                ok = DeleteAllDefaultRoute();
                            }

                            // Release the obtained prdr lock and decide whether to exit the process.
                            prdr_.unlock();
                            if (!ok) {
                                break;
                            }
                        }

                        // Calculate how much time the thread has to wait for sleep.
                        uint64_t now = ppp::GetTickCount();
                        uint64_t delta = 0;
                        if (now >= start) {
                            delta = 1000 - std::min<uint64_t>(1000, now - start);
                        }

                        // Check whether the default gateway route is faulty every second.
                        ppp::Sleep(delta);
                        }
                        route_protector_running_.store(false);
                    }).detach();
                }
                catch (const std::exception& e) {
                    route_protector_running_.store(false);
                    LOG_ERROR("VEthernetNetworkSwitcher::ProtectDefaultRoute: worker creation failed, exception=%s", e.what());
                    return false;
                }
                return true;
            }
#endif

#if defined(_ANDROID) || defined(_IPHONE)
            bool VEthernetNetworkSwitcher::RefreshDirectDnsServers() noexcept {
                direct_dns_servers_.clear();
                direct_dns_server_index_.store(0);
                if (!geo_rules_) {
                    return true;
                }

                auto append = [this](boost::asio::ip::address address) noexcept {
                    if (IPEndPoint::IsInvalid(address) || address.is_unspecified() ||
                        address.is_multicast() || address.is_loopback()) {
                        return;
                    }
                    if (std::find(direct_dns_servers_.begin(), direct_dns_servers_.end(), address) == direct_dns_servers_.end()) {
                        direct_dns_servers_.emplace_back(std::move(address));
                    }
                };

                if (geo_rules_->UsesLocalDirectDns()) {
                    if (underlying_ni_) {
                        for (const auto& address : underlying_ni_->DnsAddresses) {
                            append(address);
                        }
                    }
                }
                for (const auto& address : geo_rules_->GetDirectDnsServers()) {
                    append(address);
                }

                for (const auto& address : direct_dns_servers_) {
                    LOG_INFO("Direct DNS: server=%s, source=%s",
                        address.to_string().data(),
                        geo_rules_->UsesLocalDirectDns() && underlying_ni_ &&
                            std::find(underlying_ni_->DnsAddresses.begin(), underlying_ni_->DnsAddresses.end(), address) != underlying_ni_->DnsAddresses.end()
                            ? "local" : "configured");
                }
                if (geo_rules_->UsesLocalDirectDns() && direct_dns_servers_.empty()) {
                    LOG_ERROR("Direct DNS: direct_dns=local but the selected physical interface has no usable IPv4/IPv6 DNS server");
                    return false;
                }
                return true;
            }

            void VEthernetNetworkSwitcher::ObserveGeoDnsResponse(
                const void* packet, int packet_size) noexcept {
                if (!geo_rules_) {
                    return;
                }
                // Mobile packets are routed in user space. ObserveDnsResponse
                // records the domain-derived policy in GeoRuleEngine, and the
                // subsequent IsBypassIpAddress() lookup consumes it directly;
                // no host /32 or /128 route installation is required.
                ppp::vector<ppp::app::client::geo::GeoRuleEngine::RouteUpdate> updates;
                if (geo_rules_->ObserveDnsResponse(
                    packet, packet_size,
                    ppp::threading::Executors::GetTickCount(), updates)) {
                    LOG_INFO("GeoSite DNS policy learned (mobile): routes=%llu",
                        (unsigned long long)updates.size());
                }
            }

            ppp::string VEthernetNetworkSwitcher::GetRemoteUri() noexcept {
                return server_ru_;
            }

            std::size_t VEthernetNetworkSwitcher::GetIPListCount() noexcept {
                LoadIPListFileVectorPtr ribs = ribs_;
                if (NULLPTR == ribs) return 0;
                return ribs->size();
            }

            std::size_t VEthernetNetworkSwitcher::GetIPList6Count() noexcept {
                LoadIPv6ListFileVectorPtr ribs6 = ribs6_;
                if (NULLPTR == ribs6) return 0;
                return ribs6->size();
            }

            bool VEthernetNetworkSwitcher::SelectDirectDnsServer(
                const ppp::string& host, boost::asio::ip::address& server) noexcept {
                if (!geo_rules_ || direct_dns_servers_.empty()) {
                    return false;
                }
                const auto decision = geo_rules_->MatchDomain(host);
                if (decision.action != ppp::app::client::geo::GeoRuleEngine::Action::Direct) {
                    return false;
                }
                const size_t index = direct_dns_server_index_.fetch_add(1) % direct_dns_servers_.size();
                server = direct_dns_servers_[index];
                return true;
            }
#endif

            bool VEthernetNetworkSwitcher::IsBypassIpAddress(const boost::asio::ip::address& ip) noexcept {
                if (!ip.is_v4()) {
                    return false;
                }

                if (ip.is_unspecified()) {
                    return false;
                }

                if (ip.is_multicast()) {
                    return false;
                }

                if (ppp::net::IPEndPoint::IsInvalid(ip)) {
                    return false;
                }

                if (geo_rules_) {
                    auto decision = geo_rules_->MatchAddress(ip, ppp::threading::Executors::GetTickCount());
                    if (decision.Matched()) {
                        return decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                    }
                }

                auto tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

                uint32_t nip = htonl(ip.to_v4().to_uint());
#if defined(_ANDROID)
                // Android builds the bypass routes in the RIB directly. The FIB
                // is not populated on this path, so querying it makes every
                // destination fall through to the main tunnel.
                if (auto rib = rib_; NULLPTR != rib) {
                    uint32_t ngw = ppp::net::native::ForwardInformationTable::GetNextHop(
                        nip, rib->GetAllRoutes());
                    if (ngw != ppp::net::IPEndPoint::NoneAddress) {
                        return ngw != tap->GatewayServer;
                    }
                }

                return false;
#elif defined(_WIN32)
                DWORD dwInterfaceIndex;
                if (!::GetBestInterface((IPAddr)nip, &dwInterfaceIndex)) {
                    return false;
                }
                return dwInterfaceIndex != (DWORD)tap->GetInterfaceIndex();
#else
                // OS X provides basic routing table processing so that the HTTP proxy provided by the VPN can route 
                // The traffic instead of having to deliver it to the VPN server for processing.
                // 
                // It is only supported when the VPN opens the network card promisbity mode, 
                // Which is to support the PC only a single network card can provide a reliable VPN virtual network 
                // For the local area network through the kernel SNAT mechanism.
                // 
                // Note: Google Android and Huawei HarmonyOS platforms (the VPN network adapter promiscuous mode must be enabled)
                // Snat: iptables -t nat -I POSTROUTING -s 192.168.0.24 -j SNAT --to-source 10.0.0.2
                return ppp::net::Socket::GetBestInterfaceIP(nip) != tap->IPAddress;
#endif
            }

            bool VEthernetNetworkSwitcher::IsBypassIpAddress6(const boost::asio::ip::address& ip) noexcept {
                if (!ip.is_v6()) {
                    return false;
                }

                if (ip.is_unspecified()) {
                    return false;
                }

                if (ip.is_multicast()) {
                    return false;
                }

                if (ppp::net::IPEndPoint::IsInvalid(ip)) {
                    return false;
                }

                if (geo_rules_) {
                    auto decision = geo_rules_->MatchAddress(ip, ppp::threading::Executors::GetTickCount());
                    if (decision.Matched()) {
                        return decision.action == ppp::app::client::geo::GeoRuleEngine::Action::Direct;
                    }
                }

                auto tap = GetTap();
                if (NULLPTR == tap) {
                    return false;
                }

#if defined(_ANDROID)
                // Android builds the bypass routes in the RIB6 directly. The
                // FIB6 is not populated on this path, so do a longest-prefix
                // match over the raw IPv6RouteEntry table.
                if (auto rib6 = rib6_; NULLPTR != rib6 && !rib6->empty()) {
                    boost::asio::ip::address_v6 dst = ip.to_v6();
                    int best_prefix = -1;
                    boost::asio::ip::address_v6 best_nh;
                    for (const IPv6RouteEntry& entry : *rib6) {
                        if (entry.Prefix <= best_prefix) {
                            continue;
                        }

                        // Longest-prefix match: compare the masked bytes of the
                        // destination against the entry network.
                        const auto& nb = entry.Network.to_bytes();
                        const auto& db = dst.to_bytes();
                        const int full = entry.Prefix / 8;
                        const int rem = entry.Prefix % 8;
                        bool matched = true;
                        for (int k = 0; k < full; k++) {
                            if (nb[k] != db[k]) {
                                matched = false;
                                break;
                            }
                        }
                        if (matched && rem > 0) {
                            const uint8_t mask = (uint8_t)(0xFF << (8 - rem));
                            if ((nb[full] & mask) != (db[full] & mask)) {
                                matched = false;
                            }
                        }

                        if (matched) {
                            best_prefix = entry.Prefix;
                            best_nh = entry.NextHop;
                        }
                    }

                    if (best_prefix >= 0) {
                        return !best_nh.is_unspecified() &&
                            best_nh != tap->IPv6GatewayServer;
                    }
                }

                return false;
#elif defined(_WIN32)
                // Windows: use connect+getsockname to find the local exit address.
                boost::asio::ip::address local = ppp::net::Socket::GetBestInterfaceIP6(ip);
                return local.is_v6() && !local.is_unspecified() && local != tap->IPv6Address;
#else
                // Linux/macOS: same connect+getsockname approach as v4's GetBestInterfaceIP.
                boost::asio::ip::address local = ppp::net::Socket::GetBestInterfaceIP6(ip);
                return local.is_v6() && !local.is_unspecified() && local != tap->IPv6Address;
#endif
            }

#if !defined(_ANDROID) && !defined(_IPHONE)
            void VEthernetNetworkSwitcher::RestoreNetworkTakeover(bool restore_ipv6) noexcept {
#if defined(_WIN32)
                // Stop the DNS guard before restoring physical-NIC DNS; otherwise a
                // pending tick can clear the values again during shutdown.
                dns_guard_active_.store(false);
                if (std::shared_ptr<ppp::threading::Timer> timer = std::move(dns_guard_timer_); NULLPTR != timer) {
                    timer->Stop();
                    timer->Dispose();
                }

                uint64_t guard_deadline = ppp::threading::Executors::GetTickCount() + 10000;
                while (dns_guard_workers_.load() > 0 && ppp::threading::Executors::GetTickCount() < guard_deadline) {
                    ppp::Sleep(10);
                }
                if (dns_guard_workers_.load() > 0) {
                    LOG_WARN("VEthernetNetworkSwitcher::RestoreNetworkState: DNS guard workers did not stop before timeout, workers=%d",
                        dns_guard_workers_.load());
                }
#endif

                if (!route_added_.exchange(false)) {
                    if (restore_ipv6) {
                        RestoreIPv6Assignment();
                    }
#if defined(_WIN32)
                    if (network_takeover_stopping_.load()) {
                        RemoveWindowsIPv6LeakBlock();
                    }
                    else {
                        ApplyWindowsIPv6LeakBlockRoutes();
                    }
#endif
                    return;
                }

                LOG_INFO("VEthernetNetworkSwitcher::RestoreNetworkState: restoring routes and DNS");
                DeleteRoute();

#if defined(_WIN32)
                ppp::win32::network::SetAllNicsDnsAddressesV6(ni_dns_servers_v6_);
                ppp::win32::network::SetAllNicsDnsAddresses(ni_dns_servers_);
                ppp::tap::TapWindows::DnsFlushResolverCache();
                // Router discovery is no longer disabled during takeover (see
                // ApplyNetworkTakeover), so there is nothing to re-enable here.
                ni_router_discovery_disabled_v6_.clear();
                // DNS is restored before the loopback service is stopped so there
                // is no interval where physical NICs point at an unbound port.
                StopLocalDnsProxy();
#else
                UnixNetworkInterface::SetDnsResolveConfiguration(GetUnderlyingNetworkInterface());
#endif
                // Restore the server-managed IPv6 state last. In deferred-start
                // mode it may have captured the physical NIC DNS before the route
                // takeover did, so it is the authoritative original IPv6 snapshot.
                if (restore_ipv6) {
                    RestoreIPv6Assignment();
                }
#if defined(_WIN32)
                // A temporary primary-outbound failure stays fail-closed. Only
                // Dispose() sets network_takeover_stopping_ and permits host IPv6
                // restoration.
                if (!network_takeover_stopping_.load()) {
                    ApplyWindowsIPv6LeakBlockRoutes();
                }
                else {
                    RemoveWindowsIPv6LeakBlock();
                }
#endif
                LOG_INFO("VEthernetNetworkSwitcher::RestoreNetworkState: routes and DNS restored");
            }

            void VEthernetNetworkSwitcher::RestoreNetworkState() noexcept {
                RestoreNetworkTakeover(true);
            }
#endif

            void VEthernetNetworkSwitcher::ReleaseAllObjects() noexcept {
#if !defined(_ANDROID) && !defined(_IPHONE)
                // Windows platform needs to set the prdr synchronization lock state to prevent the problem of multi-thread concurrent competition.
                SynchronizedObjectScope scope(prdr_);
#endif

                // Clear event bindings.
                TickEvent = NULLPTR;

#if !defined(_ANDROID) && !defined(_IPHONE)
                // Dispose normally restores this synchronously before Finalize is
                // posted. Keep the idempotent call here for destructor/failure paths.
                RestoreNetworkState();
#endif

                // Stop and release the http-proxy service.
                if (VEthernetHttpProxySwitcherPtr http_proxy = std::move(http_proxy_); NULLPTR != http_proxy) {
                    http_proxy->Dispose();
                }

                // Stop and release the socks-proxy service.
                if (VEthernetSocksProxySwitcherPtr socks_proxy = std::move(socks_proxy_); NULLPTR != socks_proxy) {
                    socks_proxy->Dispose();
                }

                // Close and release every outbound exchanger.  The primary pointer is
                // an alias into this table and must not be disposed a second time.
                exchanger_.reset();
                std::shared_ptr<VEthernetExchanger> pending =
                    std::move(pending_outbound_exchanger_);
                pending_outbound_.clear();
                pending_outbound_deadline_ = 0;
                pending_primary_switch_ = false;
                OutboundExchangerTable outbounds = std::move(outbound_exchangers_);
                outbound_exchangers_.clear();
                ppp::vector<std::shared_ptr<VEthernetExchanger>> disposed_exchangers;
                for (auto& entry : outbounds) {
                    if (NULLPTR == entry.second) continue;
                    bool duplicate = false;
                    for (const auto& disposed : disposed_exchangers) {
                        if (disposed == entry.second) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        disposed_exchangers.emplace_back(entry.second);
                        entry.second->Dispose();
                    }
                }
                if (NULLPTR != pending) {
                    bool duplicate = false;
                    for (const auto& disposed : disposed_exchangers) {
                        if (disposed == pending) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) pending->Dispose();
                }

                // Shutdown and release the qos control module.
                if (std::shared_ptr<ppp::transmissions::ITransmissionQoS> qos = std::move(qos_);  NULLPTR != qos) {
                    qos->Dispose();
                }

                // Close and release the aggligator.
                if (std::shared_ptr<aggligator::aggligator> aggligator = std::move(aggligator_); NULLPTR != aggligator) {
                    aggligator->close();
                }

                // Close and release the forwarding.
                if (IForwardingPtr forwarding = std::move(forwarding_); NULLPTR != forwarding) {
                    forwarding->Dispose();
                }

#if defined(_WIN32)
                // On Windows platforms, you need to try to turn off the [PaperAirplane NSP/LSP] server-side controller.
                if (PaperAirplaneControllerPtr controller = std::move(paper_airplane_ctrl_);  NULLPTR != controller) {
                    controller->Dispose();
                }
#endif

#if !defined(_ANDROID) && !defined(_IPHONE)
                // To clean up the managed and unmanaged data currently held by the class, 
                // You need to go through the complete construct fill process again after the Release of this function.
                ribs_.reset();
                ribs6_.reset(); 
                tun_ni_.reset();
                underlying_ni_.reset();

#if !defined(_MACOS)
                // Clear the routing table, forwarding table, and DNS server list of the network card, including cache.
                rib_ = NULLPTR;
                rib6_ = NULLPTR;
                fib_ = NULLPTR;
                fib6_ = NULLPTR;
#endif

                // Clear all route tables and forwarding tables held by the current object.
                LoadAllIPListWithFilePaths(boost::asio::ip::address_v4::any());
                LoadAllIPListWithFilePaths6(boost::asio::ip::address_v6::any());
#endif

#if defined(_LINUX)
                // Release the network protector held by the current VPN local client switcher.
                if (auto protector = std::move(protect_network_); NULLPTR != protector) {
                    // In android platform you need to request the DetachJNI function of the network protector.
#if defined(_ANDROID)
                    protector->DetachJNI();
#endif
                }
#endif
            }

            bool VEthernetNetworkSwitcher::DeleteTimeout(void* k) noexcept {
                if (NULLPTR == k) {
                    return false;
                }

                SynchronizedObjectScope scope(GetSynchronizedObject());
                return Dictionary::RemoveValueByKey(timeouts_, k);
            }

            bool VEthernetNetworkSwitcher::EmplaceTimeout(void* k, const std::shared_ptr<ppp::threading::Timer::TimeoutEventHandler>& timeout) noexcept {
                if (NULLPTR == k || NULLPTR == timeout) {
                    return false;
                }

                SynchronizedObjectScope scope(GetSynchronizedObject());
                auto r = timeouts_.emplace(k, timeout);
                return r.second;
            }

            bool VEthernetNetworkSwitcher::LoadAllDnsRules(const ppp::string& rules, bool load_file_or_string) noexcept {
                if (rules.empty()) {
                    return false;
                }

                int events = 0;
                if (load_file_or_string) {
                    events = ppp::app::client::dns::Rule::LoadFile(rules, dns_ruless_[0], dns_ruless_[1], dns_ruless_[2]);
                }
                else {
                    events = ppp::app::client::dns::Rule::Load(rules, dns_ruless_[0], dns_ruless_[1], dns_ruless_[2]);
                }

                return events > 0;
            }

            bool VEthernetNetworkSwitcher::LoadGeoRules(const ppp::string& rules_path,
                const ppp::string& geosite_path, const ppp::string& geoip_path) noexcept {
                auto engine = make_shared_object<ppp::app::client::geo::GeoRuleEngine>();
                if (!engine) {
                    return false;
                }

                ppp::string error;
                if (!engine->Load(rules_path, geosite_path, geoip_path, error)) {
                    LOG_ERROR("VEthernetNetworkSwitcher::LoadGeoRules: %s", error.data());
                    fprintf(stdout, "Geo rules error: %s\r\n", error.data());
                    return false;
                }

                final_outbound_ = engine->GetFinalOutbound();
                geo_rules_ = std::move(engine);
                return true;
            }

            bool VEthernetNetworkSwitcher::UpdateRemoteUri() noexcept {
                using ProtocolType = VEthernetExchanger::ProtocolType;

                std::shared_ptr<VEthernetExchanger> exchanger;
                {
                    SynchronizedObjectScope scope(GetSynchronizedObject());
                    ppp::string tag = active_outbound_.empty() ? ppp::string("main") : active_outbound_;
                    auto selected = outbound_exchangers_.find(tag);
                    exchanger = selected != outbound_exchangers_.end() ?
                        selected->second : exchanger_;
                }
                if (NULLPTR == exchanger) {
                    return false;
                }

                boost::asio::ip::tcp::endpoint remoteEP;
                ppp::string hostname;
                ppp::string address;
                ppp::string path;
                ppp::string server;
                int port = IPEndPoint::MinPort;
                ProtocolType protocol_type = ProtocolType::ProtocolType_PPP;
                static constexpr ppp::coroutines::YieldContext* y = NULLPTR;
                if (!exchanger->GetRemoteEndPoint(y, hostname, address, path, port, protocol_type, server, remoteEP)) {
                    return false;
                }

                server_ru_ = "[";
                server_ru_ += hostname;
                server_ru_ += "]:";
                server_ru_ += stl::to_string<ppp::string>(port);
                server_ru_ += "/";
                if (protocol_type == ProtocolType::ProtocolType_Http || protocol_type == ProtocolType::ProtocolType_WebSocket) {
                    server_ru_ += "ppp+ws";
                }
                elif(protocol_type == ProtocolType::ProtocolType_HttpSSL || protocol_type == ProtocolType::ProtocolType_WebSocketSSL) {
                    server_ru_ += "ppp+wss";
                }
                else {
                    server_ru_ += "ppp+tcp";
                }
                return true;
            }

            bool VEthernetNetworkSwitcher::AddRemoteEndPointToIPList(const boost::asio::ip::address& gw) noexcept {
                using ProtocolType = VEthernetExchanger::ProtocolType;

                // This function must be executed after the remote exchanger object has been created.
                std::shared_ptr<VEthernetExchanger> exchanger = exchanger_;
                if (NULLPTR == exchanger) {
                    return false;
                }

                // Initialize and try the proxy forwarding object if the link does require proxy forwarding services.
                IForwardingPtr forwarding = make_shared_object<IForwarding>(GetContext(), configuration_);
                if (NULLPTR == forwarding) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::AddRemoteEndPointToIPList: forwarding is null");
                    return false;
                }
                elif(forwarding->Open()) {
                    forwarding_ = forwarding;
#if defined(_LINUX)
                    forwarding->ProtectorNetwork = GetProtectorNetwork();
#endif
                }
                else {
                    forwarding->Dispose();
                    forwarding.reset();
                }

                boost::asio::ip::tcp::endpoint remoteEP;
                ppp::string hostname;
                ppp::string address;
                ppp::string path;
                ppp::string server;
                int port;
                ProtocolType protocol_type = ProtocolType::ProtocolType_PPP;

                // Obtaining the IP endpoint address of the VPN remote server may involve synchronizing the network, as it may be in domain-name format.
                static constexpr ppp::coroutines::YieldContext* y = NULLPTR;
                
                if (!exchanger->GetRemoteEndPoint(y, hostname, address, path, port, protocol_type, server, remoteEP)) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::AddRemoteEndPointToIPList: GetRemoteEndPoint failed");
                    return false;
                }
                else {
                    server_ru_ = "[";
                    server_ru_ += hostname;
                    server_ru_ += "]";
                    server_ru_ += ":";
                    server_ru_ += stl::to_string<ppp::string>(NULLPTR != forwarding ? forwarding->GetRemotePort() : port);
                    server_ru_ += "/";

                    if (protocol_type == ProtocolType::ProtocolType_Http || protocol_type == ProtocolType::ProtocolType_WebSocket) {
                        server_ru_ += "ppp+ws";
                    }
                    elif(protocol_type == ProtocolType::ProtocolType_HttpSSL || protocol_type == ProtocolType::ProtocolType_WebSocketSSL) {
                        server_ru_ += "ppp+wss";
                    }
                    else {
                        server_ru_ += "ppp+tcp";
                    }

                    if (NULLPTR != forwarding) {
                        remoteEP = forwarding->GetProxyEndPoint();
                    }
                }

                // Add the default IP address of the vpn virtual network adapter to the RIB route table.
                RouteInformationTablePtr rib = rib_;
                if (NULLPTR == rib) {
                    rib = make_shared_object<RouteInformationTable>();
                    rib_ = rib;
                }

                // CIDR: 0.0.0.0/0; 0.0.0.0/1; 128.0.0.0/1
                if (NULLPTR != rib) {
                    if (auto tap = GetTap(); NULLPTR != tap) {
                        rib->AddRoute(IPEndPoint::AnyAddress, 0, tap->GatewayServer);
                        rib->AddRoute(IPEndPoint::AnyAddress, 1, tap->GatewayServer);
                        rib->AddRoute(inet_addr("128.0.0.0"), 1, tap->GatewayServer);
                    }
                }

                // Note that we only need to set IPV4 routes, not IPV6 routes.
                boost::asio::ip::address remoteIP = remoteEP.address();
                IPEndPoint serverEP = IPEndPoint::ToEndPoint(remoteEP);
                if (IPEndPoint::IsInvalid(serverEP)) {
                    LOG_DEBUG("VEthernetNetworkSwitcher::AddRemoteEndPointToIPList: serverEP is invalid");
                    return false;
                }

                // Add IPV4 route table settings.
                auto fib_add_route_ipv4 =
                    [&rib, &gw](const boost::asio::ip::address& remoteIP) noexcept {
                        if (remoteIP.is_v6()) {
                            return true;
                        }

                        if (NULLPTR == rib) {
                            return false;
                        }
                        
                        bool processed = gw.is_v4() && remoteIP.is_v4();
                        if (!processed) {
                            return false;
                        }

                        // First convert the IP addresses of both.
                        uint32_t ip = htonl(remoteIP.to_v4().to_uint());
                        uint32_t nx = htonl(gw.to_v4().to_uint());

                        // Add route information to rib!
                        return rib->AddRoute(ip, 32, nx);
                    };

                // Check whether the static tunnel specifies an IP address endpoint (required for transit).
                ppp::unordered_set<boost::asio::ip::tcp::endpoint> servers;
                auto StaticEchoAddRemoteEndPoint = 
                    [this, &servers, &fib_add_route_ipv4, &exchanger](const ppp::string& server_string) noexcept {
                        if (server_string.empty()) {
                            return false;
                        }

                        ppp::string host_string;
                        int port;

                        if (!ppp::net::Ipep::ParseEndPoint(server_string, host_string, port)) {
                            return false;
                        }

                        if (port <= IPEndPoint::MinPort || port > IPEndPoint::MaxPort) {
                            return false;
                        }

                        IPEndPoint remoteEP = ppp::net::Ipep::GetEndPoint(host_string, port);
                        if (IPEndPoint::IsInvalid(remoteEP)) {
                            return false;
                        }

                        boost::asio::ip::udp::endpoint ep =
                            IPEndPoint::ToEndPoint<boost::asio::ip::udp>(remoteEP);
                        if (!remoteEP.IsLoopback() && !fib_add_route_ipv4(ep.address())) {
                            return false;
                        }

                        if (aggligator_) {
                            auto r = servers.emplace(
                                IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(remoteEP));
                            return r.second;
                        }
                       
                        return exchanger->StaticEchoAddRemoteEndPoint(ep);
                    };

                for (const ppp::string& server_string : configuration_->udp.static_.servers) {
                    if (!StaticEchoAddRemoteEndPoint(server_string)) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::AddRemoteEndPointToIPList: StaticEchoAddRemoteEndPoint failed");
                        return false;
                    }
                }

                // Open the beast network bandwidth aggregator.
                if (std::shared_ptr<aggligator::aggligator> aggligator = aggligator_; NULLPTR != aggligator) {
                    if (servers.empty()) {
                        aggligator_.reset();
                        aggligator->close();
                    }
                    elif(!aggligator->client_open(configuration_->udp.static_.aggligator, servers)) {
                        LOG_DEBUG("VEthernetNetworkSwitcher::AddRemoteEndPointToIPList: aggligator client_open failed");
                        return false;
                    }
                }

                // The gateway address must be IPV4 or it is considered a failure because there is no V6 gateway serving the V4 address.
                if (serverEP.IsLoopback()) {
                    return true;
                }

                // Also add the IPv6 server route to the bypass table if applicable.
                if (remoteIP.is_v6()) {
                    if (NULLPTR == rib6_) {
                        rib6_ = make_shared_object<IPv6RouteTable>();
                    }
                    if (NULLPTR != rib6_ && underlying_ni_) {
                        boost::asio::ip::address ngw6_addr = underlying_ni_->IPv6GatewayServer;
                        if (ngw6_addr.is_v6() && !ngw6_addr.is_unspecified()) {
                            boost::asio::ip::address_v6 ngw6 = ngw6_addr.to_v6();
                            IPv6RouteEntry entry;
                            entry.Network = remoteIP.to_v6();
                            entry.Prefix = 128;
                            entry.NextHop = ngw6;
                            rib6_->emplace_back(entry);
                        }
                    }
                }
                if (!fib_add_route_ipv4(remoteIP)) return false;

                // Every secondary tunnel endpoint must bypass the TAP as well,
                // otherwise establishing it after the default route is installed
                // recursively sends the tunnel connection through the primary.
                ppp::vector<std::shared_ptr<VEthernetExchanger>> protected_exchangers;
                for (auto& entry : outbound_exchangers_) {
                    if (entry.first == "main" || NULLPTR == entry.second ||
                        entry.second->IsPrimaryOutbound()) continue;
                    bool duplicate = false;
                    for (const auto& protected_exchanger : protected_exchangers) {
                        if (protected_exchanger == entry.second) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (duplicate) continue;
                    protected_exchangers.emplace_back(entry.second);

                    boost::asio::ip::tcp::endpoint secondary_ep;
                    ppp::string secondary_hostname, secondary_address, secondary_path, secondary_server;
                    int secondary_port = 0;
                    ProtocolType secondary_protocol = ProtocolType::ProtocolType_PPP;
                    if (!entry.second->GetRemoteEndPoint(y, secondary_hostname, secondary_address,
                        secondary_path, secondary_port, secondary_protocol, secondary_server, secondary_ep)) {
                        LOG_ERROR("VEthernetNetworkSwitcher::AddRemoteEndPointToIPList: cannot resolve outbound '%s'",
                            entry.first.data());
                        return false;
                    }

                    // When this outbound uses its own upstream proxy, protect the
                    // proxy endpoint rather than the loopback forwarding listener.
                    if (auto secondary_forwarding = entry.second->forwarding_; NULLPTR != secondary_forwarding) {
                        secondary_ep = secondary_forwarding->GetProxyEndPoint();
                    }

                    boost::asio::ip::address secondary_ip = secondary_ep.address();
                    if (secondary_ip.is_v4()) {
                        if (!secondary_ip.is_loopback() && !fib_add_route_ipv4(secondary_ip)) return false;
                    }
                    elif(secondary_ip.is_v6() && underlying_ni_) {
                        boost::asio::ip::address ngw6_addr = underlying_ni_->IPv6GatewayServer;
                        if (!ngw6_addr.is_v6() || ngw6_addr.is_unspecified()) return false;
                        if (NULLPTR == rib6_) rib6_ = make_shared_object<IPv6RouteTable>();
                        if (NULLPTR == rib6_) return false;
                        IPv6RouteEntry entry6;
                        entry6.Network = secondary_ip.to_v6();
                        entry6.Prefix = 128;
                        entry6.NextHop = ngw6_addr.to_v6();
                        rib6_->emplace_back(std::move(entry6));
                    }
                }

                return true;
            }

            bool VEthernetNetworkSwitcher::RedirectDnsServer(
                ppp::coroutines::YieldContext&                              y,
                const std::shared_ptr<boost::asio::ip::udp::socket>&        socket,
                const std::shared_ptr<Byte>&                                buffer,
                const boost::asio::ip::address&                             serverIP,
                const std::shared_ptr<UdpFrame>&                            frame,
                const std::shared_ptr<ppp::net::packet::BufferSegment>&     messages,
                const std::shared_ptr<boost::asio::io_context>&             context,
                const boost::asio::ip::address&                             destinationIP) noexcept {

                boost::system::error_code ec;
                boost::asio::ip::udp::endpoint serverEP(serverIP, frame->Destination.Port);

                LOG_INFO("DirectDNS enter: server=%s port=%u", serverIP.to_string().c_str(), (uint32_t)frame->Destination.Port);
                bool opened = ppp::coroutines::asio::async_open(y, *socket, serverEP.protocol());
                if (!opened) {
                    LOG_INFO("DirectDNS enter: server=%s async_open=FAILED", serverIP.to_string().c_str());
                    return false;
                }

                int handle = socket->native_handle();
                ppp::net::Socket::AdjustDefaultSocketOptional(handle, serverIP.is_v4());
                ppp::net::Socket::SetTypeOfService(handle);
                ppp::net::Socket::SetSignalPipeline(handle, false);
                ppp::net::Socket::ReuseSocketAddress(handle, true);

#if defined(_LINUX)
                // Direct DNS servers are by definition reachable on the
                // physical network (SelectDirectDnsServer only returns them
                // for geo Direct decisions), so they must bypass the VPN.
                // On ColorOS the ip rule 13000 set hijacks any unmarked
                // socket back into the tunnel (IPv4 lands in an empty route
                // table and the query is dropped), and the VPN also claims
                // IPv6 via its ip -6 rules. Protect unconditionally - the
                // IsBypassIpAddress() geo/FIB gate is unreliable here and
                // previously skipped protect() for 223.5.5.5/119.29.29.29.
                if (!serverIP.is_loopback()) {
                    auto protector_network = GetProtectorNetwork();
                    LOG_INFO("DirectDNS protect: server=%s port=%u handle=%d protector=%s", serverIP.to_string().c_str(), (uint32_t)frame->Destination.Port, handle, (NULLPTR != protector_network) ? "yes" : "NO");
                    if (NULLPTR != protector_network) {
                        bool protect_ok = protector_network->Protect(handle, y);
                        LOG_INFO("DirectDNS protect: server=%s handle=%d result=%d", serverIP.to_string().c_str(), handle, (int)protect_ok);
                        if (!protect_ok) {
                            return false;
                        }
                    }
                }
#endif

                socket->send_to(boost::asio::buffer(messages->Buffer.get(), messages->Length), serverEP,
                    boost::asio::socket_base::message_end_of_record, ec);
                if (ec) {
                    return false;
                }

                const std::weak_ptr<boost::asio::ip::udp::socket> socket_weak(socket);
                const std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                
                const auto self = shared_from_this();
                const auto cb = make_shared_object<Timer::TimeoutEventHandler>(
                    [self, socket_weak](Timer*) noexcept {
                        const std::shared_ptr<boost::asio::ip::udp::socket> socket = socket_weak.lock();
                        if (socket) {
                            ppp::net::Socket::Closesocket(socket);
                        }
                    });
                if (NULLPTR == cb) {
                    return false;
                }

                const auto timeout = Timer::Timeout(context, (uint64_t)configuration->udp.dns.timeout * 1000, *cb);
                if (NULLPTR == timeout) {
                    return false;
                }

                if (!EmplaceTimeout(socket.get(), cb)) {
                    return false;
                }

                const auto max_buffer_size = PPP_BUFFER_SIZE - sizeof(serverEP);
                boost::asio::ip::udp::endpoint sourceEP = IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Source);
                boost::asio::ip::udp::endpoint destinationEP(destinationIP, frame->Destination.Port);

                socket->async_receive_from(boost::asio::buffer(buffer.get(), max_buffer_size), *reinterpret_cast<boost::asio::ip::udp::endpoint*>(buffer.get() + max_buffer_size),
                    [self, this, socket, timeout, buffer, sourceEP, destinationEP](boost::system::error_code ec, size_t sz) noexcept {
                        DeleteTimeout(socket.get());
                        if (ec == boost::system::errc::success) {
                            if (sz > 0) {
                                ::dns::Message m;
                                if (m.decode(reinterpret_cast<uint8_t*>(buffer.get()), sz) == ::dns::BufferResult::NoError) {
                                    // Prefer IPv4: strip AAAA from upstream responses.
                                    // Returns false if AAAA needs deferral (A cache not yet populated).
                                    if (!StripAAAADnsResponseIfIPv4Available(m)) {
                                        auto pending = make_shared_object<PendingAAAAResponse>();
                                        if (pending) {
                                            pending->EncodedPacket.assign(reinterpret_cast<char*>(buffer.get()), sz);
                                            pending->IsIPv6 = false;
                                            pending->SourceEP = sourceEP;
                                            pending->DestinationEP = destinationEP;
                                            pending->expire_time = Executors::GetTickCount() + static_cast<uint64_t>(configuration_->udp.dns.timeout) * 1000;
                                            pending_aaaa_[ppp::string(m.questions[0].mName.data())] = pending;
                                        }
                                    } else {
                                        size_t new_sz = 0;
                                        if (m.encode(reinterpret_cast<uint8_t*>(buffer.get()), sz, new_sz) == ::dns::BufferResult::NoError && new_sz > 0) {
                                            const bool output = DatagramOutput(sourceEP, destinationEP, buffer.get(), static_cast<int>(new_sz));
                                            if (!m.questions.empty()) {
                                                char answer_ips[512] = { 0 };
                                                size_t used = 0;
                                                for (auto& ans : m.answers) {
                                                    if (used >= sizeof(answer_ips) - 64) {
                                                        break;
                                                    }
                                                    char tmp[64];
                                                    if (ans.mType == ::dns::RecordType::kA) {
                                                        auto rdata = ans.getRData<::dns::RDataA>();
                                                        if (rdata && inet_ntop(AF_INET, rdata->getAddress(), tmp, sizeof(tmp))) {
                                                            used += (size_t)snprintf(answer_ips + used, sizeof(answer_ips) - used, "%s%s", used ? "," : "", tmp);
                                                        }
                                                    }
                                                    else if (ans.mType == ::dns::RecordType::kAAAA) {
                                                        auto rdata = ans.getRData<::dns::RDataAAAA>();
                                                        if (rdata && inet_ntop(AF_INET6, rdata->getAddress(), tmp, sizeof(tmp))) {
                                                            used += (size_t)snprintf(answer_ips + used, sizeof(answer_ips) - used, "%s%s", used ? "," : "", tmp);
                                                        }
                                                    }
                                                }
                                                LOG_INFO("DirectDNS response: host=%s bytes=%u output=%d ips=[%s]", m.questions[0].mName.data(), (uint32_t)new_sz, (int)output, answer_ips);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else if (ec) {
                            LOG_INFO("DirectDNS response: ec=%s sz=%u", ec.message().c_str(), (uint32_t)sz);
                        }

                        ppp::net::Socket::Closesocket(socket);
                        if (timeout) {
                            timeout->Stop();
                            timeout->Dispose();
                        }
                    });
                return true;
            }

            bool VEthernetNetworkSwitcher::RedirectDnsServer(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<UdpFrame>& frame, const std::shared_ptr<BufferSegment>& messages) noexcept {
                ::dns::Message m;
                if (m.decode(static_cast<uint8_t*>(messages->Buffer.get()), messages->Length) != ::dns::BufferResult::NoError) {
                    return false;
                }

                if (m.questions.empty()) {
                    return false;
                }
                
                boost::asio::ip::address destinationIP = Ipep::ToAddress(packet->Destination);
                ::dns::QuestionSection& qs = *m.questions.data();

#if defined(_WIN32)
                // Only host DNS packets that actually originated on our TUN may
                // enter the shared resolver.  Never accept a physical/ICS/WSL
                // source and inject its response through the TUN.
                if (std::shared_ptr<ITap> tap = GetTap(); NULLPTR != tap &&
                    packet->Source == tap->IPAddress &&
                    IPAddressIsGatewayServer(packet->Destination, tap->GatewayServer, tap->SubmaskAddress)) {
                    auto query = make_shared_object<ppp::string>(
                        reinterpret_cast<const char*>(messages->Buffer.get()), messages->Length);
                    const auto sourceEP = IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Source);
                    const boost::asio::ip::udp::endpoint destinationEP(destinationIP, frame->Destination.Port);
                    auto self = std::static_pointer_cast<VEthernetNetworkSwitcher>(shared_from_this());
                    DispatchLocalDnsQuery(query, false,
                        [self, sourceEP, destinationEP](const std::shared_ptr<ppp::string>& response) noexcept {
                            if (!response || response->empty()) {
                                return;
                            }
                            const bool output = self->DatagramOutput(sourceEP, destinationEP,
                                const_cast<char*>(response->data()), static_cast<int>(response->size()), false);
                            LOG_DEBUG("DNS pipeline: inject source=%s:%u, dns=%s:%u, bytes=%llu, output=%d",
                                sourceEP.address().to_string().data(), sourceEP.port(),
                                destinationEP.address().to_string().data(), destinationEP.port(),
                                (unsigned long long)response->size(), (int)output);
                        });
                    return true;
                }
#endif

                const bool address_query = qs.mType == ::dns::RecordType::kA ||
                    qs.mType == ::dns::RecordType::kAAAA;
                boost::asio::ip::address serverIP;
                bool geo_direct_dns = SelectDirectDnsServer(
                    stl::transform<ppp::string>(qs.mName), serverIP);
                LOG_DEBUG("DNS pipeline: query host=%s type=%d address_query=%d geo_direct_dns=%d dest=%s",
                    qs.mName.data(), (int)qs.mType, (int)address_query, (int)geo_direct_dns,
                    destinationIP.to_string().data());
                // Direct GeoSite decisions must be resolved by a direct DNS server.
                // A tunnel-origin cache answer can point a domestic domain at an
                // overseas CDN and must not win before the domain policy is observed.
                if (address_query && !geo_direct_dns &&
                    !ppp::net::asio::vdns::QueryCache2(qs.mName.data(), m, qs.mType == ::dns::RecordType::kA ?
                    ppp::net::asio::vdns::AddressFamily::kA : ppp::net::asio::vdns::AddressFamily::kAAAA).empty()) {

                    LOG_DEBUG("DNS pipeline: cache hit host=%s, answering from cache", qs.mName.data());
                    // Prefer IPv4: cache is always clean (filtered before AddCache),
                    // so no need to strip here. Forward directly.
                    std::size_t dns_size = 0;
                    char dns_packet[PPP_MAX_DNS_PACKET_BUFFER_SIZE]; 

                    if (m.encode(dns_packet, PPP_MAX_DNS_PACKET_BUFFER_SIZE, dns_size) == ::dns::BufferResult::NoError && dns_size > 0) {
                        return DatagramOutput(
                            IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Source), 
                            boost::asio::ip::udp::endpoint(destinationIP, PPP_DNS_SYS_PORT), dns_packet, dns_size, false);
                    }
                }

                if (geo_direct_dns) {
                    LOG_INFO("GeoSite DNS direct: host=%s, server=%s",
                        qs.mName.data(), serverIP.to_string().data());
                }
                elif(std::shared_ptr<ITap> tap = GetTap(); IPAddressIsGatewayServer(packet->Destination, tap->GatewayServer, tap->SubmaskAddress)) {
                    LOG_DEBUG("DNS pipeline: gateway target host=%s, using dnsServers head", qs.mName.data());
                    auto& dnsServers = ppp::net::asio::vdns::servers;
                    if (dnsServers->empty()) {
                        return false;
                    }

                    serverIP = dnsServers->begin()->address();
                }
                else {
                    ppp::app::client::dns::Rule::Ptr rulePtr = ppp::app::client::dns::Rule::Get(stl::transform<ppp::string>(qs.mName), dns_ruless_[0], dns_ruless_[1], dns_ruless_[2]);
                    if (NULLPTR == rulePtr) {
                        LOG_DEBUG("DNS pipeline: rule miss host=%s, falling back to tunnel", qs.mName.data());
                        return false;
                    }

                    if (rulePtr->Server == destinationIP) {
                        LOG_DEBUG("DNS pipeline: rule server == destination, falling back to tunnel host=%s server=%s",
                            qs.mName.data(), rulePtr->Server.to_string().data());
                        return false;
                    }

                    serverIP = rulePtr->Server;
                    LOG_DEBUG("DNS pipeline: rule hit host=%s server=%s nic=%d",
                        qs.mName.data(), serverIP.to_string().data(), (int)rulePtr->Nic);
                }

                std::shared_ptr<boost::asio::io_context> context = exchanger->GetContext();
                if (NULLPTR == context) {
                    LOG_INFO("DirectDNS spawn: host=%s context=NO", qs.mName.data());
                    return false;
                }

                std::shared_ptr<Byte> buffer = exchanger->GetBuffer();
                if (NULLPTR == buffer) {
                    LOG_INFO("DirectDNS spawn: host=%s buffer=NO", qs.mName.data());
                    return false;
                }

                const std::shared_ptr<boost::asio::ip::udp::socket> socket = make_shared_object<boost::asio::ip::udp::socket>(*context);
                if (!socket) {
                    LOG_INFO("DirectDNS spawn: host=%s socket=NO", qs.mName.data());
                    return false;
                }

                const auto self = shared_from_this();
                const auto allocator = configuration_->GetBufferAllocator();

                LOG_INFO("DirectDNS spawn: host=%s server=%s context=OK buffer=OK socket=OK alloc=%s", qs.mName.data(), serverIP.to_string().c_str(), (NULLPTR != allocator) ? "OK" : "NO");
                return ppp::coroutines::YieldContext::Spawn(allocator.get(), *context,
                    [self, this, socket, buffer, frame, messages, context, serverIP, destinationIP](ppp::coroutines::YieldContext& y) noexcept {
                        return RedirectDnsServer(y, socket, buffer, serverIP, frame, messages, context, destinationIP);
                    });
            }

            bool VEthernetNetworkSwitcher::StaticMode(bool* static_mode) noexcept {
                SynchronizedObjectScope scope(GetSynchronizedObject());
                bool snow = static_mode_;
                if (NULLPTR != static_mode) {
                    static_mode_ = *static_mode;
                }

                return snow;
            }

            uint16_t VEthernetNetworkSwitcher::Mux(uint16_t* mux) noexcept {
                SynchronizedObjectScope scope(GetSynchronizedObject());
                uint16_t snow = mux_;
                if (NULLPTR != mux) {
                    mux_ = *mux;
                }

                return snow;
            }

            uint8_t VEthernetNetworkSwitcher::MuxAcceleration(uint8_t* mux_acceleration) noexcept {
                SynchronizedObjectScope scope(GetSynchronizedObject());
                uint8_t snow = mux_acceleration_;
                if (NULLPTR != mux_acceleration) {
                    mux_acceleration_ = *mux_acceleration;
                }

                return snow;
            }

            bool VEthernetNetworkSwitcher::OnUpdate(uint64_t now) noexcept {
                if (VEthernet::OnUpdate(now)) {
                    if (!outbound_exchangers_.empty()) {
                        ppp::vector<std::shared_ptr<VEthernetExchanger>> updated_exchangers;
                        for (auto& entry : outbound_exchangers_) {
                            if (NULLPTR == entry.second) continue;
                            bool duplicate = false;
                            for (const auto& updated : updated_exchangers) {
                                if (updated == entry.second) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (!duplicate) {
                                updated_exchangers.emplace_back(entry.second);
                                entry.second->StaticEchoSwapAsynchronousSocket();
                            }
                        }
                    }
                    else {
                        std::shared_ptr<VEthernetExchanger> exchanger = exchanger_;
                        if (NULLPTR != exchanger) exchanger->StaticEchoSwapAsynchronousSocket();
                    }
                }

                return false;
            }

#if !defined(_ANDROID) && !defined(_IPHONE)   
#if defined(_LINUX)
            bool VEthernetNetworkSwitcher::ProtectMode(bool* protect_mode) noexcept {
                SynchronizedObjectScope scope(GetSynchronizedObject());
                bool snow = protect_mode_;
                if (NULLPTR != protect_mode) {
                    protect_mode_ = *protect_mode;
                }

                return snow;
            }
#endif
#endif
        }
    }
}
