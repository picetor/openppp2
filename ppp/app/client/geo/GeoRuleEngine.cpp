#include <ppp/app/client/geo/GeoRuleEngine.h>

#include <ppp/io/File.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>

#include <common/dnslib/message.h>
#include <common/dnslib/rr.h>

#include <fstream>

namespace ppp {
    namespace app {
        namespace client {
            namespace geo {
                namespace {
                    struct ProtoSlice final {
                        const uint8_t* current = NULLPTR;
                        const uint8_t* end = NULLPTR;

                        bool ReadVarint(uint64_t& value) noexcept {
                            value = 0;
                            int shift = 0;
                            while (current < end && shift < 64) {
                                uint8_t byte = *current++;
                                value |= static_cast<uint64_t>(byte & 0x7f) << shift;
                                if ((byte & 0x80) == 0) {
                                    return true;
                                }
                                shift += 7;
                            }
                            return false;
                        }

                        bool ReadBytes(ProtoSlice& value) noexcept {
                            uint64_t length = 0;
                            if (!ReadVarint(length) || length > static_cast<uint64_t>(end - current)) {
                                return false;
                            }
                            value.current = current;
                            value.end = current + static_cast<size_t>(length);
                            current = value.end;
                            return true;
                        }

                        bool Skip(int wire_type) noexcept {
                            uint64_t ignored = 0;
                            ProtoSlice bytes;
                            switch (wire_type) {
                            case 0:
                                return ReadVarint(ignored);
                            case 1:
                                if (end - current < 8) return false;
                                current += 8;
                                return true;
                            case 2:
                                return ReadBytes(bytes);
                            case 5:
                                if (end - current < 4) return false;
                                current += 4;
                                return true;
                            default:
                                return false;
                            }
                        }
                    };

                    struct SiteDomain final {
                        int type = 0;
                        ppp::string value;
                        ppp::unordered_set<ppp::string> attributes;
                    };

                    typedef ppp::unordered_map<ppp::string, ppp::vector<SiteDomain>/**/> SiteTable;
                    typedef ppp::unordered_map<ppp::string, ppp::vector<GeoRuleEngine::Network>/**/> IpTable;

                    static ppp::string LowerTrim(const ppp::string& value) noexcept {
                        return ToLower<ppp::string>(ATrim<ppp::string>(value));
                    }

                    static bool ReadBinaryFile(const ppp::string& path, ppp::vector<uint8_t>& bytes) noexcept {
                        bytes.clear();
                        if (path.empty()) {
                            return false;
                        }

                        std::ifstream stream(path.data(), std::ios::binary | std::ios::ate);
                        if (!stream.good()) {
                            return false;
                        }

                        std::streamoff length = stream.tellg();
                        if (length <= 0 || static_cast<uint64_t>(length) > static_cast<uint64_t>(SIZE_MAX)) {
                            return false;
                        }

                        try {
                            bytes.resize(static_cast<size_t>(length));
                        }
                        catch (const std::bad_alloc&) {
                            return false;
                        }

                        stream.seekg(0, std::ios::beg);
                        stream.read(reinterpret_cast<char*>(bytes.data()), length);
                        return stream.good() || stream.eof();
                    }

                    static ppp::string SliceString(const ProtoSlice& value) {
                        return ppp::string(reinterpret_cast<const char*>(value.current),
                            static_cast<size_t>(value.end - value.current));
                    }

                    static bool ParseAttribute(ProtoSlice slice, ppp::string& key, bool& enabled) noexcept {
                        key.clear();
                        enabled = true;
                        while (slice.current < slice.end) {
                            uint64_t tag = 0;
                            if (!slice.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 2) {
                                ProtoSlice text;
                                if (!slice.ReadBytes(text)) return false;
                                key = LowerTrim(SliceString(text));
                            }
                            else if (field == 2 && wire == 0) {
                                uint64_t value = 0;
                                if (!slice.ReadVarint(value)) return false;
                                enabled = value != 0;
                            }
                            else if (!slice.Skip(wire)) {
                                return false;
                            }
                        }
                        return !key.empty();
                    }

