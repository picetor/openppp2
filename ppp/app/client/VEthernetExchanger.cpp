#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetDatagramPort.h>
#include <ppp/app/protocol/VirtualEthernetPacket.h>
#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/collections/Dictionary.h>
#include <ppp/auxiliary/UriAuxiliary.h>
#include <ppp/auxiliary/StringAuxiliary.h>
#include <ppp/IDisposable.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/asio/asio.h>
#include <ppp/net/packet/IPFrame.h>
#include <ppp/threading/Timer.h>
#include <ppp/threading/Executors.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/transmissions/ITransmission.h>
#include <ppp/transmissions/ITcpipTransmission.h>
#include <ppp/transmissions/IWebsocketTransmission.h>
#include <ppp/diagnostics/Telemetry.h>

typedef ppp::app::protocol::VirtualEthernetInformation              VirtualEthernetInformation;
typedef ppp::app::protocol::VirtualEthernetPacket                   VirtualEthernetPacket;
typedef ppp::collections::Dictionary                                Dictionary;
typedef ppp::auxiliary::StringAuxiliary                             StringAuxiliary;
typedef ppp::net::AddressFamily                                     AddressFamily;
typedef ppp::net::Socket                                            Socket;
typedef ppp::net::IPEndPoint                                        IPEndPoint;
typedef ppp::net::Ipep                                              Ipep;
typedef ppp::threading::Timer                                       Timer;
typedef ppp::threading::Executors                                   Executors;
typedef ppp::transmissions::ITransmission                           ITransmission;
typedef ppp::transmissions::ITcpipTransmission                      ITcpipTransmission;
typedef ppp::transmissions::IWebsocketTransmission                  IWebsocketTransmission;
typedef ppp::transmissions::ISslWebsocketTransmission               ISslWebsocketTransmission;

namespace ppp {
    namespace app {
        namespace client {
            static constexpr int SEND_ECHO_KEEP_ALIVE_PACKET_MIN_TIMEOUT = 1000;
            static constexpr int SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT = 5000;
            static constexpr int SEND_ECHO_KEEP_ALIVE_PACKET_MMX_TIMEOUT = SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT << 2;
            static constexpr int STATIC_ECHO_KEEP_ALIVED_ID              = IPEndPoint::NoneAddress - 1;

            namespace {
                /**
                 * @brief One tunnel entry candidate that can be probed.
                 */
                struct ProbeCandidateEntry final {
                    ppp::string                         entry;              ///< Normalized "host:port" cache key.
                    ppp::string                         hostname;           ///< Host name parsed from the entry URL.
                    ppp::string                         address;            ///< Resolved IP address.
                    ppp::string                         path;               ///< WebSocket path (inherited from client.server).
                    ppp::string                         server;             ///< Normalized URL used by server_url_.
                    int                                 port = IPEndPoint::MinPort;
                    VEthernetExchanger::ProtocolType    protocol_type = VEthernetExchanger::ProtocolType::ProtocolType_PPP;
                    boost::asio::ip::tcp::endpoint      remoteEP;
                    ConnectivityProbe::ProbeType        probe_type = ConnectivityProbe::ProbeType_Tcp;
                    ppp::string                         probe_category;
                    bool                                probed = false;
                    bool                                reachable = false;
                    int                                 rtt_ms = 0;
                };

                /**
                 * @brief Maps a tunnel protocol to the probe that exercises its transport.
                 */
                ConnectivityProbe::ProbeType ProbeTypeFromProtocol(VEthernetExchanger::ProtocolType protocol_type) noexcept {
                    if (protocol_type == VEthernetExchanger::ProtocolType::ProtocolType_Http ||
                        protocol_type == VEthernetExchanger::ProtocolType::ProtocolType_WebSocket) {
                        return ConnectivityProbe::ProbeType_WebSocket;
                    }
                    elif(protocol_type == VEthernetExchanger::ProtocolType::ProtocolType_HttpSSL ||
                        protocol_type == VEthernetExchanger::ProtocolType::ProtocolType_WebSocketSSL) {
                        return ConnectivityProbe::ProbeType_WebSocketSSL;
                    }
                    return ConnectivityProbe::ProbeType_Tcp;
                }

                /**
                 * @brief Normalizes a resolved address into the "host:port" probe key.
                 */
                ppp::string NormalizeProbeEntry(const ppp::string& address, int port) noexcept {
                    if (address.empty() || port <= IPEndPoint::MinPort || port > IPEndPoint::MaxPort) {
                        return ppp::string();
                    }

                    ppp::string entry = address.find(':') != ppp::string::npos ? "[" + address + "]" : address;
                    entry += ":";
                    entry += stl::to_string<ppp::string>(port);
                    return entry;
                }

                /**
                 * @brief Median of a sorted value sequence; 0 for an empty sequence.
                 */
                int MedianOfSortedValues(const ppp::vector<int>& sorted_values) noexcept {
                    if (sorted_values.empty()) {
                        return 0;
                    }
                    const std::size_t n = sorted_values.size();
                    return (n % 2 == 1) ? sorted_values[n / 2] : (sorted_values[n / 2 - 1] + sorted_values[n / 2]) / 2;
                }

                static constexpr std::size_t ENTRY_QUALITY_HISTORY_CAP = 60;
                static constexpr std::size_t ENTRY_QUALITY_MIN_SAMPLES = 6;

                int EntryReliabilityTier(const ConnectivityProbe::Result& result) noexcept {
                    const std::size_t samples = result.reachability_samples.size();
                    if (samples < ENTRY_QUALITY_MIN_SAMPLES) {
                        return 2; // Bootstrap: usable, but never outrank proven A/B entries.
                    }

                    std::size_t successes = 0;
                    for (uint8_t sample : result.reachability_samples) {
                        successes += sample != 0 ? 1 : 0;
                    }
                    const double success_rate = static_cast<double>(successes) / static_cast<double>(samples);
                    if (success_rate >= 0.98) {
                        return 0;
                    }
                    if (success_rate >= 0.90) {
                        return 1;
                    }
                    return 3; // Last probe succeeded, but recent reliability is poor.
                }

                double EntryLatencyRisk(const ConnectivityProbe::Result& result) noexcept {
                    if (result.rtt_samples.empty()) {
                        return std::numeric_limits<double>::max();
                    }

                    ppp::vector<int> sorted = result.rtt_samples;
                    std::stable_sort(sorted.begin(), sorted.end());
                    const std::size_t n = sorted.size();
                    const int p50 = MedianOfSortedValues(sorted);
                    const int p95 = sorted[((n - 1) * 95) / 100];

                    ppp::vector<int> deviations;
                    deviations.reserve(n);
                    for (int sample : sorted) {
                        deviations.emplace_back(sample >= p50 ? sample - p50 : p50 - sample);
                    }
                    std::stable_sort(deviations.begin(), deviations.end());
                    const int mad = MedianOfSortedValues(deviations);

                    double risk = static_cast<double>(p50) +
                        1.5 * static_cast<double>(std::max<int>(0, p95 - p50)) +
                        0.5 * static_cast<double>(mad);
                    if (result.reachability_samples.size() < ENTRY_QUALITY_MIN_SAMPLES) {
                        risk += 100.0; // Confidence penalty for a newly discovered entry.
                    }
                    return risk;
                }
            }

            VEthernetExchanger::VEthernetExchanger(
                const VEthernetNetworkSwitcherPtr&      switcher,
                const AppConfigurationPtr&              configuration,
                const ContextPtr&                       context,
                const Int128&                           id,
                const ppp::string&                      outbound_tag,
                bool                                    primary_outbound) noexcept
                : VirtualEthernetLinklayer(configuration, context, id)
                , disposed_(false)
                , sekap_last_(0)
                , sekap_next_(0)
                , switcher_(switcher)
                , outbound_tag_(outbound_tag)
                , primary_outbound_(primary_outbound)
                , network_state_(NetworkState_Connecting)
                , static_echo_input_(false)
                , static_echo_timeout_(UINT64_MAX)
                , static_echo_session_id_(0)
                , static_echo_remote_port_(IPEndPoint::MinPort) {

                if (configuration->key.protocol.size() > 0 && configuration->key.protocol_key.size() > 0 && 
                    configuration->key.transport.size() > 0 && configuration->key.transport_key.size() > 0) {
                    if (Ciphertext::Support(configuration->key.protocol) && Ciphertext::Support(configuration->key.transport)) {
                        static_echo_protocol_ = make_shared_object<Ciphertext>(configuration->key.protocol, configuration->key.protocol_key);
                        static_echo_transport_ = make_shared_object<Ciphertext>(configuration->key.transport, configuration->key.transport_key);
                    }
                }
                
                buffer_                   = Executors::GetCachedBuffer(context);
                // Android's libopenppp2 run() creates a dedicated io_context
                // that is never registered with Executors::Internal->Buffers,
                // so GetCachedBuffer() returns null there and DNS-direct
                // (RedirectDnsServer) silently bails out before the query is
                // even sent (buffer==NO), leaving the resolver to fall back
                // to the tunnel and answer domestic domains with overseas IPs.
                // Fall back to a dedicated allocation in that case.
                if (NULLPTR == buffer_) {
                    buffer_ = ppp::threading::BufferswapAllocator::MakeByteArray(
                        configuration->GetBufferAllocator(), PPP_BUFFER_SIZE);
                }
                server_url_.port          = 0;
                server_url_.protocol_type = ProtocolType::ProtocolType_PPP;
            }

            bool VEthernetExchanger::SetPrimaryOutbound(bool primary) noexcept {
                if (disposed_) {
                    return false;
                }

                if (primary_outbound_.load() == primary) {
                    return true;
                }

                // A secondary may own a forwarding proxy from its own JSON,
                // while the primary uses the switcher's forwarding path.  On
                // demotion, prepare the secondary proxy before changing the
                // role; on promotion, release the secondary-only proxy after
                // the role has changed.
                IForwardingPtr secondary_forwarding;
                if (!primary) {
                    AppConfigurationPtr configuration = GetConfiguration();
                    if (NULLPTR == configuration) {
                        return false;
                    }

                    if (!configuration->client.server_proxy.empty()) {
                        ContextPtr context = GetContext();
                        if (NULLPTR == context) {
                            return false;
                        }

                        secondary_forwarding = make_shared_object<IForwarding>(context, configuration);
                        if (NULLPTR == secondary_forwarding || !secondary_forwarding->Open()) {
                            if (NULLPTR != secondary_forwarding) {
                                secondary_forwarding->Dispose();
                            }
                            LOG_ERROR("VEthernetExchanger::SetPrimaryOutbound: cannot prepare forwarding proxy, outbound=%s, primary=0",
                                outbound_tag_.data());
                            return false;
                        }
#if defined(_LINUX)
                        secondary_forwarding->ProtectorNetwork = switcher_->GetProtectorNetwork();
#endif
                    }
                }

                IForwardingPtr obsolete_forwarding;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    if (primary_outbound_.load() == primary) {
                        // Another transition won the race while the
                        // secondary forwarding proxy was being prepared.
                        // It is not installed in this case.
                        obsolete_forwarding = std::move(secondary_forwarding);
                    }
                    else {
                        if (primary) {
                            obsolete_forwarding = std::move(forwarding_);
                        }
                        else {
                            forwarding_ = secondary_forwarding;
                            assigned_ipv6_address_ = boost::asio::ip::address();
                        }
                        primary_outbound_.store(primary);
                    }
                }

                if (NULLPTR != obsolete_forwarding) {
                    obsolete_forwarding->Dispose();
                }

                // Static mappings belong to the active client configuration,
                // not to every warm split/backup exchanger.  Remove them
                // when this exchanger is demoted; when it is promoted below,
                // register the mappings from its own configuration.
                if (!primary) {
                    UnregisterAllMappingPorts();
                }
                else {
                    RegisterAllMappingPorts();
                }

                LOG_INFO("VEthernetExchanger::SetPrimaryOutbound: outbound=%s, primary=%d",
                    outbound_tag_.data(), (int)primary);
                return true;
            }

            int VEthernetExchanger::GetProbeRtt() noexcept {
                return probe_rtt_ms_.load();
            }

            bool VEthernetExchanger::GetProbeReachable() noexcept {
                return probe_reachable_.load();
            }

            bool VEthernetExchanger::GetProbeChecked() noexcept {
                return probe_checked_.load();
            }

            ppp::string VEthernetExchanger::GetProbeServer() noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return probe_server_;
            }

            ppp::string VEthernetExchanger::GetCurrentEntry() noexcept {
                SynchronizedObjectScope scope(syncobj_);
                if (network_state_.load() != NetworkState_Established) {
                    return ppp::string();
                }

                // A MUX generation has its own pinned entry.  It is the
                // effective data path while that MUX is established; probe
                // ranking and the next-generation preference are not the
                // same thing as the current path.
                std::shared_ptr<vmux::vmux_net> mux = mux_;
                if (mux != NULLPTR && mux->is_established() && !mux_entry_.empty()) {
                    return mux_entry_;
                }

                // Without an established MUX, report the transport endpoint
                // that currently owns the live connection.
                if (!server_url_.hostname.empty() &&
                    server_url_.port > IPEndPoint::MinPort &&
                    server_url_.port <= IPEndPoint::MaxPort) {
                    return NormalizeProbeEntry(server_url_.hostname, server_url_.port);
                }
                return ppp::string();
            }

            ppp::string VEthernetExchanger::GetRankedFirstEntry() noexcept {
                const ppp::vector<ppp::string> ranked = HotSwitchRankedEntries(Executors::GetTickCount());
                return ranked.empty() ? ppp::string() : ranked.front();
            }

            bool VEthernetExchanger::SwitchToRankedFirstEntry() noexcept {
                const ppp::string target_entry = GetRankedFirstEntry();
                if (target_entry.empty()) {
                    return false;
                }
                if (target_entry == GetCurrentEntry()) {
                    return true;
                }

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context || context->stopped() || disposed_) {
                    return false;
                }

