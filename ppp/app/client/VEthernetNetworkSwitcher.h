 #pragma once

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/native/rib.h>
#include <ppp/net/packet/IPFrame.h>
#include <ppp/ethernet/VEthernet.h>
#include <ppp/ethernet/VNetstack.h>
#include <ppp/ipv6/IPv6Auxiliary.h>
#include <ppp/ipv6/IPv6Packet.h>
#include <ppp/transmissions/proxys/IForwarding.h>
#include <ppp/transmissions/ITransmission.h>
#include <ppp/transmissions/ITransmissionQoS.h>
#include <ppp/transmissions/ITransmissionStatistics.h>
#include <ppp/app/protocol/VirtualEthernetLinklayer.h>
#include <ppp/app/protocol/VirtualEthernetInformation.h>
#include <ppp/app/protocol/VirtualEthernetLogger.h>
#include <ppp/app/client/dns/Rule.h>
#include <ppp/app/client/geo/GeoRuleEngine.h>
#include <ppp/app/client/proxys/VEthernetHttpProxySwitcher.h>
#include <ppp/app/client/proxys/VEthernetSocksProxySwitcher.h>

#if defined(_WIN32)
#include <windows/ppp/win32/network/Router.h>
#include <windows/ppp/win32/network/NetworkInterface.h>
#include <windows/ppp/app/client/lsp/PaperAirplaneController.h>
#elif defined(_LINUX)
#include <linux/ppp/net/ProtectorNetwork.h>
#endif

#include <common/aggligator/aggligator.h>

namespace ppp {
    namespace app {
        namespace client {
            class VEthernetExchanger;
            class VEthernetDatagramPort;
            class PeerPrefixRouteManager;

            class VEthernetNetworkSwitcher : public ppp::ethernet::VEthernet {
            private:
                friend class                                                        VEthernetExchanger;
                friend class                                                        VEthernetDatagramPort;
                friend class                                                        PeerPrefixRouteManager;

            private:    
                typedef struct {    
                    UInt64                                                          datetime;
                    IPFrame::IPFramePtr                                             packet;
                }                                                                   VEthernetIcmpPacket;
                typedef ppp::unordered_map<int, VEthernetIcmpPacket>                VEthernetIcmpPacketTable;
                typedef ppp::app::client::dns::Rule::Ptr                            DNSRulePtr;
                typedef ppp::unordered_map<ppp::string, DNSRulePtr>                 DNSRuleTable;
                typedef ppp::threading::Timer                                       Timer;
                typedef std::weak_ptr<Timer::TimeoutEventHandler>                   TimeoutEventHandlerWeakPtr;
                typedef ppp::unordered_map<void*, TimeoutEventHandlerWeakPtr>       TimeoutEventHandlerTable;
                typedef ppp::vector<std::pair<ppp::string, uint32_t>/**/>           LoadIPListFileVector;
                typedef std::shared_ptr<LoadIPListFileVector>                       LoadIPListFileVectorPtr;
                typedef ppp::vector<std::pair<ppp::string, boost::asio::ip::address>> LoadIPv6ListFileVector;
                typedef std::shared_ptr<LoadIPv6ListFileVector>                     LoadIPv6ListFileVectorPtr;
                struct IPv6RouteEntry {
                    boost::asio::ip::address_v6                                    Network;
                    int                                                             Prefix;
                    boost::asio::ip::address_v6                                    NextHop;
                };
                typedef ppp::vector<IPv6RouteEntry>                                 IPv6RouteTable;
                typedef std::shared_ptr<IPv6RouteTable>                             IPv6RouteTablePtr;
                typedef ppp::net::native::RouteInformationTable6                    RouteInformationTable6;
                typedef std::shared_ptr<RouteInformationTable6>                     RouteInformationTable6Ptr;
                typedef ppp::net::native::ForwardInformationTable6                  ForwardInformationTable6;
                typedef std::shared_ptr<ForwardInformationTable6>                   ForwardInformationTable6Ptr;
                typedef ppp::vector<boost::asio::ip::address>                       NicDnsServerAddresses;
                typedef ppp::unordered_map<int, NicDnsServerAddresses>              AllNicDnsServerAddresses;
                typedef ppp::transmissions::proxys::IForwarding                     IForwarding;
                typedef std::shared_ptr<IForwarding>                                IForwardingPtr;