                    static bool ParseSiteDomain(ProtoSlice slice, SiteDomain& domain) noexcept {
                        while (slice.current < slice.end) {
                            uint64_t tag = 0;
                            if (!slice.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 0) {
                                uint64_t type = 0;
                                if (!slice.ReadVarint(type)) return false;
                                domain.type = static_cast<int>(type);
                            }
                            else if (field == 2 && wire == 2) {
                                ProtoSlice text;
                                if (!slice.ReadBytes(text)) return false;
                                domain.value = LowerTrim(SliceString(text));
                            }
                            else if (field == 3 && wire == 2) {
                                ProtoSlice attribute;
                                if (!slice.ReadBytes(attribute)) return false;
                                ppp::string key;
                                bool enabled = true;
                                if (ParseAttribute(attribute, key, enabled) && enabled) {
                                    domain.attributes.emplace(std::move(key));
                                }
                            }
                            else if (!slice.Skip(wire)) {
                                return false;
                            }
                        }
                        return !domain.value.empty();
                    }

                    static bool ParseSiteEntry(ProtoSlice slice, const ppp::unordered_set<ppp::string>& wanted,
                        SiteTable& sites) noexcept {
                        ppp::string category;
                        ProtoSlice scan = slice;
                        while (scan.current < scan.end) {
                            uint64_t tag = 0;
                            if (!scan.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 2) {
                                ProtoSlice text;
                                if (!scan.ReadBytes(text)) return false;
                                category = LowerTrim(SliceString(text));
                                break;
                            }
                            if (!scan.Skip(wire)) return false;
                        }

                        if (category.empty() || wanted.find(category) == wanted.end()) {
                            return true;
                        }

                        ppp::vector<SiteDomain>& domains = sites[category];
                        while (slice.current < slice.end) {
                            uint64_t tag = 0;
                            if (!slice.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 2 && wire == 2) {
                                ProtoSlice encoded;
                                if (!slice.ReadBytes(encoded)) return false;
                                SiteDomain domain;
                                if (ParseSiteDomain(encoded, domain)) {
                                    domains.emplace_back(std::move(domain));
                                }
                            }
                            else if (!slice.Skip(wire)) {
                                return false;
                            }
                        }
                        return true;
                    }

                    static bool LoadSiteData(const ppp::string& path,
                        const ppp::unordered_set<ppp::string>& wanted, SiteTable& sites) noexcept {
                        ppp::vector<uint8_t> bytes;
                        if (!ReadBinaryFile(path, bytes)) return false;
                        ProtoSlice root{ bytes.data(), bytes.data() + bytes.size() };
                        while (root.current < root.end) {
                            uint64_t tag = 0;
                            if (!root.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 2) {
                                ProtoSlice entry;
                                if (!root.ReadBytes(entry) || !ParseSiteEntry(entry, wanted, sites)) return false;
                            }
                            else if (!root.Skip(wire)) {
                                return false;
                            }
                        }
                        return true;
                    }

                    static void NormalizeNetwork(boost::asio::ip::address& address, int prefix) noexcept {
                        if (address.is_v4()) {
                            uint32_t value = address.to_v4().to_uint();
                            uint32_t mask = prefix == 0 ? 0 : (0xffffffffu << (32 - prefix));
                            address = boost::asio::ip::address_v4(value & mask);
                        }
                        else if (address.is_v6()) {
                            auto bytes = address.to_v6().to_bytes();
                            for (int bit = prefix; bit < 128; bit++) {
                                bytes[bit >> 3] &= static_cast<uint8_t>(~(0x80u >> (bit & 7)));
                            }
                            address = boost::asio::ip::address_v6(bytes);
                        }
                    }

                    static bool ParseCidrMessage(ProtoSlice slice, GeoRuleEngine::Network& network) noexcept {
                        ppp::vector<uint8_t> ip;
                        uint64_t prefix = UINT64_MAX;
                        while (slice.current < slice.end) {
                            uint64_t tag = 0;
                            if (!slice.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 2) {
                                ProtoSlice bytes;
                                if (!slice.ReadBytes(bytes)) return false;
                                ip.assign(bytes.current, bytes.end);
                            }
                            else if (field == 2 && wire == 0) {
                                if (!slice.ReadVarint(prefix)) return false;
                            }
                            else if (!slice.Skip(wire)) {
                                return false;
                            }
                        }

                        if (ip.size() == 4 && prefix <= 32) {
                            boost::asio::ip::address_v4::bytes_type bytes;
                            memcpy(bytes.data(), ip.data(), bytes.size());
                            network.address = boost::asio::ip::address_v4(bytes);
                        }
                        else if (ip.size() == 16 && prefix <= 128) {
                            boost::asio::ip::address_v6::bytes_type bytes;
                            memcpy(bytes.data(), ip.data(), bytes.size());
                            network.address = boost::asio::ip::address_v6(bytes);
                        }
                        else {
                            return false;
                        }
                        network.prefix = static_cast<int>(prefix);
                        NormalizeNetwork(network.address, network.prefix);
                        return true;
                    }