                auto self = shared_from_this();
                boost::asio::post(*context, [self, this, target_entry]() noexcept {
                    if (disposed_) {
                        return;
                    }

                    // The entry was obtained from the same fresh/reachable/
                    // unpenalized ranking used by automatic failover.  Pin the
                    // next MUX generation to it; never mix entries inside one
                    // generation, which would amplify packet reordering.
                    const bool mux_enabled = NULLPTR != switcher_ && switcher_->mux_ > 0;
                    std::shared_ptr<vmux::vmux_net> old_mux;
                    ITransmissionPtr owner_transmission;
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        probe_server_ = target_entry;
                        mux_entry_ = target_entry;
                        mux_failure_entry_.clear();
                        mux_failure_last_ = 0;
                        mux_failure_streak_ = 0;
                        mux_retry_not_before_ = 0;
                        mux_retry_backoff_ms_ = 0;
                        if (mux_enabled) {
                            old_mux = std::move(mux_);
                        }
                        else {
                            owner_transmission = transmission_;
                        }
                    }

                    if (NULLPTR != old_mux) {
                        LOG_INFO("VEthernetExchanger::SwitchToRankedFirstEntry: rebuilding MUX on ranked #1 entry %s",
                            target_entry.data());
                        old_mux->close_exec();
                    }
                    elif(NULLPTR != owner_transmission) {
                        LOG_INFO("VEthernetExchanger::SwitchToRankedFirstEntry: reconnecting owner transport on ranked #1 entry %s",
                            target_entry.data());
                        owner_transmission->Dispose();
                    }
                    else {
                        LOG_INFO("VEthernetExchanger::SwitchToRankedFirstEntry: next connection uses ranked #1 entry %s",
                            target_entry.data());
                    }
                });
                return true;
            }

            bool VEthernetExchanger::ProbeCandidateEndpoint(
                ConnectivityProbe::ProbeType                                    probe_type,
                const boost::asio::ip::tcp::endpoint&                           remoteEP,
                const ppp::string&                                              hostname,
                const ppp::string&                                              path,
                int                                                             stage,
                int                                                             timeout_ms,
                const ppp::string&                                              ws_host,
                const ppp::string&                                              ws_sni,
                YieldContext&                                                   y,
                int&                                                            rtt_ms,
                const ConnectivityProbe::ProtectSocketHandler&                  protect) noexcept {

                rtt_ms = 0;
                if (disposed_) {
                    return false;
                }
                if (remoteEP.port() <= IPEndPoint::MinPort || remoteEP.port() > IPEndPoint::MaxPort) {
                    return false;
                }
                if (IPEndPoint::IsInvalid(remoteEP.address())) {
                    return false;
                }

                // stage 1/2 only exercises the TCP transport; stage 3 upgrades
                // the WebSocket/TLS layers exactly like the real transmission.
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

            void VEthernetExchanger::StoreProbeResult(
                const ppp::string&                                              entry,
                ConnectivityProbe::ProbeType                                    probe_type,
                bool                                                            reachable,
                int                                                             rtt_ms,
                int                                                             stage,
                uint64_t                                                        now,
                uint64_t                                                        ttl_ms) noexcept {

                if (entry.empty()) {
                    return;
                }

                ConnectivityProbe::Result result;
                result.entry = entry;
                result.type = static_cast<uint8_t>(probe_type);
                result.reachable = reachable;
                result.rtt_ms = rtt_ms;
                result.stage = static_cast<uint8_t>(stage);
                result.timestamp = now;
                result.ttl_ms = static_cast<int>(ttl_ms);
                result.penalty_until = 0;

                SynchronizedObjectScope scope(syncobj_);
                // Never clear an active blacklist penalty with a fresh result:
                // the inline probe path calls StoreProbeResult without the
                // switcher-side blacklist check, so a reachable entry must not
                // reset penalty_until while the penalty period is still running.
                ProbeResultTable::iterator it = probe_results_.find(entry);
                if (it != probe_results_.end() && it->second.penalty_until > now) {
                    return;
                }

                // Keep roughly five minutes of outcomes at the normal 5-second refresh
                // cadence.  Healthy operation only updates this ranking history; it
                // never causes a live entry switch.
                if (it != probe_results_.end()) {
                    result.rtt_samples = it->second.rtt_samples;
                    result.reachability_samples = it->second.reachability_samples;
                }
                result.reachability_samples.emplace_back(reachable ? 1 : 0);
                if (result.reachability_samples.size() > ENTRY_QUALITY_HISTORY_CAP) {
                    result.reachability_samples.erase(result.reachability_samples.begin());
                }

                if (reachable && rtt_ms >= 0) {
                    result.rtt_samples.push_back(rtt_ms);
                    if (result.rtt_samples.size() > ENTRY_QUALITY_HISTORY_CAP) {
                        result.rtt_samples.erase(result.rtt_samples.begin());
                    }
                }
                result.samples_full = result.rtt_samples.size() >= ENTRY_QUALITY_MIN_SAMPLES &&
                    result.reachability_samples.size() >= ENTRY_QUALITY_MIN_SAMPLES;
                result.jitter_ms = ConnectivityProbe::ComputeJitter(result.rtt_samples);

                probe_results_[entry] = std::move(result);
            }

            bool VEthernetExchanger::ProbeSelectServerEndPoint(
                YieldContext&                                                   y,
                const ppp::vector<ppp::string>&                                 entries,
                ppp::string&                                                    hostname,
                ppp::string&                                                    address,
                ppp::string&                                                    path,
                int&                                                            port,
                ProtocolType&                                                   protocol_type,
                ppp::string&                                                    server,
                boost::asio::ip::tcp::endpoint&                                 remoteEP,
                const ppp::string*                                              forced_entry) noexcept {

                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }
                if (!y) {
                    return false;
                }
                if (disposed_) {
                    return false;
                }

                // Parse every entry.  The primary carries a full URL (scheme,
                // path and websocket overrides); backup entries are bare
                // "IP:port" strings that inherit the primary transport.
                ppp::string primary_address;
                ppp::string primary_path;
                ProtocolType primary_protocol = ProtocolType::ProtocolType_PPP;
                ppp::vector<ProbeCandidateEntry> candidates;
                candidates.reserve(entries.size());
                for (const ppp::string& entry : entries) {
                    if (entry.empty()) {
                        continue;
                    }

                    ProbeCandidateEntry candidate;
                    bool is_primary = primary_address.empty();
                    if (is_primary) {
                        ppp::string entry_hostname;
                        ppp::string entry_address;
                        ppp::string entry_path;
                        int entry_port = IPEndPoint::MinPort;
                        ProtocolType entry_protocol = ProtocolType::ProtocolType_PPP;
                        ppp::string abs_url;
                        ppp::string entry_server = UriAuxiliary::Parse(entry, entry_hostname, entry_address,
                            entry_path, entry_port, entry_protocol, &abs_url, y);
                        if (entry_server.empty() || entry_hostname.empty() || entry_address.empty()) {
                            continue;
                        }
                        if (entry_port <= IPEndPoint::MinPort || entry_port > IPEndPoint::MaxPort) {
                            continue;
                        }

                        // The socks scheme is parser-only for tunnels; it lands
                        // on the raw TCP transport, so probe it as plain TCP.
                        if (entry_protocol == ProtocolType::ProtocolType_Socks) {
                            entry_protocol = ProtocolType::ProtocolType_PPP;
                        }

                        IPEndPoint ipep(entry_address.data(), entry_port);
                        if (IPEndPoint::IsInvalid(ipep)) {
                            continue;
                        }

                        candidate.hostname = entry_hostname;
                        candidate.address = entry_address;
                        candidate.path = entry_path;
                        candidate.server = entry_server;
                        candidate.port = entry_port;
                        candidate.protocol_type = entry_protocol;
                        candidate.remoteEP = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(ipep);
                        candidate.probe_type = ProbeTypeFromProtocol(entry_protocol);

                        primary_address = entry_address;
                        primary_path = entry_path;
                        primary_protocol = entry_protocol;
                    }
                    else {
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
                            // Hostname backup entry: resolve it now so ws/wss and
                            // ppp tunnels can use domains exactly like the primary.
                            // Resolution may suspend this coroutine; no lock is
                            // held on this path.
                            boost::asio::ip::udp::endpoint resolved =
                                ppp::coroutines::asio::GetAddressByHostName<boost::asio::ip::udp>(
                                    host_string.data(), entry_port, y);
                            address = resolved.address();
                        }

                        std::string address_string = address.to_string();
                        ppp::string address_string_ppp(address_string.data(), address_string.size());
                        IPEndPoint ipep(address_string_ppp.data(), entry_port);
                        if (IPEndPoint::IsInvalid(ipep)) {
                            // Unresolvable host: keep the entry visible in the
                            // status page but mark it unreachable instead of
                            // silently dropping it.
                            candidate.hostname = host_string;
                            candidate.address = host_string;
                            candidate.path = primary_path;
                            candidate.port = entry_port;
                            candidate.protocol_type = primary_protocol;
                            candidate.probe_type = ProbeTypeFromProtocol(primary_protocol);
                            candidate.server = entry;
                            candidate.probed = true;   // Decided without a probe.
                            candidate.reachable = false;
                        }
                        else {
                            // Backups reuse the primary scheme and path.
                            candidate.hostname = host_string;
                            candidate.address = host_string;
                            candidate.path = primary_path;
                            candidate.port = entry_port;
                            candidate.protocol_type = primary_protocol;
                            candidate.remoteEP = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(ipep);
                            candidate.probe_type = ProbeTypeFromProtocol(primary_protocol);
                            candidate.server = entry; // Informational only; the legacy path owns the URL.
                        }
                    }

                    candidate.probe_category = candidate.probe_type == ConnectivityProbe::ProbeType_WebSocket ? "ws" :
                        (candidate.probe_type == ConnectivityProbe::ProbeType_WebSocketSSL ? "wss" : "tcp");
                    // Key the probe cache/stickiness on the logical entry
                    // (hostname:port) instead of the resolved IP:port, so a
                    // DNS re-resolution (CDN) never invalidates the sticky
                    // preference or the cached result.
                    candidate.entry = NormalizeProbeEntry(candidate.hostname, candidate.port);
                    if (candidate.entry.empty()) {
                        continue;
                    }

                    candidates.emplace_back(std::move(candidate));
                }
                if (candidates.empty()) {
                    LOG_DEBUG("VEthernetExchanger::ProbeSelectServerEndPoint: no parseable candidates, forced=%s", forced_entry ? forced_entry->data() : "<null>");
                    return false;
                }

#if defined(_WIN32)
                // Pin every candidate's route on the physical adapter so probe
                // sockets never enter the TAP (self-loop) during reconnection.
                if (NULLPTR != switcher_) {
                    for (const ProbeCandidateEntry& candidate : candidates) {
                        boost::asio::ip::address probe_ip = candidate.remoteEP.address();
                        if (probe_ip.is_v4()) {
                            switcher_->EnsureWindowsIPv4ServerRoute(probe_ip);
                        }
                        elif(probe_ip.is_v6()) {
                            switcher_->EnsureWindowsIPv6ServerRoute(probe_ip);
                        }
                    }
                }
#endif

                ConnectivityProbe::ProtectSocketHandler protector;
#if defined(_LINUX)
                if (NULLPTR != switcher_) {
                    std::shared_ptr<ppp::net::ProtectorNetwork> protector_network = switcher_->GetProtectorNetwork();
                    if (NULLPTR != protector_network) {
                        protector = [protector_network](int sockfd) noexcept {
                            return protector_network->Protect(sockfd);
                        };
                    }
                }
