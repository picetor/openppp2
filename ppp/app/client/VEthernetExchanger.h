#pragma once

#include <ppp/app/protocol/VirtualEthernetLinklayer.h>
#include <ppp/app/protocol/VirtualEthernetMappingPort.h>
#include <ppp/app/protocol/VirtualEthernetPacket.h>
#include <ppp/app/mux/vmux_net.h>
#include <ppp/cryptography/Ciphertext.h>
#include <ppp/Int128.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/native/ip.h>
#include <ppp/net/packet/UdpFrame.h>
#include <ppp/net/packet/IPFrame.h>
#include <ppp/net/packet/IcmpFrame.h>
#include <ppp/threading/Timer.h>
#include <ppp/auxiliary/UriAuxiliary.h>
#include <ppp/transmissions/proxys/IForwarding.h>
#include <ppp/app/client/ConnectivityProbe.h>

namespace ppp {
    namespace app {
        namespace client {
            class VEthernetNetworkSwitcher;
            class VEthernetDatagramPort;

            class VEthernetExchanger : public ppp::app::protocol::VirtualEthernetLinklayer {
                friend class                                                            VEthernetDatagramPort;
                friend class                                                            VEthernetNetworkSwitcher;

            public:
                typedef std::shared_ptr<VEthernetNetworkSwitcher>                       VEthernetNetworkSwitcherPtr;
                typedef ppp::app::protocol::VirtualEthernetInformation                  VirtualEthernetInformation;
                typedef ppp::app::protocol::VirtualEthernetInformationExtensions       VirtualEthernetInformationExtensions;
                typedef ppp::auxiliary::UriAuxiliary                                    UriAuxiliary;
                typedef UriAuxiliary::ProtocolType                                      ProtocolType;
                typedef ppp::threading::Timer                                           Timer;
                typedef std::shared_ptr<Timer>                                          TimerPtr;
                typedef ppp::unordered_map<void*, TimerPtr>                             TimerTable;
                typedef std::shared_ptr<VEthernetDatagramPort>                          VEthernetDatagramPortPtr;
                typedef ppp::threading::Executors::StrandPtr                            StrandPtr;
                typedef std::mutex                                                      SynchronizedObject;
                typedef std::lock_guard<SynchronizedObject>                             SynchronizedObjectScope;
                typedef ppp::function<bool(const boost::asio::ip::udp::endpoint&, const boost::asio::ip::udp::endpoint&, void*, int)> DatagramPacketHandler;
                typedef ppp::unordered_map<boost::asio::ip::udp::endpoint, DatagramPacketHandler> DatagramPacketHandlerTable;

            private:
                typedef ppp::unordered_map<boost::asio::ip::udp::endpoint,
                    VEthernetDatagramPortPtr>                                           VEthernetDatagramPortTable;
                typedef ppp::app::protocol::VirtualEthernetMappingPort                  VirtualEthernetMappingPort;
                typedef std::shared_ptr<VirtualEthernetMappingPort>                     VirtualEthernetMappingPortPtr;
                typedef ppp::unordered_map<uint32_t, VirtualEthernetMappingPortPtr>     VirtualEthernetMappingPortTable;
                typedef ppp::cryptography::Ciphertext                                   Ciphertext;
                typedef std::shared_ptr<Ciphertext>                                     CiphertextPtr;
                typedef std::shared_ptr<boost::asio::deadline_timer>                    DeadlineTimerPtr;
                typedef ppp::unordered_map<void*, DeadlineTimerPtr>                     DeadlineTimerTable;
                typedef ppp::transmissions::proxys::IForwarding                         IForwarding;
                typedef std::shared_ptr<IForwarding>                                    IForwardingPtr;

            public:
                VEthernetExchanger(
                    const VEthernetNetworkSwitcherPtr&                                  switcher,
                    const AppConfigurationPtr&                                          configuration,
                    const ContextPtr&                                                   context,
                    const Int128&                                                       id,
                    const ppp::string&                                                  outbound_tag = "main",
                    bool                                                                primary_outbound = true) noexcept;
                virtual ~VEthernetExchanger() noexcept;