            public: 
                typedef ppp::app::protocol::VirtualEthernetInformation              VirtualEthernetInformation;
                typedef ppp::app::protocol::VirtualEthernetInformationExtensions   VirtualEthernetInformationExtensions;
                typedef ppp::app::protocol::VirtualEthernetLogger                   VirtualEthernetLogger;
                typedef std::shared_ptr<VirtualEthernetLogger>                      VirtualEthernetLoggerPtr;
                typedef ppp::app::client::proxys::VEthernetHttpProxySwitcher        VEthernetHttpProxySwitcher;
                typedef std::shared_ptr<VEthernetHttpProxySwitcher>                 VEthernetHttpProxySwitcherPtr;
                typedef ppp::app::client::proxys::VEthernetSocksProxySwitcher       VEthernetSocksProxySwitcher;
                typedef std::shared_ptr<VEthernetSocksProxySwitcher>                VEthernetSocksProxySwitcherPtr;
                typedef ppp::function<void(VEthernetNetworkSwitcher*, UInt64)>      VEthernetTickEventHandler;
                typedef ppp::transmissions::ITransmissionStatistics                 ITransmissionStatistics;
                typedef std::shared_ptr<ITransmissionStatistics>                    ITransmissionStatisticsPtr;
                struct OutboundConfiguration final {
                    ppp::string                                                     tag;
                    std::shared_ptr<ppp::configurations::AppConfiguration>          configuration;
                    ppp::string                                                     display_name;
                    bool                                                            server_menu = false;
                    ppp::string                                                     source_path;
                    bool                                                            route_used = false;
                };
                struct OutboundStatus final {
                    ppp::string                                                     tag;
                    ppp::string                                                     display_name;
                    ppp::string                                                     server;
                    int                                                             state = 0;
                    int                                                             reconnects = 0;
                    bool                                                            active = false;
                    bool                                                            server_menu = false;
                    bool                                                            route_used = false;
                };
                typedef ppp::vector<OutboundConfiguration>                         OutboundConfigurationList;
                typedef ppp::vector<OutboundStatus>                                OutboundStatusList;
                typedef ppp::unordered_map<ppp::string, std::shared_ptr<VEthernetExchanger>> OutboundExchangerTable;
                class NetworkInterface {    
                public: 
                    ppp::string                                                     Name;
#if !defined(_MACOS)    
                    ppp::string                                                     Id;
#endif  
                    int                                                             Index = -1;
                    ppp::vector<boost::asio::ip::address>                           DnsAddresses;

                public: 
                    NetworkInterface() noexcept;    
                    virtual ~NetworkInterface() noexcept = default;

                public: 
                    boost::asio::ip::address                                        IPAddress;
                    boost::asio::ip::address                                        GatewayServer;
                    boost::asio::ip::address                                        IPv6GatewayServer;
                    boost::asio::ip::address                                        SubmaskAddress;

#if defined(_WIN32) 
                public: 
                    ppp::string                                                     Description;
#elif defined(_MACOS)   
                    ppp::unordered_map<uint32_t, uint32_t>                          DefaultRoutes;
#endif  
                };
                typedef ppp::net::native::RouteInformationTable                     RouteInformationTable;
                typedef std::shared_ptr<RouteInformationTable>                      RouteInformationTablePtr;
                typedef ppp::net::native::ForwardInformationTable                   ForwardInformationTable;
                typedef std::shared_ptr<ForwardInformationTable>                    ForwardInformationTablePtr;
                typedef ppp::unordered_map<ppp::string, ppp::string>                RouteIPListTable;
                typedef std::shared_ptr<RouteIPListTable>                           RouteIPListTablePtr;
#if defined(_WIN32)
                typedef lsp::PaperAirplaneController                                PaperAirplaneController;
                typedef std::shared_ptr<PaperAirplaneController>                    PaperAirplaneControllerPtr;
#elif defined(_LINUX)   
                typedef ppp::net::ProtectorNetwork                                  ProtectorNetwork;
                typedef std::shared_ptr<ProtectorNetwork>                           ProtectorNetworkPtr;
#endif

            public: 
                VEthernetTickEventHandler                                           TickEvent;

            public:
                VEthernetNetworkSwitcher(const std::shared_ptr<boost::asio::io_context>& context, bool lwip, bool vnet, bool mta, const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration) noexcept;
                virtual ~VEthernetNetworkSwitcher() noexcept;