                    static bool ParseIpEntry(ProtoSlice slice, const ppp::unordered_set<ppp::string>& wanted,
                        IpTable& ips) noexcept {
                        ppp::string category;
                        ProtoSlice scan = slice;
                        while (scan.current < scan.end) {
                            uint64_t tag = 0;
                            if (!scan.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 2) {
                                ProtoSlice text;
                                if (!scan.ReadBytes(text)) return false;
                                category = LowerTrim(SliceString(text));
                                break;
                            }
                            if (!scan.Skip(wire)) return false;
                        }

                        if (category.empty() || wanted.find(category) == wanted.end()) return true;
                        ppp::vector<GeoRuleEngine::Network>& networks = ips[category];
                        while (slice.current < slice.end) {
                            uint64_t tag = 0;
                            if (!slice.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 2 && wire == 2) {
                                ProtoSlice encoded;
                                if (!slice.ReadBytes(encoded)) return false;
                                GeoRuleEngine::Network network;
                                if (ParseCidrMessage(encoded, network)) {
                                    networks.emplace_back(std::move(network));
                                }
                            }
                            else if (!slice.Skip(wire)) {
                                return false;
                            }
                        }
                        return true;
                    }

                    static bool LoadIpData(const ppp::string& path,
                        const ppp::unordered_set<ppp::string>& wanted, IpTable& ips) noexcept {
                        ppp::vector<uint8_t> bytes;
                        if (!ReadBinaryFile(path, bytes)) return false;
                        ProtoSlice root{ bytes.data(), bytes.data() + bytes.size() };
                        while (root.current < root.end) {
                            uint64_t tag = 0;
                            if (!root.ReadVarint(tag)) return false;
                            int field = static_cast<int>(tag >> 3);
                            int wire = static_cast<int>(tag & 7);
                            if (field == 1 && wire == 2) {
                                ProtoSlice entry;
                                if (!root.ReadBytes(entry) || !ParseIpEntry(entry, wanted, ips)) return false;
                            }
                            else if (!root.Skip(wire)) {
                                return false;
                            }
                        }
                        return true;
                    }

                    static bool IsValidOutboundTag(const ppp::string& value) noexcept {
                        if (value.empty()) return false;
                        for (char ch : value) {
                            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
                                continue;
                            }
                            return false;
                        }
                        return true;
                    }

                    static GeoRuleEngine::Action ParseAction(const ppp::string& value,
                        const ppp::unordered_set<ppp::string>& outbound_tags,
                        ppp::string& outbound) noexcept {
                        ppp::string action = LowerTrim(value);
                        outbound.clear();
                        if (action == "direct") return GeoRuleEngine::Action::Direct;
                        if (action == "tunnel") {
                            outbound = "main";
                            return GeoRuleEngine::Action::Tunnel;
                        }
                        if (outbound_tags.find(action) != outbound_tags.end()) {
                            outbound = action;
                            return GeoRuleEngine::Action::Tunnel;
                        }
                        return GeoRuleEngine::Action::None;
                    }

                    static bool ParseCidrText(const ppp::string& text, GeoRuleEngine::Network& network) noexcept {
                        ppp::string value = ATrim<ppp::string>(text);
                        size_t slash = value.find('/');
                        ppp::string host = slash == ppp::string::npos ? value : value.substr(0, slash);
                        boost::system::error_code ec;
                        network.address = StringToAddress(host.data(), ec);
                        if (ec || ppp::net::IPEndPoint::IsInvalid(network.address)) return false;
                        int max_prefix = network.address.is_v4() ? 32 : 128;
                        network.prefix = slash == ppp::string::npos ? max_prefix : atoi(value.data() + slash + 1);
                        if (network.prefix < 0 || network.prefix > max_prefix) return false;
                        NormalizeNetwork(network.address, network.prefix);
                        return true;
                    }