            public:
                typedef enum {
                    NetworkState_Connecting,
                    NetworkState_Established,
                    NetworkState_Reconnecting,
                }                                                                       NetworkState;

            public:
                NetworkState                                                            GetNetworkState()       noexcept { return network_state_.load(); }
                const ppp::string&                                                      GetOutboundTag()        const noexcept { return outbound_tag_; }
                bool                                                                    IsPrimaryOutbound()     const noexcept { return primary_outbound_.load(); }
                // A split outbound can be promoted to the primary role without
                // opening another control/MUX session.  The role is dynamic:
                // "main" is an alias owned by the switcher, not a second
                // exchanger.
                bool                                                                    SetPrimaryOutbound(bool primary) noexcept;
                std::shared_ptr<Byte>                                                   GetBuffer()             noexcept { return buffer_; }
                std::shared_ptr<vmux::vmux_net>                                         GetMux()                noexcept { return mux_; }
                VEthernetNetworkSwitcherPtr                                             GetSwitcher()           noexcept { return switcher_; }
                std::shared_ptr<VirtualEthernetInformation>                             GetInformation()        noexcept { return information_; }
                const VirtualEthernetInformationExtensions&                             GetInformationExtensions() const noexcept { return information_extensions_; }
                ITransmissionPtr                                                        GetTransmission()       noexcept { return transmission_; }
                int                                                                     GetReconnectionCount()  noexcept { return reconnection_count_; }
                int                                                                     GetProbeRtt()           noexcept;
                bool                                                                    GetProbeReachable()     noexcept;
                bool                                                                    GetProbeChecked()       noexcept;
                ppp::string                                                             GetProbeServer()        noexcept;
                ppp::string                                                             GetCurrentEntry()       noexcept;
                ppp::string                                                             GetRankedFirstEntry()   noexcept;
                bool                                                                    SwitchToRankedFirstEntry() noexcept;
                NetworkState                                                            GetMuxNetworkState()    noexcept;
                virtual bool                                                            Open()                  noexcept;
                virtual void                                                            Dispose()               noexcept;
                virtual ITransmissionPtr                                                ConnectTransmission(const ContextPtr& context, const StrandPtr& strand, YieldContext& y, const ppp::string* entry = NULLPTR) noexcept;
                bool                                                                    AcquireActiveTransmission(const ContextPtr& context, YieldContext& y) noexcept;
                void                                                                    ReleaseActiveTransmission() noexcept;
                bool                                                                    Sleep(int64_t timeout, const ContextPtr& context, YieldContext& y) noexcept;
                
            public:
                template <typename F>
                void                                                                    Post(F&& f) noexcept {
#if defined(_ANDROID)
                    auto context = GetContext();
                    if (context) {
                        auto self = shared_from_this();
                        boost::asio::post(*context, 
                            [self, f]() noexcept {
                                f();
                            });
                    }
#else   
                    f();
#endif
                }

            public:
                virtual bool                                                            Nat(const void* packet, int packet_size) noexcept;
                virtual bool                                                            Echo(int ack_id) noexcept;
                virtual bool                                                            Echo(const void* packet, int packet_size) noexcept;
                virtual bool                                                            SendTo(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, const void* packet, int packet_size) noexcept;
                virtual bool                                                            SendTo(const boost::asio::ip::udp::endpoint& sourceEP, const ppp::string& destinationHost, const boost::asio::ip::udp::endpoint& destinationEP, const void* packet, int packet_size) noexcept;
                virtual bool                                                            Update() noexcept;
                void                                                                    GetDebugObjectCounts(size_t& mappings, size_t& datagrams, size_t& timers) noexcept;
                void                                                                    ResetMuxDataPlane() noexcept;
                void                                                                    ResetDataChannels() noexcept;
                bool                                                                    StaticEchoAllocated() noexcept;
                virtual bool                                                            GetRemoteEndPoint(YieldContext* y, ppp::string& hostname, ppp::string& address, ppp::string& path, int& port, ProtocolType& protocol_type, ppp::string& server, boost::asio::ip::tcp::endpoint& remoteEP, const ppp::string* entry = NULLPTR) noexcept;