#endif

                const auto& probe_cfg = configuration->client.probe;
                // Probe depth and concurrency are built-in fixed behavior: the
                // established path always probes to L3 (WebSocket upgrade), all
                // entries probe in parallel, and the category is derived from the
                // entry URI protocol.
                const int stage = 3;
                const int timeout_ms = std::max<int>(50, probe_cfg.timeout_ms);
                const uint64_t ttl_ms = static_cast<uint64_t>(std::max<int>(1, probe_cfg.ttl_seconds)) * 1000;
                const ppp::string ws_host = configuration->client.websocket.host;
                const ppp::string ws_sni = configuration->client.websocket.sni;
                const uint64_t now = Executors::GetTickCount();

                // Build the job list: entries that need a live probe this round.
                ppp::vector<int> job_indices;
                job_indices.reserve(candidates.size());
                for (std::size_t i = 0; i < candidates.size(); i++) {
                    ProbeCandidateEntry& candidate = candidates[i];
                    if (candidate.probed) {
                        continue; // Already decided (e.g. unresolvable host); no probe needed.
                    }

                    bool cached = false;
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        auto it = probe_results_.find(candidate.entry);
                        if (it != probe_results_.end()) {
                            const ConnectivityProbe::Result& result = it->second;
                            if (result.penalty_until > now) {
                                candidate.reachable = false; // Temporarily blacklisted.
                                candidate.probed = true;
                                cached = true;
                            }
                            elif(result.reachable && static_cast<int>(result.stage) >= stage &&
                                now < result.timestamp + static_cast<uint64_t>(result.ttl_ms)) {
                                candidate.reachable = true;
                                candidate.rtt_ms = result.rtt_ms;
                                candidate.probed = true;
                                cached = true;
                            }
                        }
                    }
                    if (!cached) {
                        job_indices.emplace_back(static_cast<int>(i));
                    }
                }

                // Probe the pending entries (always parallel when more than one).
                if (!job_indices.empty()) {
                    if (job_indices.size() > 1) {
                        struct ProbeWaitState final {
                            std::atomic<int> pending = 0;
                            ppp::vector<ProbeCandidateEntry>* candidates = NULLPTR;
                            YieldContext* parent = NULLPTR;
                            int stage = 0;
                            int timeout_ms = 0;
                            ppp::string ws_host;
                            ppp::string ws_sni;
                        };
                        ProbeWaitState state;
                        state.pending = static_cast<int>(job_indices.size());
                        state.candidates = &candidates;
                        state.parent = &y;
                        state.stage = stage;
                        state.timeout_ms = timeout_ms;
                        state.ws_host = ws_host;
                        state.ws_sni = ws_sni;

                        boost::asio::io_context& context = y.GetContext();
                        auto self = shared_from_this();
                        for (int index : job_indices) {
                            bool spawned = YieldContext::Spawn(NULLPTR, context,
                                [self, this, index, protector, &state](YieldContext& cy) noexcept {
                                    ProbeCandidateEntry& candidate = (*state.candidates)[index];
                                    int rtt = 0;
                                    bool ok = false;
                                    if (!disposed_) {
                                        ok = ProbeCandidateEndpoint(candidate.probe_type,
                                            candidate.remoteEP, candidate.hostname, candidate.path,
                                            state.stage, state.timeout_ms, state.ws_host, state.ws_sni, cy, rtt, protector);
                                    }
                                    candidate.reachable = ok;
                                    candidate.rtt_ms = rtt;
                                    candidate.probed = true;

                                    if (state.pending.fetch_sub(1) == 1) {
                                        state.parent->R(); // Last one wakes the caller.
                                    }
                                });
                            if (!spawned) {
                                // Spawn failed; account for the missing job so the
                                // caller is still woken when the remaining probes finish.
                                if (state.pending.fetch_sub(1) == 1) {
                                    state.parent->R();
                                }
                            }
                        }
                        y.Suspend();
                    }
                    else {
                        for (int index : job_indices) {
                            ProbeCandidateEntry& candidate = candidates[index];
                            int rtt = 0;
                            bool ok = ProbeCandidateEndpoint(candidate.probe_type,
                                candidate.remoteEP, candidate.hostname, candidate.path,
                                stage, timeout_ms, ws_host, ws_sni, y, rtt, protector);
                            candidate.reachable = ok;
                            candidate.rtt_ms = rtt;
                            candidate.probed = true;
                        }
                    }

                    // Persist the fresh outcomes so the next round can use the cache.
                    for (int index : job_indices) {
                        const ProbeCandidateEntry& candidate = candidates[index];
                        StoreProbeResult(candidate.entry, candidate.probe_type,
                            candidate.reachable, candidate.rtt_ms, stage, now, ttl_ms);
                    }
                }

                // Forced entry (hot-switch preheat): select exactly this entry when
                // reachable; stickiness is intentionally NOT updated so a preheated
                // backup never steals the primary preference before activation.
                if (forced_entry != NULLPTR && !forced_entry->empty()) {
                    int forced_index = -1;
                    for (std::size_t i = 0; i < candidates.size(); i++) {
                        const ProbeCandidateEntry& candidate = candidates[i];
                        if (candidate.probed && candidate.reachable && candidate.entry == *forced_entry) {
                            forced_index = static_cast<int>(i);
                            break;
                        }
                    }
                    if (forced_index < 0) {
                        LOG_DEBUG("VEthernetExchanger::ProbeSelectServerEndPoint: forced entry not reachable this round, forced=%s", forced_entry->data());
                        // Do not touch the displayed outbound RTT: a preheat attempt
                        // against a backup must not look like the primary went dark.
                        return false; // The forced entry is not reachable this round.
                    }

                    const ProbeCandidateEntry& best = candidates[forced_index];
                    hostname = best.hostname;
                    address = best.address;
                    path = best.path;
                    port = best.port;
                    protocol_type = best.protocol_type;
                    server = best.server;
                    remoteEP = best.remoteEP;
                    // Keep the displayed outbound RTT/reachability untouched: a
                    // preheat probe against a backup must not look like the
                    // primary went dark (the state machine reads probe_results_,
                    // not these display fields).
                    return true;
                }

                // Use the continuously maintained reliability/risk ordering.  This
                // list is consulted only while establishing/re-establishing a
                // transport; ranking changes never move a healthy live session.
                int best_index = -1;
                const ppp::vector<ppp::string> ranked_entries = HotSwitchRankedEntries(now);
                for (const ppp::string& ranked_entry : ranked_entries) {
                    for (std::size_t i = 0; i < candidates.size(); i++) {
                        const ProbeCandidateEntry& candidate = candidates[i];
                        if (candidate.entry == ranked_entry && candidate.probed &&
                            candidate.reachable && candidate.rtt_ms >= 0) {
                            best_index = static_cast<int>(i);
                            break;
                        }
                    }
                    if (best_index >= 0) {
                        break;
                    }
                }
                // Defensive fallback for a just-probed candidate that could not be
                // represented in the cache ordering.
                if (best_index < 0) {
                    for (std::size_t i = 0; i < candidates.size(); i++) {
                        if (candidates[i].probed && candidates[i].reachable && candidates[i].rtt_ms >= 0 &&
                            (best_index < 0 || candidates[i].rtt_ms < candidates[best_index].rtt_ms)) {
                            best_index = static_cast<int>(i);
                        }
                    }
                }
                // Sticky entry: keep the last successfully selected entry
                // whenever it is still reachable this round, regardless of
                // quality.  A reconnect must never bounce to a different entry
                // on probe jitter - on wss/ws/ppp tunnels switching entries
                // mid-session breaks the stream.  Only a genuinely unreachable
                // or blacklisted entry loses stickiness and lets the
                // best-quality candidate win.  probe_server_ persists across
                // reconnects (server_url_ is cleared by
                // ExchangeToReconnectingState), so the preference survives
                // reconnect cycles.
                if (best_index >= 0) {
                    ppp::string preferred_entry;
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        preferred_entry = probe_server_;
                    }
                    if (!preferred_entry.empty()) {
                        for (std::size_t i = 0; i < candidates.size(); i++) {
                            const ProbeCandidateEntry& candidate = candidates[i];
                            if (candidate.entry == preferred_entry && candidate.probed &&
                                candidate.reachable) {
                                best_index = static_cast<int>(i); // Sticky: keep it.
                                break;
                            }
                        }
                    }
                }

                if (best_index < 0) {
                    probe_rtt_ms_.store(-1);
                    probe_reachable_.store(false);
                    probe_checked_.store(true);
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        probe_server_.clear();
                    }
                    return false; // Fall back to the legacy primary-only path.
                }

                const ProbeCandidateEntry& best = candidates[best_index];
                hostname = best.hostname;
                address = best.address;
                path = best.path;
                port = best.port;
                protocol_type = best.protocol_type;
                server = best.server;
                remoteEP = best.remoteEP;
                probe_rtt_ms_.store(best.rtt_ms);
                probe_reachable_.store(true);
                probe_checked_.store(true);
                {
                    SynchronizedObjectScope scope(syncobj_);
                    probe_server_ = best.entry;
                }
                return true;
            }


            VEthernetExchanger::~VEthernetExchanger() noexcept {
                Finalize();
            }

            bool VEthernetExchanger::TranslateIPv6Packet(Byte* packet, int packet_length, bool outbound) noexcept {
                // NOTE: the primary_outbound_ fast-path is intentionally removed.
                // The desktop primary outbound uses ApplyClientAddress to make the
                // TAP address equal the server-assigned address, so the identity
                // checks below (source == assigned / destination == tap) return
                // true and no translation happens -- identical behavior.
                // On Android ApplyClientAddress cannot change the TUN address
                // (VpnService.Builder fixes it to the leak-block ULA), so the TAP
                // address stays fd00:6f70:656e:7070::2 while the server assigns
                // fd42:4242:4242::/64. Skipping translation here sent packets with
                // a fd00::2 source to the server; its NAT66 reply had destination
                // fd00::2, which fails the server-side fd42::/64 PrefixMatch and is
                // silently dropped. Removing the fast-path lets Android translate
                // between the TUN ULA and the assigned address like the server
                // expects, without any behavior change on desktop.
                if (NULLPTR == packet || packet_length < ppp::ipv6::IPv6_HEADER_MIN_SIZE) return true;
                if ((packet[0] >> 4) != ppp::ipv6::IPv6_VERSION) return true;
                if (NULLPTR == switcher_) return false;
                auto tap = switcher_->GetTap();

                boost::asio::ip::address assigned;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    assigned = assigned_ipv6_address_;
                }
                // The TUN address is global (populated by the primary outbound via
                // ApplyClientAddress) and is the translation anchor this outbound
                // must rewrite to/from its own server-assigned address.  The
                // assigned address is also the per-outbound runtime capability
                // signal; a client profile does not describe the remote server's
                // IPv6 support.  If the TUN has no IPv6 address there is nothing
                // to translate on behalf of any outbound, so this remains a hard
                // requirement.
                if (!assigned.is_v6() || NULLPTR == tap || !tap->IPv6Address.is_v6()) {
                    LOG_DEBUG("VEthernetExchanger::TranslateIPv6Packet: outbound=%s has no usable IPv6 assignment",
                        outbound_tag_.data());
                    return false;
                }

                boost::asio::ip::address_v6 source;
                boost::asio::ip::address_v6 destination;
                Byte next_header = 0;
                int payload_length = 0;
                if (!ppp::ipv6::TryParsePacket(packet, packet_length, source, destination,
                    &next_header, &payload_length)) return false;

                boost::asio::ip::address_v6 tap_address = tap->IPv6Address.to_v6();
                boost::asio::ip::address_v6 assigned_address = assigned.to_v6();
                if (outbound) {
                    if (source == assigned_address) return true;
                    if (source != tap_address) return false;
                    source = assigned_address;
                }
                else {
                    if (destination == tap_address) return true;
                    if (destination != assigned_address) return false;
                    destination = tap_address;
                }

                int checksum_offset = -1;
                if (next_header == IPPROTO_TCP && payload_length >= 20) checksum_offset = 16;
                elif(next_header == IPPROTO_UDP && payload_length >= 8) checksum_offset = 6;
                elif(next_header == IPPROTO_ICMPV6 && payload_length >= 4) checksum_offset = 2;
                else return false;

                ppp::ipv6::PacketHeader* header = reinterpret_cast<ppp::ipv6::PacketHeader*>(packet);
                auto source_bytes = source.to_bytes();
                auto destination_bytes = destination.to_bytes();
                memcpy(header->Source, source_bytes.data(), source_bytes.size());
                memcpy(header->Destination, destination_bytes.data(), destination_bytes.size());

                Byte* payload = packet + ppp::ipv6::IPv6_HEADER_MIN_SIZE;
                payload[checksum_offset] = 0;
                payload[checksum_offset + 1] = 0;
                unsigned short checksum = ppp::ipv6::ComputePseudoChecksum(payload,
                    static_cast<unsigned int>(payload_length), source, destination, next_header);
                if (next_header == IPPROTO_UDP && checksum == 0) checksum = 0xffff;
                memcpy(payload + checksum_offset, &checksum, sizeof(checksum));
                return true;
            }

            void VEthernetExchanger::Finalize() noexcept {
                VirtualEthernetMappingPortTable mappings;
                VEthernetDatagramPortTable datagrams;
                ITransmissionPtr transmission;
                DeadlineTimerTable deadline_timers;
                std::shared_ptr<vmux::vmux_net> mux;
                IForwardingPtr forwarding;

                for (;;) {
                    SynchronizedObjectScope scope(syncobj_);
                    disposed_ = true;

                    mappings = std::move(mappings_);
                    mappings_.clear();

                    datagrams = std::move(datagrams_);
                    datagrams_.clear();

                    deadline_timers = std::move(deadline_timers_);
                    deadline_timers_.clear();
                    
                    mux_vlan_ = 0;
                    mux = std::move(mux_);
                    forwarding = std::move(forwarding_);
                    transmission = std::move(transmission_);
                    break;
                }

                StaticEchoClean();
                if (NULLPTR != transmission) {
                    LOG_DEBUG("VEthernetExchanger::Finalize: disposing transmission, disposed=%d", (int)disposed_);
                    transmission->Dispose();
                }

                for (auto&& [_, deadline_timer] : deadline_timers) {
                    ppp::net::Socket::Cancel(*deadline_timer);
                }

                Dictionary::ReleaseAllObjects(mappings);
                Dictionary::ReleaseAllObjects(datagrams);

                if (NULLPTR != mux) {
                    mux->close_exec();
                }
                if (NULLPTR != forwarding) {
                    forwarding->Dispose();
                }
            }

            void VEthernetExchanger::Dispose() noexcept {
                if (disposed_.exchange(true)) {
                    return;
                }

                LOG_DEBUG("VEthernetExchanger::Dispose: posting Finalize, disposed=1");
                auto self = shared_from_this();
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                boost::asio::post(*context, 
                    [self, this, context]() noexcept {
                        Finalize();
                    });
            }

            VEthernetExchanger::ITransmissionPtr VEthernetExchanger::NewTransmission(
                const ContextPtr&                                                   context,
                const StrandPtr&                                                    strand,
                const std::shared_ptr<boost::asio::ip::tcp::socket>&                socket,
                ProtocolType                                                        protocol_type,
                const ppp::string&                                                  host,
                const ppp::string&                                                  path) noexcept {

                ITransmissionPtr transmission;
                if (protocol_type == ProtocolType::ProtocolType_Http ||
                    protocol_type == ProtocolType::ProtocolType_WebSocket) {
                    transmission = NewWebsocketTransmission<IWebsocketTransmission>(context, strand, socket, host, path);
                }
                elif(protocol_type == ProtocolType::ProtocolType_HttpSSL ||
                    protocol_type == ProtocolType::ProtocolType_WebSocketSSL) {
                    transmission = NewWebsocketTransmission<ISslWebsocketTransmission>(context, strand, socket, host, path);
                }
                else {
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    transmission = make_shared_object<ITcpipTransmission>(context, strand, socket, configuration);
                }

                if (NULLPTR != transmission) {
                    transmission->QoS = switcher_->GetQoS();
                    transmission->Statistics = switcher_->GetStatistics();
                }
                
                return transmission;
            }

            std::shared_ptr<boost::asio::ip::tcp::socket> VEthernetExchanger::NewAsynchronousSocket(const ContextPtr& context, const StrandPtr& strand, const boost::asio::ip::tcp& protocol, ppp::coroutines::YieldContext& y) noexcept {
                if (disposed_) {
                    return NULLPTR;
                }

                if (!context) {
                    return NULLPTR;
                }

                std::shared_ptr<boost::asio::ip::tcp::socket> socket = strand ?
                    make_shared_object<boost::asio::ip::tcp::socket>(*strand) : make_shared_object<boost::asio::ip::tcp::socket>(*context);
                if (!socket) {
                    return NULLPTR;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (!configuration) {
                    return NULLPTR;
                }

                boost::system::error_code open_ec;
                if (!ppp::coroutines::asio::async_open(y, *socket, protocol, &open_ec)) {
                    LOG_DEBUG("VEthernetExchanger::NewAsynchronousSocket: async_open failed, protocol=%d, ec=%d, category=%s, message=%s",
                        protocol.family(), open_ec.value(), open_ec.category().name(), open_ec.message().data());
                    return NULLPTR;
                }

                Socket::SetWindowSizeIfNotZero(socket->native_handle(), configuration->tcp.cwnd, configuration->tcp.rwnd);
                Socket::AdjustSocketOptional(*socket, protocol == boost::asio::ip::tcp::v4(), configuration->tcp.fast_open, configuration->tcp.turbo);
                return socket;
            }

            bool VEthernetExchanger::GetRemoteEndPoint(YieldContext* y, ppp::string& hostname, ppp::string& address, ppp::string& path, int& port, ProtocolType& protocol_type, ppp::string& server, boost::asio::ip::tcp::endpoint& remoteEP, const ppp::string* entry) noexcept {
                if (disposed_) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: exchanger disposed, entry=%s", entry ? entry->data() : "<null>");
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (!configuration) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: configuration missing, entry=%s", entry ? entry->data() : "<null>");
                    return false;
                }

                ppp::string& client_server_string = configuration->client.server;
                if (client_server_string.empty()) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: client.server empty, entry=%s", entry ? entry->data() : "<null>");
                    return false;
                }

                // The primary keeps the traditional switcher-owned proxy. Each
                // secondary owns a forwarding proxy built from its own JSON.
                std::shared_ptr<ppp::transmissions::proxys::IForwarding> forwarding;
                if (primary_outbound_.load()) {
                    forwarding = switcher_->GetForwarding();
                }
                else {
                    SynchronizedObjectScope scope(syncobj_);
                    forwarding = forwarding_;
                }
                // Probe-driven entry selection: pick the lowest-latency reachable
                // candidate from [client.server, client.servers ...] before each
                // connection attempt.  The switcher pre-resolves the endpoint
                // (server_url_) with a NULL coroutine context during startup, so
                // the probe must run even when that cached endpoint is already
                // valid.  Skipped while a forwarding proxy owns the path, when
                // no coroutine context is available, or when the master switch
                // client.probe.enabled is false (legacy single-entry behavior).
                if (NULLPTR == forwarding && y != NULLPTR && configuration->client.probe.enabled) {
                    ppp::vector<ppp::string> entries;
                    entries.reserve(1 + configuration->client.servers.size());
                    entries.emplace_back(configuration->client.server);
                    for (const ppp::string& entry : configuration->client.servers) {
                        entries.emplace_back(entry);
                    }
                    if (ProbeSelectServerEndPoint(*y, entries, hostname, address, path, port, protocol_type, server, remoteEP, entry)) {
                        if (entry == NULLPTR) {
                            server_url_.remoteEP      = remoteEP;
                            server_url_.hostname      = hostname;
                            server_url_.address       = address;
                            server_url_.path          = path;
                            server_url_.server        = server;
                            server_url_.port          = port;
                            server_url_.protocol_type = protocol_type;
                        }
                        return true;
                    }
                    if (entry != NULLPTR) {
                        LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: forced entry unreachable, entry=%s", entry->data());
                        return false; // Forced entry failed; never fall through to the sticky cache.
                    }
                }
                if (server_url_.port > IPEndPoint::MinPort && server_url_.port <= IPEndPoint::MaxPort) {
                    remoteEP      = server_url_.remoteEP;
                    hostname      = server_url_.hostname;
                    address       = server_url_.address;
                    path          = server_url_.path;
                    server        = server_url_.server;
                    port          = server_url_.port;
                    protocol_type = server_url_.protocol_type;
                    return true;
                }
                if (NULLPTR != forwarding) {
                    ppp::string abs_url;
                    server = UriAuxiliary::Parse(client_server_string, hostname, address, path, port, protocol_type, &abs_url, *y, false);
                }
                else {
                    server = UriAuxiliary::Parse(client_server_string, hostname, address, path, port, protocol_type, *y);
                }

                if (server.empty()) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: UriAuxiliary::Parse failed, entry=%s, client.server=%s", entry ? entry->data() : "<null>", client_server_string.data());
                    return false;
                }

                if (hostname.empty()) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: hostname empty after parse, entry=%s", entry ? entry->data() : "<null>");
                    return false;
                }

                if (NULLPTR != forwarding) {
                    boost::asio::ip::tcp::endpoint forwarding_to_endpoint = forwarding->GetLocalEndPoint();
                    if (int forwarding_to_port = forwarding_to_endpoint.port(); forwarding_to_port > IPEndPoint::MinPort && forwarding_to_port <= IPEndPoint::MaxPort) {
                        forwarding->SetRemoteEndPoint(hostname, port);
                        port = forwarding_to_port;
                        address = forwarding_to_endpoint.address().to_string();
                    }
                }

                if (address.empty()) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: address empty after parse, entry=%s", entry ? entry->data() : "<null>");
                    return false;
                }

                if (port <= IPEndPoint::MinPort || port > IPEndPoint::MaxPort) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: invalid port=%d, entry=%s", port, entry ? entry->data() : "<null>");
                    return false;
                }

                IPEndPoint ipep(address.data(), port);
                if (IPEndPoint::IsInvalid(ipep)) {
                    LOG_DEBUG("VEthernetExchanger::GetRemoteEndPoint: invalid IP=%s, entry=%s", address.data(), entry ? entry->data() : "<null>");
                    return false;
                }

                remoteEP                  = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(ipep);
                server_url_.remoteEP      = remoteEP;
                server_url_.hostname      = hostname;
                server_url_.address       = address;
                server_url_.path          = path;
                server_url_.server        = server;
                server_url_.port          = port;
                server_url_.protocol_type = protocol_type;
                return true;
            }

            VEthernetExchanger::ITransmissionPtr VEthernetExchanger::OpenTransmission(const ContextPtr& context, const StrandPtr& strand, YieldContext& y, const ppp::string* entry) noexcept {
                boost::asio::ip::tcp::endpoint remoteEP;
                ppp::string hostname;
                ppp::string address;
                ppp::string path;
                ppp::string server;
                int port = IPEndPoint::MinPort;
                ProtocolType protocol_type = ProtocolType::ProtocolType_PPP;

                if (!GetRemoteEndPoint(y.GetPtr(), hostname, address, path, port, protocol_type, server, remoteEP, entry)) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: GetRemoteEndPoint failed");
                    return NULLPTR;
                }

                boost::asio::ip::address remoteIP = remoteEP.address();
                if (IPEndPoint::IsInvalid(remoteIP)) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: invalid remote IP");
                    return NULLPTR;
                }

                int remotePort = remoteEP.port();
                if (remotePort <= IPEndPoint::MinPort || remotePort > IPEndPoint::MaxPort) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: invalid remote port=%d", remotePort);
                    return NULLPTR;
                }

#if defined(_WIN32)
                if (remoteIP.is_v4() && !remoteIP.is_loopback() &&
                    !switcher_->EnsureWindowsIPv4ServerRoute(remoteIP)) {
                    LOG_ERROR("VEthernetExchanger::OpenTransmission: cannot pin IPv4 server route, outbound=%s, remote=%s:%d",
                        outbound_tag_.data(), address.data(), remotePort);
                    return NULLPTR;
                }

                // The switcher intentionally suppresses the physical IPv6 default
                // route while the peer has not provided an IPv6 data plane. The
                // VPN transport itself is the exception: resolve it first, then pin
                // that exact /128 through the physical gateway before connecting.
                if (remoteIP.is_v6() && !remoteIP.is_loopback() &&
                    !switcher_->EnsureWindowsIPv6ServerRoute(remoteIP)) {
                    LOG_ERROR("VEthernetExchanger::OpenTransmission: cannot pin IPv6 server route, outbound=%s, remote=%s:%d",
                        outbound_tag_.data(), address.data(), remotePort);
                    return NULLPTR;
                }
#endif

                LOG_DEBUG("VEthernetExchanger::OpenTransmission: outbound=%s, transport_trace=%p, connecting to %s:%d, protocol=%d, hostname=%s, path=%s",
                    outbound_tag_.data(), strand.get(), address.data(), remotePort, (int)protocol_type, hostname.data(), path.data());

                std::shared_ptr<boost::asio::ip::tcp::socket> socket = NewAsynchronousSocket(context, strand, remoteEP.protocol(), y);
                if (!socket) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: NewAsynchronousSocket failed");
                    return NULLPTR;
                }