            public:
#if defined(_WIN32)
                PaperAirplaneControllerPtr                                          GetPaperAirplaneController() noexcept { return paper_airplane_ctrl_; }
                virtual bool                                                        SetHttpProxyToSystemEnv()    noexcept;
                virtual bool                                                        ClearHttpProxyToSystemEnv()  noexcept;
                bool                                                                LocalDns(bool* value)        noexcept;
                boost::asio::ip::address                                             ResolveProxyDomainThroughTunnel(
                    const ppp::string& hostname,
                    ppp::coroutines::YieldContext& y) noexcept;
#elif defined(_LINUX)   
                ProtectorNetworkPtr                                                 GetProtectorNetwork()        noexcept { return protect_network_; }
#endif  
                std::shared_ptr<ppp::configurations::AppConfiguration>              GetConfiguration()           noexcept { return configuration_; }
                std::shared_ptr<VEthernetExchanger>                                 GetExchanger()               noexcept { return exchanger_; }
                std::shared_ptr<VEthernetExchanger>                                 GetExchanger(const boost::asio::ip::address& destination) noexcept;
                VirtualEthernetLoggerPtr                                            GetLogger()                  noexcept { return logger_; }
                std::shared_ptr<ppp::transmissions::ITransmissionQoS>               GetQoS()                     noexcept { return qos_; }
                std::shared_ptr<ppp::transmissions::ITransmissionStatistics>        GetStatistics()              noexcept { return statistics_; }
                std::shared_ptr<VirtualEthernetInformation>                         GetInformation()             noexcept;
                VEthernetHttpProxySwitcherPtr                                       GetHttpProxy()               noexcept { return http_proxy_; }
                VEthernetSocksProxySwitcherPtr                                      GetSocksProxy()              noexcept { return socks_proxy_; }
                RouteInformationTablePtr                                            GetRib()                     noexcept { return rib_; }
                ForwardInformationTablePtr                                          GetFib()                     noexcept { return fib_; }
                IForwardingPtr                                                      GetForwarding()              noexcept { return forwarding_; }
                std::shared_ptr<aggligator::aggligator>                             GetAggligator()              noexcept { return aggligator_; }
                bool                                                                IsBlockQUIC()                noexcept { return block_quic_; }
                bool                                                                IsMuxEnabled()               noexcept { return mux_ > 0; }
                bool                                                                IsBypassIpAddress(const boost::asio::ip::address& ip) noexcept;
                bool                                                                IsBypassIpAddress6(const boost::asio::ip::address& ip) noexcept;
                void                                                                ClearPeerPrefixRoutes() noexcept;
                bool                                                                ApplyPeerPrefixRoutes(const ppp::app::protocol::VirtualEthernetInformationExtensions& extensions) noexcept;
                const ppp::vector<ppp::net::native::RouteEntry>&                     GetAppliedPeerPrefixRoutes() const noexcept { return applied_peer_prefix_routes_; }

            public: 
                virtual bool                                                        LoadAllDnsRules(const ppp::string& rules, bool load_file_or_string) noexcept;
                bool                                                                LoadGeoRules(const ppp::string& rules_path, const ppp::string& geosite_path, const ppp::string& geoip_path) noexcept;
                bool                                                                SetOutboundConfigurations(const OutboundConfigurationList& configurations) noexcept;
                OutboundStatusList                                                  GetOutboundStatuses() noexcept;
                ppp::string                                                         GetActiveOutbound() noexcept;
                ppp::string                                                         GetActiveOutboundSourcePath() noexcept;
                bool                                                                SwitchOutbound(const ppp::string& tag) noexcept;
                bool                                                                SwitchPrimaryOutbound(const ppp::string& tag) noexcept;
                bool                                                                StaticMode(bool* static_mode) noexcept;
                uint16_t                                                            Mux(uint16_t* mux) noexcept;
                uint8_t                                                             MuxAcceleration(uint8_t* mux_acceleration) noexcept;
                virtual std::size_t                                                 GetIPListCount() noexcept;
                virtual std::size_t                                                 GetIPList6Count() noexcept;

#if defined(_ANDROID) || defined(_IPHONE)   
                void                                                                SetBypassIpList(ppp::string&& bypass_ip_list) noexcept;
                std::shared_ptr<NetworkInterface>                                   GetTapNetworkInterface()        noexcept { return tun_ni_; }
                std::shared_ptr<NetworkInterface>                                   GetUnderlyingNetworkInterface() noexcept { return underlying_ni_; }
                virtual ppp::string                                                 GetRemoteUri() noexcept;
#else   
#if defined(_LINUX)
                bool                                                                ProtectMode(bool* protect_mode) noexcept;
#endif
                std::shared_ptr<NetworkInterface>                                   GetTapNetworkInterface()        noexcept { return tun_ni_; }
                std::shared_ptr<NetworkInterface>                                   GetUnderlyingNetworkInterface() noexcept { return underlying_ni_; }
                virtual void                                                        PreferredNgw(const boost::asio::ip::address& gw) noexcept;
                virtual void                                                        PreferredNgw6(const boost::asio::ip::address& gw6) noexcept;
                virtual void                                                        PreferredNic(const ppp::string& nic) noexcept;
                virtual bool                                                        AddLoadIPList(
                    const ppp::string&                                              path, 
#if defined(_LINUX) 
                    const ppp::string&                                              nic,
#endif  
                    const boost::asio::ip::address&                                 gw,
                    const ppp::string&                                              url) noexcept;
                virtual bool                                                        AddLoadIPList6(
                    const ppp::string&                                              path, 
#if defined(_LINUX) 
                    const ppp::string&                                              nic,
#endif  
                    const boost::asio::ip::address&                                 gw6,
                    const ppp::string&                                              url) noexcept;
                virtual ppp::string                                                 GetRemoteUri() noexcept;
#endif  
            public: 
                virtual bool                                                        Open(const std::shared_ptr<ITap>& tap) noexcept override;
                virtual void                                                        Dispose() noexcept override;
                virtual bool                                                        OpenLogger() noexcept;
                virtual std::shared_ptr<ppp::threading::BufferswapAllocator>        GetBufferAllocator() noexcept override;
                virtual bool                                                        BlockQUIC(bool value) noexcept;
                bool                                                                ProxyOnly(bool* value = NULLPTR) noexcept;
                bool                                                                IsProxyOnly() const noexcept { return proxy_only_; }
#if defined(_WIN32)
                // Keep the VPN's own IPv4 transport on the physical interface
                // before the main outbound has established network takeover.
                bool                                                                EnsureWindowsIPv4ServerRoute(const boost::asio::ip::address& address) noexcept;
                // Keep the VPN's own IPv6 transport reachable after the physical
                // interface default route has been suppressed for leak prevention.
                bool                                                                EnsureWindowsIPv6ServerRoute(const boost::asio::ip::address& address) noexcept;
#endif