            public:
                bool                                                                    RegisterDatagramHandler(const boost::asio::ip::udp::endpoint& sourceEP, const DatagramPacketHandler& handler) noexcept;
                bool                                                                    ReleaseDatagramHandler(const boost::asio::ip::udp::endpoint& sourceEP) noexcept;
                bool                                                                    TryHandleDatagram(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length) noexcept;

            protected:
                virtual bool                                                            OnLan(const ITransmissionPtr& transmission, uint32_t ip, uint32_t mask, YieldContext& y) noexcept override;
                virtual bool                                                            OnNat(const ITransmissionPtr& transmission, Byte* packet, int packet_length, YieldContext& y) noexcept override;
                virtual bool                                                            OnInformation(const ITransmissionPtr& transmission, const VirtualEthernetInformation& information, YieldContext& y) noexcept override;
                virtual bool                                                            OnInformation(const ITransmissionPtr& transmission, const InformationEnvelope& information, YieldContext& y) noexcept override;
                virtual bool                                                            OnPush(const ITransmissionPtr& transmission, int connection_id, Byte* packet, int packet_length, YieldContext& y) noexcept override;
                virtual bool                                                            OnConnect(const ITransmissionPtr& transmission, int connection_id, const boost::asio::ip::tcp::endpoint& destinationEP, YieldContext& y) noexcept override;
                virtual bool                                                            OnConnectOK(const ITransmissionPtr& transmission, int connection_id, Byte error_code, YieldContext& y) noexcept override;
                virtual bool                                                            OnDisconnect(const ITransmissionPtr& transmission, int connection_id, YieldContext& y) noexcept override;
                virtual bool                                                            OnEcho(const ITransmissionPtr& transmission, int ack_id, YieldContext& y) noexcept override;
                virtual bool                                                            OnEcho(const ITransmissionPtr& transmission, Byte* packet, int packet_length, YieldContext& y) noexcept override;
                virtual bool                                                            OnSendTo(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length, YieldContext& y) noexcept override;
                virtual bool                                                            OnStatic(const ITransmissionPtr& transmission, YieldContext& y) noexcept override;
                virtual bool                                                            OnStatic(const ITransmissionPtr& transmission, Int128 fsid, int session_id, int remote_port, YieldContext& y) noexcept override;
                virtual bool                                                            OnMux(const ITransmissionPtr& transmission, uint16_t vlan, uint16_t max_connections, bool acceleration, Byte ordering_caps, YieldContext& y) noexcept override;