                    // Convert file-order priority into routes that still behave correctly in an
                    // operating-system routing table, where the longest prefix normally wins.
                    // A later subnet fully covered by an earlier route can never win and is removed;
                    // an earlier narrow subnet plus a later broad fallback are both retained.
                    static void CompileFirstMatchRoutes(ppp::vector<GeoRuleEngine::Network>& networks) noexcept {
                        std::stable_sort(networks.begin(), networks.end(),
                            [](const GeoRuleEngine::Network& left, const GeoRuleEngine::Network& right) noexcept {
                                return left.priority < right.priority;
                            });
                        ppp::unordered_set<ppp::string> accepted_prefixes;
                        ppp::vector<GeoRuleEngine::Network> compiled;
                        compiled.reserve(networks.size());

                        for (const GeoRuleEngine::Network& network : networks) {
                            bool covered = false;
                            for (int prefix = 0; prefix <= network.prefix; prefix++) {
                                boost::asio::ip::address ancestor = network.address;
                                NormalizeNetwork(ancestor, prefix);
                                ppp::string key = (ancestor.is_v4() ? "4:" : "6:") +
                                    stl::to_string<ppp::string>(prefix) + ":" +
                                    ppp::net::Ipep::ToAddressString<ppp::string>(ancestor);
                                if (accepted_prefixes.find(key) != accepted_prefixes.end()) {
                                    covered = true;
                                    break;
                                }
                            }
                            if (covered) continue;

                            ppp::string key = (network.address.is_v4() ? "4:" : "6:") +
                                stl::to_string<ppp::string>(network.prefix) + ":" +
                                ppp::net::Ipep::ToAddressString<ppp::string>(network.address);
                            accepted_prefixes.emplace(std::move(key));
                            compiled.emplace_back(network);
                        }
                        networks.swap(compiled);
                    }
                }

                bool GeoRuleEngine::ParseOutboundConfigurations(const ppp::string& rules_path,
                    ppp::vector<OutboundConfiguration>& configurations,
                    ppp::string& final_outbound,
                    ppp::string& error) noexcept {
                    configurations.clear();
                    final_outbound = "main";
                    error.clear();

                    ppp::string text = ppp::io::File::ReadAllText(rules_path.data());
                    if (text.empty()) {
                        error = "geo rules file is empty or cannot be read: " + rules_path;
                        return false;
                    }
                    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
                        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) {
                        text.erase(0, 3);
                    }

                    ppp::unordered_set<ppp::string> tags;
                    bool has_final = false;
                    ppp::vector<ppp::string> lines;
                    Tokenize<ppp::string>(text, lines, "\r\n");
                    for (size_t line_number = 0; line_number < lines.size(); line_number++) {
                        ppp::string line = ATrim<ppp::string>(lines[line_number]);
                        size_t comment = line.find('#');
                        if (comment != ppp::string::npos) line = ATrim<ppp::string>(line.substr(0, comment));
                        if (line.empty() || line.find(',') != ppp::string::npos) continue;

                        size_t equals = line.find('=');
                        if (equals == ppp::string::npos) continue;
                        ppp::string tag = LowerTrim(line.substr(0, equals));
                        ppp::string value = ATrim<ppp::string>(line.substr(equals + 1));
                        if (tag == "direct_dns") continue;
                        if (tag == "final") {
                            if (has_final) {
                                error = "duplicate final outbound at line " + stl::to_string<ppp::string>(line_number + 1);
                                return false;
                            }
                            if (value.empty()) {
                                error = "empty final outbound at line " + stl::to_string<ppp::string>(line_number + 1);
                                return false;
                            }
                            has_final = true;
                            final_outbound = LowerTrim(value);
                            continue;
                        }
                        if (!IsValidOutboundTag(tag) || tag == "direct" || tag == "tunnel" || tag == "reject") {
                            error = "invalid or reserved outbound tag at line " + stl::to_string<ppp::string>(line_number + 1) + ": " + tag;
                            return false;
                        }
                        if (value.empty()) {
                            error = "empty outbound configuration path at line " + stl::to_string<ppp::string>(line_number + 1);
                            return false;
                        }
                        if (!tags.emplace(tag).second) {
                            error = "duplicate outbound tag at line " + stl::to_string<ppp::string>(line_number + 1) + ": " + tag;
                            return false;
                        }

                        ppp::string path = ppp::io::File::GetFullPath(ppp::io::File::RewritePath(value.data()).data());
                        if (path.empty() || !ppp::io::File::CanAccess(path.data(), ppp::io::FileAccess::Read)) {
                            error = "outbound configuration cannot be read at line " + stl::to_string<ppp::string>(line_number + 1) + ": " + value;
                            return false;
                        }
                        configurations.emplace_back(OutboundConfiguration{ tag, path, tag == "main" });
                    }

                    if (final_outbound == "tunnel") final_outbound = "main";
                    if (configurations.empty()) {
                        // Traditional geo mode has no outbound declarations.
                        if (has_final && final_outbound != "main") {
                            error = "final requires outbound declarations: " + final_outbound;
                            return false;
                        }
                        return true;
                    }
                    if (tags.find("main") == tags.end()) {
                        error = "multi-outbound geo rules must define main=<configuration.json>";
                        return false;
                    }
                    if (tags.find(final_outbound) == tags.end()) {
                        error = "final references an undefined outbound: " + final_outbound;
                        return false;
                    }
                    return true;
                }