            protected:  
                virtual bool                                                        OnPacketInput(ppp::net::native::ip_hdr* packet, int packet_length, int header_length, int proto, bool vnet) noexcept override;
                virtual bool                                                        OnPacketInput(const std::shared_ptr<IPFrame>& packet) noexcept override;
                virtual bool                                                        OnIPv6PacketInput(Byte* packet, int packet_length) noexcept override;
                virtual bool                                                        OnIPv6UdpPacketInput(Byte* packet, int packet_length, ppp::ipv6::PacketHeader* ipv6_header) noexcept;
                virtual bool                                                        OnIPv6IcmpPacketInput(Byte* packet, int packet_length, ppp::ipv6::PacketHeader* ipv6_header) noexcept;
                virtual bool                                                        OnTick(uint64_t now) noexcept override;
                virtual bool                                                        OnUpdate(uint64_t now) noexcept override;
                virtual bool                                                        OnInformation(const std::shared_ptr<VirtualEthernetInformation>& information) noexcept;
                virtual void                                                        ApplyIPv6Assignment(const VirtualEthernetInformationExtensions& extensions) noexcept;
                bool                                                                StripAAAADnsResponseIfIPv4Available(::dns::Message& m) noexcept;
                void                                                                FlushPendingAAAAResponses() noexcept;
                void                                                                FlushExpiredPendingAAAAResponses() noexcept;