            protected:
                virtual VEthernetDatagramPortPtr                                        NewDatagramPort(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept;
                virtual VEthernetDatagramPortPtr                                        GetDatagramPort(const boost::asio::ip::udp::endpoint& sourceEP) noexcept;
                virtual VEthernetDatagramPortPtr                                        ReleaseDatagramPort(const boost::asio::ip::udp::endpoint& sourceEP) noexcept;

            protected:
                virtual ITransmissionPtr                                                NewTransmission(
                    const ContextPtr&                                                   context,
                    const StrandPtr&                                                    strand,
                    const std::shared_ptr<boost::asio::ip::tcp::socket>&                socket,
                    ProtocolType                                                        protocol_type,
                    const ppp::string&                                                  host,
                    const ppp::string&                                                  path) noexcept;
                virtual ITransmissionPtr                                                OpenTransmission(const ContextPtr& context, const StrandPtr& strand, YieldContext& y, const ppp::string* entry = NULLPTR) noexcept;

            protected:
                virtual std::shared_ptr<boost::asio::ip::tcp::socket>                   NewAsynchronousSocket(const ContextPtr& context, const StrandPtr& strand, const boost::asio::ip::tcp& protocol, ppp::coroutines::YieldContext& y) noexcept;
                virtual bool                                                            Loopback(const ContextPtr& context, YieldContext& y) noexcept;
                virtual bool                                                            PacketInput(const ITransmissionPtr& transmission, Byte* p, int packet_length, YieldContext& y) noexcept;

            private:
                ITransmissionPtr                                                        OpenTransmission(const ContextPtr& context, YieldContext& y) noexcept {
                    StrandPtr strand;
                    return OpenTransmission(context, strand, y);
                }
                bool                                                                    TranslateIPv6Packet(Byte* packet, int packet_length, bool outbound) noexcept;
                void                                                                    Finalize() noexcept;
                void                                                                    ExchangeToEstablishState() noexcept;
                void                                                                    ExchangeToConnectingState() noexcept;
                void                                                                    ExchangeToReconnectingState() noexcept;
                int64_t                                                                 GetReconnectDelayMilliseconds() noexcept;
                bool                                                                    ProbeSelectServerEndPoint(
                    YieldContext&                                                       y,
                    const ppp::vector<ppp::string>&                                     entries,
                    ppp::string&                                                        hostname,
                    ppp::string&                                                        address,
                    ppp::string&                                                        path,
                    int&                                                                port,
                    ProtocolType&                                                       protocol_type,
                    ppp::string&                                                        server,
                    boost::asio::ip::tcp::endpoint&                                     remoteEP,
                    const ppp::string*                                                  forced_entry = NULLPTR) noexcept;
                bool                                                                    ProbeCandidateEndpoint(
                    ConnectivityProbe::ProbeType                                        probe_type,
                    const boost::asio::ip::tcp::endpoint&                               remoteEP,
                    const ppp::string&                                                  hostname,
                    const ppp::string&                                                  path,
                    int                                                                 stage,
                    int                                                                 timeout_ms,
                    const ppp::string&                                                  ws_host,
                    const ppp::string&                                                  ws_sni,
                    YieldContext&                                                       y,
                    int&                                                                rtt_ms,
                    const ConnectivityProbe::ProtectSocketHandler&                     protect) noexcept;
                void                                                                    StoreProbeResult(
                    const ppp::string&                                                  entry,
                    ConnectivityProbe::ProbeType                                        probe_type,
                    bool                                                                reachable,
                    int                                                                 rtt_ms,
                    int                                                                 stage,
                    uint64_t                                                            now,
                    uint64_t                                                            ttl_ms) noexcept;
                int                                                                     EchoLanToRemoteExchanger(const ITransmissionPtr& transmission, YieldContext& y) noexcept;
                bool                                                                    SendEchoKeepAlivePacket(UInt64 now, bool immediately) noexcept;
                bool                                                                    ReceiveFromDestination(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length) noexcept;
                VEthernetDatagramPortPtr                                                AddNewDatagramPort(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept;

            private:
                template <typename TTransmission>
                typename std::enable_if<std::is_base_of<ITransmission, TTransmission>::value, std::shared_ptr<TTransmission>/**/>::type
                inline                                                                  NewWebsocketTransmission(const ContextPtr& context, const StrandPtr& strand, const std::shared_ptr<boost::asio::ip::tcp::socket>& socket, const ppp::string& host, const ppp::string& path) noexcept {
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    if (NULLPTR == configuration) {
                        return NULLPTR;
                    }

                    auto transmission = make_shared_object<TTransmission>(context, strand, socket, configuration);
                    if (NULLPTR == transmission) {
                        return NULLPTR;
                    }
                    
                    if (host.size() > 0 && path.size() > 0) {
                        transmission->Host = host;
                        transmission->Path = path;
                    }

                    return transmission;
                }

            private:
                VirtualEthernetMappingPortPtr                                           GetMappingPort(bool in, bool tcp, int remote_port) noexcept;
                VirtualEthernetMappingPortPtr                                           NewMappingPort(bool in, bool tcp, int remote_port) noexcept;
                bool                                                                    RegisterMappingPort(ppp::configurations::AppConfiguration::MappingConfiguration& mapping) noexcept;
                void                                                                    UnregisterAllMappingPorts() noexcept;
                bool                                                                    RegisterAllMappingPorts() noexcept;
                bool                                                                    ReleaseDeadlineTimer(const boost::asio::deadline_timer* deadline_timer) noexcept;
                bool                                                                    NewDeadlineTimer(const ContextPtr& context, int64_t timeout, const ppp::function<void(bool)>& event) noexcept;
#if defined(_ANDROID)
                bool                                                                    AwaitJniAttachThread(const ContextPtr& context, YieldContext& y) noexcept;
#endif
                virtual bool                                                            DoKeepAlived(const ITransmissionPtr& transmission, uint64_t now) noexcept override;
                bool                                                                    DoMuxEvents() noexcept;
                bool                                                                    MuxConnectAllLinklayers(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator, const std::shared_ptr<vmux::vmux_net>& mux) noexcept;
                bool                                                                    MuxGrowLinklayers(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator, const std::shared_ptr<vmux::vmux_net>& mux, int count, ppp::string entry = ppp::string()) noexcept;
                /** @brief Rank fresh reachable entries by reliability tier and latency risk. */
                ppp::vector<ppp::string>                                                HotSwitchRankedEntries(uint64_t now) noexcept;
                /** @brief Blacklist an entry for the configured penalty window. */
                void                                                                    HotSwitchBlacklistEntry(const ppp::string& entry, uint64_t now) noexcept;

            private:
                class StaticEchoDatagarmSocket final : public boost::asio::ip::udp::socket {
                public:
                    StaticEchoDatagarmSocket(boost::asio::io_context& context) noexcept 
                        : basic_datagram_socket(context)
                        , opened(false)
                        , is_v6(false) {

                    }
                    virtual ~StaticEchoDatagarmSocket() noexcept {
                        boost::asio::ip::udp::socket* my = this;
                        destructor_invoked(my);
                    }

                public:
                    bool                                                                is_open(bool only_native = false) noexcept { return only_native ? basic_datagram_socket::is_open() : opened && basic_datagram_socket::is_open(); }

                public:
                    bool                                                                opened = false;
                    bool                                                                is_v6 = false;
                };
                bool                                                                    StaticEchoAddRemoteEndPoint(boost::asio::ip::udp::endpoint& remoteEP) noexcept;
                boost::asio::ip::udp::endpoint                                          StaticEchoGetRemoteEndPoint() noexcept;
                void                                                                    StaticEchoClean() noexcept;
                bool                                                                    StaticEchoNextTimeout() noexcept;
                bool                                                                    StaticEchoSwapAsynchronousSocket() noexcept;
                bool                                                                    StaticEchoGatewayServer(int ack_id) noexcept;
                int                                                                     StaticEchoYieldReceiveForm(Byte* incoming_packet, int incoming_traffic) noexcept;
                bool                                                                    StaticEchoLoopbackSocket(const std::shared_ptr<StaticEchoDatagarmSocket>& socket) noexcept;
                bool                                                                    StaticEchoOpenAsynchronousSocket(StaticEchoDatagarmSocket& socket, YieldContext& y) noexcept;
                bool                                                                    StaticEchoAllocatedToRemoteExchanger(YieldContext& y) noexcept;
                bool                                                                    StaticEchoPacketToRemoteExchanger(const std::shared_ptr<Byte>& packet, int packet_length) noexcept;
                bool                                                                    StaticEchoPacketToRemoteExchanger(const ppp::net::packet::IPFrame* packet) noexcept;
                bool                                                                    StaticEchoPacketToRemoteExchanger(const std::shared_ptr<ppp::net::packet::UdpFrame>& frame) noexcept;
                bool                                                                    StaticEchoPacketInput(const std::shared_ptr<ppp::app::protocol::VirtualEthernetPacket>& packet) noexcept;
                std::shared_ptr<ppp::app::protocol::VirtualEthernetPacket>              StaticEchoReadPacket(const void* packet, int packet_length) noexcept;

            private:
                virtual bool                                                            OnFrpSendTo(const ITransmissionPtr& transmission, bool in, int remote_port, const boost::asio::ip::udp::endpoint& sourceEP, Byte* packet, int packet_length, YieldContext& y) noexcept override;
                virtual bool                                                            OnFrpConnect(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port, YieldContext& y) noexcept override;
                virtual bool                                                            OnFrpDisconnect(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port) noexcept override;
                virtual bool                                                            OnFrpPush(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port, const void* packet, int packet_length) noexcept override;

            private:
                SynchronizedObject                                                      syncobj_;

                std::atomic<bool>                                                       disposed_ = false;
                bool                                                                    static_echo_input_ = false;

                std::shared_ptr<Byte>                                                   buffer_;            

                UInt64                                                                  sekap_last_         = 0;
                UInt64                                                                  sekap_next_         = 0;

                VEthernetNetworkSwitcherPtr                                             switcher_;
                ppp::string                                                             outbound_tag_;
                std::atomic<bool>                                                       primary_outbound_ = true;
                boost::asio::ip::address                                                assigned_ipv6_address_;
                IForwardingPtr                                                          forwarding_;
                std::shared_ptr<VirtualEthernetInformation>                             information_;
                VirtualEthernetInformationExtensions                                    information_extensions_;
                VEthernetDatagramPortTable                                              datagrams_;
                DatagramPacketHandlerTable                                              datagram_handlers_;
                ITransmissionPtr                                                        transmission_;
                std::atomic<NetworkState>                                               network_state_      = NetworkState_Connecting;
                VirtualEthernetMappingPortTable                                         mappings_;
                DeadlineTimerTable                                                      deadline_timers_;

                std::shared_ptr<vmux::vmux_net>                                         mux_;
                uint16_t                                                                mux_vlan_           = 0;
                ppp::string                                                             mux_entry_;          ///< One immutable entry per active MUX generation.
                ppp::string                                                             mux_failure_entry_;  ///< Entry associated with the current consecutive M1/M4 streak.
                uint64_t                                                                mux_failure_last_   = 0;
                int                                                                     mux_failure_streak_ = 0;
                uint64_t                                                                mux_retry_not_before_ = 0; ///< Do not create another MUX before this tick.
                uint32_t                                                                mux_retry_backoff_ms_ = 0; ///< Bounded MUX-only retry backoff.
                
                int                                                                     reconnection_count_ = 0;

                typedef ppp::unordered_map<ppp::string, ConnectivityProbe::Result>      ProbeResultTable;
                ProbeResultTable                                                        probe_results_;
                std::atomic<int>                                                        probe_rtt_ms_       = -1;
                std::atomic<bool>                                                       probe_reachable_    = false;
                std::atomic<bool>                                                       probe_checked_      = false;
                ppp::string                                                             probe_server_;

                struct {
                    boost::asio::ip::tcp::endpoint                                      remoteEP;
                    ppp::string                                                         hostname;
                    ppp::string                                                         address;
                    ppp::string                                                         path;
                    ppp::string                                                         server;
                    int                                                                 port                = 0;
                    ProtocolType                                                        protocol_type       = ProtocolType::ProtocolType_PPP;
                }                                                                       server_url_;

                CiphertextPtr                                                           static_echo_protocol_;
                CiphertextPtr                                                           static_echo_transport_;
                std::shared_ptr<StaticEchoDatagarmSocket>                               static_echo_sockets_[2];
                boost::asio::ip::udp::endpoint                                          static_echo_source_ep_;
                ppp::list<boost::asio::ip::udp::endpoint>                               static_echo_server_ep_balances_;
                ppp::unordered_set<boost::asio::ip::udp::endpoint>                      static_echo_server_ep_set_;
                
                uint64_t                                                                static_echo_timeout_     = 0;
                int                                                                     static_echo_session_id_  = 0;
                int                                                                     static_echo_remote_port_ = 0;
            };
        }
    }
}