                bool GeoRuleEngine::Cidr::Contains(const boost::asio::ip::address& value) const noexcept {
                    if (address.is_v4() != value.is_v4() || address.is_v6() != value.is_v6()) return false;
                    if (address.is_v4()) {
                        uint32_t left = address.to_v4().to_uint();
                        uint32_t right = value.to_v4().to_uint();
                        uint32_t mask = prefix == 0 ? 0 : (0xffffffffu << (32 - prefix));
                        return (left & mask) == (right & mask);
                    }

                    auto left = address.to_v6().to_bytes();
                    auto right = value.to_v6().to_bytes();
                    int bytes = prefix >> 3;
                    int bits = prefix & 7;
                    if (bytes > 0 && memcmp(left.data(), right.data(), static_cast<size_t>(bytes)) != 0) return false;
                    if (bits == 0) return true;
                    uint8_t mask = static_cast<uint8_t>(0xffu << (8 - bits));
                    return (left[bytes] & mask) == (right[bytes] & mask);
                }

                bool GeoRuleEngine::Load(const ppp::string& rules_path, const ppp::string& geosite_path,
                    const ppp::string& geoip_path, ppp::string& error) noexcept {
                    error.clear();
                    rules_.clear();
                    static_networks_.clear();
                    direct_dns_.clear();
                    outbound_configurations_.clear();
                    final_outbound_ = "main";
                    {
                        std::lock_guard<std::mutex> scope(syncobj_);
                        dynamic_policies_.clear();
                    }

                    if (!ParseOutboundConfigurations(rules_path, outbound_configurations_, final_outbound_, error)) {
                        return false;
                    }
                    ppp::unordered_set<ppp::string> outbound_tags;
                    for (const OutboundConfiguration& outbound : outbound_configurations_) {
                        outbound_tags.emplace(outbound.tag);
                    }

                    ppp::string text = ppp::io::File::ReadAllText(rules_path.data());
                    if (text.empty()) {
                        error = "geo rules file is empty or cannot be read: " + rules_path;
                        return false;
                    }
                    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
                        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) {
                        text.erase(0, 3);
                    }