#if defined(_LINUX)
                // The VPN transport socket must bypass the VPN itself or it
                // loops back into the tunnel. On Android, VpnService claims
                // 0.0.0.0/0 AND ::/0 (IPv6 leak protection), and the ip rule
                // set hijacks any unmarked socket (fwmark 0x0/0x20000) back
                // into tun0. A server that is IPv6-only (e.g. a ppp://
                // endpoint on 2400::/12) previously skipped protect() here
                // because "VPN is IPv4", so the handshake packet entered the
                // tunnel, got re-dispatched as ordinary VPN traffic, and the
                // server was never reached - phase stayed "connected" (the
                // TUN exists) while every tunneled flow timed out. Protect
                // IPv4 and IPv6 alike.
                if (!remoteIP.is_loopback()) {
                    auto protector_network = switcher_->GetProtectorNetwork(); 
                    if (NULLPTR != protector_network) {
                        if (!protector_network->Protect(socket->native_handle(), y)) {
                            LOG_DEBUG("VEthernetExchanger::OpenTransmission: Protect failed (Linux)");
                            return NULLPTR;
                        }
                    }
                }
#elif defined(_WIN32)
                // Windows 不绑定物理网卡接口索引，依赖路由表防止环路。
                // 原版 openppp2_main 没有 IP_UNICAST_IF 也能正常工作，
                // 因为 AddRoute() 机制已经将 VPN 服务器 IP 通过物理网卡添加了特定路由。
                // 移除 IP_UNICAST_IF 避免物理网卡索引变化（WiFi 重连、睡眠唤醒等）导致断流。
#endif

                boost::system::error_code connect_ec;
                bool ok = ppp::coroutines::asio::async_connect(*socket, remoteEP, y, &connect_ec);
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: outbound=%s, transport_trace=%p, async_connect failed, remote=%s:%d, native=%lld, ec=%d, category=%s, message=%s",
                        outbound_tag_.data(), strand.get(), address.data(), remotePort, (long long)socket->native_handle(), connect_ec.value(),
                        connect_ec.category().name(), connect_ec.message().data());
                    return NULLPTR;
                }

                LOG_DEBUG("VEthernetExchanger::OpenTransmission: outbound=%s, transport_trace=%p, connected to %s:%d, creating transmission",
                    outbound_tag_.data(), strand.get(), address.data(), remotePort);
                return NewTransmission(context, strand, socket, protocol_type, hostname, path);
            }

            bool VEthernetExchanger::Open() noexcept {
                if (disposed_) {
                    return false;
                }

                AppConfigurationPtr configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }

                ContextPtr context = GetContext();
                if (!context) {
                    return false;
                }

                // A secondary configuration may use a different upstream proxy.
                // Failure is fatal for that outbound: silently connecting directly
                // would violate the selected route.
                if (!primary_outbound_.load() && !configuration->client.server_proxy.empty()) {
                    IForwardingPtr forwarding = make_shared_object<IForwarding>(context, configuration);
                    if (NULLPTR == forwarding || !forwarding->Open()) {
                        if (NULLPTR != forwarding) forwarding->Dispose();
                        LOG_ERROR("VEthernetExchanger::Open: cannot open forwarding proxy for outbound '%s'",
                            outbound_tag_.data());
                        return false;
                    }
#if defined(_LINUX)
                    forwarding->ProtectorNetwork = switcher_->GetProtectorNetwork();
#endif
                    SynchronizedObjectScope scope(syncobj_);
                    forwarding_ = std::move(forwarding);
                }

                auto self = shared_from_this();
                auto allocator = configuration->GetBufferAllocator();

                return YieldContext::Spawn(allocator.get(), *context,
                    [self, this, context](YieldContext& y) noexcept {
                        Loopback(context, y);
                    });
            }

            bool VEthernetExchanger::Update() noexcept {
                if (disposed_) {
                    return false;
                }

                auto self = shared_from_this();
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                boost::asio::post(*context, 
                    [self, this, context]() noexcept {
                        uint64_t now = ppp::threading::Executors::GetTickCount();
                        IForwardingPtr forwarding;
                        {
                            SynchronizedObjectScope scope(syncobj_);
                            forwarding = forwarding_;
                        }
                        if (NULLPTR != forwarding) {
                            forwarding->Update(now);
                        }
                        SendEchoKeepAlivePacket(now, false);
                        if (disposed_) {
                            return;
                        }
                        DoMuxEvents();
                        if (disposed_) {
                            return;
                        }
                        DoKeepAlived(GetTransmission(), now);

                        for (;;) {
                            SynchronizedObjectScope scope(syncobj_);
                            Dictionary::UpdateAllObjects(datagrams_, now);
                            Dictionary::UpdateAllObjects2(mappings_, now);
                            break;
                        }

                        // Entry scores are refreshed continuously by the probe path,
                        // but healthy sessions are deliberately never migrated.
                    });
                return true;
            }

            void VEthernetExchanger::GetDebugObjectCounts(size_t& mappings, size_t& datagrams, size_t& timers) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                mappings = mappings_.size();
                datagrams = datagrams_.size();
                timers = deadline_timers_.size();
            }

            bool VEthernetExchanger::DoKeepAlived(const ITransmissionPtr& transmission, uint64_t now) noexcept {
                if (disposed_) {
                    return false;
                }
                
                NetworkState network_state = GetNetworkState();
                if (network_state != NetworkState_Established) {
                    return true;
                }

                if (VirtualEthernetLinklayer::DoKeepAlived(transmission, now)) {
                    return true;
                }

                LOG_DEBUG("VEthernetExchanger::DoKeepAlived: keepalive timeout, disposing transmission");
                IDisposable::Dispose(transmission);
                return false;
            }

            VEthernetExchanger::ITransmissionPtr VEthernetExchanger::ConnectTransmission(const ContextPtr& context, const StrandPtr& strand, YieldContext& y, const ppp::string* entry) noexcept {
                if (NULLPTR == context) {
                    LOG_DEBUG("VEthernetExchanger::ConnectTransmission: context is null, entry=%s", entry ? entry->data() : "<null>");
                    return NULLPTR;
                }

                if (disposed_) {
                    LOG_DEBUG("VEthernetExchanger::ConnectTransmission: exchanger disposed, entry=%s", entry ? entry->data() : "<null>");
                    return NULLPTR;
                }

                // VPN client A link can be created only after a link is established between the local switch and the remote VPN server.
                ITransmissionPtr owner_link = transmission_; 
                if (NULLPTR == owner_link) {
                    LOG_DEBUG("VEthernetExchanger::ConnectTransmission: no owner link, entry=%s", entry ? entry->data() : "<null>");
                    return NULLPTR;
                }

                ITransmissionPtr transmission = OpenTransmission(context, strand, y, entry);
                if (NULLPTR == transmission) {
                    LOG_DEBUG("VEthernetExchanger::ConnectTransmission: OpenTransmission failed, entry=%s", entry ? entry->data() : "<null>");
                    return NULLPTR;
                }

                bool noerror = transmission->HandshakeServer(y, GetId(), false);
                {
                    auto logger = switcher_->GetLogger();
                    if (NULLPTR != logger) {
                        logger->Handshake(GetId(), transmission, "", "", "", noerror);
                    }
                }
                if (noerror) {
                    return transmission;
                }
                else {
                    LOG_DEBUG("VEthernetExchanger::ConnectTransmission: handshake failed, entry=%s", entry ? entry->data() : "<null>");
                    transmission->Dispose();
                    return NULLPTR;
                }
            }

            bool VEthernetExchanger::AcquireActiveTransmission(const ContextPtr& context, YieldContext& y) noexcept {
                return true;
            }

            void VEthernetExchanger::ReleaseActiveTransmission() noexcept {
            }

#if defined(_ANDROID)
            bool VEthernetExchanger::AwaitJniAttachThread(const ContextPtr& context, YieldContext& y) noexcept {
                // On the Android platform, when the VPN tunnel transport layer is enabled, 
                // Ensure that the JVM thread has been attached to the PPP. Otherwise, the link cannot be protected, 
                // Resulting in loop problems and VPN loopback crashes.
                bool attach_ok = false;
                while (!disposed_) {
                    if (std::shared_ptr<ppp::net::ProtectorNetwork> protector = switcher_->GetProtectorNetwork(); NULLPTR != protector) {
                        if (NULLPTR != protector->GetContext() && NULLPTR != protector->GetEnvironment()) {
                            attach_ok = true;
                            break;
                        }
                    }

                    bool sleep_ok = Sleep(10, context, y); // Poll.
                    if (!sleep_ok) {
                        break;
                    }
                }

                return attach_ok;
            }
#endif

            bool VEthernetExchanger::Loopback(const ContextPtr& context, YieldContext& y) noexcept {
                AppConfigurationPtr configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }
#if defined(_ANDROID)
                elif(!AwaitJniAttachThread(context, y)) {
                    return false;
                }
#endif
                bool run_once = false;
                while (!disposed_) {
                    ExchangeToConnectingState(); {
                        LOG_DEBUG("VEthernetExchanger::Loopback: connecting to server...");
                        ITransmissionPtr transmission = OpenTransmission(context, y);
                        if (transmission) {
                            LOG_DEBUG("VEthernetExchanger::Loopback: TCP connected, starting handshake...");
                            bool handshake_ok = transmission->HandshakeServer(y, GetId(), true);
                            {
                                auto logger = switcher_->GetLogger();
                                if (NULLPTR != logger) {
                                    logger->Handshake(GetId(), transmission, "", "", "", handshake_ok);
                                }
                            }
                            LOG_DEBUG("VEthernetExchanger::Loopback: handshake %s", handshake_ok ? "success" : "failed");
                            if (handshake_ok && EchoLanToRemoteExchanger(transmission, y) > -1) {
                                LOG_DEBUG("VEthernetExchanger::Loopback: link established, entering data loop");
                                ExchangeToEstablishState(); {
                                    transmission_ = transmission; {
                                        // Static client mappings belong only to
                                        // the current main role. Split
                                        // outbounds keep their transport/MUX
                                        // warm but must not bind the same local
                                        // ports as main.
                                        if (IsPrimaryOutbound()) {
                                            RegisterAllMappingPorts();
                                        }
                                        if (StaticEchoAllocatedToRemoteExchanger(y) && Run(transmission, y)) {
                                            run_once = true;
                                            StaticEchoClean();
                                        }

                                        UnregisterAllMappingPorts();
                                    }
                                    transmission_.reset();
                                }
                                LOG_DEBUG("VEthernetExchanger::Loopback: data loop exited, reconnecting...");
                            }
                            else {
                                LOG_DEBUG("VEthernetExchanger::Loopback: handshake or LAN exchange failed");
                            }

                            transmission->Dispose();
                        }
                        else {
                            LOG_DEBUG("VEthernetExchanger::Loopback: OpenTransmission failed");
                        }
                    } ExchangeToReconnectingState();

                    // Dispose can race with the data loop ending. Do not create a new
                    // reconnect wait after shutdown has already begun; Finalize will
                    // cancel any timers that were active before Dispose.
                    if (disposed_) {
                        break;
                    }

                    const int64_t reconnection_timeout = GetReconnectDelayMilliseconds();
                    if (reconnection_timeout <= 0) {
                        LOG_INFO("VEthernetExchanger::Loopback: starting immediate failover attempt #%d", (int)reconnection_count_);
                        continue;
                    }
                    LOG_DEBUG("VEthernetExchanger::Loopback: waiting %lld ms before reconnection attempt #%d", reconnection_timeout, (int)reconnection_count_);
                    if (!Sleep(reconnection_timeout, context, y)) {
                        break;
                    }
                }
                return run_once;
            }

            bool VEthernetExchanger::DoMuxEvents() noexcept {
                bool successes = false;
                while (!disposed_) {
                    uint16_t max_connections = switcher_->mux_;
                    if (max_connections == 0) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: mux disabled by switcher");
                        break;
                    }

                    if (network_state_.load() != NetworkState_Established) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: network not established, state=%d", (int)network_state_.load());
                        break;
                    }

                    AppConfigurationPtr configuration = GetConfiguration();
                    if (NULLPTR == configuration) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: configuration missing");
                        break;
                    }

                    // MUX construction failures are not entry failover events.
                    // Keep the current entry pinned and pause the next generation
                    // for a bounded interval so a single bad entry cannot create a
                    // tight destroy/recreate loop.  Main transport reconnects and
                    // consecutive M1/M4 failures are handled below and may change
                    // the entry when the failover policy allows it.
                    const uint64_t now = Executors::GetTickCount();
                    bool mux_retry_blocked = false;
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        mux_retry_blocked = NULLPTR == mux_ && mux_retry_not_before_ > now;
                    }
                    if (mux_retry_blocked) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: MUX retry cooling down");
                        break;
                    }

                    std::shared_ptr<vmux::vmux_net> mux = mux_;
                    if (NULLPTR != mux) {
                        bool breaking = true;
                        successes = true;

                        if (mux->Vlan != mux_vlan_) {
                            LOG_DEBUG("VEthernetExchanger::DoMuxEvents: VLAN mismatch, mux_vlan=%u, expected=%u, closing", mux->Vlan, mux_vlan_);
                            mux->close_exec();
                        }
                        elif(!mux->update()) {
                            uint64_t now = mux->now_tick();
                            const vmux::vmux_net::close_reason reason = mux->get_close_reason();
                            const bool integrity_rebuild = reason == vmux::vmux_net::close_reason_m1_gap ||
                                reason == vmux::vmux_net::close_reason_m4_retransmit_exhausted;

                            if (integrity_rebuild) {
                                static constexpr uint64_t MUX_FAILURE_WINDOW_MS = 60 * 1000ULL;
                                ppp::string failed_entry;
                                int streak = 0;
                                {
                                    SynchronizedObjectScope scope(syncobj_);
                                    failed_entry = mux_entry_.empty() ? probe_server_ : mux_entry_;
                                    if (failed_entry == mux_failure_entry_ && mux_failure_last_ != 0 &&
                                        now <= mux_failure_last_ + MUX_FAILURE_WINDOW_MS) {
                                        mux_failure_streak_++;
                                    }
                                    else {
                                        mux_failure_entry_ = failed_entry;
                                        mux_failure_streak_ = 1;
                                    }
                                    mux_failure_last_ = now;
                                    streak = mux_failure_streak_;
                                }

                                ppp::string target_entry;
                                if (configuration->client.hot_switch.enabled && streak >= 2) {
                                    const ppp::vector<ppp::string> ranked = HotSwitchRankedEntries(now);
                                    for (const ppp::string& entry : ranked) {
                                        if (!entry.empty() && entry != failed_entry) {
                                            target_entry = entry;
                                            break;
                                        }
                                    }
                                }

                                 if (!target_entry.empty()) {
                                     {
                                         SynchronizedObjectScope scope(syncobj_);
                                         mux_entry_ = target_entry;
                                         probe_server_ = target_entry;
                                         mux_failure_entry_.clear();
                                         mux_failure_last_ = 0;
                                         mux_failure_streak_ = 0;
                                         mux_retry_not_before_ = 0;
                                         mux_retry_backoff_ms_ = 0;
                                     }
                                     LOG_WARN("VEthernetExchanger::DoMuxEvents: consecutive M%d on entry %s (streak=%d), rebuilding immediately on ranked backup %s",
                                        reason == vmux::vmux_net::close_reason_m1_gap ? 1 : 4,
                                        failed_entry.empty() ? "<unknown>" : failed_entry.data(), streak, target_entry.data());
                                 }
                                 else {
                                     {
                                         SynchronizedObjectScope scope(syncobj_);
                                         // M1/M4 means the established data
                                         // generation is no longer safe to
                                         // keep serving.  Rebuild immediately
                                         // on the same entry; retry backoff is
                                         // reserved for ordinary MUX
                                         // construction/maintenance failures.
                                         mux_retry_backoff_ms_ = 0;
                                         mux_retry_not_before_ = 0;
                                     }
                                     LOG_WARN("VEthernetExchanger::DoMuxEvents: M%d on entry %s (streak=%d), rebuilding immediately on the same entry",
                                         reason == vmux::vmux_net::close_reason_m1_gap ? 1 : 4,
                                         failed_entry.empty() ? "<unknown>" : failed_entry.data(), streak);
                                }

                                mux_.reset();
                                breaking = false;
                            }
                            else {
                                 {
                                     SynchronizedObjectScope scope(syncobj_);
                                     mux_failure_entry_.clear();
                                     mux_failure_last_ = 0;
                                     mux_failure_streak_ = 0;
                                     mux_retry_backoff_ms_ = mux_retry_backoff_ms_ == 0
                                         ? 1000U
                                         : std::min<uint32_t>(mux_retry_backoff_ms_ * 2U, 5000U);
                                     mux_retry_not_before_ = now + mux_retry_backoff_ms_;
                                 }

                                // update() returns false when the MUX is already
                                // disposed or its executor can no longer accept
                                // maintenance work.  Keeping that object in mux_
                                // until the reconnection timeout creates a data
                                // black hole: new traffic keeps selecting a dead
                                // MUX generation.  Drop it immediately and let
                                // the loop create a fresh generation.
                                LOG_WARN("VEthernetExchanger::DoMuxEvents: mux update failed, resetting data plane immediately");
                                if (mux_ == mux) {
                                    mux_.reset();
                                }
                                breaking = false;
                            }

                            mux->close_exec();
                            // Do not create another generation in this same
                            // DoMuxEvents invocation for ordinary failures.  An
                            // M1/M4 integrity failure is different: the old
                            // generation has already been invalidated and the
                            // affected streams must get a fresh generation now.
                            if (!breaking && !integrity_rebuild) {
                                break;
                            }
                        }

                         if (breaking) {
                            LOG_DEBUG("VEthernetExchanger::DoMuxEvents: keeping existing mux, state=%d, established=%d, disposed=%d",
                                (int)GetMuxNetworkState(), (int)mux->is_established(), (int)mux->is_disposed());
                             if (mux->is_established()) {
                                 {
                                     SynchronizedObjectScope scope(syncobj_);
                                     mux_retry_not_before_ = 0;
                                     mux_retry_backoff_ms_ = 0;
                                 }
                                 int grow = mux->take_turbo_pending_grow();
                                if (grow > 0) {
                                    MuxGrowLinklayers(switcher_->GetBufferAllocator(), mux, grow);
                                }
                            }
                            break;
                        }
                    }

                    ppp::threading::Executors::StrandPtr vmux_strand;
                    ppp::threading::Executors::ContextPtr vmux_context = ppp::threading::Executors::SelectScheduler(vmux_strand);
                    if (NULLPTR == vmux_context) {
                        break;
                    }
                    else {
                        vmux::vmux_net::mux_mode mux_mode = vmux::vmux_net::parse_mode(configuration->GetEffectiveMuxMode());
                        mux = make_shared_object<vmux::vmux_net>(vmux_context, vmux_strand, max_connections, false,
                            (switcher_->mux_acceleration_ & PPP_MUX_ACCELERATION_LOCAL) != 0, mux_mode);
                        if (NULLPTR == mux) {
                            break;
                        }

                        if (mux_mode == vmux::vmux_net::mux_mode_flow && configuration->mux.turbo) {
                            uint32_t hard = static_cast<uint32_t>(max_connections) * static_cast<uint32_t>(PPP_MUX_TURBO_FACTOR_MAX);
                            mux->set_pool_hard_max(static_cast<uint16_t>(std::min<uint32_t>(hard, UINT16_MAX)));
                        }
                    }

                    ITransmissionPtr vnet_transmission = GetTransmission();
                    if (NULLPTR == vnet_transmission) {
                        break;
                    }

                    ppp::threading::Executors::ContextPtr vnet_context = GetContext();
                    if (NULLPTR == vnet_context) {
                        break;
                    }

                    std::shared_ptr<ppp::threading::BufferswapAllocator> buffer_allocator = switcher_->GetBufferAllocator();
                    mux->AppConfiguration = configuration;
                    mux->BufferAllocator  = buffer_allocator;
                    mux->Logger           = switcher_->GetLogger();