            protected:  
                virtual std::shared_ptr<VEthernetExchanger>                         NewExchanger() noexcept;
                virtual std::shared_ptr<VEthernetExchanger>                         NewExchanger(
                    const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration,
                    const ppp::string& tag, bool primary) noexcept;
                std::shared_ptr<VEthernetExchanger>                                 EnsureOutbound(const ppp::string& tag) noexcept;
                std::shared_ptr<ppp::configurations::AppConfiguration>              ReloadOutboundConfiguration(
                                                                                        OutboundConfiguration& outbound) noexcept;
                bool                                                                IsRouteOutbound(const ppp::string& tag) const noexcept;
                void                                                                CompletePendingOutboundSwitch(uint64_t now) noexcept;
                virtual std::shared_ptr<ppp::ethernet::VNetstack>                   NewNetstack() noexcept override;
                virtual VEthernetHttpProxySwitcherPtr                               NewHttpProxy(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept;
                virtual VEthernetSocksProxySwitcherPtr                              NewSocksProxy(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept;
                virtual std::shared_ptr<ppp::transmissions::ITransmissionQoS>       NewQoS() noexcept;
                virtual ITransmissionStatisticsPtr                                  NewStatistics() noexcept;
#if defined(_WIN32) 
                virtual PaperAirplaneControllerPtr                                  NewPaperAirplaneController() noexcept;
#elif defined(_LINUX)   
                virtual ProtectorNetworkPtr                                         NewProtectorNetwork() noexcept;
#endif  
                virtual bool                                                        DatagramOutput(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, void* packet, int packet_size, bool caching = true) noexcept;

            protected:  
#if !defined(_ANDROID) && !defined(_IPHONE)     
                virtual void                                                        AddRoute() noexcept;
                virtual void                                                        DeleteRoute() noexcept;
#endif  
                virtual bool                                                        OnUdpPacketInput(const std::shared_ptr<IPFrame>& packet) noexcept;
                virtual bool                                                        OnIcmpPacketInput(const std::shared_ptr<IPFrame>& packet) noexcept;

            private:    
#if !defined(_ANDROID) && !defined(_IPHONE) 
                bool                                                                FixUnderlyingNgw() noexcept;
                bool                                                                DeleteAllDefaultRoute() noexcept;
#else   
                bool                                                                AddAllRoute(const std::shared_ptr<ITap>& tap) noexcept;
#endif  

            private:
                bool                                                                RedirectDnsServer(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<UdpFrame>& frame, const std::shared_ptr<ppp::net::packet::BufferSegment>& messages) noexcept;
                bool                                                                RedirectDnsServer(
                    ppp::coroutines::YieldContext&                                  y,
                    const std::shared_ptr<boost::asio::ip::udp::socket>&            socket,
                    const std::shared_ptr<Byte>&                                    buffer,
                    const boost::asio::ip::address&                                 serverIP,
                    const std::shared_ptr<UdpFrame>&                                frame,
                    const std::shared_ptr<ppp::net::packet::BufferSegment>&         messages,
                    const std::shared_ptr<boost::asio::io_context>&                 context,
                    const boost::asio::ip::address&                                 destinationIP) noexcept;
                bool                                                                EmplaceTimeout(void* k, const std::shared_ptr<ppp::threading::Timer::TimeoutEventHandler>& timeout) noexcept;
                bool                                                                DeleteTimeout(void* k) noexcept;

            private:
                void                                                                ReleaseAllObjects() noexcept;
                void                                                                ReleaseAllPackets() noexcept;
                void                                                                ReleaseAllTimeouts() noexcept;
#if !defined(_ANDROID) && !defined(_IPHONE)
                void                                                                UpdateNetworkTakeover(uint64_t now) noexcept;
                void                                                                QueueNetworkTakeover(bool activate) noexcept;
                bool                                                                ApplyNetworkTakeover() noexcept;
                void                                                                RestoreNetworkTakeover(bool restore_ipv6) noexcept;
                void                                                                RestoreNetworkState() noexcept;
                void                                                                RestoreIPv6Assignment() noexcept;
#endif

            private:    
#if !defined(_ANDROID) && !defined(_IPHONE)     
#if defined(_WIN32) 
                bool                                                                UsePaperAirplaneController() noexcept;
                bool                                                                StartLocalDnsProxy() noexcept;
                void                                                                StopLocalDnsProxy() noexcept;
                void                                                                ReceiveLocalDnsUdp(const std::shared_ptr<boost::asio::ip::udp::socket>& socket) noexcept;
                void                                                                AcceptLocalDnsTcp(const std::shared_ptr<boost::asio::ip::tcp::acceptor>& acceptor) noexcept;
                void                                                                ReadLocalDnsTcp(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept;
                void                                                                DispatchLocalDnsQuery(const std::shared_ptr<ppp::string>& query, bool tcp,
                    const ppp::function<void(const std::shared_ptr<ppp::string>&)>& callback) noexcept;
                ppp::vector<boost::asio::ip::address>                              SelectLocalDnsServers(const void* packet, int packet_size) noexcept;
                struct LocalDnsUpstream;
                bool                                                                RegisterTunnelDnsHandler(const std::shared_ptr<LocalDnsUpstream>& upstream) noexcept;
                bool                                                                SendLocalDnsUdp(const boost::asio::ip::address& server,
                    const std::shared_ptr<ppp::string>& query,
                    const ppp::function<void(const std::shared_ptr<ppp::string>&)>& callback,
                    bool through_tunnel, ppp::string& upstream_key, uint16_t& upstream_id) noexcept;
                void                                                                ReceiveLocalDnsUpstream(const std::shared_ptr<LocalDnsUpstream>& upstream) noexcept;
                void                                                                CancelLocalDnsUdp(const ppp::vector<std::pair<ppp::string, uint16_t>>& requests) noexcept;
                void                                                                RebindTunnelDnsUpstreams() noexcept;
#endif  
                void                                                                AddRouteWithDnsServers() noexcept;
                void                                                                DeleteRouteWithDnsServers() noexcept;
                bool                                                                AddRoute(uint32_t ip, uint32_t gw, int prefix) noexcept;
#if defined(_WIN32) 
                bool                                                                DeleteRoute(const std::shared_ptr<MIB_IPFORWARDTABLE>& mib, uint32_t ip, uint32_t gw, int prefix) noexcept;
#else   
                bool                                                                DeleteRoute(uint32_t ip, uint32_t gw, int prefix) noexcept;
#endif  
                bool                                                                ProtectDefaultRoute() noexcept;
                bool                                                                LoadAllIPListWithFilePaths(const boost::asio::ip::address& gw) noexcept;
                bool                                                                LoadAllIPListWithFilePaths6(const boost::asio::ip::address& gw6) noexcept;
                void                                                                AddIPv6Route() noexcept;
#if defined(_WIN32)
                bool                                                                ApplyWindowsIPv6LeakBlockRoutes() noexcept;
                void                                                                RemoveWindowsIPv6LeakBlock() noexcept;
                void                                                                RemoveWindowsIPv4ServerRoutes() noexcept;
                void                                                                RemoveWindowsIPv6ServerRoutes() noexcept;
                int                                                                 DeleteWindowsIPv6BypassRoutes() noexcept;
#endif
#endif
                bool                                                                ApplyGeoStaticRoutes() noexcept;
                bool                                                                RefreshDirectDnsServers() noexcept;
                bool                                                                SelectDirectDnsServer(const ppp::string& host, boost::asio::ip::address& server) noexcept;
                void                                                                ObserveGeoDnsResponse(const void* packet, int packet_size) noexcept;
                void                                                                AddGeoDynamicRoute(const ppp::app::client::geo::GeoRuleEngine::RouteUpdate& update) noexcept;
                void                                                                DeleteGeoDynamicRoute(const boost::asio::ip::address& address) noexcept;
                void                                                                Finalize() noexcept;
#if defined(PPP_LOG_VERBOSE)
                void                                                                StopDebugWatchdog() noexcept;
#endif
                bool                                                                AddRemoteEndPointToIPList(const boost::asio::ip::address& gw) noexcept;
                bool                                                                UpdateRemoteUri() noexcept;
                
            private:    
                bool                                                                ER(const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<IcmpFrame>& frame, int ttl, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept;
                bool                                                                TE(const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<IcmpFrame>& frame, UInt32 source, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept;
                bool                                                                ERORTE(int ack_id) noexcept;
                
            private:
                bool                                                                PreparedAggregator() noexcept;
                bool                                                                IPAddressIsGatewayServer(UInt32 ip, UInt32 gw, UInt32 mask) noexcept { return ip == gw ? true : htonl((ntohl(gw) & ntohl(mask)) + 1) == ip; }
                bool                                                                EchoOtherServer(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept;
                bool                                                                EchoGatewayServer(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<IPFrame>& packet, const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept;
                const ppp::net::native::RouteEntry*                                 FindAppliedPeerPrefixRoute(uint32_t destination) noexcept;
                bool                                                                IsLocalAnnouncedPeerPrefix(uint32_t source) noexcept;

            private:    
                VirtualEthernetLoggerPtr                                            logger_;
                std::shared_ptr<VEthernetExchanger>                                 exchanger_;
                OutboundConfigurationList                                           outbound_configurations_;
                OutboundExchangerTable                                              outbound_exchangers_;
                ppp::string                                                         final_outbound_ = "main";
                ppp::string                                                         active_outbound_ = "main";
                ppp::string                                                         primary_outbound_ = "main";
                ppp::string                                                         pending_outbound_;
                uint64_t                                                            pending_outbound_deadline_ = 0;
                std::shared_ptr<VEthernetExchanger>                                 pending_outbound_exchanger_;
                bool                                                                pending_primary_switch_ = false;
                struct OutboundAffinity final {
                    ppp::string                                                     tag;
                    uint64_t                                                        expires_at = 0;
                };
                ppp::unordered_map<ppp::string, OutboundAffinity>                  outbound_affinities_;
                std::shared_ptr<ppp::configurations::AppConfiguration>              configuration_;
                std::shared_ptr<ppp::transmissions::ITransmissionQoS>               qos_;
                std::shared_ptr<ppp::transmissions::ITransmissionStatistics>        statistics_;
#if defined(PPP_LOG_VERBOSE)
                uint64_t                                                            debug_diagnostics_next_ = 0;
                std::atomic<uint64_t>                                               debug_last_tick_ = 0;
                std::atomic<bool>                                                   debug_watchdog_stop_ = false;
                std::thread                                                         debug_watchdog_;
#endif
                VEthernetIcmpPacketTable                                            icmppackets_;
                struct {
                    int                                                             icmppackets_aid_  = 0;
                    bool                                                            block_quic_       = false;
                    bool                                                            static_mode_      = false;
                    bool                                                            proxy_only_       = false;
                    uint16_t                                                        mux_              = 0;
                    uint8_t                                                         mux_acceleration_ = 0;
                };
                VEthernetHttpProxySwitcherPtr                                       http_proxy_;
                VEthernetSocksProxySwitcherPtr                                      socks_proxy_;
                TimeoutEventHandlerTable                                            timeouts_;
                DNSRuleTable                                                        dns_ruless_[3];
                std::shared_ptr<ppp::app::client::geo::GeoRuleEngine>               geo_rules_;
                ppp::vector<boost::asio::ip::address>                               direct_dns_servers_;
                std::atomic<size_t>                                                  direct_dns_server_index_ = 0;

                // Prefer IPv4: pending AAAA responses awaiting A-cache population or timeout.
                // When an AAAA DNS response arrives before the corresponding A response,
                // we hold it here with an expire_time. Once the A response is cached,
                // FlushPendingAAAAResponses() strips and forwards. If the expire_time
                // elapses (pure IPv6 site where no A will ever arrive), the AAAA is
                // forwarded as-is.
                struct PendingAAAAResponse {
                    ppp::string                                                     EncodedPacket;
                    uint64_t                                                        expire_time = 0; ///< Tick count when this entry expires (0 = not set)
                    // IPv6 path fields:
                    bool                                                            IsIPv6 = false;
                    boost::asio::ip::address_v6                                     SrcV6;
                    boost::asio::ip::address_v6                                     DstV6;
                    uint16_t                                                        SrcPort = 0;
                    uint16_t                                                        DstPort = 0;
                    // IPv4 path fields:
                    boost::asio::ip::udp::endpoint                                  SourceEP;
                    boost::asio::ip::udp::endpoint                                  DestinationEP;
                };
                std::unordered_map<ppp::string, std::shared_ptr<PendingAAAAResponse>> pending_aaaa_;

                RouteInformationTablePtr                                            rib_;
                ForwardInformationTablePtr                                          fib_;
                // Peer-prefix site-to-site routing state. applied_peer_prefix_routes_
                // mirrors the host routes installed through PeerPrefixRouteManager so
                // OnPacketInput can longest-prefix-match outbound packets that target
                // remote site prefixes outside the local TAP subnet.
                ppp::vector<ppp::net::native::RouteEntry>                          applied_peer_prefix_routes_;
                ppp::vector<ppp::app::protocol::PeerPrefixRouteEntry>              dynamic_peer_routes_;
                std::unique_ptr<PeerPrefixRouteManager>                            peer_prefix_routes_;
                ppp::string                                                         server_ru_;
                std::shared_ptr<aggligator::aggligator>                             aggligator_;
                IForwardingPtr                                                      forwarding_;
                
#if !defined(_ANDROID) && !defined(_IPHONE)
                SynchronizedObject                                                  prdr_;
#if defined(_LINUX)
                bool                                                                protect_mode_  = false;
                ppp::unordered_map<uint32_t, ppp::string>                           nics_;
                ppp::unordered_map<ppp::string, ppp::string>                        nics6_;
#endif
#endif

#if defined(_LINUX)
                ProtectorNetworkPtr                                                 protect_network_;
#endif

#if defined(_ANDROID) || defined(_IPHONE)   
                ppp::string                                                         bypass_ip_list_;
#endif
                std::atomic<bool>                                                   route_added_   = false;
                std::atomic<bool>                                                   network_takeover_worker_ = false;
                std::atomic<bool>                                                   network_takeover_stopping_ = false;
                std::atomic<bool>                                                   route_protector_running_ = false;
                std::atomic<uint64_t>                                               main_outbound_unavailable_since_ = 0;
                LoadIPListFileVectorPtr                                             ribs_;
                LoadIPv6ListFileVectorPtr                                           ribs6_;
                IPv6RouteTablePtr                                                   rib6_;
                ForwardInformationTable6Ptr                                         fib6_;

                std::shared_ptr<NetworkInterface>                                   tun_ni_;
                std::shared_ptr<NetworkInterface>                                   underlying_ni_;
                ppp::string                                                         preferred_nic_;
                boost::asio::ip::address                                            preferred_ngw_;
                boost::asio::ip::address                                            preferred_ngw6_;
                ppp::unordered_set<uint32_t>                                        dns_serverss_[3];
                ppp::unordered_map<uint32_t, uint32_t>                              geo_dynamic_routes_;
                struct GeoDynamicRoute6 final {
                    ppp::string                                                     gateway;
                    ppp::string                                                     interface_name;
                    int                                                             interface_index = -1;
                };
                ppp::unordered_map<ppp::string, GeoDynamicRoute6>                  geo_dynamic_routes6_;
#if defined(_WIN32)
                ppp::unordered_map<ppp::string, GeoDynamicRoute6>                  direct_dns_routes6_;
#endif
                ppp::ipv6::auxiliary::ClientState                                  ipv6_client_state_;
                boost::asio::ip::address                                            ipv6_client_address_;
                boost::asio::ip::address                                            ipv6_client_gateway_;
                boost::asio::ip::address                                            ipv6_client_route_prefix_;
                int                                                                 ipv6_client_prefix_length_ = 0;
                int                                                                 ipv6_client_route_prefix_length_ = 0;
                bool                                                                ipv6_client_nat_mode_ = false;
                bool                                                                ipv6_client_state_captured_ = false;
                
#if defined(_WIN32)
                PaperAirplaneControllerPtr                                          paper_airplane_ctrl_;
                ppp::vector<MIB_IPFORWARDROW>                                       default_routes_;
                bool                                                                ipv6_block_routes_added_ = false;
                bool                                                                ipv6_block_prefix_policy_applied_ = false;
                bool                                                                ipv6_physical_default_block_applied_ = false;
                bool                                                                ipv6_ignore_default_routes_captured_ = false;
                bool                                                                ipv6_original_ignore_default_routes_ = false;
                ppp::vector<MIB_IPFORWARD_ROW2>                                     ipv6_physical_default_routes_;
                struct IPv4ServerRoute final {
                    uint32_t                                                        address = 0;
                    uint32_t                                                        gateway = 0;
                    int                                                             interface_index = -1;
                    bool                                                            owned = false;
                };
                ppp::vector<IPv4ServerRoute>                                        ipv4_server_routes_;
                struct IPv6ServerRoute final {
                    boost::asio::ip::address                                        address;
                    boost::asio::ip::address                                        gateway;
                    int                                                             interface_index = -1;
                    bool                                                            owned = false;
                };
                ppp::vector<IPv6ServerRoute>                                        ipv6_server_routes_;
                AllNicDnsServerAddresses                                            ni_dns_servers_;
                ppp::unordered_map<int, ppp::vector<ppp::string>>                   ni_dns_servers_v6_;
                std::shared_ptr<ppp::threading::Timer>                              dns_guard_timer_;
                std::atomic<bool>                                                   dns_guard_active_ = false;
                std::atomic<int>                                                    dns_guard_workers_ = 0;
                bool                                                                local_dns_enabled_ = true;
                std::shared_ptr<boost::asio::ip::udp::socket>                      local_dns_udp4_;
                std::shared_ptr<boost::asio::ip::udp::socket>                      local_dns_udp6_;
                std::shared_ptr<boost::asio::ip::tcp::acceptor>                    local_dns_tcp4_;
                std::shared_ptr<boost::asio::ip::tcp::acceptor>                    local_dns_tcp6_;
                struct LocalDnsWaiter final {
                    uint16_t                                                        transaction_id = 0;
                    ppp::function<void(const std::shared_ptr<ppp::string>&)>        callback;
                };
                ppp::unordered_map<ppp::string, ppp::vector<LocalDnsWaiter>>       local_dns_pending_;
                struct LocalDnsUpstream final {
                    boost::asio::ip::address                                        server;
                    std::shared_ptr<boost::asio::ip::udp::socket>                  socket;
                    std::shared_ptr<VEthernetExchanger>                            exchanger;
                    boost::asio::ip::udp::endpoint                                 tunnel_source;
                    uint16_t                                                        next_id = 0;
                    bool                                                            receiving = false;
                    bool                                                            through_tunnel = false;
                    ppp::unordered_map<uint16_t,
                        ppp::function<void(const std::shared_ptr<ppp::string>&)>>   requests;
                };
                ppp::unordered_map<ppp::string, std::shared_ptr<LocalDnsUpstream>> local_dns_upstreams_;
                uint16_t                                                            local_dns_tunnel_port_ = 19999;
#elif defined(_LINUX)
                ppp::string                                                         ni_dns_servers_;
                RouteInformationTablePtr                                            default_routes_;
#endif
            };
        }
    }
}