                    ppp::unordered_set<ppp::string> wanted_sites;
                    ppp::unordered_set<ppp::string> wanted_ips;
                    ppp::vector<ppp::string> lines;
                    Tokenize<ppp::string>(text, lines, "\r\n");
                    for (size_t line_number = 0; line_number < lines.size(); line_number++) {
                        ppp::string line = ATrim<ppp::string>(lines[line_number]);
                        size_t comment = line.find('#');
                        if (comment != ppp::string::npos) line = ATrim<ppp::string>(line.substr(0, comment));
                        if (line.empty()) continue;

                        ppp::string lower = ToLower<ppp::string>(line);
                        static const ppp::string dns_prefix = "direct_dns=";
                        if (lower.compare(0, dns_prefix.size(), dns_prefix) == 0) {
                            ppp::vector<ppp::string> servers;
                            Tokenize<ppp::string>(line.substr(dns_prefix.size()), servers, ",");
                            for (const ppp::string& item : servers) {
                                boost::system::error_code ec;
                                boost::asio::ip::address address = StringToAddress(ATrim<ppp::string>(item).data(), ec);
                                if (ec || ppp::net::IPEndPoint::IsInvalid(address)) {
                                    error = "invalid direct_dns address at line " + stl::to_string<ppp::string>(line_number + 1);
                                    return false;
                                }
                                direct_dns_.emplace_back(address);
                            }
                            continue;
                        }

                        // Outbound declarations and the default route are parsed by
                        // ParseOutboundConfigurations before the match rules.
                        if (line.find(',') == ppp::string::npos && line.find('=') != ppp::string::npos) {
                            continue;
                        }

                        ppp::vector<ppp::string> fields;
                        Tokenize<ppp::string>(line, fields, ",");
                        if (fields.size() != 3) {
                            error = "geo rule must have exactly 3 fields at line " + stl::to_string<ppp::string>(line_number + 1);
                            return false;
                        }

                        Rule rule;
                        ppp::string type = LowerTrim(fields[0]);
                        rule.value = LowerTrim(fields[1]);
                        rule.action = ParseAction(fields[2], outbound_tags, rule.outbound);
                        rule.priority = rules_.size();
                        if (rule.value.empty() || rule.action == Action::None) {
                            error = "invalid geo rule value or action at line " + stl::to_string<ppp::string>(line_number + 1);
                            return false;
                        }

                        if (type == "geosite") {
                            rule.type = RuleType::Geosite;
                            ppp::vector<ppp::string> parts;
                            Tokenize<ppp::string>(rule.value, parts, "@");
                            rule.value = parts.empty() ? ppp::string() : parts[0];
                            for (size_t i = 1; i < parts.size(); i++) {
                                ppp::string attribute = LowerTrim(parts[i]);
                                if (!attribute.empty()) rule.attributes.emplace_back(std::move(attribute));
                            }
                            if (rule.value.empty()) {
                                error = "empty geosite category at line " + stl::to_string<ppp::string>(line_number + 1);
                                return false;
                            }
                            wanted_sites.emplace(rule.value);
                        }
                        else if (type == "geoip") {
                            rule.type = RuleType::Geoip;
                            wanted_ips.emplace(rule.value);
                        }
                        else if (type == "domain" || type == "full") {
                            rule.type = RuleType::Domain;
                        }
                        else if (type == "domain-suffix") {
                            rule.type = RuleType::DomainSuffix;
                        }
                        else if (type == "domain-keyword") {
                            rule.type = RuleType::DomainKeyword;
                        }
                        else if (type == "domain-regex" || type == "regexp") {
                            rule.type = RuleType::DomainRegex;
                            try {
                                rule.regex = make_shared_object<boost::regex>(rule.value.data(),
                                    boost::regex_constants::icase | boost::regex_constants::perl);
                            }
                            catch (const std::exception&) {
                                error = "invalid domain regex at line " + stl::to_string<ppp::string>(line_number + 1);
                                return false;
                            }
                        }
                        else if (type == "ip-cidr" || type == "ip-cidr6") {
                            rule.type = RuleType::IpCidr;
                            Network network;
                            if (!ParseCidrText(rule.value, network) ||
                                (type == "ip-cidr" && !network.address.is_v4()) ||
                                (type == "ip-cidr6" && !network.address.is_v6())) {
                                error = "invalid IP CIDR at line " + stl::to_string<ppp::string>(line_number + 1);
                                return false;
                            }
                            network.action = rule.action;
                            network.priority = rule.priority;
                            network.outbound = rule.outbound;
                            rule.cidrs.emplace_back(Cidr{ network.address, network.prefix });
                            static_networks_.emplace_back(std::move(network));
                        }
                        else {
                            error = "unsupported geo rule type at line " + stl::to_string<ppp::string>(line_number + 1) + ": " + type;
                            return false;
                        }
                        rules_.emplace_back(std::move(rule));
                    }

                    if (rules_.empty()) {
                        error = "geo rules file contains no routing rules";
                        return false;
                    }

                    SiteTable sites;
                    if (!wanted_sites.empty()) {
                        if (!LoadSiteData(geosite_path, wanted_sites, sites)) {
                            error = "cannot parse geosite data: " + geosite_path;
                            return false;
                        }
                    }

                    IpTable ips;
                    if (!wanted_ips.empty()) {
                        if (!LoadIpData(geoip_path, wanted_ips, ips)) {
                            error = "cannot parse geoip data: " + geoip_path;
                            return false;
                        }
                    }

                    for (Rule& rule : rules_) {
                        if (rule.type == RuleType::Geosite) {
                            auto category = sites.find(rule.value);
                            if (category == sites.end() || category->second.empty()) {
                                error = "geosite category not found: " + rule.value;
                                return false;
                            }

                            for (const SiteDomain& source : category->second) {
                                bool attributes_match = true;
                                for (const ppp::string& attribute : rule.attributes) {
                                    if (source.attributes.find(attribute) == source.attributes.end()) {
                                        attributes_match = false;
                                        break;
                                    }
                                }
                                if (!attributes_match) continue;

                                DomainPattern pattern;
                                pattern.type = source.type;
                                pattern.value = source.value;
                                if (pattern.type == 1) {
                                    try {
                                        pattern.regex = make_shared_object<boost::regex>(pattern.value.data(),
                                            boost::regex_constants::icase | boost::regex_constants::perl);
                                    }
                                    catch (const std::exception&) {
                                        continue;
                                    }
                                }
                                rule.domains.emplace_back(std::move(pattern));
                            }
                            if (rule.domains.empty()) {
                                error = "geosite category has no entries for requested attributes: " + rule.value;
                                return false;
                            }
                        }
                        else if (rule.type == RuleType::Geoip) {
                            auto category = ips.find(rule.value);
                            if (category == ips.end() || category->second.empty()) {
                                error = "geoip category not found: " + rule.value;
                                return false;
                            }
                            for (Network network : category->second) {
                                network.action = rule.action;
                                network.priority = rule.priority;
                                network.outbound = rule.outbound;
                                rule.cidrs.emplace_back(Cidr{ network.address, network.prefix });
                                static_networks_.emplace_back(std::move(network));
                            }
                        }
                    }