#if defined(_LINUX)
                    mux->ProtectorNetwork = switcher_->GetProtectorNetwork();
#endif

                    for (;;) {
                        uint16_t vlan = (uint16_t)vmux::vmux_net::ftt_random_aid(1, UINT16_MAX);
                        if (vlan != 0 && vlan != mux_vlan_) {
                            mux_vlan_ = vlan;
                            mux->Vlan = vlan;
                            break;
                        }
                    }

                    ppp::string mux_entry;
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        if (mux_entry_.empty()) {
                            mux_entry_ = probe_server_;
                        }
                        mux_entry = mux_entry_;
                    }
                    LOG_DEBUG("VEthernetExchanger::DoMuxEvents: creating new mux, vlan=%u, max_connections=%u, entry=%s",
                        mux->Vlan, max_connections, mux_entry.empty() ? "<auto>" : mux_entry.data());
                    std::shared_ptr<VirtualEthernetLinklayer> self = shared_from_this();
                    mux_ = mux;

                    successes = YieldContext::Spawn(buffer_allocator.get(), *vnet_context, 
                        [self, this, vnet_transmission, mux, vnet_context, configuration](YieldContext& y) noexcept {
                            bool ok = false;
                            if (!disposed_) {
                                uint16_t max_connections = mux->get_max_connections();
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: sending mux request, vlan=%u, max_connections=%u, acceleration=%d",
                                    mux->Vlan, max_connections, (int)((switcher_->mux_acceleration_ & PPP_MUX_ACCELERATION_REMOTE) != 0));
                                bool advertise_flow_v2 = vmux::vmux_net::mode_requires_flow_v2(
                                    mux->get_mode(), configuration->mux.turbo);
                                Byte ordering_caps = 0;
                                if (advertise_flow_v2) {
                                    ordering_caps = static_cast<Byte>(vmux::vmux_net::ordering_caps_flow_v2);
                                    if (configuration->mux.flow.retransmit.enabled) {
                                        ordering_caps |= static_cast<Byte>(vmux::vmux_net::ordering_caps_nack);
                                    }
                                }
                                ok = DoMux(vnet_transmission, mux->Vlan, max_connections,
                                    (switcher_->mux_acceleration_ & PPP_MUX_ACCELERATION_REMOTE) != 0,
                                    ordering_caps, y);
                            }

                            if (!ok) {
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: DoMux failed, closing mux");
                                mux->close_exec();
                            }
                            else {
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: DoMux succeeded");
                            }
                        });
                    break;
                }

                if (!successes) {
                    LOG_DEBUG("VEthernetExchanger::DoMuxEvents: no mux active, cleaning up");
                    std::shared_ptr<vmux::vmux_net> mux = std::move(mux_);
                    if (NULLPTR != mux) {
                        mux->close_exec();
                    }
                }

                return successes;
            }

            VEthernetExchanger::NetworkState VEthernetExchanger::GetMuxNetworkState() noexcept {
                if (disposed_) {
                    return NetworkState_Reconnecting;
                }

                std::shared_ptr<vmux::vmux_net> mux = mux_;
                if (NULLPTR == mux) {
                    return NetworkState_Connecting;
                }

                if (mux->is_disposed()) {
                    return NetworkState_Reconnecting;
                }

                if (mux->is_established()) {
                    return NetworkState_Established;
                }

                return NetworkState_Connecting;
            }

            bool VEthernetExchanger::MuxConnectAllLinklayers(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator, const std::shared_ptr<vmux::vmux_net>& mux) noexcept {
                using ppp::app::protocol::VirtualEthernetTcpipConnection;
                
                std::shared_ptr<boost::asio::io_context> context = mux->get_context();
                if (NULLPTR == context) {
                    return false;
                }

                auto self = shared_from_this();
                auto strand = mux->get_strand();

                return YieldContext::Spawn(allocator.get(), *context, strand.get(),
                    [self, this, mux, context, strand, allocator](YieldContext& y) noexcept -> bool {
                        if (disposed_ || mux != mux_) {
                            mux->close_exec();
                            return false;
                        }

                        int max_connections = mux->get_max_connections();

                        if (!mux->ftt(vmux::vmux_net::ftt_random_aid(1, INT32_MAX), vmux::vmux_net::ftt_random_aid(1, INT32_MAX))) {
                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: ftt failed");
                            mux->close_exec();
                            return false;
                        }

                        auto context = mux->get_context();
                        auto strand = mux->get_strand();
                        
                        LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: connecting %d linklayers, vlan=%u", max_connections, mux->Vlan);

                        // One MUX generation is pinned to exactly one entry.  Ranking
                        // changes are intentionally ignored until a permitted failure
                        // event creates the next generation, preventing cross-entry
                        // packet timing differences from creating additional reordering.
                        ppp::string mux_entry;
                        {
                            SynchronizedObjectScope scope(syncobj_);
                            if (mux_entry_.empty()) {
                                mux_entry_ = probe_server_;
                                if (mux_entry_.empty() &&
                                    server_url_.port > IPEndPoint::MinPort && server_url_.port <= IPEndPoint::MaxPort) {
                                    mux_entry_ = NormalizeProbeEntry(server_url_.hostname, server_url_.port);
                                }
                            }
                            mux_entry = mux_entry_;
                        }

                        // Parallel assembly: every slot is spawned as its own coroutine
                        // on the same mux strand, so the TCP/WS handshakes of different
                        // slots overlap instead of serializing connect timeouts back to
                        // back (4 links x up to 5 s each used to stall mux establishment
                        // for ~40 s). The strand still serializes shared mux access
                        // (add_linklayer etc.), and the last slot to finish performs the
                        // aggregation that the serial loop used to do at its tail.
                        constexpr int LINKLAYER_MAX_ATTEMPTS = 3;
                        std::shared_ptr<std::atomic<int>> done_slots = make_shared_object<std::atomic<int>>(0);
                        std::shared_ptr<std::atomic<int>> ok_slots = make_shared_object<std::atomic<int>>(0);

                        for (int i = 0; i < max_connections; i++) {
                            const ppp::string forced_entry = mux_entry;

                            YieldContext::Spawn(allocator.get(), *context, strand.get(),
                                [self, this, mux, context, strand, allocator, i, max_connections, forced_entry, done_slots, ok_slots](YieldContext& y) noexcept {
                                    bool slot_ok = false;
                                    for (int attempt = 0; attempt < LINKLAYER_MAX_ATTEMPTS; attempt++) {
                                        if (disposed_ || mux != mux_) {
                                            break;
                                        }

                                        if (mux->is_established()) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: mux already established at connection %d/%d", i, max_connections);
                                            slot_ok = true;
                                            break;
                                        }

                                        const ppp::string* attempt_entry = forced_entry.empty() ? NULLPTR : &forced_entry;
                                        ITransmissionPtr transmission = ConnectTransmission(context, strand, y, attempt_entry);
                                        if (NULLPTR == transmission) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: ConnectTransmission failed at %d/%d, attempt %d/%d", i, max_connections, attempt + 1, LINKLAYER_MAX_ATTEMPTS);
                                            continue;
                                        }

                                        std::shared_ptr<boost::asio::ip::tcp::socket> default_socket;
                                        std::shared_ptr<VirtualEthernetTcpipConnection> connection =
                                            make_shared_object<VirtualEthernetTcpipConnection>(
                                                mux->AppConfiguration, context, strand, GetId(), default_socket);
                                        if (NULLPTR == connection) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: failed to create connection at %d/%d, attempt %d/%d", i, max_connections, attempt + 1, LINKLAYER_MAX_ATTEMPTS);
                                            transmission->Dispose();
                                            continue;
                                        }

                                        // In this lightweight and simple vmux circuit switch, seq and ack are delivered by the client, and the server and client are opposite.
                                        if (!connection->ConnectMux(y, transmission, mux->Vlan, mux->get_rx_ack(), mux->get_tx_seq())) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: ConnectMux failed at %d/%d, attempt %d/%d", i, max_connections, attempt + 1, LINKLAYER_MAX_ATTEMPTS);
                                            connection->Dispose();
                                            continue;
                                        }

                                        ppp::string tagged_entry = (attempt_entry != NULLPTR) ? *attempt_entry : ppp::string();
                                        bool bok = mux->do_yield(y,
                                            [self, mux, connection, tagged_entry]() noexcept -> bool {
                                                vmux::vmux_net::vmux_linklayer_ptr linklayer;
                                                vmux::vmux_net::vmux_native_add_linklayer_after_success_before_callback handling;
                                                if (!mux->add_linklayer(connection, linklayer, handling)) {
                                                    return false;
                                                }
                                                if (!tagged_entry.empty()) {
                                                    mux->set_linklayer_entry(linklayer, tagged_entry);
                                                }
                                                return true;
                                            });

                                        if (!bok) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: add_linklayer failed at %d/%d, attempt %d/%d", i, max_connections, attempt + 1, LINKLAYER_MAX_ATTEMPTS);
                                            connection->Dispose();
                                            continue;
                                        }

                                        slot_ok = true;
                                        break;
                                    }

                                    if (disposed_ || mux != mux_) {
                                        done_slots->fetch_add(1, std::memory_order_release);
                                        return;
                                    }

                                    if (slot_ok) {
                                        ok_slots->fetch_add(1, std::memory_order_release);
                                        LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: linklayer %d/%d connected", i + 1, max_connections);
                                    }
                                    else {
                                        LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: linklayer %d/%d failed after %d attempts, closing mux", i + 1, max_connections, LINKLAYER_MAX_ATTEMPTS);
                                    }

                                    // Aggregation: the last slot to finish (all slots run
                                    // on the mux strand, so this is race-free) applies the
                                    // outcome of the serial loop tail.
                                    if (done_slots->fetch_add(1, std::memory_order_release) + 1 >= max_connections) {
                                        if (disposed_ || mux != mux_) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: only -1/%d connected, closing mux", max_connections);
                                            mux->close_exec();
                                            return;
                                        }
                                        if (mux->is_established()) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: mux established with %d/%d linklayers connected",
                                                ok_slots->load(std::memory_order_acquire), max_connections);
                                            return;
                                        }

                                        int bok = ok_slots->load(std::memory_order_acquire);
                                        if (bok >= max_connections) {
                                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: all %d linklayers connected successfully", max_connections);
                                            return;
                                        }

                                        // A carrier/build failure is a MUX failure,
                                        // not an entry failover event.  Keep the
                                        // generation pinned to the same entry and
                                        // let the bounded retry backoff prevent a
                                        // tight loop.  Entry switching is reserved
                                        // for the main transport or consecutive M1/M4.
                                        const uint64_t now = Executors::GetTickCount();
                                        {
                                            SynchronizedObjectScope scope(syncobj_);
                                            mux_retry_backoff_ms_ = mux_retry_backoff_ms_ == 0
                                                ? 1000U
                                                : std::min<uint32_t>(mux_retry_backoff_ms_ * 2U, 5000U);
                                            mux_retry_not_before_ = now + mux_retry_backoff_ms_;
                                            if (mux_ == mux) {
                                                mux_.reset();
                                            }
                                        }

                                        LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: only %d/%d connected, closing mux", bok, max_connections);
                                        mux->close_exec();
                                    }
                                });
                        }

                        return true;
                    });
            }

            bool VEthernetExchanger::MuxGrowLinklayers(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator, const std::shared_ptr<vmux::vmux_net>& mux, int count, ppp::string entry) noexcept {
                using ppp::app::protocol::VirtualEthernetTcpipConnection;

                if (NULLPTR == mux || count <= 0) {
                    return false;
                }

                std::shared_ptr<boost::asio::io_context> context = mux->get_context();
                if (NULLPTR == context) {
                    return false;
                }

                if (entry.empty()) {
                    SynchronizedObjectScope scope(syncobj_);
                    entry = mux_entry_;
                }

                auto self = shared_from_this();
                auto strand = mux->get_strand();
                return YieldContext::Spawn(allocator.get(), *context, strand.get(),
                    [self, this, mux, count, context, strand, entry](YieldContext& y) noexcept -> bool {
                        const uint32_t& tx_seq = mux->get_tx_seq();
                        const uint32_t& rx_ack = mux->get_rx_ack();

                        for (int i = 0; i < count; i++) {
                            if (disposed_ || mux != mux_ || mux->is_disposed()) {
                                break;
                            }

                            const ppp::string* entry_ptr = entry.empty() ? NULLPTR : &entry;
                            ITransmissionPtr transmission = ConnectTransmission(context, strand, y, entry_ptr);
                            if (NULLPTR == transmission) {
                                break;
                            }

                            std::shared_ptr<boost::asio::ip::tcp::socket> default_socket;
                            std::shared_ptr<VirtualEthernetTcpipConnection> connection =
                                make_shared_object<VirtualEthernetTcpipConnection>(
                                    mux->AppConfiguration, context, strand, GetId(), default_socket);
                            if (NULLPTR == connection) {
                                break;
                            }

                            if (!connection->ConnectMux(y, transmission, mux->Vlan, rx_ack, tx_seq)) {
                                break;
                            }

                            ppp::string tagged_entry = entry;
                            bool added = mux->do_yield(y,
                                [self, mux, connection, tagged_entry]() noexcept -> bool {
                                    vmux::vmux_net::vmux_linklayer_ptr linklayer;
                                    vmux::vmux_net::vmux_native_add_linklayer_after_success_before_callback handling;
                                    if (!mux->add_linklayer(connection, linklayer, handling)) {
                                        return false;
                                    }
                                    if (!tagged_entry.empty()) {
                                        mux->set_linklayer_entry(linklayer, tagged_entry);
                                    }
                                    return true;
                                });
                            if (!added) {
                                break;
                            }
                        }

                        // Runtime growth is best-effort. The established base pool
                        // remains usable when an extra carrier cannot be created.
                        return true;
                    });
            }

            ppp::vector<ppp::string> VEthernetExchanger::HotSwitchRankedEntries(uint64_t now) noexcept {
                struct EntryQuality final {
                    ppp::string entry;
                    int tier;
                    double risk;
                    int rtt_ms;
                };
                ppp::vector<EntryQuality> entries;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    entries.reserve(probe_results_.size());
                    for (const auto& kv : probe_results_) {
                        const ConnectivityProbe::Result& result = kv.second;
                        if (result.entry.empty() || !result.reachable || result.rtt_ms < 0 ||
                            result.penalty_until > now ||
                            now >= result.timestamp + static_cast<uint64_t>(result.ttl_ms)) {
                            continue;
                        }
                        entries.emplace_back(EntryQuality{
                            result.entry,
                            EntryReliabilityTier(result),
                            EntryLatencyRisk(result),
                            result.rtt_ms });
                    }
                }

                std::stable_sort(entries.begin(), entries.end(),
                    [](const EntryQuality& a, const EntryQuality& b) noexcept {
                        if (a.tier != b.tier) {
                            return a.tier < b.tier;
                        }
                        if (a.risk != b.risk) {
                            return a.risk < b.risk;
                        }
                        if (a.rtt_ms != b.rtt_ms) {
                            return a.rtt_ms < b.rtt_ms;
                        }
                        return a.entry < b.entry;
                    });

                ppp::vector<ppp::string> ranked;
                ranked.reserve(entries.size());
                for (const EntryQuality& e : entries) {
                    ranked.emplace_back(e.entry);
                }
                return ranked;
            }

            void VEthernetExchanger::HotSwitchBlacklistEntry(const ppp::string& entry, uint64_t now) noexcept {
                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR == configuration || entry.empty()) {
                    return;
                }
                SynchronizedObjectScope scope(syncobj_);
                ConnectivityProbe::Result& result = probe_results_[entry];
                result.entry = entry;
                result.reachable = false;
                result.penalty_until = now + static_cast<uint64_t>(std::max<int>(1, configuration->client.hot_switch.penalty_seconds)) * 1000ULL;
            }

            bool VEthernetExchanger::ReleaseDeadlineTimer(const boost::asio::deadline_timer* deadline_timer) noexcept {
                if (NULLPTR == deadline_timer) {
                    return false;
                }

                DeadlineTimerPtr reference;
                for (;;) {
                    SynchronizedObjectScope scope(syncobj_);
                    Dictionary::TryRemove(deadline_timers_, (void*)deadline_timer, reference);
                    break;
                }

                if (NULLPTR == reference) {
                    return false;
                }

                Socket::Cancel(*reference);
                return true;
            }

            bool VEthernetExchanger::NewDeadlineTimer(const ContextPtr& context, int64_t timeout, const ppp::function<void(bool)>& event) noexcept {
                std::shared_ptr<boost::asio::deadline_timer> t = make_shared_object<boost::asio::deadline_timer>(*context);
                if (NULLPTR == t) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                if (disposed_) {
                    return false;
                }
                else {
                    timeout = std::max<int64_t>(1, timeout);
                }

                auto self = shared_from_this();
                boost::asio::deadline_timer* deadline_timer = t.get();

                t->expires_from_now(Timer::DurationTime(timeout));
                t->async_wait(
                    [self, this, deadline_timer, event](const boost::system::error_code& ec) noexcept {
                        ReleaseDeadlineTimer(deadline_timer);
                        event(ec == boost::system::errc::success);
                    });

                auto r = deadline_timers_.emplace(deadline_timer, std::move(t));
                if (r.second) {
                    return true;
                }

                Socket::Cancel(*t);
                return false;
            }

            void VEthernetExchanger::ExchangeToEstablishState() noexcept {
                uint64_t now = Executors::GetTickCount();
                sekap_last_ = now;
                sekap_next_ = now + RandomNext(SEND_ECHO_KEEP_ALIVE_PACKET_MIN_TIMEOUT, SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT);
                network_state_.exchange(NetworkState_Established);
                reconnection_count_ = 0;
            }

            int64_t VEthernetExchanger::GetReconnectDelayMilliseconds() noexcept {
                switch (reconnection_count_) {
                case 0:
                case 1:
                    return 0;
                case 2:
                    return 250;
                case 3:
                    return 1000;
                case 4:
                    return 2000;
                default: {
                    AppConfigurationPtr configuration = GetConfiguration();
                    const int timeout_seconds = NULLPTR != configuration
                        ? std::max<int>(1, configuration->client.reconnections.timeout)
                        : PPP_TCP_CONNECT_TIMEOUT;
                    return static_cast<int64_t>(std::min<int>(5, timeout_seconds)) * 1000;
                }
                }
            }

            void VEthernetExchanger::ExchangeToConnectingState() noexcept {
                sekap_last_ = 0;
                sekap_next_ = 0;
                network_state_.exchange(NetworkState_Connecting);
            }

            void VEthernetExchanger::ExchangeToReconnectingState() noexcept {
                sekap_last_ = 0;
                sekap_next_ = 0;
                network_state_.exchange(NetworkState_Reconnecting);
                reconnection_count_++;

                {
                    SynchronizedObjectScope scope(syncobj_);
                    mux_entry_.clear();
                    mux_failure_entry_.clear();
                    mux_failure_last_ = 0;
                    mux_failure_streak_ = 0;
                    mux_retry_not_before_ = 0;
                    mux_retry_backoff_ms_ = 0;
                }

                // Probe-driven failover: blacklist the entry that just failed
                // and drop the cached endpoint so the next connection attempt
                // re-selects the best reachable candidate.
                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR != configuration) {
                    if (configuration->client.probe.enabled &&
                        server_url_.port > IPEndPoint::MinPort && server_url_.port <= IPEndPoint::MaxPort) {
                        // Both backups and the primary are keyed on hostname:port
                        // (the same key the probe cache and stickiness use), so a
                        // DNS re-resolution never breaks the blacklist.
                        ppp::string entry = NormalizeProbeEntry(server_url_.hostname, server_url_.port);
                        if (!entry.empty()) {
                            SynchronizedObjectScope scope(syncobj_);
                            ConnectivityProbe::Result& result = probe_results_[entry];
                            result.entry = entry;
                            result.reachable = false;
                            const uint64_t penalty_ms = static_cast<uint64_t>(
                                std::max<int>(1, configuration->client.probe.ttl_seconds)) * 1000ULL;
                            result.ttl_ms = static_cast<int>(penalty_ms);
                            result.timestamp = Executors::GetTickCount();
                            result.penalty_until = result.timestamp + penalty_ms;
                            result.reachability_samples.emplace_back(0);
                            if (result.reachability_samples.size() > ENTRY_QUALITY_HISTORY_CAP) {
                                result.reachability_samples.erase(result.reachability_samples.begin());
                            }
                            LOG_INFO("VEthernetExchanger::ExchangeToReconnectingState: entry %s failed, penalized for %llu ms; next attempt uses ranked backup",
                                entry.data(), (unsigned long long)penalty_ms);
                        }
                    }
                    server_url_.port = 0;
                }
            }


            bool VEthernetExchanger::RegisterAllMappingPorts() noexcept {
                if (disposed_ || !IsPrimaryOutbound()) {
                    return false;
                }

                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                for (AppConfiguration::MappingConfiguration& mapping : configuration->client.mappings) {
                    RegisterMappingPort(mapping);
                }

                return true;
            }

            void VEthernetExchanger::UnregisterAllMappingPorts() noexcept {
                VirtualEthernetMappingPortTable mappings; {
                    SynchronizedObjectScope scope(syncobj_);
                    mappings = std::move(mappings_);
                    mappings_.clear();
                }

                ppp::collections::Dictionary::ReleaseAllObjects(mappings);
            }

            void VEthernetExchanger::ResetMuxDataPlane() noexcept {
                std::shared_ptr<vmux::vmux_net> mux;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    mux = std::move(mux_);
                    mux_entry_.clear();
                    mux_failure_entry_.clear();
                    mux_failure_last_ = 0;
                    mux_failure_streak_ = 0;
                    mux_retry_not_before_ = 0;
                    mux_retry_backoff_ms_ = 0;
                }

                if (NULLPTR != mux) {
                    mux->close_exec();
                }

                LOG_INFO("VEthernetExchanger::ResetMuxDataPlane: outbound=%s, reset=1",
                    outbound_tag_.data());
            }

            void VEthernetExchanger::ResetDataChannels() noexcept {
                VirtualEthernetMappingPortTable mappings;
                VEthernetDatagramPortTable datagrams;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    mappings = std::move(mappings_);
                    mappings_.clear();
                    datagrams = std::move(datagrams_);
                    datagrams_.clear();
                }
                Dictionary::ReleaseAllObjects(mappings);
                Dictionary::ReleaseAllObjects(datagrams);
                LOG_INFO("VEthernetExchanger::ResetDataChannels: outbound=%s, tcp=%llu, udp=%llu",
                    outbound_tag_.data(),
                    (unsigned long long)mappings.size(),
                    (unsigned long long)datagrams.size());
            }

            bool VEthernetExchanger::OnLan(const ITransmissionPtr& transmission, uint32_t ip, uint32_t mask, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnNat(const ITransmissionPtr& transmission, Byte* packet, int packet_length, YieldContext& y) noexcept {
                bool vnet = switcher_->IsVNet();
                bool is_ipv6 = NULLPTR != packet && packet_length >= ppp::ipv6::IPv6_HEADER_MIN_SIZE &&
                    (packet[0] >> 4) == ppp::ipv6::IPv6_VERSION;
                LOG_DEBUG("DATAPLANE VEthernetExchanger::OnNat: entry, vnet=%d, is_ipv6=%d, len=%d, first=0x%02x",
                    (int)vnet, (int)is_ipv6, packet_length, NULLPTR != packet ? (int)packet[0] : -1);
                if (vnet || is_ipv6 || (NULLPTR != packet && packet_length >= (int)sizeof(ppp::net::native::ip_hdr) && (packet[0] >> 4) == ppp::net::native::ip_hdr::IP_VER)) {
                    if (!TranslateIPv6Packet(packet, packet_length, false)) return false;
                    return switcher_->Output(packet, packet_length);
                }
                else {
                    return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
                }
            }

            bool VEthernetExchanger::OnMux(const ITransmissionPtr& transmission, uint16_t vlan, uint16_t max_connections, bool acceleration, Byte ordering_caps, YieldContext& y) noexcept {
                LOG_DEBUG("VEthernetExchanger::OnMux: vlan=%u, max_connections=%u, acceleration=%d, ordering_caps=%u",
                    vlan, max_connections, (int)acceleration, (unsigned int)ordering_caps);
                std::shared_ptr<vmux::vmux_net> mux = mux_;
                if (NULLPTR != mux) {
                    bool successed = false;
                    if (vlan != 0 && max_connections > 0 && mux->Vlan == vlan && max_connections == mux->get_max_connections() && !mux->is_disposed()) {
                        bool established = mux->is_established();
                        successed = true;

                        if (!established) {
                            auto configuration = GetConfiguration();
                            auto allocator = configuration->GetBufferAllocator();

                            bool local_supports_flow_v2 = vmux::vmux_net::mode_requires_flow_v2(
                                mux->get_mode(), configuration->mux.turbo);
                            bool agreed_flow_v2 = local_supports_flow_v2 &&
                                ((ordering_caps & vmux::vmux_net::ordering_caps_flow_v2) != 0);
                            mux->set_ordering_mode(agreed_flow_v2
                                ? vmux::vmux_net::ordering_flow_v2
                                : vmux::vmux_net::ordering_compat);
                            // M4: per-flow retransmit requires flow-v2 on both ends plus
                            // an explicit NACK capability on the peer and local config.
                            bool agreed_nack = agreed_flow_v2 &&
                                configuration->mux.flow.retransmit.enabled &&
                                ((ordering_caps & vmux::vmux_net::ordering_caps_nack) != 0);
                            mux->set_retransmit_enabled(agreed_nack);

                            successed = MuxConnectAllLinklayers(allocator, mux);
                        }
                    }
                    
                    if (!successed) {
                        LOG_DEBUG("VEthernetExchanger::OnMux: mux rejected, local_vlan=%u, local_max=%u, disposed=%d",
                            mux->Vlan, mux->get_max_connections(), (int)mux->is_disposed());
                        mux->close_exec();
                    }
                }
                else {
                    LOG_DEBUG("VEthernetExchanger::OnMux: no local mux exists");
                }

                return true;
            }

            bool VEthernetExchanger::OnInformation(const ITransmissionPtr& transmission, const VirtualEthernetInformation& information, YieldContext& y) noexcept {
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                auto ei = make_shared_object<VirtualEthernetInformation>(information);
                if (NULLPTR == ei) {
                    return false;
                }
                
                auto self = shared_from_this();
                boost::asio::post(*context, 
                    [self, this, context, ei]() noexcept {
                        information_ = ei;
                        if (!disposed_) {
                            if (!primary_outbound_.load()) {
                                SynchronizedObjectScope scope(syncobj_);
                                assigned_ipv6_address_ = boost::asio::ip::address();
                            }
                            if (primary_outbound_.load()) {
                                switcher_->OnInformation(ei);
                            }
                            elif(!ei->Valid()) {
                                if (ITransmissionPtr transmission = transmission_; NULLPTR != transmission) {
                                    transmission->Dispose();
                                }
                            }
                        }
                    });
                return true;
            }

            bool VEthernetExchanger::OnInformation(const ITransmissionPtr& transmission, const InformationEnvelope& information, YieldContext& y) noexcept {
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                // Process base information (quota/expiry) same as before
                auto ei = make_shared_object<VirtualEthernetInformation>(information.Base);
                if (NULLPTR == ei) {
                    return false;
                }

                auto self = shared_from_this();
                boost::asio::post(*context,
                    [self, this, context, ei, information]() noexcept {
                        information_ = ei;
                        // Cache the latest extensions (IPv6 lease, NAT mode, ...)
                        // so a hot outbound switch can replay the assignment after
                        // CompletePendingOutboundSwitch promotes this exchanger to
                        // primary, even if the server never re-sends extensions.
                        information_extensions_ = information.Extensions;
                        if (!disposed_) {
                            {
                                // A reconnect may intentionally return no IPv6 lease,
                                // so an old per-outbound address must not survive it.
                                SynchronizedObjectScope scope(syncobj_);
                                assigned_ipv6_address_ = information.Extensions.AssignedIPv6Address.is_v6()
                                    ? information.Extensions.AssignedIPv6Address
                                    : boost::asio::ip::address();
                            }
                            if (primary_outbound_.load()) {
                                switcher_->OnInformation(ei);
                            }
                            elif(!ei->Valid()) {
                                if (ITransmissionPtr transmission = transmission_; NULLPTR != transmission) {
                                    transmission->Dispose();
                                }
                            }

                            // Apply IPv6 (and optionally IPv4) assignment from ExtendedJson
                            LOG_DEBUG("VEthernetExchanger::OnInformation: outbound=%s, primary=%d, ExtendedJson.empty()=%d, len=%d, AssignedIPv6Mode=%d",
                                outbound_tag_.data(), (int)primary_outbound_.load(),
                                (int)information.ExtendedJson.empty(),
                                (int)information.ExtendedJson.size(),
                                (int)information.Extensions.AssignedIPv6Mode);
                            if (primary_outbound_.load()) {
                                if (!information.ExtendedJson.empty()) {
                                    // Pass `self` as the source: during a hot outbound
                                    // switch this exchanger is already primary and
                                    // received server extensions BEFORE
                                    // CompletePendingOutboundSwitch updated the
                                    // switcher's exchanger_, so the IPv6 data plane
                                    // decision must follow THIS outbound's config.
                                    std::shared_ptr<VEthernetExchanger> source =
                                        std::dynamic_pointer_cast<VEthernetExchanger>(self);
                                    switcher_->ApplyIPv6Assignment(information.Extensions, source);
                                }
                            }
                        }
                    });
                return true;
            }

            bool VEthernetExchanger::OnPush(const ITransmissionPtr& transmission, int connection_id, Byte* packet, int packet_length, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnConnect(const ITransmissionPtr& transmission, int connection_id, const boost::asio::ip::tcp::endpoint& destinationEP, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnConnectOK(const ITransmissionPtr& transmission, int connection_id, Byte error_code, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnDisconnect(const ITransmissionPtr& transmission, int connection_id, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnStatic(const ITransmissionPtr& transmission, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnStatic(const ITransmissionPtr& transmission, Int128 fsid, int session_id, int remote_port, YieldContext& y) noexcept {                
                if (remote_port < IPEndPoint::MinPort || remote_port > IPEndPoint::MaxPort) {
                    return false;
                }

                if (session_id < 0) {
                    return false;
                }

                // If the server does not support static tunneling, clean up the pre-prepared resources.
                if (remote_port == IPEndPoint::MinPort || session_id == 0) {
                    StaticEchoClean();
                }
                else {
                    static_echo_session_id_ = session_id;
                    static_echo_remote_port_ = remote_port;

                    AppConfigurationPtr configuration = GetConfiguration();
                    VirtualEthernetPacket::Ciphertext(configuration, GetId(), fsid, session_id, static_echo_protocol_, static_echo_transport_);
                }

                StaticEchoGatewayServer(STATIC_ECHO_KEEP_ALIVED_ID);
                return true;
            }

            bool VEthernetExchanger::OnEcho(const ITransmissionPtr& transmission, int ack_id, YieldContext& y) noexcept {
                if (ack_id != 0) {
                    switcher_->ERORTE(ack_id);
                }
                
                return true;
            }

            bool VEthernetExchanger::OnEcho(const ITransmissionPtr& transmission, Byte* packet, int packet_length, YieldContext& y) noexcept {
                switcher_->Output(packet, packet_length);
                return true;
            }

            bool VEthernetExchanger::OnSendTo(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length, YieldContext& y) noexcept {
                ReceiveFromDestination(sourceEP, destinationEP, packet, packet_length);
                return true;
            }

            bool VEthernetExchanger::ReceiveFromDestination(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length) noexcept {
                if (disposed_) {
                    return false;
                }

                if (TryHandleDatagram(sourceEP, destinationEP, packet, packet_length)) {
                    return true;
                }

                VEthernetDatagramPortPtr datagram = GetDatagramPort(sourceEP);
                if (NULLPTR != datagram) {
                    if (NULLPTR != packet && packet_length > 0) {
                        datagram->OnMessage(packet, packet_length, destinationEP);
                    }
                    else {
                        datagram->MarkFinalize();
                        datagram->Dispose();
                    }
                }
                elif(NULLPTR != packet && packet_length > 0) {
                    switcher_->DatagramOutput(sourceEP, destinationEP, packet, packet_length);
                }

                return true;
            }

            bool VEthernetExchanger::SendTo(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, const void* packet, int packet_size) noexcept {
                return SendTo(sourceEP, ppp::string(), destinationEP, packet, packet_size);
            }

            bool VEthernetExchanger::SendTo(const boost::asio::ip::udp::endpoint& sourceEP, const ppp::string& destinationHost, const boost::asio::ip::udp::endpoint& destinationEP, const void* packet, int packet_size) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                VEthernetDatagramPortPtr datagram = AddNewDatagramPort(transmission, sourceEP);
                if (NULLPTR == datagram) {
                    return false;
                }

                return datagram->SendTo(packet, packet_size, destinationHost, destinationEP);
            }

            bool VEthernetExchanger::RegisterDatagramHandler(const boost::asio::ip::udp::endpoint& sourceEP, const DatagramPacketHandler& handler) noexcept {
                if (sourceEP.port() <= ppp::net::IPEndPoint::MinPort) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                auto r = datagram_handlers_.emplace(sourceEP, handler);
                return r.second;
            }

            bool VEthernetExchanger::ReleaseDatagramHandler(const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                if (sourceEP.port() <= ppp::net::IPEndPoint::MinPort) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                auto r = datagram_handlers_.erase(sourceEP);
                return r > 0;
            }

            bool VEthernetExchanger::TryHandleDatagram(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length) noexcept {
                if (sourceEP.port() <= ppp::net::IPEndPoint::MinPort) {
                    return false;
                }

                DatagramPacketHandler handler;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    auto tail = datagram_handlers_.find(sourceEP);
                    if (tail == datagram_handlers_.end()) {
                        return false;
                    }
                    handler = tail->second;
                }
                if (NULLPTR == handler) return false;
                // A handler may enter the switcher and later release/register an
                // exchanger handler.  Invoking it while syncobj_ is held creates a
                // lock inversion with the switcher's DNS request lock and can stall
                // every UDP response during a query burst.
                return handler(sourceEP, destinationEP, packet, packet_length);
            }

            bool VEthernetExchanger::Echo(int ack_id) noexcept {
                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                bool ok = DoEcho(transmission, ack_id, nullof<YieldContext>());
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::Echo(ack_id=%d): DoEcho failed, disposing transmission", ack_id);
                    transmission->Dispose();
                }

                return ok;
            }

            bool VEthernetExchanger::Echo(const void* packet, int packet_size) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                bool ok = DoEcho(transmission, (Byte*)packet, packet_size, nullof<YieldContext>());
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::Echo(packet): DoEcho failed, disposing transmission, packet_size=%d", packet_size);
                    transmission->Dispose();
                }

                return ok;
            }

            bool VEthernetExchanger::Nat(const void* packet, int packet_size) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                if (!TranslateIPv6Packet((Byte*)packet, packet_size, true)) {
                    return false;
                }

                bool ok = DoNat(transmission, (Byte*)packet, packet_size, nullof<YieldContext>());
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::Nat: DoNat failed, disposing transmission, packet_size=%d", packet_size);
                    transmission->Dispose();
                }

                return ok;
            }

            int VEthernetExchanger::EchoLanToRemoteExchanger(const ITransmissionPtr& transmission, YieldContext& y) noexcept {
                if (disposed_) {
                    return -1;
                }

                bool vnet = switcher_->IsVNet();
                if (!vnet) {
                    return 0;
                }

                if (NULLPTR == transmission) {
                    return -1;
                }

                std::shared_ptr<ppp::tap::ITap> tap = switcher_->GetTap();
                if (NULLPTR == tap) {
                    return -1;
                }

                bool ok = DoLan(transmission, tap->IPAddress, tap->SubmaskAddress, y);
                if (ok) {
                    return 1;
                }

                LOG_DEBUG("VEthernetExchanger::EchoLanToRemoteExchanger: DoLan failed, disposing transmission");
                transmission->Dispose();
                return -1;
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::AddNewDatagramPort(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                VEthernetDatagramPortPtr datagram = GetDatagramPort(sourceEP);
                if (NULLPTR != datagram) {
                    return datagram;
                }

                if (disposed_) {
                    return NULLPTR;
                }

                bool ok = true; 
                datagram = NewDatagramPort(transmission, sourceEP);

                if (NULLPTR == datagram) {
                    return NULLPTR;
                }
                else {
                    SynchronizedObjectScope scope(syncobj_);
                    auto r = datagrams_.emplace(sourceEP, datagram);
                    ok = r.second;
                }

                if (!ok) {
                    datagram->Dispose();
                    return NULLPTR;
                }

                return datagram;
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::NewDatagramPort(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                auto my = shared_from_this();
                std::shared_ptr<VEthernetExchanger> exchanger 
                    = std::dynamic_pointer_cast<VEthernetExchanger>(my);

                return make_shared_object<VEthernetDatagramPort>(exchanger, transmission, sourceEP);
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::GetDatagramPort(const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return Dictionary::FindObjectByKey(datagrams_, sourceEP);
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::ReleaseDatagramPort(const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return Dictionary::ReleaseObjectByKey(datagrams_, sourceEP);
            }

            bool VEthernetExchanger::SendEchoKeepAlivePacket(UInt64 now, bool immediately) noexcept {
                if (network_state_ != NetworkState_Established) {
                    return false;
                }

                UInt64 next = sekap_last_ + SEND_ECHO_KEEP_ALIVE_PACKET_MMX_TIMEOUT;
                if (now >= next) {
                    LOG_DEBUG("VEthernetExchanger::SendEchoKeepAlivePacket: echo timeout (%llu ms since last), disposing transmission",
                        (unsigned long long)(now - sekap_last_));
                    ITransmissionPtr transmission = transmission_;
                    if (transmission) {
                        transmission->Dispose();
                        return false;
                    }
                }

                if (!immediately) {
                    if (now < sekap_next_) {
                        return false;
                    }
                }

                sekap_next_ = now + RandomNext(SEND_ECHO_KEEP_ALIVE_PACKET_MIN_TIMEOUT, SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT);
                return Echo(0);
            }

            bool VEthernetExchanger::PacketInput(const ITransmissionPtr& transmission, Byte* p, int packet_length, YieldContext& y) noexcept {
                bool successed = VirtualEthernetLinklayer::PacketInput(transmission, p, packet_length, y);
                if (successed) {
                    if (network_state_ == NetworkState_Established) {
                        sekap_last_ = Executors::GetTickCount();
                    }
                }

                return successed;
            }

            bool VEthernetExchanger::RegisterMappingPort(ppp::configurations::AppConfiguration::MappingConfiguration& mapping) noexcept {
                if (disposed_) {
                    return false;
                }

                boost::system::error_code ec;
                boost::asio::ip::address local_ip = StringToAddress(mapping.local_ip.data(), ec);
                if (ec) {
                    return false;
                }

                boost::asio::ip::address remote_ip = StringToAddress(mapping.remote_ip.data(), ec);
                if (ec) {
                    return false;
                }

                bool in = remote_ip.is_v4();
                bool protocol_tcp_or_udp = mapping.protocol_tcp_or_udp;

                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, protocol_tcp_or_udp, mapping.remote_port);
                if (NULLPTR != mapping_port) {
                    return false;
                }

                mapping_port = NewMappingPort(in, protocol_tcp_or_udp, mapping.remote_port);
                if (NULLPTR == mapping_port) {
                    return false;
                }

                bool ok = mapping_port->OpenFrpClient(local_ip, mapping.local_port);
                if (ok) {
                    SynchronizedObjectScope scope(syncobj_);
                    ok = VirtualEthernetMappingPort::AddMappingPort(mappings_, in, protocol_tcp_or_udp, mapping.remote_port, mapping_port);
                }

                if (!ok) {
                    mapping_port->Dispose();
                }

                return ok;
            }

            VEthernetExchanger::VirtualEthernetMappingPortPtr VEthernetExchanger::NewMappingPort(bool in, bool tcp, int remote_port) noexcept {
                class VIRTUAL_ETHERNET_MAPPING_PORT final : public VirtualEthernetMappingPort {
                public:
                    VIRTUAL_ETHERNET_MAPPING_PORT(const std::shared_ptr<VirtualEthernetLinklayer>& linklayer, const ITransmissionPtr& transmission, bool tcp, bool in, int remote_port) noexcept
                        : VirtualEthernetMappingPort(linklayer, transmission, tcp, in, remote_port) {

                    }

                public:
                    virtual void Dispose() noexcept override {
                        if (std::shared_ptr<VirtualEthernetLinklayer> linklayer = GetLinklayer();  NULLPTR != linklayer) {
                            VEthernetExchanger* exchanger = dynamic_cast<VEthernetExchanger*>(linklayer.get());
                            if (NULLPTR != exchanger) {
                                SynchronizedObjectScope scope(exchanger->syncobj_);
                                VirtualEthernetMappingPort::DeleteMappingPort(
                                    exchanger->mappings_, ProtocolIsNetworkV4(), ProtocolIsTcpNetwork(), GetRemotePort());
                            }
                        }

                        VirtualEthernetMappingPort::Dispose();
                    }
                };

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                auto self = shared_from_this();
                return make_shared_object<VIRTUAL_ETHERNET_MAPPING_PORT>(self, transmission, tcp, in, remote_port);
            }

            VEthernetExchanger::VirtualEthernetMappingPortPtr VEthernetExchanger::GetMappingPort(bool in, bool tcp, int remote_port) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return VirtualEthernetMappingPort::FindMappingPort(mappings_, in, tcp, remote_port);
            }

            bool VEthernetExchanger::OnFrpSendTo(const ITransmissionPtr& transmission, bool in, int remote_port, const boost::asio::ip::udp::endpoint& sourceEP, Byte* packet, int packet_length, YieldContext& y) noexcept {
#if defined(_ANDROID)
                AppConfigurationPtr configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }

                std::shared_ptr<Byte> packet_managed = ppp::net::asio::IAsynchronousWriteIoQueue::Copy(configuration->GetBufferAllocator(), packet, packet_length);
                Post(
                    [this, packet_managed, sourceEP, packet_length, in, remote_port]() noexcept {
                        VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, false, remote_port);
                        if (NULLPTR != mapping_port) {
                            mapping_port->Client_OnFrpSendTo(packet_managed.get(), packet_length, sourceEP);
                        }
                    });
#else
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, false, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpSendTo(packet, packet_length, sourceEP);
                }
