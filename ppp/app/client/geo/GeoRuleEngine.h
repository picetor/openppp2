#pragma once

#include <ppp/stdafx.h>

#include <boost/asio/ip/address.hpp>
#include <boost/regex.hpp>

namespace ppp {
    namespace app {
        namespace client {
            namespace geo {
                class GeoRuleEngine final {
                public:
                    struct OutboundConfiguration final {
                        ppp::string tag;
                        ppp::string path;
                        bool primary = false;
                    };

                    enum class Action : uint8_t {
                        None = 0,
                        Direct,
                        Tunnel,
                    };

                    struct Decision final {
                        Action action = Action::None;
                        size_t priority = SIZE_MAX;
                        ppp::string outbound;

                        bool Matched() const noexcept { return action != Action::None; }
                    };

                    // Compact, display-safe representation of a configured
                    // rule.  The compiled geosite/geoip data can be very
                    // large, so the UI should expose the original matcher,
                    // action and outbound rather than every expanded entry.
                    struct RuleSummary final {
                        ppp::string type;
                        ppp::string value;
                        Action action = Action::None;
                        size_t priority = SIZE_MAX;
                        ppp::string outbound;
                    };

                    struct Network final {
                        boost::asio::ip::address address;
                        int prefix = 0;
                        Action action = Action::None;
                        size_t priority = SIZE_MAX;
                        ppp::string outbound;
                    };

                    struct RouteUpdate final {
                        boost::asio::ip::address address;
                        Action action = Action::None;
                        size_t priority = SIZE_MAX;
                        uint64_t expires_at = 0;
                        ppp::string outbound;
                    };

                public:
                    static bool ParseOutboundConfigurations(const ppp::string& rules_path,
                        ppp::vector<OutboundConfiguration>& configurations,
                        ppp::string& final_outbound,
                        ppp::string& error,
                        bool require_main = false) noexcept;

                    bool Load(const ppp::string& rules_path,
                        const ppp::string& geosite_path,
                        const ppp::string& geoip_path,
                        ppp::string& error) noexcept;

                    Decision MatchDomain(const ppp::string& host) const noexcept;
                    Decision MatchAddress(const boost::asio::ip::address& address, uint64_t now) const noexcept;
                    bool SelectDirectDns(const ppp::string& host, boost::asio::ip::address& server) noexcept;
                    bool ObserveDnsResponse(const void* packet, int packet_size, uint64_t now,
                        ppp::vector<RouteUpdate>& updates) noexcept;
                    void Update(uint64_t now, ppp::vector<RouteUpdate>& expired) noexcept;

                    const ppp::vector<boost::asio::ip::address>& GetDirectDnsServers() const noexcept { return direct_dns_; }
                    bool UsesLocalDirectDns() const noexcept { return direct_dns_local_; }
                    const ppp::vector<Network>& GetStaticNetworks() const noexcept { return static_networks_; }
                    const ppp::vector<OutboundConfiguration>& GetOutboundConfigurations() const noexcept { return outbound_configurations_; }
                    const ppp::vector<RuleSummary>& GetRuleSummaries() const noexcept { return rule_summaries_; }
                    const ppp::string& GetFinalOutbound() const noexcept { return final_outbound_; }
                    size_t GetRuleCount() const noexcept { return rules_.size(); }

                private:
                    enum class RuleType : uint8_t {
                        Geosite,
                        Geoip,
                        Domain,
                        DomainSuffix,
                        DomainKeyword,
                        DomainRegex,
                        IpCidr,
                    };

                    struct DomainPattern final {
                        int type = 0;
                        ppp::string value;
                        std::shared_ptr<boost::regex> regex;
                    };

                    struct Cidr final {
                        boost::asio::ip::address address;
                        int prefix = 0;

                        bool Contains(const boost::asio::ip::address& value) const noexcept;
                    };

                    struct Rule final {
                        RuleType type = RuleType::Domain;
                        Action action = Action::None;
                        size_t priority = SIZE_MAX;
                        ppp::string value;
                        ppp::vector<ppp::string> attributes;
                        ppp::vector<DomainPattern> domains;
                        ppp::vector<Cidr> cidrs;
                        std::shared_ptr<boost::regex> regex;
                        ppp::string outbound;
                    };

                    struct DynamicPolicy final {
                        Action action = Action::None;
                        size_t priority = SIZE_MAX;
                        uint64_t expires_at = 0;
                        ppp::string domain;
                        ppp::string outbound;
                    };

                private:
                    Decision MatchDomainRule(const Rule& rule, const ppp::string& host) const noexcept;
                    Decision MatchStaticAddress(const boost::asio::ip::address& address) const noexcept;
                    Decision MergeDomainAndAddressDecision(const Decision& domain,
                        const boost::asio::ip::address& address) const noexcept;
                    static bool DomainSuffixMatch(const ppp::string& host, const ppp::string& suffix) noexcept;
                    static ppp::string AddressKey(const boost::asio::ip::address& address) noexcept;

                private:
                    ppp::vector<Rule> rules_;
                    ppp::vector<Network> static_networks_;
                    ppp::vector<RuleSummary> rule_summaries_;
                    ppp::vector<boost::asio::ip::address> direct_dns_;
                    bool direct_dns_local_ = false;
                    ppp::vector<OutboundConfiguration> outbound_configurations_;
                    ppp::string final_outbound_ = "main";
                    mutable std::mutex syncobj_;
                    ppp::unordered_map<ppp::string, DynamicPolicy> dynamic_policies_;
                    std::atomic<size_t> direct_dns_index_ = 0;
                };
            }
        }
    }
}