                    CompileFirstMatchRoutes(static_networks_);

                    LOG_INFO("GeoRuleEngine::Load: rules=%llu, networks=%llu, direct_dns=%llu, outbounds=%llu, final=%s",
                        (unsigned long long)rules_.size(), (unsigned long long)static_networks_.size(),
                        (unsigned long long)direct_dns_.size(), (unsigned long long)outbound_configurations_.size(),
                        final_outbound_.data());
                    return true;
                }

                bool GeoRuleEngine::DomainSuffixMatch(const ppp::string& host, const ppp::string& suffix) noexcept {
                    if (host == suffix) return true;
                    if (host.size() <= suffix.size()) return false;
                    size_t offset = host.size() - suffix.size();
                    return offset > 0 && host[offset - 1] == '.' && host.compare(offset, suffix.size(), suffix) == 0;
                }

                GeoRuleEngine::Decision GeoRuleEngine::MatchDomainRule(const Rule& rule, const ppp::string& host) const noexcept {
                    bool matched = false;
                    switch (rule.type) {
                    case RuleType::Domain:
                        matched = host == rule.value;
                        break;
                    case RuleType::DomainSuffix:
                        matched = DomainSuffixMatch(host, rule.value);
                        break;
                    case RuleType::DomainKeyword:
                        matched = host.find(rule.value) != ppp::string::npos;
                        break;
                    case RuleType::DomainRegex:
                        try { matched = rule.regex && boost::regex_search(host.data(), *rule.regex); }
                        catch (const std::exception&) { matched = false; }
                        break;
                    case RuleType::Geosite:
                        for (const DomainPattern& pattern : rule.domains) {
                            switch (pattern.type) {
                            case 0: matched = host.find(pattern.value) != ppp::string::npos; break;
                            case 1:
                                try { matched = pattern.regex && boost::regex_search(host.data(), *pattern.regex); }
                                catch (const std::exception&) { matched = false; }
                                break;
                            case 2: matched = DomainSuffixMatch(host, pattern.value); break;
                            case 3: matched = host == pattern.value; break;
                            default: break;
                            }
                            if (matched) break;
                        }
                        break;
                    default:
                        break;
                    }
                    return matched ? Decision{ rule.action, rule.priority, rule.outbound } : Decision{};
                }

                GeoRuleEngine::Decision GeoRuleEngine::MatchDomain(const ppp::string& host) const noexcept {
                    ppp::string normalized = LowerTrim(host);
                    while (!normalized.empty() && normalized.back() == '.') normalized.pop_back();
                    if (normalized.empty()) return Decision{};
                    for (const Rule& rule : rules_) {
                        Decision result = MatchDomainRule(rule, normalized);
                        if (result.Matched()) return result;
                    }
                    return Decision{};
                }

                GeoRuleEngine::Decision GeoRuleEngine::MatchStaticAddress(const boost::asio::ip::address& address) const noexcept {
                    for (const Rule& rule : rules_) {
                        if (rule.type != RuleType::Geoip && rule.type != RuleType::IpCidr) continue;
                        for (const Cidr& cidr : rule.cidrs) {
                            if (cidr.Contains(address)) return Decision{ rule.action, rule.priority, rule.outbound };
                        }
                    }
                    return Decision{};
                }

                ppp::string GeoRuleEngine::AddressKey(const boost::asio::ip::address& address) noexcept {
                    return ppp::net::Ipep::ToAddressString<ppp::string>(address);
                }

                GeoRuleEngine::Decision GeoRuleEngine::MatchAddress(const boost::asio::ip::address& address, uint64_t now) const noexcept {
                    Decision dynamic;
                    {
                        std::lock_guard<std::mutex> scope(syncobj_);
                        auto existing = dynamic_policies_.find(AddressKey(address));
                        if (existing != dynamic_policies_.end() && existing->second.expires_at > now) {
                            dynamic = Decision{ existing->second.action, existing->second.priority, existing->second.outbound };
                        }
                    }
                    Decision fixed = MatchStaticAddress(address);
                    if (!dynamic.Matched()) return fixed;
                    if (!fixed.Matched()) return dynamic;
                    return dynamic.priority <= fixed.priority ? dynamic : fixed;
                }