#endif
                return true;
            }

            bool VEthernetExchanger::OnFrpConnect(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port, YieldContext& y) noexcept {
#if defined(_ANDROID)
                Post(
                    [this, in, remote_port, connection_id]() noexcept {
                        VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                        if (NULLPTR != mapping_port) {
                            mapping_port->Client_OnFrpConnect(connection_id);
                        }
                    });
#else
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpConnect(connection_id);
                }
#endif
                return true;
            }

            bool VEthernetExchanger::OnFrpDisconnect(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port) noexcept {
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpDisconnect(connection_id);
                }

                return true;
            }

            bool VEthernetExchanger::OnFrpPush(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port, const void* packet, int packet_length) noexcept {
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpPush(connection_id, packet, packet_length);
                }

                return true;
            }

            void VEthernetExchanger::StaticEchoClean() noexcept {
                for (int i = 0; i < arraysizeof(static_echo_sockets_); i++) {
                    std::shared_ptr<StaticEchoDatagarmSocket>& r = static_echo_sockets_[i];
                    std::shared_ptr<StaticEchoDatagarmSocket> socket = std::move(r);
  
                    Socket::Closesocket(socket);
                }
                
                static_echo_input_       = false;
                static_echo_timeout_     = UINT64_MAX;
                static_echo_session_id_  = 0;
                static_echo_remote_port_ = IPEndPoint::MinPort;

                static_echo_protocol_    = NULLPTR;
                static_echo_transport_   = NULLPTR;
            }

            bool VEthernetExchanger::StaticEchoAllocated() noexcept {
                if (disposed_) {
                    return false;
                }

                std::shared_ptr<StaticEchoDatagarmSocket> socket = static_echo_sockets_[0];
                if (NULLPTR == socket) {
                    return false;
                }

                return socket->is_open() && static_echo_timeout_ != 0 && static_echo_session_id_ != 0 && static_echo_remote_port_ != 0;
            }

            bool VEthernetExchanger::StaticEchoSwapAsynchronousSocket() noexcept {
                if (disposed_) {
                    return false;
                }

                if (static_echo_timeout_ != UINT64_MAX && switcher_->StaticMode(NULLPTR)) {
                    UInt64 now = ppp::threading::Executors::GetTickCount();
                    if (now >= static_echo_timeout_) {
                        if (static_echo_input_) {
                            static_echo_input_ = false;
                            return StaticEchoNextTimeout();
                        }

                        std::shared_ptr<StaticEchoDatagarmSocket> socket = std::move(static_echo_sockets_[0]);
                        static_echo_sockets_[0] = std::move(static_echo_sockets_[1]);
                        static_echo_sockets_[1] = NULLPTR;
                        
                        static_echo_input_ = false;
                        if (!StaticEchoNextTimeout()) {
                            return false;
                        }

                        auto self = shared_from_this();
                        auto notifiy_if_need = 
                            [self, this]() noexcept {
                                // Notifies the VPN server of domestic port changes for smoother dynamic switchover of virtual links.
                                if (!static_echo_input_ && static_echo_sockets_[0]) {
                                    StaticEchoGatewayServer(STATIC_ECHO_KEEP_ALIVED_ID);
                                }
                            };
                        
                        // Here do not close the socket immediately, delay one second, because the data sent by the VPN server may not reach the network card, 
                        // Reduce the packet loss rate during switching and improve the smoothness of the cross.
                        bool closesocket = true;
                        std::shared_ptr<boost::asio::io_context> context = GetContext();
                        if (NULLPTR != context) {
                            int milliseconds = RandomNext(500, 1000);
                            std::shared_ptr<Timer> timeout = Timer::Timeout(context, milliseconds, 
                                [socket, notifiy_if_need](Timer*) noexcept {
                                    notifiy_if_need();
                                    Socket::Closesocket(socket);
                                });
                            if (NULLPTR != timeout) {
                                closesocket = false;
                            }
                        }

                        // Handles whether you can delay closing the socket. If not, close the socket immediately.
                        if (closesocket) {
                            Socket::Closesocket(socket);
                        }

                        notifiy_if_need();
                        if (NULLPTR == context) {
                            return false;
                        }

                        // Re-instance and try to open the Datagram Port.
                        socket = make_shared_object<StaticEchoDatagarmSocket>(*context);
                        if (NULLPTR == socket) {
                            return false;
                        }

                        auto configuration = GetConfiguration();
                        auto allocator = configuration->GetBufferAllocator();
                        static_echo_sockets_[1] = socket;

                        return YieldContext::Spawn(allocator.get(), *context,
                            [self, this, socket, context](YieldContext& y) noexcept {
                                bool opened = StaticEchoOpenAsynchronousSocket(*socket, y);
                                if (opened) {
                                    StaticEchoLoopbackSocket(socket);
                                }
                            });
                    }
                }

                return true;
            }

            bool VEthernetExchanger::StaticEchoGatewayServer(int ack_id) noexcept {
                if (disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::net::packet::IPFrame> packet = make_shared_object<ppp::net::packet::IPFrame>(); 
                if (NULLPTR == packet) {
                    return false;
                }

                packet->AddressesFamily = AddressFamily::InterNetwork;
                packet->Destination     = htonl(ack_id);
                packet->Id              = ppp::net::packet::IPFrame::NewId();
                packet->Source          = IPEndPoint::LoopbackAddress;
                packet->ProtocolType    = ppp::net::native::ip_hdr::IP_PROTO_ICMP;
                ppp::app::protocol::VirtualEthernetPacket::FillBytesToPayload(packet.get());
            
                return StaticEchoPacketToRemoteExchanger(packet.get());
            }

            bool VEthernetExchanger::StaticEchoAllocatedToRemoteExchanger(YieldContext& y) noexcept {
                StaticEchoClean();
                if (disposed_) {
                    return false;
                }

                if (StaticEchoAllocated()) {
                    return true;
                }

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                bool static_mode = switcher_->StaticMode(NULLPTR);
                if (!static_mode) {
                    return true;
                }

                for (int i = 0; i < arraysizeof(static_echo_sockets_); i++) {
                    std::shared_ptr<StaticEchoDatagarmSocket>& socket = static_echo_sockets_[i];
                    if (NULLPTR == socket) {
                        socket = make_shared_object<StaticEchoDatagarmSocket>(*context);
                        if (NULLPTR == socket) {
                            return false;
                        }
                    }

                    if (socket->is_open(true)) {
                        continue;
                    }

                    bool opened = StaticEchoOpenAsynchronousSocket(*socket, y) && StaticEchoLoopbackSocket(socket);
                    if (!opened) {
                        socket.reset();
                        return false;
                    }
                }

                ITransmissionPtr transmission = GetTransmission();
                if (NULLPTR == transmission) {
                    return false;
                }

                return DoStatic(transmission, y);
            }

            bool VEthernetExchanger::StaticEchoNextTimeout() noexcept {
                if (disposed_) {
                    return false;
                }

                std::shared_ptr<StaticEchoDatagarmSocket> socket = static_echo_sockets_[0];
                if (NULLPTR == socket) {
                    return false;
                }

                bool opened = socket->is_open(true);
                if (!opened) {
                    return false;
                }

                AppConfigurationPtr configuration = GetConfiguration();
                int min = std::max<int>(0, configuration->udp.static_.keep_alived[0]);
                int max = std::max<int>(0, configuration->udp.static_.keep_alived[1]);
                if (min == 0) {
                    min = PPP_UDP_KEEP_ALIVED_MIN_TIMEOUT;
                }

                if (max == 0) {
                    max = PPP_UDP_KEEP_ALIVED_MAX_TIMEOUT;
                }

                if (min > max) {
                    std::swap(min, max);
                }

                uint64_t tick = ppp::threading::Executors::GetTickCount();
                min = std::max<int>(1, min) * 1000;
                max = std::max<int>(1, max) * 1000;

                if (min == max) {
                    static_echo_timeout_ = tick + min;
                }
                else {
                    uint64_t next = RandomNext(min, max + 1);
                    static_echo_timeout_ = tick + next;
                }

                return true;
            }

            bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const ppp::net::packet::IPFrame* packet) noexcept {
                if (NULLPTR == packet || packet->AddressesFamily != AddressFamily::InterNetwork) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                int session_id = static_echo_session_id_;
                if (session_id < 1) {
                    return false;
                }

                int message_length = -1;
                std::shared_ptr<Byte> messages = VirtualEthernetPacket::Pack(configuration,
                    configuration->GetBufferAllocator(),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_protocol_; }),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_transport_; }),
                    session_id,
                    packet,
                    message_length);
                return StaticEchoPacketToRemoteExchanger(messages, message_length);
            }

            bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const std::shared_ptr<ppp::net::packet::UdpFrame>& frame) noexcept {
                if (NULLPTR == frame || frame->AddressesFamily != AddressFamily::InterNetwork) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                int session_id = static_echo_session_id_;
                if (session_id < 1) {
                    return false;
                }

                std::shared_ptr<ppp::net::packet::BufferSegment> payload_buffers = frame->Payload;
                if (NULLPTR == payload_buffers) {
                    return false;
                }

                int packet_length = -1;
                uint32_t source_ip = frame->Source.GetAddress();
                uint32_t destination_ip = frame->Destination.GetAddress();
                std::shared_ptr<Byte> packet = VirtualEthernetPacket::Pack(configuration,
                    configuration->GetBufferAllocator(),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_protocol_; }),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_transport_; }),
                    session_id,
                    source_ip,
                    frame->Source.Port,
                    destination_ip,
                    frame->Destination.Port,
                    payload_buffers->Buffer.get(),
                    payload_buffers->Length,
                    packet_length);
                return StaticEchoPacketToRemoteExchanger(packet, packet_length);
            }

            bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const std::shared_ptr<Byte>& packet, int packet_length) noexcept {
                if (NULLPTR == packet || packet_length < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                std::shared_ptr<StaticEchoDatagarmSocket> socket = static_echo_sockets_[0];
                if (NULLPTR == socket) {
                    return false;
                }

                bool opened = socket->is_open();
                if (!opened) {
                    return false;
                }

                boost::asio::ip::udp::endpoint serverEP = StaticEchoGetRemoteEndPoint();
                if (!socket->is_v6 && serverEP.address().is_v6()) {
                    boost::asio::ip::address_v6 address_v6 = serverEP.address().to_v6();
                    if (address_v6.is_v4_mapped()) {
                        boost::asio::ip::address_v6::bytes_type bytes = address_v6.to_bytes();
                        boost::asio::ip::address_v4::bytes_type v4_bytes = { bytes[12], bytes[13], bytes[14], bytes[15] };
                        serverEP = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4(v4_bytes), serverEP.port());
                    }
                }
                if (int serverPort = serverEP.port(); serverPort > IPEndPoint::MinPort && serverPort <= IPEndPoint::MaxPort) {
                    std::shared_ptr<ppp::transmissions::ITransmissionStatistics> statistics = switcher_->GetStatistics();
                    boost::asio::post(socket->get_executor(),
                        [statistics, socket, packet, packet_length, serverEP]() noexcept {
                            boost::system::error_code ec;
                            socket->send_to(boost::asio::buffer(packet.get(), packet_length), serverEP,
                                boost::asio::socket_base::message_end_of_record, ec);

                            if (ec == boost::system::errc::success) {
                                if (NULLPTR != statistics) {
                                    statistics->AddOutgoingTraffic(packet_length);
                                }
                            }
                        });
                    return true;
                }

                return false;
            }

            std::shared_ptr<ppp::app::protocol::VirtualEthernetPacket> VEthernetExchanger::StaticEchoReadPacket(const void* packet, int packet_length) noexcept {
                if (NULLPTR == packet || packet_length < 1) {
                    return NULLPTR;
                }

                if (disposed_) {
                    return NULLPTR;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return NULLPTR;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = configuration->GetBufferAllocator();
                return VirtualEthernetPacket::Unpack(configuration, 
                    allocator, 
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_protocol_; }), 
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_transport_; }),
                    packet, 
                    packet_length);
            }

            bool VEthernetExchanger::StaticEchoPacketInput(const std::shared_ptr<ppp::app::protocol::VirtualEthernetPacket>& packet) noexcept {
                if (NULLPTR == packet || disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = configuration->GetBufferAllocator();
                static_echo_input_ = true;

                if (packet->Protocol == ppp::net::native::ip_hdr::IP_PROTO_UDP) {
                    auto tap = switcher_->GetTap();
                    if (NULLPTR == tap) {
                        return false;
                    }

                    std::shared_ptr<ppp::net::packet::UdpFrame> frame = packet->GetUdpPacket();
                    if (NULLPTR == frame) {
                        return false;
                    }

                    auto payload = frame->Payload;
                    if (NULLPTR != payload) {
                        const boost::asio::ip::udp::endpoint localEP =
                            IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Destination);
                        const boost::asio::ip::udp::endpoint remoteEP =
                            IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Source);
                        if (TryHandleDatagram(localEP, remoteEP,
                            payload->Buffer.get(), payload->Length)) {
                            LOG_DEBUG("VEthernetExchanger::StaticEchoPacketInput: DNS response handled, local=%s:%u, remote=%s:%u, bytes=%d",
                                localEP.address().to_string().data(), localEP.port(),
                                remoteEP.address().to_string().data(), remoteEP.port(), payload->Length);
                            return true;
                        }
                    }

                    std::shared_ptr<ppp::net::packet::IPFrame> ip = frame->ToIp(allocator);
                    if (NULLPTR == ip) {
                        return false;
                    }
                    
                    if (configuration->udp.dns.cache && frame->Source.Port == PPP_DNS_SYS_PORT) {
                        if (NULLPTR != payload) {
                            ppp::net::asio::vdns::AddCache(payload->Buffer.get(), payload->Length);
                        }
                    }

                    return switcher_->Output(ip.get());
                }
                elif(packet->Protocol == ppp::net::native::ip_hdr::IP_PROTO_IP) {
                    std::shared_ptr<ppp::net::packet::IPFrame> frame = packet->GetIPPacket(allocator);
                    if (NULLPTR == frame) {
                        return false;
                    }

                    if (frame->ProtocolType == ppp::net::native::ip_hdr::IP_PROTO_ICMP) {
                        if (frame->Source == IPEndPoint::LoopbackAddress) {
                            int ack_id = ntohl(frame->Destination);
                            if (ack_id == 0 || ack_id == STATIC_ECHO_KEEP_ALIVED_ID) {
                                return false;
                            }

                            return switcher_->ERORTE(ack_id);
                        }
                    }

                    return switcher_->Output(frame.get());
                }
                else {
                    return false;
                }
            }

            int VEthernetExchanger::StaticEchoYieldReceiveForm(Byte* incoming_packet, int incoming_traffic) noexcept {
                std::shared_ptr<VirtualEthernetPacket> packet = StaticEchoReadPacket(incoming_packet, incoming_traffic);
                if (NULLPTR != packet) {
                    StaticEchoPacketInput(packet);
                }

                auto statistics = switcher_->GetStatistics(); 
                if (NULLPTR != statistics) {
                    statistics->AddIncomingTraffic(incoming_traffic);
                }

                return incoming_traffic;
            }

            bool VEthernetExchanger::Sleep(int64_t timeout, const ContextPtr& context, YieldContext& y) noexcept {
                using atomic_int = std::atomic<int>;

                std::shared_ptr<atomic_int> status = ppp::make_shared_object<atomic_int>(-1);
                if (NULLPTR == status) {
                    return false;
                }

                auto self = shared_from_this();
                boost::asio::post(*context,
                    [self, this, context, timeout, status, &y]() noexcept {
                        bool ok = NewDeadlineTimer(context, timeout, 
                            [status, &y](bool b) noexcept {
                                ppp::coroutines::asio::R(y, *status, b);
                            });
                        
                        if (!ok) {
                            ppp::coroutines::asio::R(y, *status, false);
                        }
                    });

                y.Suspend();
                return status->load() > 0;
            }
            
            bool VEthernetExchanger::StaticEchoLoopbackSocket(const std::shared_ptr<StaticEchoDatagarmSocket>& socket) noexcept {
                if (disposed_) {
                    return false;
                }

                bool openped = socket->is_open();
                if (!openped) {
                    return false;
                }

                auto self = shared_from_this();
                if (std::shared_ptr<ppp::transmissions::ITransmissionQoS> qos = switcher_->GetQoS(); NULLPTR != qos) {
                    return qos->BeginRead(
                        [self, this, socket, qos]() noexcept {
                            socket->async_receive_from(boost::asio::buffer(buffer_.get(), PPP_BUFFER_SIZE), static_echo_source_ep_,
                                [self, this, qos, socket](const boost::system::error_code& ec, std::size_t sz) noexcept {
                                    int bytes_transferred = std::max<int>(-1, ec ? -1 : (int)sz);
                                    if (bytes_transferred > 0) { 
                                        qos->EndRead(StaticEchoYieldReceiveForm(buffer_.get(), bytes_transferred));
                                    }

                                    StaticEchoLoopbackSocket(socket);
                                });
                        });
                }
                else {
                    socket->async_receive_from(boost::asio::buffer(buffer_.get(), PPP_BUFFER_SIZE), static_echo_source_ep_,
                        [self, this, qos, socket](const boost::system::error_code& ec, std::size_t sz) noexcept {
                            int bytes_transferred = std::max<int>(-1, ec ? -1 : (int)sz);
                            if (bytes_transferred > 0) {
                                StaticEchoYieldReceiveForm(buffer_.get(), bytes_transferred);
                            }

                            StaticEchoLoopbackSocket(socket);
                        });
                    return true;
                }
            }

            bool VEthernetExchanger::StaticEchoAddRemoteEndPoint(boost::asio::ip::udp::endpoint& remoteEP) noexcept {
                boost::asio::ip::udp::endpoint destinationEP = Ipep::V4ToV6(remoteEP);
                boost::asio::ip::address destinationIP = destinationEP.address();
                if (!destinationIP.is_v6()) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                auto r = static_echo_server_ep_set_.emplace(destinationEP);
                if (!r.second) {
                    return false;
                }

                static_echo_server_ep_balances_.emplace_back(destinationEP);
                return true;
            }

            boost::asio::ip::udp::endpoint VEthernetExchanger::StaticEchoGetRemoteEndPoint() noexcept {
                std::shared_ptr<aggligator::aggligator> aggligator = switcher_->GetAggligator();
                if (NULLPTR != aggligator) {
#if !defined(_ANDROID) && !defined(_IPHONE)
                    auto ni = switcher_->GetUnderlyingNetworkInterface(); 
                    if (NULLPTR != ni) {
                        boost::asio::ip::udp::endpoint ep = aggligator->client_endpoint(ni->IPAddress);
                        return Ipep::V4ToV6(ep);
                    }
#endif
                    return aggligator->client_endpoint(boost::asio::ip::address_v6::loopback());
                }

                boost::asio::ip::udp::endpoint destinationEP;
                for (SynchronizedObjectScope scope(syncobj_);;) {
                    auto tail = static_echo_server_ep_balances_.begin();
                    auto endl = static_echo_server_ep_balances_.end();
                    if (tail == endl) {
                        destinationEP = boost::asio::ip::udp::endpoint(server_url_.remoteEP.address(), static_echo_remote_port_);
                        break;
                    }
                    
                    std::size_t server_addrsss_num = static_echo_server_ep_set_.size();
                    if (server_addrsss_num == 1) {
                        destinationEP = *static_echo_server_ep_balances_.begin();
                    }
                    else {
                        destinationEP = *tail;
                        static_echo_server_ep_balances_.erase(tail);
                        static_echo_server_ep_balances_.emplace_back(destinationEP);
                    }

                    break;
                }

                return Ipep::V4ToV6(destinationEP);
            }

            bool VEthernetExchanger::StaticEchoOpenAsynchronousSocket(StaticEchoDatagarmSocket& socket, YieldContext& y) noexcept {
                if (disposed_) {
                    return false;
                }

                bool opened = socket.is_open(true);
                if (opened) {
                    return true;
                }

                if (server_url_.port <= IPEndPoint::MinPort || server_url_.port > IPEndPoint::MaxPort) {
                    return false;
                }   

                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                // Try IPv6 (dual-stack socket on Windows) first, fall back to IPv4
                // if the system has no IPv6 stack available.
                bool is_v6 = true;
                opened = ppp::coroutines::asio::async_open<boost::asio::ip::udp::socket>(y, socket, boost::asio::ip::udp::v6()) && !disposed_;
                if (!opened) {
                    is_v6 = false;
                    opened = ppp::coroutines::asio::async_open<boost::asio::ip::udp::socket>(y, socket, boost::asio::ip::udp::v4()) && !disposed_;
                }
                if (!opened) {
                    return false;
                }

                bool ok = false;
                for (;;) {
                    boost::asio::ip::address listen_address = is_v6
                        ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
                        : boost::asio::ip::address(boost::asio::ip::address_v4::any());
                    opened = Socket::OpenSocket(socket, listen_address, IPEndPoint::MinPort, opened);
                    if (!opened) {
                        break;
                    }
                    else {
                        Socket::SetWindowSizeIfNotZero(socket.native_handle(), configuration->udp.cwnd, configuration->udp.rwnd);
                    }
                    
#if defined(_ANDROID)
                    std::shared_ptr<aggligator::aggligator> aggligator = switcher_->GetAggligator();
                    if (NULLPTR == aggligator) {
                        auto protector_network = switcher_->GetProtectorNetwork(); 
                        if (NULLPTR != protector_network) {
                            opened = protector_network->Protect(socket.native_handle(), y);
                            if (!opened) {
                                break;
                            }
                        }
                    }
#endif
                    // Mark that the socket has been opened.
                    socket.opened = opened;
                    socket.is_v6 = is_v6;

                    // Set the timeout period for closing and re-opening the socket next-timed.
                    ok = StaticEchoNextTimeout();
                    break;
                }

                if (!ok) {
                    Socket::Closesocket(socket);
                }

                return ok;
            }
        }
    }
}