                GeoRuleEngine::Decision GeoRuleEngine::MergeDomainAndAddressDecision(const Decision& domain,
                    const boost::asio::ip::address& address) const noexcept {
                    Decision fixed = MatchStaticAddress(address);
                    if (!fixed.Matched()) return domain;
                    if (!domain.Matched()) return fixed;
                    return domain.priority <= fixed.priority ? domain : fixed;
                }

                bool GeoRuleEngine::SelectDirectDns(const ppp::string& host, boost::asio::ip::address& server) noexcept {
                    Decision decision = MatchDomain(host);
                    if (decision.action != Action::Direct || direct_dns_.empty()) return false;
                    size_t index = direct_dns_index_.fetch_add(1) % direct_dns_.size();
                    server = direct_dns_[index];
                    return true;
                }

                bool GeoRuleEngine::ObserveDnsResponse(const void* packet, int packet_size, uint64_t now,
                    ppp::vector<RouteUpdate>& updates) noexcept {
                    updates.clear();
                    if (!packet || packet_size < 1) return false;
                    ::dns::Message message;
                    if (message.decode(reinterpret_cast<uint8_t*>(const_cast<void*>(packet)), static_cast<size_t>(packet_size)) != ::dns::BufferResult::NoError ||
                        message.questions.empty()) return false;

                    ppp::string domain = LowerTrim(stl::transform<ppp::string>(message.questions[0].mName));
                    Decision domain_decision = MatchDomain(domain);
                    if (!domain_decision.Matched()) return false;

                    for (::dns::ResourceRecord& answer : message.answers) {
                        boost::asio::ip::address address;
                        if (answer.mType == ::dns::RecordType::kA) {
                            auto data = answer.getRData<::dns::RDataA>();
                            if (!data) continue;
                            ppp::net::IPEndPoint ep(ppp::net::AddressFamily::InterNetwork,
                                data->getAddress(), 4, ppp::net::IPEndPoint::MinPort);
                            address = ppp::net::IPEndPoint::ToEndPoint<boost::asio::ip::udp>(ep).address();
                        }
                        else if (answer.mType == ::dns::RecordType::kAAAA) {
                            auto data = answer.getRData<::dns::RDataAAAA>();
                            if (!data) continue;
                            ppp::net::IPEndPoint ep(ppp::net::AddressFamily::InterNetworkV6,
                                data->getAddress(), 16, ppp::net::IPEndPoint::MinPort);
                            address = ppp::net::IPEndPoint::ToEndPoint<boost::asio::ip::udp>(ep).address();
                        }
                        else {
                            continue;
                        }

                        Decision final_decision = MergeDomainAndAddressDecision(domain_decision, address);
                        uint64_t ttl_seconds = std::max<uint64_t>(1, std::min<uint64_t>(answer.mTtl, 86400));
                        DynamicPolicy policy{ final_decision.action, final_decision.priority,
                            now + ttl_seconds * 1000, domain, final_decision.outbound };
                        bool changed = false;
                        {
                            std::lock_guard<std::mutex> scope(syncobj_);
                            ppp::string key = AddressKey(address);
                            auto existing = dynamic_policies_.find(key);
                            if (existing == dynamic_policies_.end() || existing->second.expires_at <= now ||
                                policy.priority < existing->second.priority) {
                                dynamic_policies_[key] = policy;
                                changed = true;
                            }
                            else if (policy.priority == existing->second.priority) {
                                changed = existing->second.action != policy.action || existing->second.outbound != policy.outbound;
                                existing->second = policy;
                            }
                        }
                        if (changed) {
                            updates.emplace_back(RouteUpdate{ address, policy.action, policy.priority, policy.expires_at, policy.outbound });
                        }
                    }
                    return !updates.empty();
                }

                void GeoRuleEngine::Update(uint64_t now, ppp::vector<RouteUpdate>& expired) noexcept {
                    expired.clear();
                    std::lock_guard<std::mutex> scope(syncobj_);
                    for (auto iterator = dynamic_policies_.begin(); iterator != dynamic_policies_.end();) {
                        if (iterator->second.expires_at <= now) {
                            boost::system::error_code ec;
                            boost::asio::ip::address address = StringToAddress(iterator->first.data(), ec);
                            if (!ec) {
                                expired.emplace_back(RouteUpdate{ address, iterator->second.action,
                                    iterator->second.priority, iterator->second.expires_at, iterator->second.outbound });
                            }
                            iterator = dynamic_policies_.erase(iterator);
                        }
                        else ++iterator;
                    }
                }
            }
        }
    }
}
