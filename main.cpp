#include <glibc_compat.h>
#include <ppp/configurations/AppConfiguration.h>
#include <ppp/Int128.h>
#include <ppp/io/File.h>
#include <ppp/tap/ITap.h>
#include <ppp/tap/TapStub.h>
#include <ppp/net/http/HttpClient.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/asio/vdns.h>
#include <ppp/auxiliary/StringAuxiliary.h>
#include <ppp/diagnostics/Stopwatch.h>
#include <ppp/diagnostics/PreventReturn.h>
#include <ppp/threading/Timer.h>
#include <ppp/threading/Thread.h>
#include <ppp/threading/Executors.h>
#include <ppp/threading/BufferswapAllocator.h>
#include <ppp/app/server/VirtualEthernetSwitcher.h>
#include <ppp/app/server/VirtualEthernetManagedServer.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>

// Platform-specific includes
#if defined(_WIN32)
#include <windows/ppp/net/proxies/HttpProxy.h>
#include <windows/ppp/tap/TapWindows.h>
#include <windows/ppp/win32/Win32Native.h>
#include <windows/ppp/win32/network/Firewall.h>
#include <windows/ppp/win32/network/NetworkInterface.h>
#else
#include <common/unix/UnixAfx.h>
#if defined(_MACOS)
#include <cerrno>
#include <sys/resource.h>
#include <darwin/ppp/tap/TapDarwin.h>
#else
#include <linux/ppp/tap/TapLinux.h>
#include <linux/ppp/diagnostics/UnixStackTrace.h>
#endif
#endif

// Third-party library includes
#if defined(CURLINC_CURL)
#include <curl/curl.h>
#endif

#include <openssl/opensslv.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/err.h>

#include <common/aesni/aes.h>
#include <common/chnroutes2/chnroutes2.h>

// Console input includes for tab switching
#if defined(_WIN32)
#include <conio.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#endif

// Using declarations for cleaner code
using ppp::configurations::AppConfiguration;
using ppp::threading::Executors;
using ppp::threading::Thread;
using ppp::threading::Timer;
using ppp::threading::BufferswapAllocator;
using ppp::diagnostics::Stopwatch;
using ppp::diagnostics::PreventReturn;
using ppp::tap::ITap;
using ppp::tap::TapStub;
using ppp::net::Ipep;
using ppp::net::IPEndPoint;
using ppp::net::AddressFamily;
using ppp::net::Socket;
using ppp::net::asio::IAsynchronousWriteIoQueue;
using ppp::io::File;
using ppp::io::FileAccess;
using ppp::auxiliary::StringAuxiliary;
using ppp::app::server::VirtualEthernetSwitcher;
using ppp::app::client::VEthernetNetworkSwitcher;
using ppp::app::client::VEthernetExchanger;
using ppp::app::client::proxys::VEthernetLocalProxySwitcher;
using ppp::app::client::proxys::VEthernetHttpProxySwitcher;
using ppp::app::client::proxys::VEthernetSocksProxySwitcher;
using ppp::Int128;

// Custom restart signal definition for Unix-like platforms
#if !defined(_WIN32) && !defined(_ANDROID) && !defined(_IPHONE)  
#define SIGRESTART 64 
#endif

// Constants for IP list update intervals

// Network interface configuration structure
struct NetworkInterface final
{
    typedef ppp::unordered_set<ppp::string>             BypassSet;
    enum class BypassMode : uint8_t {
        Ip,
        Geo,
        No,
    };

#if defined(_WIN32)
    uint32_t                                            LeaseTimeInSeconds = 0;     // DHCP lease time
    bool                                                SetHttpProxy       = false; // Enable HTTP proxy
    bool                                                LocalDns           = true;  // Listen on loopback for system DNS
#else   
    bool                                                Promisc            = false; // Promiscuous mode
    int                                                 Ssmt               = 0;     // SSMT thread count
#if defined(_LINUX) 
    bool                                                SsmtMQ             = false; // SSMT message queue mode
    bool                                                ProtectNetwork     = false; // Protect network routes
#endif  
#endif  

    bool                                                StaticMode         = false; // Static tunnel mode
    bool                                                Lwip               = false; // Use LWIP stack
    bool                                                VNet               = false; // Subnet forwarding
    bool                                                HostedNetwork      = false; // Prefer host network
    bool                                                BlockQUIC          = false; // Block QUIC protocol

    uint16_t                                            Mux                = 0;     // MUX connection count
    uint8_t                                             MuxAcceleration    = 0;     // MUX acceleration mode

    const std::shared_ptr<BypassSet>                    Bypass;                     // IP bypass list file path
#if defined(_LINUX)
    ppp::string                                         BypassNic;                  // Network interface for bypass
#endif  
    boost::asio::ip::address                            BypassNgw;                  // Gateway for bypass routes

    const std::shared_ptr<BypassSet>                    Bypass6;                    // IPv6 bypass list file path
#if defined(_LINUX)
    ppp::string                                         BypassNic6;                 // Network interface for IPv6 bypass
#endif  
    boost::asio::ip::address                            BypassNgw6;                 // Gateway for IPv6 bypass routes
    BypassMode                                          SplitMode = BypassMode::Ip;
    ppp::string                                         GeoRules;                   // Geo routing rules file
    ppp::string                                         GeoSite;                    // V2Ray/Mihomo geosite.dat file
    ppp::string                                         GeoIP;                      // V2Ray/Mihomo geoip.dat file

    ppp::string                                         ComponentId;                // TAP device identifier
#if defined(_WIN32) 
    ppp::string                                         Wintun;                     // TAP device name
#endif  

    ppp::string                                         FirewallRules;              // Firewall rules file path
    ppp::string                                         DNSRules;                   // DNS rules file path
    ppp::string                                         Nic;                        // Physical network interface

    ppp::vector<boost::asio::ip::address>               DnsAddresses;               // DNS server addresses

    boost::asio::ip::address                            Ngw;                        // Preferred gateway
    boost::asio::ip::address                            IPAddress;                  // Virtual adapter IP
    boost::asio::ip::address                            GatewayServer;              // Virtual adapter gateway
    boost::asio::ip::address                            SubmaskAddress;             // Subnet mask

    static ppp::string                                  GetDefaultTun() noexcept;   // Default tun-name

    int                                                 BypassLoadList(const ppp::string& s) noexcept;
    int                                                 BypassLoadList6(const ppp::string& s) noexcept;

    NetworkInterface() noexcept 
        : Bypass(ppp::make_shared_object<BypassSet>()),
          Bypass6(ppp::make_shared_object<BypassSet>()) { }
};

// Console window dimensions structure
struct ConsoleForegroundWindowSize final
{
    int                                                 x   = -1;   // Width in characters
    int                                                 y   = -1;   // Height in characters
    bool                                                tty = true; // Is a terminal device
};  

// Console printing helper class with line counting 
class PrintToConsoleForegroundWindow final  
{   
public: 
    ConsoleForegroundWindowSize*                        console_window_size    = NULLPTR;    // Console dimensions
    ppp::string*                                        console_window_content = NULLPTR;    // Output buffer
    int*                                                console_window_heights = NULLPTR;    // Line counter

public:
    // Print formatted text with line counting and truncation
    template <class... A>
    void                                            operator()(const char* format, A&&... args) noexcept 
    {
        // Control the number of lines that need to be printed to the console window to prevent crowding the visible display area 
        // Of the console window, and when the console window size changes, follow the printed content until it is fully printed.
        if (console_window_size->y > *console_window_heights) 
        {
            ppp::string st = PrintToString(console_window_size->x, ' ', format, std::forward<A&&>(args)...);

            (*console_window_heights)++;
            console_window_content->append(st);
        }
    }

    template <class... A>
    void                                            Highlighted(const char* format, A&&... args) noexcept
    {
        if (console_window_size->y <= *console_window_heights) return;
        ppp::string line = PrintToString(console_window_size->x, ' ', format,
            std::forward<A&&>(args)...);
        if (console_window_size->tty)
        {
            line.insert(0, "\x1b[7m");
            std::size_t ending = line.empty() ? 0 : line.size() - 1;
            line.insert(ending, "\x1b[0m");
        }
        (*console_window_heights)++;
        console_window_content->append(line);
    }

private:
    // Return the number of console cells occupied by a Unicode code point.
    // Windows Terminal renders CJK/full-width characters as two cells, while
    // UTF-8 stores them in multiple bytes.  Byte-count based padding therefore
    // breaks the fixed-width screen buffer as soon as a localized NIC name is
    // printed.
    static std::size_t                              UnicodeConsoleCellWidth(std::uint32_t cp) noexcept
    {
        if (cp == 0 || (cp >= 0x0300 && cp <= 0x036f) ||
            (cp >= 0x1ab0 && cp <= 0x1aff) || (cp >= 0x1dc0 && cp <= 0x1dff) ||
            (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
            (cp >= 0xfe20 && cp <= 0xfe2f))
        {
            return 0;
        }

        return (cp >= 0x1100 &&
            (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
             (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
             (cp >= 0xac00 && cp <= 0xd7a3) ||
             (cp >= 0xf900 && cp <= 0xfaff) ||
             (cp >= 0xfe10 && cp <= 0xfe19) ||
             (cp >= 0xfe30 && cp <= 0xfe6f) ||
             (cp >= 0xff00 && cp <= 0xff60) ||
             (cp >= 0xffe0 && cp <= 0xffe6) ||
             (cp >= 0x1f300 && cp <= 0x1faff) ||
             (cp >= 0x20000 && cp <= 0x3fffd))) ? 2 : 1;
    }

    // Copy a UTF-8 string up to a console-cell limit and report its display
    // width. Invalid sequences are copied one byte at a time as narrow text.
    static ppp::string                              FitToConsoleWidth(const char* text, std::size_t length,
                                                                      std::size_t limit, std::size_t& width) noexcept
    {
        ppp::string result;
        result.reserve(length);
        width = 0;

        for (std::size_t i = 0; i < length;)
        {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            std::size_t count = 1;
            std::uint32_t cp = lead;
            if ((lead & 0xe0) == 0xc0) { count = 2; cp = lead & 0x1f; }
            elif ((lead & 0xf0) == 0xe0) { count = 3; cp = lead & 0x0f; }
            elif ((lead & 0xf8) == 0xf0) { count = 4; cp = lead & 0x07; }

            bool valid = count > 1 && i + count <= length;
            for (std::size_t j = 1; valid && j < count; ++j)
            {
                const unsigned char continuation = static_cast<unsigned char>(text[i + j]);
                valid = (continuation & 0xc0) == 0x80;
                cp = (cp << 6) | (continuation & 0x3f);
            }
            if (!valid)
            {
                count = 1;
                cp = lead;
            }

            const std::size_t cells = UnicodeConsoleCellWidth(cp);
            if (width + cells > limit)
            {
                break;
            }
            result.append(text + i, count);
            width += cells;
            i += count;
        }
        return result;
    }

    // Format text with padding and line endings
    template <class... A>
    ppp::string                                     PrintToString(std::size_t padding_length, char padding_char, const char* format, A&&... args) noexcept 
    {
        char buf[8096];
        int dw = snprintf(buf, sizeof(buf), format, std::forward<A&&>(args)...);

        // Handle buffer overflow
        if (dw >= sizeof(buf))
        {
            dw = sizeof(buf) - 1;
        }
        elif(dw < 0) 
        {
            dw = 0;
        }

        ppp::string result;
        buf[dw] = '\x0';

        // Fit and pad by displayed console cells, not UTF-8 byte count. Each
        // terminal row must occupy exactly padding_length cells because TTY
        // refresh deliberately relies on automatic wrapping instead of CRLF.
        std::size_t display_width = 0;
        result = FitToConsoleWidth(buf, static_cast<std::size_t>(dw), padding_length, display_width);
        if (display_width < padding_length)
        {
            result.append(padding_length - display_width, padding_char);
        }

        // Add line endings for non-terminal output
        if (!console_window_size->tty) 
        {
            result.append("\r\n");
        }
        else
        {
            // For TTY, use just \n to avoid issues with cursor positioning
            result.append("\n");
        }

        return result;
    }
};

// Main application class
class PppApplication : public std::enable_shared_from_this<PppApplication>
{
public:
    PppApplication() noexcept;
    virtual ~PppApplication() noexcept;

public:
    // Application entry point
    int                                             Main(int argc, const char* argv[]) noexcept;
    // Clean up resources
    void                                            Dispose() noexcept;
    // Final release
    void                                            Release() noexcept;

public:
    // Singleton access
    static std::shared_ptr<PppApplication>          GetDefault() noexcept;
    // Shutdown handler
    static bool                                     OnShutdownApplication() noexcept;
    // Trigger application shutdown/restart
    static bool                                     ShutdownApplication(bool restart) noexcept;
    // Register shutdown handlers
    static bool                                     AddShutdownApplicationEventHandler() noexcept;

public:
    // Configuration accessors
    std::shared_ptr<AppConfiguration>               GetConfiguration() noexcept;
    std::shared_ptr<VirtualEthernetSwitcher>        GetServer() noexcept;
    std::shared_ptr<VEthernetNetworkSwitcher>       GetClient() noexcept;
    std::shared_ptr<BufferswapAllocator>            GetBufferAllocator() noexcept;

public:
    // Display help information
    void                                            PrintHelpInformation() noexcept;
    // Download IP lists from APNIC
    void                                            PullIPList(const ppp::string& command) noexcept;
    // Synchronous IP list download
    int                                             PullIPList(const ppp::string& url, ppp::set<ppp::string>& ips) noexcept;
    // Asynchronous IP list download with callback
    bool                                            PullIPList(const ppp::string& url, const ppp::function<void(int, const ppp::set<ppp::string>&)>& cb) noexcept;
    // Parse command line arguments
    int                                             PreparedArgumentEnvironment(int argc, const char* argv[]) noexcept;

protected:
    // Main tick handler - called every second
    virtual bool                                    OnTick(uint64_t now) noexcept;

private:
    struct ClientOutboundConfiguration final
    {
        ppp::string                                     tag;
        std::shared_ptr<AppConfiguration>               configuration;
        ppp::string                                     display_name;
        bool                                            server_menu = false;
        ppp::string                                     source_path;
        bool                                            route_used = false;
    };

    // Load configuration file
    std::shared_ptr<AppConfiguration>               LoadConfiguration(int argc, const char* argv[], ppp::string& path) noexcept;
    bool                                            LoadServerConfigurations(int argc, const char* argv[],
                                                        const std::shared_ptr<AppConfiguration>& primary) noexcept;
    bool                                            LoadGeoOutboundConfigurations(
                                                        const std::shared_ptr<NetworkInterface>& network_interface,
                                                        const std::shared_ptr<AppConfiguration>& primary) noexcept;
    // Determine if running in client or server mode
    bool                                            IsModeClientOrServer(int argc, const char* argv[]) noexcept;
    // Parse network interface configuration from arguments
    std::shared_ptr<NetworkInterface>               GetNetworkInterface(int argc, const char* argv[]) noexcept;
    // Parse IP address from command line with validation
    boost::asio::ip::address                        GetNetworkAddress(const char* name, int MIN_PREFIX_ADDRESS, int MAX_PREFIX_ADDRESS, int argc, const char* argv[]) noexcept;
    // Parse IP address with default value
    boost::asio::ip::address                        GetNetworkAddress(const char* name, int MIN_PREFIX_ADDRESS, int MAX_PREFIX_ADDRESS, const char* default_address_string, int argc, const char* argv[]) noexcept;
    // Parse DNS server addresses
    void                                            GetDnsAddresses(ppp::vector<boost::asio::ip::address>& addresses, int argc, const char* argv[]) noexcept;
    // Initialize network environment
    bool                                            PreparedLoopbackEnvironment(const std::shared_ptr<NetworkInterface>& network_interface) noexcept;
    // Print current status and statistics
    bool                                            PrintEnvironmentInformation() noexcept;

private:
    // Start/stop periodic tick handler
    static bool                                     NextTickAlwaysTimeout(bool next) noexcept;
    void                                            ClearTickAlwaysTimeout() noexcept;

private:
    // Get traffic statistics
    bool                                            GetTransmissionStatistics(uint64_t& incoming_traffic, uint64_t& outgoing_traffic, std::shared_ptr<ppp::transmissions::ITransmissionStatistics>& statistics_snapshot) noexcept;
    // Handle keyboard input for tab switching
    void                                            HandleConsoleInput() noexcept;
    void                                            HandleServerSelection(int delta, bool activate) noexcept;

private:
    ConsoleForegroundWindowSize                     console_window_size_last_;           // Previous console size
    std::size_t                                     console_window_buff_size_   = 0;     // Console buffer size
    int                                             console_tab_page_           = 0;     // Current TUI tab page (0-3)
    std::size_t                                     server_selection_           = 0;     // Selected row on Servers page
    bool                                            client_mode_                = false; // Current mode flag
    bool                                            proxy_mode_                 = false; // Local HTTP/SOCKS only; no TUN/routes/DNS
    bool                                            quic_                       = false; // Original QUIC setting (Windows)
    std::shared_ptr<AppConfiguration>               configuration_;                      // Application configuration
    ppp::vector<ClientOutboundConfiguration>        outbound_configurations_;             // Geo multi-outbound configurations
    ppp::string                                     geo_configuration_path_;              // geo-rules.txt used as --config
    std::shared_ptr<VirtualEthernetSwitcher>        server_;                             // Server switcher
    std::shared_ptr<VEthernetNetworkSwitcher>       client_;                             // Client switcher
    ppp::string                                     configuration_path_;                 // Configuration file path
    ppp::string                                     server_directory_;                   // Independent server JSON directory
    std::shared_ptr<NetworkInterface>               network_interface_;                  // Network interface config
    std::shared_ptr<Timer>                          timeout_                    = 0;     // Periodic timer
    Stopwatch                                       stopwatch_;                          // Application uptime
    PreventReturn                                   prevent_rerun_;                      // Prevent multiple instances
    ppp::transmissions::ITransmissionStatistics     transmission_statistics_;            // Traffic statistics
};

// Global variables
static std::shared_ptr<PppApplication>              DEFAULT_;                            // Application instance
static ppp::string                                   LOG_FILE_PATH_;                      // Log file path from --log-file argument
FILE*                                                ppp::g_log_stream = stdout;          // Log output stream, redirected by --log-file

#if defined(_MACOS)
static void ConfigureOpenFileDescriptorLimit() noexcept
{
    static constexpr rlim_t kDesiredOpenFiles = 65536;

    struct rlimit current = {};
    if (getrlimit(RLIMIT_NOFILE, &current) != 0)
    {
        LOG_WARN("ConfigureOpenFileDescriptorLimit: getrlimit failed, errno=%d", errno);
        return;
    }

    rlim_t before = current.rlim_cur;
    rlim_t ceiling = current.rlim_max == RLIM_INFINITY ?
        kDesiredOpenFiles : std::min<rlim_t>(kDesiredOpenFiles, current.rlim_max);
    if (before >= ceiling)
    {
        LOG_INFO("ConfigureOpenFileDescriptorLimit: soft=%llu, hard=%llu, unchanged=1",
            (unsigned long long)before, (unsigned long long)current.rlim_max);
        return;
    }

    int last_error = 0;
    rlim_t applied = before;
    for (rlim_t candidate = ceiling; candidate > before;)
    {
        struct rlimit desired = current;
        desired.rlim_cur = candidate; // Never alter the process hard limit.
        if (setrlimit(RLIMIT_NOFILE, &desired) == 0)
        {
            applied = candidate;
            break;
        }

        last_error = errno;
        rlim_t fallback = candidate / 2;
        if (fallback < 1024 && before < 1024)
        {
            fallback = 1024;
        }
        if (fallback <= before || fallback >= candidate)
        {
            break;
        }
        candidate = fallback;
    }

    if (applied > before)
    {
        LOG_INFO("ConfigureOpenFileDescriptorLimit: soft_before=%llu, soft_after=%llu, hard=%llu",
            (unsigned long long)before, (unsigned long long)applied,
            (unsigned long long)current.rlim_max);
    }
    else
    {
        LOG_WARN("ConfigureOpenFileDescriptorLimit: unable to raise soft limit, soft=%llu, hard=%llu, errno=%d",
            (unsigned long long)before, (unsigned long long)current.rlim_max, last_error);
    }
}
#endif
static struct {
    using BypassSet = NetworkInterface::BypassSet;

    bool                                            restart                     = false; // Restart flag

    int                                             link_restart                = 0;     // Link restart count
    int                                             auto_restart                = 0;     // Auto restart interval

    std::shared_ptr<BypassSet>                      bypass;                              // Bypass file path
}                                                   GLOBAL_;                             // Global application state

// Constructor
PppApplication::PppApplication() noexcept
{
    // Hide console cursor for cleaner output
    ppp::HideConsoleCursor(true);

#if defined(_WIN32)
    // Set console window title
    SetConsoleTitleW(L"PPP PRIVATE NETWORK\u2122 2");

    // Set console buffer and window size on Windows
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE); 
    if (NULLPTR != hConsole)
    {
        COORD cSize = { 120, ppp::win32::Win32Native::IsWindows11OrLaterVersion() ? 46 : 47 };
        if (SetConsoleScreenBufferSize(hConsole, cSize))
        {
            SMALL_RECT rSize = { 0, 0, cSize.X - 1, cSize.Y - 1 };
            SetConsoleWindowInfo(hConsole, TRUE, &rSize);
        }
    }

    // Disable console close button to prevent accidental termination
    ppp::win32::Win32Native::EnabledConsoleWindowClosedButton(false);
#endif
}

// Destructor
PppApplication::~PppApplication() noexcept
{
    Release();
}

// Clean up resources
void PppApplication::Release() noexcept 
{
    // Restore console cursor
    ppp::HideConsoleCursor(false);

#if defined(_WIN32)
    // Re-enable console close button
    ppp::win32::Win32Native::EnabledConsoleWindowClosedButton(true);
#endif

    // Release prevention lock
    prevent_rerun_.Close();
}

// Default tun-name
ppp::string NetworkInterface::GetDefaultTun() noexcept
{
    const char* default_tun_name = NULLPTR;
#if defined(_WIN32)
    default_tun_name = "PPP";
#elif defined(_MACOS)
    default_tun_name = "utun0";
#else
    default_tun_name = "ppp";
#endif
    return default_tun_name;
}

// NetworkInterface::BypassLoadList
// 
// Parses a pipe-separated list of file paths, converts each to an absolute path,
// and inserts them into the bypass set. If the input string contains a single
// entry, it is added directly without tokenization.
//
// Parameters:
//   s - A string containing one or more file paths separated by '|*?<>'
// Returns:
//   Number of successfully added entries (0 if none or input empty)
int NetworkInterface::BypassLoadList(const ppp::string& s) noexcept
{
    // Get reference to the underlying bypass set (shared_ptr is always valid)
    BypassSet& set = *Bypass;
    set.clear();  // Clear any previous entries

    // Nothing to do if input is empty
    if (s.empty())
    {
        return 0;
    }

    // Split the input string by '|' into segments
    ppp::vector<ppp::string> segments;
    ppp::string work = s;
    for (char& ch : work)
    {
        // Replace any of * ? < > with '|'
        if (ch == '*' || ch == '?' || ch == '<' || ch == '>')
        {
            ch = '|';
        }
    }
    ppp::Tokenize<ppp::string>(work, segments, "|");

    // Optimization: if there's only one segment, add it directly without trimming
    if (segments.size() == 1)
    {
        set.emplace(std::move(segments[0]));
        return 1;
    }

    int events = 0;
    // Process each segment
    for (const ppp::string& i : segments)
    {
        // Skip empty segments
        if (i.empty())
        {
            continue;
        }
        
        // Trim whitespace from both ends
        ppp::string t = ppp::LTrim(ppp::RTrim(i));
        if (t.empty())
        {
            continue;
        }

        // Convert to absolute path, handling any path rewriting (e.g., environment variables)
        t = File::GetFullPath(File::RewritePath(t.data()).data());
        if (t.empty())
        {
            continue;
        }

        // Insert the absolute path into the bypass set
        // The move avoids an extra copy of the string
        auto r = set.emplace(std::move(t));
        if (r.second)  // insertion succeeded (path was not already present)
        {
            events++;
        }
    }

    // Return the count of newly added entries
    return events;
}

// NetworkInterface::BypassLoadList6
// 
// Same as BypassLoadList but operates on the Bypass6 set for IPv6 bypass files.
int NetworkInterface::BypassLoadList6(const ppp::string& s) noexcept
{
    BypassSet& set = *Bypass6;
    set.clear();

    if (s.empty())
    {
        return 0;
    }

    ppp::vector<ppp::string> segments;
    ppp::string work = s;
    for (char& ch : work)
    {
        if (ch == '*' || ch == '?' || ch == '<' || ch == '>')
        {
            ch = '|';
        }
    }
    ppp::Tokenize<ppp::string>(work, segments, "|");

    if (segments.size() == 1)
    {
        set.emplace(std::move(segments[0]));
        return 1;
    }

    int events = 0;
    for (const ppp::string& i : segments)
    {
        if (i.empty())
        {
            continue;
        }
        
        ppp::string t = ppp::LTrim(ppp::RTrim(i));
        if (t.empty())
        {
            continue;
        }

        t = File::GetFullPath(File::RewritePath(t.data()).data());
        if (t.empty())
        {
            continue;
        }

        auto r = set.emplace(std::move(t));
        if (r.second)
        {
            events++;
        }
    }

    return events;
}

// Asynchronous IP list download
bool PppApplication::PullIPList(const ppp::string& url, const ppp::function<void(int, const ppp::set<ppp::string>&)>& cb) noexcept
{
    // Download IP list from URL in background thread
    if (NULLPTR == cb || url.empty()) 
    {
        return false;
    }

    auto self = shared_from_this();
    std::thread(
        [self, this, url, cb]() noexcept 
        {
            ppp::set<ppp::string> ips;
            ppp::SetThreadName("iplist");

            int events = PullIPList(url, ips);
            cb(events, ips);
        }).detach();
    return true;
}

// Synchronous IP list download
int PppApplication::PullIPList(const ppp::string& url, ppp::set<ppp::string>& ips) noexcept 
{
    // Realize the collection of route lists captured from Internet resources of the HTTP/HTTPS protocol that comply with the ip route configuration rules.
    using HttpClient = ppp::net::http::HttpClient;

    ppp::string host;
    ppp::string path;
    int port = IPEndPoint::MinPort;
    bool https = false;

    // Parse URL components
    if (!HttpClient::VerifyUri(url, ppp::addressof(host), &port, ppp::addressof(path), &https)) 
    {
        return -1;
    }

    // Create HTTP client with SSL support if needed
    HttpClient http_client((https ? "https://" : "http://") + host, chnroutes2_cacertpath_default());
    
    int http_status_code = -1;
    std::string http_response_body = http_client.Get(path, http_status_code);

    // Check HTTP status
    if (http_status_code < 200 || http_status_code >= 300) 
    {
        return -1;
    }

    // Parse IP list from response
    return chnroutes2_getiplist(ips, ppp::string(), stl::transform<ppp::string>(http_response_body));
}

// Download IP list with progress notification
void PppApplication::PullIPList(const ppp::string& command) noexcept
{
    // Show progress message
    fprintf(stdout, "[%s]PULL\r\n", chnroutes2_gettime(chnroutes2_gettime()).data());

    // Parse command into path and country code
    ppp::string path;
    ppp::string nation;
    for (ppp::string command_string = ppp::LTrim(ppp::RTrim(command)); command_string.size() > 0;) 
    {
        std::size_t index = command_string.find('<');
        if (index == std::string::npos) 
        {
            index = command_string.find('/');
            if (index == std::string::npos) 
            {
                path = command_string;
                break;
            }
        }

        path = ppp::RTrim(command_string.substr(0, index));
        nation = ppp::LTrim(command_string.substr(index + 1));
        break;
    }

    // Use default path if not specified
    if (path.empty()) 
    {
        path = chnroutes2_filepath_default();
    }

    // Convert to absolute path
    path = File::GetFullPath(File::RewritePath(path.data()).data());

    // Synchronous download
    ppp::set<ppp::string> ips;
    bool ok = false;
    if (chnroutes2_getiplist(ips, nation) > 0)
    {
        ok = chnroutes2_saveiplist(path, ips);
    }

    // Show completion status
    if (ok)
    {
        fprintf(stdout, "[%s]OK\r\n", chnroutes2_gettime(chnroutes2_gettime()).data());
    }
    else
    {
        fprintf(stdout, "[%s]FAIL\r\n", chnroutes2_gettime(chnroutes2_gettime()).data());
    }
}

// Handle keyboard input for tab switching (non-blocking, cross-platform)
void PppApplication::HandleServerSelection(int delta, bool activate) noexcept
{
    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    if (NULLPTR == client)
    {
        server_selection_ = 0;
        return;
    }

    auto statuses = client->GetOutboundStatuses();
    ppp::vector<VEthernetNetworkSwitcher::OutboundStatus> servers;
    for (auto& status : statuses)
    {
        if (status.server_menu)
        {
            servers.emplace_back(std::move(status));
        }
    }

    if (servers.empty())
    {
        server_selection_ = 0;
        return;
    }

    if (server_selection_ >= servers.size())
    {
        server_selection_ = servers.size() - 1;
    }
    if (delta < 0)
    {
        server_selection_ = server_selection_ == 0 ?
            servers.size() - 1 : server_selection_ - 1;
    }
    elif(delta > 0)
    {
        server_selection_ = (server_selection_ + 1) % servers.size();
    }
    if (activate)
    {
        client->SwitchOutbound(servers[server_selection_].tag);
    }
}

void PppApplication::HandleConsoleInput() noexcept
{
#if !defined(_ANDROID)
    const int console_tab_count = NULLPTR != client_ ? 4 : 2;
    static constexpr int servers_tab_page = 3;
    if (console_tab_page_ >= console_tab_count)
    {
        console_tab_page_ = 0;
    }
#if defined(_WIN32)
    // Windows: use _kbhit/_getch (non-blocking)
    while (_kbhit())
    {
        int ch = _getch();
        if (ch == 0x09) // Tab key
        {
            console_tab_page_ = (console_tab_page_ + 1) % console_tab_count;
        }
        elif(ch == 0xE0) // Extended key prefix (arrows, F-keys)
        {
            int ext = _getch();
            if (console_tab_page_ == servers_tab_page && ext == 0x48) { HandleServerSelection(-1, false); }
            elif(console_tab_page_ == servers_tab_page && ext == 0x50) { HandleServerSelection(1, false); }
            elif(ext == 0x4B) { console_tab_page_ = (console_tab_page_ + console_tab_count - 1) % console_tab_count; }
            elif(ext == 0x4D) { console_tab_page_ = (console_tab_page_ + 1) % console_tab_count; }
        }
        elif(console_tab_page_ == servers_tab_page && ch == 0x0D)
        {
            HandleServerSelection(0, true);
        }
    }
#else
    // Unix: use select+read (non-blocking, no echo)
    static bool termios_initialized = false;
    static struct termios old_term;
    if (!termios_initialized)
    {
        termios_initialized = true;
        struct termios new_term;
        tcgetattr(STDIN_FILENO, &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    }

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    struct timeval tv = { 0, 0 };
    if (select(STDIN_FILENO + 1, &read_fds, NULLPTR, NULLPTR, &tv) > 0)
    {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) > 0)
        {
            if (ch == '\t') // Tab key
            {
                console_tab_page_ = (console_tab_page_ + 1) % console_tab_count;
            }
            elif(console_tab_page_ == servers_tab_page && (ch == '\r' || ch == '\n'))
            {
                HandleServerSelection(0, true);
            }
            elif(console_tab_page_ == servers_tab_page && ch == '\x1b')
            {
                char sequence[2] = {};
                if (read(STDIN_FILENO, sequence, sizeof(sequence)) == 2 && sequence[0] == '[')
                {
                    if (sequence[1] == 'A') { HandleServerSelection(-1, false); }
                    elif(sequence[1] == 'B') { HandleServerSelection(1, false); }
                    elif(sequence[1] == 'D') { console_tab_page_ = (console_tab_page_ + console_tab_count - 1) % console_tab_count; }
                    elif(sequence[1] == 'C') { console_tab_page_ = (console_tab_page_ + 1) % console_tab_count; }
                }
            }
        }
    }
#endif
#endif
}

// Print current application status to console
bool PppApplication::PrintEnvironmentInformation() noexcept
{
    std::shared_ptr<NetworkInterface> network_interface = network_interface_;
    if (NULLPTR == network_interface)
    {
        return false;
    }

    // Get console window size
    ConsoleForegroundWindowSize console_window_size;
    if (isatty(fileno(stdout)) == 0 || !ppp::GetConsoleWindowSize(console_window_size.x, console_window_size.y)) 
    {
        // Default size for non-terminal output
        fseek(stdout, 0, SEEK_SET);
        console_window_size.x = 80;
        console_window_size.y = 80; 
        console_window_size.tty = false;
    }

    // Move cursor to top-left for refresh
    if (console_window_size.tty && !ppp::SetConsoleCursorPosition(0, 0))
    {
        return false;
    }
    
    // Determine hosting environment
    ppp::string hosting_environment;
#if defined(_DEBUG)
    hosting_environment = "development";
#else
    hosting_environment = "production";
#endif

    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    hosting_environment = (NULLPTR != client ? "client:" : "server:") + hosting_environment;

    // Clear the previous frame before every TTY refresh so stale trailing rows
    // cannot remain when the next frame contains fewer lines.
    if (console_window_size.tty)
    {
        ppp::ClearConsoleOutputCharacter();
    }

    // Build console output
    ppp::string console_window_content;
    if (console_window_buff_size_ > 0)
    {
        console_window_content.reserve(console_window_buff_size_);
    }

    int console_window_heights = 0;
    PrintToConsoleForegroundWindow printfn = { &console_window_size, &console_window_content, &console_window_heights };

    bool console_highlight = console_window_size.tty;
#if defined(_WIN32)
    if (console_highlight)
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        console_highlight = output != INVALID_HANDLE_VALUE &&
            GetConsoleMode(output, &mode) &&
            SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif

    // Create separator line
    ppp::string section_separator;
    section_separator = ppp::PaddingRight(section_separator, console_window_size.x, '-');

    // Print application header
    printfn("%s", PPP_APPLICATION_NAME);
    printfn("%s", section_separator.data());
    printfn("%s", "Application started. Press Ctrl+C to shut down.");

    // Print tab bar (always visible, at top)
    {
        const char* tab_labels[] = { "Status", "Network", "Routes", "Servers" };
        const int tab_count = NULLPTR != client ? 4 : 2;

        // Calculate tab column width (evenly divide console width)
        int col_width = console_window_size.x / tab_count;
        int remainder = console_window_size.x % tab_count;

        ppp::string tab_bar;
        tab_bar.reserve(console_window_size.x);

        for (int i = 0; i < tab_count; i++)
        {
            // Adjust last column to consume remainder
            int this_col_width = col_width;
            if (i == tab_count - 1) this_col_width += remainder;

            // Get label display width
            const char* label = tab_labels[i];
            int label_width = 0;
            while (*label)
            {
                unsigned char c = *label;
                if ((c & 0x80) == 0) { label_width += 1; label++; }
                elif((c & 0xE0) == 0xC0) { label_width += 2; label += 2; }
                elif((c & 0xF0) == 0xE0) { label_width += 2; label += 3; }
                else { label_width += 2; label += 4; }
            }

            // Calculate padding
            int padding = this_col_width - label_width;
            ppp::string segment;
            if (i == console_tab_page_)
            {
                segment += " ";
                segment += tab_labels[i];
                padding -= 1;
            }
            else
            {
                segment += " ";
                segment += tab_labels[i];
                padding -= 1;
            }

            // Fill remaining space
            if (padding > 0)
            {
                segment.append(padding, ' ');
            }
            if (i == console_tab_page_ && console_highlight)
            {
                tab_bar += "\x1b[7m";
                tab_bar += segment;
                tab_bar += "\x1b[0m";
            }
            else if (i == console_tab_page_)
            {
                if (!segment.empty()) segment[0] = '[';
                if (!segment.empty()) segment.back() = ']';
                tab_bar += segment;
            }
            else
            {
                tab_bar += segment;
            }
        }

        if (console_highlight && console_window_size.y > console_window_heights)
        {
            console_window_content.append(tab_bar);
            console_window_content.push_back('\n');
            ++console_window_heights;
        }
        else
        {
            printfn("%s", tab_bar.c_str());
        }
    }

    printfn("Max Concurrent        : %d", configuration_->concurrent);
    printfn("Process               : %d", ppp::GetCurrentProcessId());

#if defined(__SIMD__)
    if (aesni::aes_cpu_is_support()) 
    {
        printfn("Triplet               : %s:%s[SIMD]", 
            ppp::GetSystemCode(), ppp::GetPlatformCode());
    }
    else 
    {
#endif
        printfn("Triplet               : %s:%s", ppp::GetSystemCode(), ppp::GetPlatformCode());
#if defined(__SIMD__)
    }
#endif

    printfn("Cwd                   : %s", ppp::GetCurrentDirectoryPath().data());
    ppp::string active_configuration_path;
    if (NULLPTR != client)
    {
        active_configuration_path = client->GetActiveOutboundSourcePath();
    }
    if (active_configuration_path.empty())
    {
        active_configuration_path = configuration_path_;
    }
    printfn("Template              : %s", active_configuration_path.data());

    // Print server-specific information in the fixed header.
    std::shared_ptr<VirtualEthernetSwitcher> server = server_;
    if (NULLPTR != server)
    {
        ppp::string node_id = configuration_->server.management.node_id;
        printfn("Node ID               : %s", node_id.empty() ? "--" : node_id.data());

        // Displays the link status and link Uri of the VPN back-end management server.
        auto managed_server = server->GetManagedServer(); 
        if (NULLPTR != managed_server)
        {
            const char* link_state = "connecting";
            if (managed_server->LinkIsAvailable()) 
            {
                link_state = "established";
            }
            elif(managed_server->LinkIsReconnecting()) 
            {
                link_state = "reconnecting";
            }

            ppp::string link_url = managed_server->GetUri();
            printfn("Managed Server        : %s %s", link_url.data(), link_state);
        }
        else
        {
            printfn("Managed Server        : %s", "disabled");
        }
    }

    // Keep runtime identity and connection information visible on every tab.
    {
        // Print client-specific information
        if (NULLPTR != client)
        {
            ppp::string guid = configuration_->client.guid;
            if (guid.size() >= 2 && guid.front() == '{' && guid.back() == '}')
            {
                guid = guid.substr(1, guid.size() - 2);
            }
            printfn("GUID                  : {%s}", guid.data());

            // Remote server information
            if (ppp::string remote_uri = client->GetRemoteUri(); remote_uri.size() > 0)
            {
                std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger();
                if (client->IsProxyOnly())
                {
                    printfn("VPN Server            : %s [%s] [proxy]", remote_uri.data(), NULLPTR != exchanger && exchanger->StaticEchoAllocated() ? "static" : "dynamic");
                }
                else
                {
                    printfn("VPN Server            : %s [%s]", remote_uri.data(), NULLPTR != exchanger && exchanger->StaticEchoAllocated() ? "static" : "dynamic");
                }
            }
            else
            {
                printfn("VPN Server            : %s", "pending");
            }

#if !defined(_ANDROID)
            if (!client->IsProxyOnly())
            {
                const char* split_mode = NULLPTR;
                if (network_interface->SplitMode == NetworkInterface::BypassMode::Ip)
                {
                    split_mode = "ip";
                }
                elif(network_interface->SplitMode == NetworkInterface::BypassMode::Geo)
                {
                    split_mode = "geo";
                }

                printfn("Split Routing         : %s", NULLPTR != split_mode ? split_mode : "global");
            }
            else
            {
                printfn("Split Routing         : %s", "off");
            }
#endif

            // Local proxy servers
            struct
            {
                const char*                                     name;
                const char*                                     proxy;
                std::shared_ptr<VEthernetLocalProxySwitcher>    switcher;
            } proxys[] =
                {
                    { "Http Proxy            : off", "Http Proxy            : %s/http", client->GetHttpProxy() },
                    { "Socks Proxy           : off", "Socks Proxy           : %s/socks", client->GetSocksProxy() }
                };
            for (auto& st : proxys)
            {
                std::shared_ptr<VEthernetLocalProxySwitcher> switcher = st.switcher;
                if (NULLPTR == switcher)
                {
                    printfn("%s", st.name);
                    continue;
                }

                boost::asio::ip::tcp::endpoint localEP = switcher->GetLocalEndPoint();
                boost::asio::ip::address localIP = localEP.address();
                if (localIP.is_unspecified())
                {
                    if (auto ni = client->GetUnderlyingNetworkInterface(); NULLPTR != ni)
                    {
                        localIP = ni->IPAddress;
                    }
                }

                // Displays the address of the http/socks proxy server for the local virtual loopback.
                ppp::string address_string = IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(localIP, localEP.port())).ToString();
                printfn(st.proxy, address_string.data());
            }

#if defined(_WIN32)
            printfn("P/A Controller        : %s", client->GetPaperAirplaneController() ? "on" : "off");
#endif

            const char* network_states[] = { "connecting", "established", "reconnecting" };
            const char* connection_state = "unavailable";
            ppp::string mux_state = "unavailable";
            if (std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger(); NULLPTR != exchanger)
            {
                int state = static_cast<int>(exchanger->GetNetworkState());
                if (state >= 0 && state < arraysizeof(network_states))
                {
                    connection_state = network_states[state];
                }

                if (client->IsMuxEnabled())
                {
                    int state = static_cast<int>(exchanger->GetMuxNetworkState());
                    mux_state = state >= 0 && state < arraysizeof(network_states) ? network_states[state] : "unknown";
                    mux_state += ", ";
                    mux_state += stl::to_string<ppp::string>(client->Mux(NULLPTR));
                    mux_state += "-channel";
                }
                else
                {
                    mux_state = "disabled";
                }
            }
            printfn("Connection            : %s", connection_state);
            printfn("Mux State             : %s", mux_state.data());
        }

        // Print server listening services.
        if (std::shared_ptr<VirtualEthernetSwitcher> server = server_; NULLPTR != server)
        {
            using NAC = VirtualEthernetSwitcher::NetworkAcceptorCategories;

            // List listening ports for various services
            const char* categories[] = { "ppp+tcp", "ppp+udp", "ppp+ws", "ppp+wss", "cdn+1", "cdn+2" };
            VirtualEthernetSwitcher::NetworkAcceptorCategories categoriess[] =
            {
                NAC::NetworkAcceptorCategories_Tcpip,
                NAC::NetworkAcceptorCategories_Udpip,
                NAC::NetworkAcceptorCategories_WebSocket,
                NAC::NetworkAcceptorCategories_WebSocketSSL,
                NAC::NetworkAcceptorCategories_CDN1,
                NAC::NetworkAcceptorCategories_CDN2,
            };
            for (int i = 0, j = 0; i < arraysizeof(categories); i++)
            {
                boost::asio::ip::tcp::endpoint serverEP = server->GetLocalEndPoint(categoriess[i]);
                if (serverEP.port() <= IPEndPoint::MinPort || serverEP.port() > IPEndPoint::MaxPort)
                {
                    continue;
                }

                ppp::string tmp = "Service ";
                tmp += stl::to_string<ppp::string>(++j);
                tmp = ppp::PaddingRight(tmp, 22, ' ');
                tmp += ": " + IPEndPoint::ToEndPoint(serverEP).ToString();
                tmp += "/";
                tmp += categories[i];
                printfn("%s", tmp.data());
            }
        }

        // Displays the current host environment type, in effect marking whether it is a released product or a development debug release.
        printfn("Hosting Environment   : %s", hosting_environment.data());
        printfn("Duration              : %s", stopwatch_.Elapsed().ToString("TT:mm:ss", false).data());

        // To print a blank line as a separator for major categories.
        printfn("");
    }

    // Tab 1: Server outbound interface.
    if (console_tab_page_ == 1 && NULLPTR == client)
    {
        ppp::string interface_name;
#if defined(_WIN32)
        interface_name = ppp::win32::network::GetInterfaceName(
            ppp::win32::network::Router::GetBestInterface(IPEndPoint("8.8.8.8", 0).GetAddress()));
#elif defined(_MACOS)
        uint32_t interface_address = ppp::unix__::UnixAfx::GetDefaultNetworkInterface();
        if (interface_address != IPEndPoint::NoneAddress)
        {
            interface_name = ppp::unix__::UnixAfx::GetInterfaceName(IPEndPoint(interface_address, 0));
        }
#else
        uint32_t interface_address = IPEndPoint::NoneAddress;
        uint32_t interface_gateway = IPEndPoint::NoneAddress;
        uint32_t interface_mask = IPEndPoint::NoneAddress;
        ppp::tap::TapLinux::GetPreferredNetworkInterface(
            interface_name,
            interface_address,
            interface_mask,
            interface_gateway,
            network_interface->Nic);
#endif
        printfn("%s", "NETWORK");
        printfn("%s", section_separator.data());
        printfn("Interface             : %s", interface_name.empty() ? "unavailable" : interface_name.data());
        printfn("");
    }

    // Tab 1: Client network interface details.
    if (console_tab_page_ == 1 && NULLPTR != client)
    {
        if (client->IsProxyOnly())
        {
            printfn("%s", "TUNNEL");
            printfn("%s", section_separator.data());
            printfn("Mode                  : %s", "proxy-only");
            printfn("Adapter               : %s", "none");

            if (std::shared_ptr<ITap> tap = client->GetTap(); NULLPTR != tap)
            {
                if (tap->IPAddress != ppp::net::IPEndPoint::AnyAddress)
                {
                    printfn("Logical IPv4          : %s", ppp::net::IPEndPoint::ToAddressString(tap->IPAddress).data());
                }
                if (tap->IPv6Address.is_v6() && !tap->IPv6Address.is_unspecified())
                {
                    printfn("Logical IPv6          : %s", tap->IPv6Address.to_string().data());
                }
            }
            if (std::shared_ptr<AppConfiguration> configuration = configuration_;
                NULLPTR != configuration && configuration->udp.dns.redirect.size() > 0)
            {
                printfn("Tunnel DNS            : %s", configuration->udp.dns.redirect.data());
            }

            if (std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger();
                NULLPTR != exchanger)
            {
                const char* network_states[] = { "connecting", "established", "reconnecting" };
                int link_state = static_cast<int>(exchanger->GetNetworkState());
                int mux_state = static_cast<int>(exchanger->GetMuxNetworkState());
                const char* link_state_text =
                    link_state >= 0 && link_state < arraysizeof(network_states) ?
                    network_states[link_state] : "unknown";
                printfn("Link State            : %s", link_state_text);

                if (client->IsMuxEnabled())
                {
                    ppp::string mux_state_text =
                        mux_state >= 0 && mux_state < arraysizeof(network_states) ?
                        network_states[mux_state] : "unknown";
                    mux_state_text += ", ";
                    mux_state_text += stl::to_string<ppp::string>(client->Mux(NULLPTR));
                    mux_state_text += "-channel";
                    printfn("Mux State             : %s", mux_state_text.data());
                }
                else
                {
                    printfn("Mux State             : %s", "disabled");
                }
            }
            else
            {
                printfn("Link State            : %s", "unavailable");
                printfn("Mux State             : %s", "unavailable");
            }

            printfn("TCP/IP Transport      : %s", "PPP tunnel");
            printfn("DNS Transport         : %s", "PPP tunnel");
            printfn("");
        }

        struct
        {
            std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface> ni;
            const char*                                                 tab;
            bool                                                        tun;
        } stnis[] = {
            { client->GetTapNetworkInterface(), "TUN", true,  },
            { client->GetUnderlyingNetworkInterface(), "NIC", false },
        };
        for (auto&& sti : stnis)
        {
            if (client->IsProxyOnly() && sti.tun)
            {
                continue;
            }
            auto ni = sti.ni; 
            if (NULLPTR != ni)
            {
                printfn("%s", sti.tab);
                printfn("%s", section_separator.data());
#if defined(_WIN32)
                printfn("Name                  : %s[%s]", ni->Name.data(), ni->Description.data());
#else
                printfn("Name                  : %s", ni->Name.data());
#endif
                printfn("Index                 : %d", ni->Index);
#if !defined(_MACOS)
                ppp::string component_id = ni->Id;
                if (component_id.size() > 0)
                {
                    printfn("Id                    : %s", component_id.data());
                }
#endif
                printfn("Interface             : %s %s %s",
                    ni->IPAddress.to_string().data(),
                    ni->GatewayServer.to_string().data(),
                    ni->SubmaskAddress.to_string().data());

                // Show IPv6 interface info if available
                if (sti.tun)
                {
                    if (std::shared_ptr<ITap> tap = client->GetTap(); NULLPTR != tap)
                    {
                        if (tap->IPv6Address.is_v6() || tap->IPv6GatewayServer.is_v6())
                        {
                            ppp::string ipv6_interface;
                            if (tap->IPv6Address.is_v6())
                            {
                                ipv6_interface = tap->IPv6Address.to_string();
                                ipv6_interface += "/64";
                            }
                            if (tap->IPv6GatewayServer.is_v6())
                            {
                                if (ipv6_interface.size() > 0)
                                {
                                    ipv6_interface += " ";
                                }
                                ipv6_interface += tap->IPv6GatewayServer.to_string();
                            }
                            if (tap->IPv6SubmaskAddress.is_v6())
                            {
                                ipv6_interface += " " + tap->IPv6SubmaskAddress.to_string();
                            }
                            if (ipv6_interface.size() > 0)
                            {
                                printfn("Interface IPv6        : %s", ipv6_interface.data());
                            }
                        }
                    }
                }
                else if (ni->IPv6GatewayServer.is_v6())
                {
                    printfn("Interface IPv6        : %s", ni->IPv6GatewayServer.to_string().data());
                }

                if (sti.tun)
                {
                    // Aggligator status
                    for (const char* aggligator_status[] = { "none", "unknown", "connecting", "reconnecting", "established" };;)
                    {
                        if (std::shared_ptr<aggligator::aggligator> aggligator = client->GetAggligator(); NULLPTR != aggligator)
                        {
                            int max_channel = 0;
                            int max_servers = 0;
                            aggligator->client_fetch_concurrency(max_servers, max_channel);
                            
                            aggligator::aggligator::link_status link_status = aggligator->status();
                            ppp::string aggligator_status_string = aggligator_status[(int)link_status];
                            aggligator_status_string += ", ";
                            aggligator_status_string += stl::to_string<ppp::string>(max_servers);
                            aggligator_status_string += "-server, ";
                            aggligator_status_string += stl::to_string<ppp::string>(max_channel);
                            aggligator_status_string += "-channel";

                            printfn("Aggligator            : %s", aggligator_status_string.data());
                        }
                        else
                        {
                            printfn("Aggligator            : %s", *aggligator_status);
                        }

                        break;
                    }

                    // Forwarding proxy
                    for (std::shared_ptr<ppp::transmissions::proxys::IForwarding> forwarding = client->GetForwarding();;)
                    {
                        if (NULLPTR != forwarding)
                        {
                            ppp::string proxy_url = forwarding->GetProxyUrl();
                            printfn("Proxy Interlayer      : %s", proxy_url.data());
                        }
                        else 
                        {
                            printfn("Proxy Interlayer      : %s", "none");
                        }

                        break;
                    }

                    printfn("TCP/IP CC             : %s", client->IsLwip() ? 
                        "lwip" : 
#ifdef SYSNAT
                        client->IsSysnat() ? "tc" : 
#endif
                        "ctcp"
                    );
                    printfn("Block QUIC            : %s", client->IsBlockQUIC() ? "blocked" : "unblocked");

                    if (std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger(); NULLPTR != exchanger)
                    {
                        const char* network_states[] = { "connecting", "established", "reconnecting" };
                        ppp::string network_state_string;
                        if (client->IsMuxEnabled())
                        {
                            network_state_string = network_states[(int)exchanger->GetMuxNetworkState()];
                            network_state_string += ", ";
                            network_state_string += stl::to_string<ppp::string>(client->Mux(NULLPTR));
                            network_state_string += "-channel";
                        }
                        else 
                        {
                            network_state_string = "none";
                        }

                        printfn("Mux State             : %s", network_state_string.data());
                        printfn("Link State            : %s", network_states[(int)exchanger->GetNetworkState()]);
                    }
                    else
                    {
                        printfn("Mux State             : %s", "none");
                        printfn("Link State            : %s", "none");
                    }
                }

                // DNS servers
                for (std::size_t i = 0, l = ni->DnsAddresses.size(); i < l; i++)
                {
                    const boost::asio::ip::address& addr = ni->DnsAddresses[i];
                    ppp::string tmp = "DNS Server " + stl::to_string<ppp::string>(i + 1);
                    tmp = ppp::PaddingRight(tmp, 22, ' ');
                    tmp += ": " + addr.to_string();
                    tmp += addr.is_v6() ? " (IPv6)" : " (IPv4)";
                    printfn("%s", tmp.data());
                }

                // To print a blank line as a separator for major categories.
                printfn("");
            }
        }
    }

    // Get transmission statistics
    struct
    {
        uint64_t incoming_traffic;
        uint64_t outgoing_traffic;
        std::shared_ptr<ppp::transmissions::ITransmissionStatistics> statistics_snapshot;
    } TransmissionStatistics;

    if (!GetTransmissionStatistics(TransmissionStatistics.incoming_traffic, TransmissionStatistics.outgoing_traffic, TransmissionStatistics.statistics_snapshot))
    {
        TransmissionStatistics.incoming_traffic = 0;
        TransmissionStatistics.outgoing_traffic = 0;
        TransmissionStatistics.statistics_snapshot = NULLPTR;
    }

    // Tab 0: Show traffic statistics in Status.
    if (console_tab_page_ == 0)
    {
        // Print VPN statistics
        printfn("%s", "VPN");
        printfn("%s", section_separator.data());
        if (NULLPTR != server)
        {
            printfn("Sessions              : %s", stl::to_string<ppp::string>(server->GetAllExchangerNumber()).data());
        }

        printfn("TX                    : %s", ppp::StrFormatByteSize(TransmissionStatistics.outgoing_traffic).data());
        printfn("RX                    : %s", ppp::StrFormatByteSize(TransmissionStatistics.incoming_traffic).data());
        if (auto statistics = TransmissionStatistics.statistics_snapshot; statistics)
        {
            printfn("IN                    : %s", ppp::StrFormatByteSize(statistics->IncomingTraffic).data());
            printfn("OUT                   : %s", ppp::StrFormatByteSize(statistics->OutgoingTraffic).data());
        }
    }

    // Tab 2: Route statistics
    if (console_tab_page_ == 2)
    {
        printfn("%s", "ROUTES");
        printfn("%s", section_separator.data());
        printfn("Bypass Mode           : %s",
            network_interface->SplitMode == NetworkInterface::BypassMode::Ip ? "ip" :
            network_interface->SplitMode == NetworkInterface::BypassMode::Geo ? "geo" : "no");
        if (network_interface->SplitMode == NetworkInterface::BypassMode::Geo)
        {
            printfn("GEO Policy YAML       : %s", network_interface->GeoRules.data());
            printfn("GeoSite Database      : %s", network_interface->GeoSite.data());
            printfn("GeoIP Database        : %s", network_interface->GeoIP.data());
        }
        elif(network_interface->SplitMode == NetworkInterface::BypassMode::Ip)
        {
            printfn("Bypass IPv4 File      : %s", network_interface->Bypass->size() > 0 ?
                network_interface->Bypass->begin()->data() : "(none)");
            printfn("Bypass IPv6 File      : %s", network_interface->Bypass6->size() > 0 ?
                network_interface->Bypass6->begin()->data() : "(none)");
            printfn("Bypass Gateway        : %s", network_interface->BypassNgw.to_string().data());
            printfn("Bypass Gateway IPv6   : %s", network_interface->BypassNgw6.to_string().data());
            printfn("DNS Rules File        : %s", network_interface->DNSRules.data());
            if (NULLPTR != client)
            {
                printfn("IP List Entries       : %zu", client->GetIPListCount());
                printfn("IPv6 List Entries     : %zu", client->GetIPList6Count());
            }
        }
        else
        {
            printfn("%s", "Split-routing files are not used in global mode.");
        }
    }

    // Tab 3: live outbound selection. All configured exchangers remain open;
    // switching changes only the default tunnel used by newly created flows.
    if (console_tab_page_ == 3)
    {
        printfn("%s", "SERVERS");
        printfn("%s", section_separator.data());
        printfn("%s", "Use Up/Down to select and Enter to switch.");
        printfn("%s", "Existing sessions keep their current connection.");
        if (NULLPTR == client)
        {
            printfn("%s", "No VPN client is running.");
        }
        else
        {
            auto statuses = client->GetOutboundStatuses();
            ppp::vector<VEthernetNetworkSwitcher::OutboundStatus> servers;
            for (auto& status : statuses)
            {
                if (status.server_menu)
                {
                    servers.emplace_back(std::move(status));
                }
            }
            if (servers.empty())
            {
                printfn("%s", "No server menu is configured. Use --server-dir=<directory>.");
            }
            else
            {
                if (server_selection_ >= servers.size())
                {
                    server_selection_ = servers.size() - 1;
                }
                static constexpr std::size_t page_size = 10;
                std::size_t page_count = (servers.size() + page_size - 1) / page_size;
                std::size_t page = server_selection_ / page_size;
                std::size_t first = page * page_size;
                std::size_t last = std::min<std::size_t>(servers.size(), first + page_size);
                printfn("Servers               : %zu  Page %zu/%zu  (%zu-%zu)",
                    servers.size(), page + 1, page_count, first + 1, last);
                auto server_endpoint = [](const ppp::string& uri) noexcept -> ppp::string
                {
                    ppp::string value = ppp::ATrim<ppp::string>(uri);
                    std::size_t authority = value.find("://");
                    std::size_t slash = value.find('/',
                        authority == ppp::string::npos ? 0 : authority + 3);
                    if (slash != ppp::string::npos) value.erase(slash);
                    return value;
                };
                for (std::size_t i = first; i < last; ++i)
                {
                    const auto& outbound = servers[i];
                    bool connected = outbound.state ==
                        VEthernetExchanger::NetworkState_Established;
                    const char* usage = outbound.route_used ? " used" :
                        (connected ? " connected" : "");
                    ppp::string endpoint = server_endpoint(outbound.server);
                    if (i == server_selection_ && console_highlight)
                    {
                        printfn.Highlighted(">%-1c %-24s%s",
                            outbound.active ? '*' : ' ',
                            outbound.display_name.data(),
                            usage);
                        printfn.Highlighted("   %s",
                            endpoint.empty() ? "(not configured)" : endpoint.data());
                    }
                    else
                    {
                        printfn("%c%c %-24s%s",
                            i == server_selection_ ? '>' : ' ',
                            outbound.active ? '*' : ' ',
                            outbound.display_name.data(),
                            usage);
                        printfn("   %s",
                            endpoint.empty() ? "(not configured)" : endpoint.data());
                    }
                }
            }
        }
    }

    // Update buffer size for next render
    std::size_t console_window_content_size = console_window_content.size();
    if (console_window_content_size > console_window_buff_size_)
    {
        console_window_buff_size_ = ppp::Malign(console_window_content_size, 1 << 6);
    }

    // Output to console
    fprintf(stdout, "%s", console_window_content.data());
    return true;
}

// Initialize network environment
bool PppApplication::PreparedLoopbackEnvironment(const std::shared_ptr<NetworkInterface>& network_interface) noexcept
{
    std::shared_ptr<AppConfiguration> configuration = GetConfiguration();
    if (NULLPTR == configuration)
    {
        return false;
    }

    std::shared_ptr<boost::asio::io_context> context = Executors::GetDefault();
    if (NULLPTR == context)
    {
        return false;
    }
    else
    {
#if defined(_WIN32)
        if (!proxy_mode_)
        {
            // Configure Windows Firewall rules only for runtimes which expose a
            // kernel network path. Proxy mode does not mutate firewall state.
            ppp::string executable_path = File::GetFullPath(File::RewritePath(ppp::GetFullExecutionFilePath().data()).data());
            ppp::win32::network::Fw::NetFirewallAddApplication(PPP_APPLICATION_NAME, executable_path.data());
            ppp::win32::network::Fw::NetFirewallAddAllApplication(PPP_APPLICATION_NAME, executable_path.data());

            // Install paper airplane plugin if needed
            if (client_mode_ && network_interface->HostedNetwork && configuration->client.paper_airplane.tcp)
            {
                if (ppp::app::client::lsp::PaperAirplaneController::Install() < 0)
                {
                    return false;
                }
            }

            // Prevent problematic programs from loading LSPs
            if (client_mode_)
            {
                ppp::app::client::lsp::PaperAirplaneController::NoLsp();
                ppp::app::client::lsp::PaperAirplaneController::Reset();
            }
        }
#endif
    }

    bool success = false;
    if (client_mode_)
    {
        std::shared_ptr<VEthernetNetworkSwitcher> ethernet = NULLPTR;
        std::shared_ptr<ITap> tap = NULLPTR;
        do
        {
            // Proxy mode uses a no-op adapter and never creates a Windows/Linux
            // kernel interface.
            if (proxy_mode_)
            {
                tap = TapStub::Create(context, network_interface->DnsAddresses);
            }
            else
            {
#if defined(_WIN32)
            tap = ITap::Create(context,
                network_interface->ComponentId,
                Ipep::ToAddressString<ppp::string>(network_interface->IPAddress),
                Ipep::ToAddressString<ppp::string>(network_interface->GatewayServer),
                Ipep::ToAddressString<ppp::string>(network_interface->SubmaskAddress),
                network_interface->LeaseTimeInSeconds,
                network_interface->HostedNetwork,
                Ipep::AddressesTransformToStrings(network_interface->DnsAddresses));
#else
            tap = ITap::Create(context,
                network_interface->ComponentId,
                Ipep::ToAddressString<ppp::string>(network_interface->IPAddress),
                Ipep::ToAddressString<ppp::string>(network_interface->GatewayServer),
                Ipep::ToAddressString<ppp::string>(network_interface->SubmaskAddress),
                network_interface->Promisc,
                network_interface->HostedNetwork,
                Ipep::AddressesTransformToStrings(network_interface->DnsAddresses));
#endif
            }
            if (NULLPTR == tap)
            {
                fprintf(stdout, "%s\r\n", "Open tun/tap driver failure.");
                break;
            }
            else
            {
                fprintf(stdout, "%s\r\n", "Open tun/tap driver success.");
            }

            // Configure TAP device
            tap->BufferAllocator = configuration->GetBufferAllocator();
            if (!tap->Open())
            {
                fprintf(stdout, "%s\r\n", "Listen tun/tap driver failure.");
                break;
            }
            else
            {
                fprintf(stdout, "%s\r\n", "Listen tun/tap driver success.");
            }

            // Create client switcher
            ethernet = ppp::make_shared_object<VEthernetNetworkSwitcher>(context, network_interface->Lwip, network_interface->VNet, configuration->concurrent > 1, configuration);
            if (NULLPTR == ethernet)
            {
                break;
            }

            {
                bool proxy_only = proxy_mode_;
                ethernet->ProxyOnly(&proxy_only);
            }

            if (!proxy_mode_ && !outbound_configurations_.empty())
            {
                VEthernetNetworkSwitcher::OutboundConfigurationList outbounds;
                for (const ClientOutboundConfiguration& outbound : outbound_configurations_)
                {
                    outbounds.emplace_back(VEthernetNetworkSwitcher::OutboundConfiguration{
                        outbound.tag, outbound.configuration,
                        outbound.display_name, outbound.server_menu,
                        outbound.source_path, outbound.route_used });
                }
                if (!ethernet->SetOutboundConfigurations(outbounds))
                {
                    fprintf(stdout, "%s\r\n", "Invalid multi-outbound configuration.");
                    break;
                }
            }

#if !defined(_WIN32)
            // Configure SSMT settings
            ethernet->Ssmt(&network_interface->Ssmt);
#if defined(_LINUX)
            ethernet->SsmtMQ(&network_interface->SsmtMQ);
            ethernet->ProtectMode(&network_interface->ProtectNetwork);
#endif
#endif
            // Proxy-only follows the normal client transport policy: prefer a
            // configured vmux logical stream and fall back to an independent
            // PPP transmission when vmux is unavailable.
            ethernet->Mux(&network_interface->Mux);
            ethernet->MuxAcceleration(&network_interface->MuxAcceleration);
            ethernet->StaticMode(&network_interface->StaticMode);
            if (!proxy_mode_)
            {
                ethernet->PreferredNgw(network_interface->Ngw);
                ethernet->PreferredNgw6(network_interface->BypassNgw6);
                ethernet->PreferredNic(network_interface->Nic);
            }
            // Load bypass policy selected by --bypass-mode.
            if (!proxy_mode_ && network_interface->SplitMode == NetworkInterface::BypassMode::Geo) {
                if (!ethernet->LoadGeoRules(network_interface->GeoRules, network_interface->GeoSite, network_interface->GeoIP)) {
                    fprintf(stdout, "%s\r\n", "Failed to load geo bypass rules.");
                    break;
                }
            }

            // Load bypass IP lists in IP mode.
            if (!proxy_mode_ && network_interface->SplitMode == NetworkInterface::BypassMode::Ip) {
#if defined(_LINUX)
                for (auto&& bypass_path : *network_interface->Bypass)
                {
                    ethernet->AddLoadIPList(bypass_path, network_interface->BypassNic, network_interface->BypassNgw, ppp::string());
                }
#else
                for (auto&& bypass_path : *network_interface->Bypass)
                {
                    ethernet->AddLoadIPList(bypass_path, network_interface->BypassNgw, ppp::string());
                }
#endif

                // Load IPv6 bypass IP lists.
#if defined(_LINUX)
                for (auto&& bypass_path : *network_interface->Bypass6)
                {
                    ethernet->AddLoadIPList6(bypass_path, network_interface->BypassNic6, network_interface->BypassNgw6, ppp::string());
                }
#else
                for (auto&& bypass_path : *network_interface->Bypass6)
                {
                    ethernet->AddLoadIPList6(bypass_path, network_interface->BypassNgw6, ppp::string());
                }
#endif
            }

            if (!proxy_mode_ && network_interface->SplitMode == NetworkInterface::BypassMode::Ip) {
                for (auto&& route : configuration->client.routes)
                {
                    ppp::string path = File::GetFullPath(File::RewritePath(route.path.data()).data());
                    if (path.empty())
                    {
                        continue;
                    }

#if defined(_LINUX)
                    ethernet->AddLoadIPList(path, route.nic, Ipep::ToAddress(route.ngw), ppp::string());
#else
                    ethernet->AddLoadIPList(path, Ipep::ToAddress(route.ngw), ppp::string());
#endif
                }
            }

            // IP-mode DNS rules are mutually exclusive with geo mode.
            if (!proxy_mode_ && network_interface->SplitMode == NetworkInterface::BypassMode::Ip) {
                ethernet->LoadAllDnsRules(network_interface->DNSRules, true);
            }

            // Open switcher
            if (!ethernet->Open(tap))
            {
                auto ni = ethernet->GetUnderlyingNetworkInterface();
                if (NULLPTR != ni)
                {
                    fprintf(stdout, "%s\r\n", "Failed to open the vpn client.");
                }
                else
                {
                    fprintf(stdout, "%s\r\n", "No available nic could be found.");
                }
                break;
            }

            // Log client module startup
            {
                auto logger = ethernet->GetLogger();
                if (NULLPTR != logger) {
                    logger->Info("main: Client module started successfully.");
                }
            }

            success = true;
            client_ = ethernet;
        } while (false);

        // Cleanup on failure
        if (!success)
        {
            client_.reset();
            if (NULLPTR != ethernet)
            {
                ethernet->Dispose();
            }

            // Turn off the tun/tap virtual network card driver that has been opened.
            if (NULLPTR != tap)
            {
                tap->Dispose();
            }
        }
    }
    else
    {
        // Server mode setup
        std::shared_ptr<VirtualEthernetSwitcher> ethernet = NULLPTR;
        do
        {
            // Create server switcher
            ethernet = ppp::make_shared_object<VirtualEthernetSwitcher>(configuration);
            if (NULLPTR == ethernet)
            {
                break;
            }

            // Open switcher
            if (!ethernet->Open(network_interface->FirewallRules))
            {
                fprintf(stdout, "%s\r\n", "Failed to open the vpn server.");
                break;
            }

            // Run services
            if (!ethernet->Run())
            {
                fprintf(stdout, "%s\r\n", "Listen to vpn server failure.");
                break;
            }

            // Log server module startup
            {
                auto logger = ethernet->GetLogger();
                if (NULLPTR != logger) {
                    logger->Info("main: Server module started successfully.");
                }
            }

            success = true;
            server_ = ethernet;
        } while (false);

        // Cleanup on failure
        if (!success)
        {
            server_.reset();
            if (NULLPTR != ethernet)
            {
                ethernet->Dispose();
            }
        }
    }
    return success;
}

// Get buffer allocator from configuration
std::shared_ptr<BufferswapAllocator> PppApplication::GetBufferAllocator() noexcept
{
    std::shared_ptr<AppConfiguration> configuration = GetConfiguration();
    if (NULLPTR == configuration)
    {
        return NULLPTR;
    }
    else
    {
        return configuration->GetBufferAllocator();
    }
}

// Parse and prepare command line arguments
int PppApplication::PreparedArgumentEnvironment(int argc, const char* argv[]) noexcept
{
    // Parse log file path from command line
    LOG_FILE_PATH_ = ppp::GetCommandArgument("--log-file", argc, argv);
    if (LOG_FILE_PATH_.size() > 0)
    {
        LOG_FILE_PATH_ = File::GetFullPath(File::RewritePath(LOG_FILE_PATH_.data()).data());
    }

    // Show help if requested
    if (ppp::IsInputHelpCommand(argc, argv))
    {
        return 1;
    }

    // Load configuration
    ppp::string path;
    std::shared_ptr<AppConfiguration> configuration = LoadConfiguration(argc, argv, path);
    if (NULLPTR == configuration)
    {
        return -1;
    }
    else
    {
        // Gets whether client mode or server mode is currently running.
        ppp::string mode;
        static constexpr const char* mode_keys[] = { "--mode", "--m", "-mode", "-m" };
        for (const char* key : mode_keys)
        {
            mode = ppp::GetCommandArgument(key, argc, argv);
            if (!mode.empty())
            {
                break;
            }
        }
        mode = ppp::ToLower<ppp::string>(ppp::ATrim<ppp::string>(mode));
        proxy_mode_ = mode == "proxy";
        client_mode_ = proxy_mode_ || IsModeClientOrServer(argc, argv);

        if (client_mode_ && !LoadServerConfigurations(argc, argv, configuration))
        {
            return -1;
        }

        if (proxy_mode_)
        {
            // Listener addresses and default ports come from the JSON
            // configuration.  Explicit proxy port arguments override only
            // their corresponding configured ports.
            ppp::string http_port = ppp::GetCommandArgument("--proxy-http-port", argc, argv);
            ppp::string socks_port = ppp::GetCommandArgument("--proxy-socks-port", argc, argv);
            auto parse_proxy_port = [](const ppp::string& text, const char* option, int& destination) noexcept -> bool
                {
                    if (text.empty()) return true;

                    char* tail = NULLPTR;
                    long value = strtol(text.data(), &tail, 10);
                    if (NULLPTR == tail || tail == text.data() || *tail != '\x0' ||
                        value <= IPEndPoint::MinPort || value > IPEndPoint::MaxPort)
                    {
                        fprintf(stdout, "Invalid %s value '%s'; expected 1-65535.\r\n",
                            option, text.data());
                        return false;
                    }

                    destination = static_cast<int>(value);
                    return true;
                };
            if (!parse_proxy_port(http_port, "--proxy-http-port", configuration->client.http_proxy.port) ||
                !parse_proxy_port(socks_port, "--proxy-socks-port", configuration->client.socks_proxy.port))
            {
                return -1;
            }
            bool http_enabled = configuration->client.http_proxy.port > IPEndPoint::MinPort &&
                configuration->client.http_proxy.port <= IPEndPoint::MaxPort;
            bool socks_enabled = configuration->client.socks_proxy.port > IPEndPoint::MinPort &&
                configuration->client.socks_proxy.port <= IPEndPoint::MaxPort;
            if (!http_enabled && !socks_enabled)
            {
                fprintf(stdout, "%s\r\n",
                    "Proxy mode requires a valid configured or command-line HTTP/SOCKS port.");
                return -1;
            }

            LOG_INFO("Proxy mode: HTTP=%s:%d SOCKS=%s:%d; TUN/routes/DNS/system-proxy disabled",
                configuration->client.http_proxy.bind.data(), configuration->client.http_proxy.port,
                configuration->client.socks_proxy.bind.data(), configuration->client.socks_proxy.port);
        }
    }
    if (!geo_configuration_path_.empty() && !client_mode_)
    {
        fprintf(stdout, "%s\r\n", "A geo-rules.txt configuration is only valid in client mode.");
        return -1;
    }

    // Configure thread pool
    int max_concurrent = configuration->concurrent - 1;
    if (max_concurrent > 0)
    {
        Executors::SetMaxSchedulers(max_concurrent);
        if (!client_mode_)
        {
            Executors::SetMaxThreads(configuration->GetBufferAllocator(), max_concurrent);
        }
    }

    // Store configuration (before GetNetworkInterface so GetDnsAddresses can access it)
    configuration_path_ = path;
    configuration_ = configuration;

    // A standalone proxy has no network-interface policy.  In particular it
    // must not parse TUN arguments, select a driver, replace the process-global
    // DNS list, or inherit LwIP/VNet/static/bypass settings from an old client
    // command line.
    std::shared_ptr<NetworkInterface> network_interface =
        proxy_mode_ ? ppp::make_shared_object<NetworkInterface>() : GetNetworkInterface(argc, argv);
    if (NULLPTR == network_interface)
    {
        return -1;
    }
    if (proxy_mode_)
    {
        network_interface->SplitMode = NetworkInterface::BypassMode::No;
        GetDnsAddresses(network_interface->DnsAddresses, argc, argv);
        // Proxy-only does not parse the TUN interface settings, but it still
        // shares the normal client's vmux transport policy.
        network_interface->Mux = (uint16_t)std::max<int>(
            0, atoi(ppp::GetCommandArgument("--tun-mux", argc, argv).data()));
        network_interface->MuxAcceleration = (uint8_t)std::max<int>(
            0, atoi(ppp::GetCommandArgument("--tun-mux-acceleration", argc, argv).data()));
        if (network_interface->MuxAcceleration > PPP_MUX_ACCELERATION_MAX)
        {
            network_interface->MuxAcceleration = 0;
        }
#if defined(_WIN32)
        network_interface->LocalDns = false;
#endif
        Socket::SetDefaultFlashTypeOfService(false);
    }
    else
    {
        Socket::SetDefaultFlashTypeOfService(
            ppp::ToBoolean(ppp::GetCommandArgument("--tun-flash", argc, argv).data()));
    }

    // Passing geo-rules.txt through --config selects multi-outbound geo mode.
    // Command-line bypass flags must not silently replace the manifest rules.
    if (!geo_configuration_path_.empty())
    {
        network_interface->SplitMode = NetworkInterface::BypassMode::Geo;
        network_interface->GeoRules = geo_configuration_path_;
    }

    if (client_mode_ && !proxy_mode_ &&
        !LoadGeoOutboundConfigurations(network_interface, configuration))
    {
        return -1;
    }

    network_interface_ = network_interface;
    
    // Configure DNS settings
    ppp::net::asio::vdns::ttl = configuration->udp.dns.ttl;
    ppp::net::asio::vdns::enabled = configuration->udp.dns.turbo;
    
    return 0;
}

// Format version string
static ppp::string GetVersionString(int major, int minor, int patch = 0) noexcept
{
    char buf[100];
    *buf = '\x0';

    if (patch != 0) 
    {
        snprintf(buf, sizeof(buf), "%d.%d.%d", major, minor, patch);
    }
    else 
    {
        snprintf(buf, sizeof(buf), "%d.%d", major, minor);
    }

    return buf;
}

// Get Boost library version string
static ppp::string GetBoostVersionString() noexcept 
{
    constexpr int version = BOOST_VERSION;

    int minor = (version / 100) % 100;
    int major = version / 100000;
    int patch = version % 100;

    return GetVersionString(major, minor, patch);
}

// Print comprehensive help information
void PppApplication::PrintHelpInformation() noexcept
{
#if defined(_WIN32)
    auto printf = [](const char* format, auto&&... args) noexcept {
        int length = std::snprintf(NULLPTR, 0, format, std::forward<decltype(args)>(args)...);
        if (length < 1) {
            return length;
        }

        std::vector<char> utf8(static_cast<std::size_t>(length) + 1);
        std::snprintf(utf8.data(), utf8.size(), format, std::forward<decltype(args)>(args)...);

        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
            int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, NULLPTR, 0);
            if (wide_length > 0) {
                std::vector<wchar_t> wide(static_cast<std::size_t>(wide_length));
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length,
                    wide.data(), wide_length) == wide_length) {
                    DWORD written = 0;
                    if (WriteConsoleW(output, wide.data(), static_cast<DWORD>(wide.size()), &written, NULLPTR)) {
                        return length;
                    }
                }
            }
        }

        ppp::string compatible(utf8.data(), static_cast<std::size_t>(length));
        const std::pair<const char*, const char*> box_characters[] = {
            { "\xE2\x94\x8C", "+" }, { "\xE2\x94\x90", "+" },
            { "\xE2\x94\x94", "+" }, { "\xE2\x94\x98", "+" },
            { "\xE2\x94\x9C", "+" }, { "\xE2\x94\xA4", "+" },
            { "\xE2\x94\xAC", "+" }, { "\xE2\x94\xB4", "+" },
            { "\xE2\x94\xBC", "+" }, { "\xE2\x94\x80", "-" },
            { "\xE2\x94\x82", "|" }
        };
        for (const auto& character : box_characters) {
            std::size_t offset = 0;
            while ((offset = compatible.find(character.first, offset)) != ppp::string::npos) {
                compatible.replace(offset, std::strlen(character.first), character.second);
                offset += std::strlen(character.second);
            }
        }
        return static_cast<int>(fwrite(compatible.data(), 1, compatible.size(), stdout));
    };
#endif

    ppp::string execution_file_name = ppp::GetExecutionFileName();
    ppp::string cwd = ppp::GetCurrentDirectoryPath();
    
    // Define column widths for alignment
    static constexpr int col_option_width = 40;
    static constexpr int col_description_width = 48;
    static constexpr int col_default_width = 23;
    static constexpr int col_command_width = 38;
    static constexpr int col_command_width_utlity = col_command_width + 2;

    // Print header
    printf("┌──────────────────────────────────────────────────────────────────────┐\n");
    printf("│                       PPP PRIVATE NETWORK™ 2                         │\n");
    printf("│  Next-generation security network access technology, providing high- │\n");
    printf("│  performance Virtual Ethernet tunneling service.                     │\n");
    printf("└──────────────────────────────────────────────────────────────────────┘\n\n");
    
    printf("Version:      %s\n", PPP_APPLICATION_VERSION);
    printf("Copyright:    (C) 2017 ~ 2055 SupersocksR ORG. All rights reserved.\n");
    printf("Current Dir:  %s\n\n", cwd.data());
    
    printf("USAGE:\n");
    printf("    %s [OPTIONS]\n\n", execution_file_name.data());
    
    // GENERAL OPTIONS table
    printf("GENERAL OPTIONS:\n");
    printf("┌──────────────────────────────────────────┬──────────────────────────────────────────────────┬─────────────────────────┐\n");
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "OPTION", 
        col_description_width, "DESCRIPTION", 
        col_default_width, "DEFAULT");
    printf("├──────────────────────────────────────────┼──────────────────────────────────────────────────┼─────────────────────────┤\n");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--rt=[yes|no]", 
        col_description_width, "Enable real-time mode", 
        col_default_width, "yes");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--mode=[client|server|proxy]",
        col_description_width, "Set running mode", 
        col_default_width, "server");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--proxy-http-port=<port>",
        col_description_width, "Override proxy-mode HTTP port",
        col_default_width, "config");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--proxy-socks-port=<port>",
        col_description_width, "Override proxy-mode SOCKS5 port",
        col_default_width, "config");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--config=<path>", 
        col_description_width, "Configuration file path", 
        col_default_width, "./appsettings.json");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--dns=<ip-list>", 
        col_description_width, "DNS server addresses", 
        col_default_width, "1.1.1.1,8.8.8.8,2606:4700:4700::1111,2001:4860:4860::8888");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-flash=[yes|no]", 
        col_description_width, "Enable advanced QoS policy", 
        col_default_width, "no");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--auto-restart=<seconds>", 
        col_description_width, "Auto restart interval", 
        col_default_width, "0 (disabled)");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--log-file=<path>", 
        col_description_width, "LOG_DEBUG output file (Debug builds only)", 
        col_default_width, "console only");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--link-restart=<count>", 
        col_description_width, "Link reconnection attempts", 
        col_default_width, "0");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--block-quic=[yes|no]", 
        col_description_width, "Block QUIC protocol traffic", 
        col_default_width, "no");

    printf("└──────────────────────────────────────────┴──────────────────────────────────────────────────┴─────────────────────────┘\n\n");
    
    // SERVER-SPECIFIC OPTIONS table
    printf("SERVER-SPECIFIC OPTIONS:\n");
    printf("┌──────────────────────────────────────────┬──────────────────────────────────────────────────┬─────────────────────────┐\n");
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "OPTION", 
        col_description_width, "DESCRIPTION", 
        col_default_width, "DEFAULT");
    printf("├──────────────────────────────────────────┼──────────────────────────────────────────────────┼─────────────────────────┤\n");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--firewall-rules=<file>", 
        col_description_width, "Firewall rules file", 
        col_default_width, "./firewall_rules.txt");
    
    printf("└──────────────────────────────────────────┴──────────────────────────────────────────────────┴─────────────────────────┘\n\n");
    
    // CLIENT-SPECIFIC OPTIONS table
    printf("CLIENT-SPECIFIC OPTIONS:\n");
    printf("┌──────────────────────────────────────────┬──────────────────────────────────────────────────┬─────────────────────────┐\n");
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "OPTION", 
        col_description_width, "DESCRIPTION", 
        col_default_width, "DEFAULT");
    printf("├──────────────────────────────────────────┼──────────────────────────────────────────────────┼─────────────────────────┤\n");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--lwip=[yes|no]",
        col_description_width, "Network protocol stack selection",
        col_default_width,
#if defined(_WIN32)
        ppp::tap::TapWindows::IsWintun() ? "no" : "yes"
#else
        "no"
#endif
    );
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--nic=<interface>", 
        col_description_width, "Specify physical network interface", 
        col_default_width, "auto-select");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--ngw=<ip>", 
        col_description_width, "Force gateway address", 
        col_default_width, "auto-detect");
        
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun=<name>", 
        col_description_width, "Virtual adapter name", 
        col_default_width, NetworkInterface::GetDefaultTun().c_str());

#if defined(_WIN32)
    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--tun-driver=<mode>",
        col_description_width, "Adapter driver: auto, wintun, or tap",
        col_default_width, "auto");
#endif
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-ip=<ip>", 
        col_description_width, "Virtual adapter IP address", 
        col_default_width, "10.0.0.2");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-gw=<ip>", 
        col_description_width, "Virtual adapter gateway", 
        col_default_width, "10.0.0.1");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-mask=<bits>", 
        col_description_width, "Subnet mask bits", 
        col_default_width, "30");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-vnet=[yes|no]", 
        col_description_width, "Enable subnet forwarding", 
        col_default_width, "yes");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-host=[yes|no]", 
        col_description_width, "Prefer host network", 
        col_default_width, "yes");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-static=[yes|no]", 
        col_description_width, "Enable static tunnel", 
        col_default_width, "no");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-mux=<connections>", 
        col_description_width, "MUX connection count (0=disabled)", 
        col_default_width, "0");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-mux-acceleration=<mode>", 
        col_description_width, "MUX acceleration mode (0-3)", 
        col_default_width, "0 (standard)");
    
#if defined(_LINUX) || defined(_MACOS)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-promisc=[yes|no]", 
        col_description_width, "Enable promiscuous mode", 
        col_default_width, "yes");
#endif
    
#if defined(_MACOS)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-ssmt=<threads>", 
        col_description_width, "SSMT thread optimization", 
        col_default_width, "0");
#elif defined(_LINUX)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-ssmt=<N>[/<mode>]", 
        col_description_width, "SSMT threads (N), mode: st or mq (optional)", 
        col_default_width, "0/st");
#endif
    
#if defined(_LINUX)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-route=[yes|no]", 
        col_description_width, "Route compatibility", 
        col_default_width, "no");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-protect=[yes|no]", 
        col_description_width, "Route protection", 
        col_default_width, "yes");
#endif
    
#if defined(_WIN32)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--tun-lease-time-in-seconds=<sec>", 
        col_description_width, "DHCP lease time", 
        col_default_width, "7200");
#endif
    
    printf("└──────────────────────────────────────────┴──────────────────────────────────────────────────┴─────────────────────────┘\n\n");
    
    // ROUTING OPTIONS table
    printf("ROUTING OPTIONS:\n");
    printf("┌──────────────────────────────────────────┬──────────────────────────────────────────────────┬─────────────────────────┐\n");
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "OPTION", 
        col_description_width, "DESCRIPTION", 
        col_default_width, "DEFAULT");
    printf("├──────────────────────────────────────────┼──────────────────────────────────────────────────┼─────────────────────────┤\n");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--bypass-mode=<ip|geo|no>",
        col_description_width, "Select bypass policy engine",
        col_default_width, "ip");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--bypass=<file1|file2>", 
        col_description_width, "Bypass IP list file", 
        col_default_width, "./ip.txt");

#if defined(_LINUX)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--bypass-nic=<interface>", 
        col_description_width, "Interface for bypass list", 
        col_default_width, "auto-select");
#endif
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--bypass-ngw=<ip>", 
        col_description_width, "Gateway for bypass list", 
        col_default_width, "auto-detect");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--bypass6=<file1|file2>", 
        col_description_width, "IPv6 bypass list file", 
        col_default_width, "./ipv6.txt");

#if defined(_LINUX)
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--bypass-nic6=<interface>", 
        col_description_width, "Interface for IPv6 bypass list", 
        col_default_width, "auto-select");
#endif
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--bypass-ngw6=<ip>", 
        col_description_width, "Gateway for IPv6 bypass list", 
        col_default_width, "auto-detect");
    
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--dns-rules=<file>", 
        col_description_width, "DNS rules configuration", 
        col_default_width, "./dns_rules.txt");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--geo-rules=<file>",
        col_description_width, "Geo routing rules",
        col_default_width, "./geo-rules.yaml");

    printf("  %-*s  %-*s  %-*s\r\n",
        col_option_width, "--server-dir=<directory>",
        col_description_width, "Server JSON directory for live switching",
        col_default_width, "disabled");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--geosite=<file>",
        col_description_width, "Mihomo geosite database",
        col_default_width, "./geosite.dat");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--geoip=<file>",
        col_description_width, "Mihomo geoip database",
        col_default_width, "./geoip.dat");
    
    printf("└──────────────────────────────────────────┴──────────────────────────────────────────────────┴─────────────────────────┘\n\n");
    
    // WINDOWS-SPECIFIC COMMANDS table
#if defined(_WIN32)
    printf("WINDOWS-SPECIFIC COMMANDS:\n");
    printf("┌──────────────────────────────────────────┬──────────────────────────────────────────────────┐\n");
    printf("│ %-*s │ %-*s │\n", 
        col_command_width_utlity, "COMMAND", 
        col_description_width, "DESCRIPTION");
    printf("├──────────────────────────────────────────┼──────────────────────────────────────────────────┤\n");
    
    printf("│ %-*s │ %-*s │\n", 
        col_command_width_utlity, "--system-network-reset", 
        col_description_width, "Reset Windows network stack");
    
    printf("│ %-*s │ %-*s │\n", 
        col_command_width_utlity, "--system-network-optimization", 
        col_description_width, "Optimize network performance");
    
    printf("│ %-*s │ %-*s │\n", 
        col_command_width_utlity, "--system-network-preferred-ipv4", 
        col_description_width, "Set IPv4 as preferred protocol");
    
    printf("│ %-*s │ %-*s │\n", 
        col_command_width_utlity, "--system-network-preferred-ipv6", 
        col_description_width, "Set IPv6 as preferred protocol");
    
    printf("│ %-*s │ %-*s │\n", 
        col_command_width_utlity, "--no-lsp <program>", 
        col_description_width, "Disable LSP for specified program");
    
    printf("└──────────────────────────────────────────┴──────────────────────────────────────────────────┘\n\n");
#endif
    
    // UTILITY COMMANDS table
    printf("UTILITY COMMANDS:\n");
    printf("┌──────────────────────────────────────────┬──────────────────────────────────────────────────┬─────────────────────────┐\n");
    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "COMMAND", 
        col_description_width, "DESCRIPTION", 
        col_default_width, "DEFAULT");
    printf("├──────────────────────────────────────────┼──────────────────────────────────────────────────┼─────────────────────────┤\n");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--help", 
        col_description_width, "Display this help information", 
        col_default_width, "none");

    printf("│ %-*s │ %-*s │ %-*s │\n", 
        col_option_width, "--pull-iplist [file/country]", 
        col_description_width, "Download country IP list from APNIC", 
        col_default_width, "./ip.txt/CN");

    printf("└──────────────────────────────────────────┴──────────────────────────────────────────────────┴─────────────────────────┘\n\n");
    
    // Dependencies information
    printf("DEPENDENCIES:\n");
    printf("    boost@%s", GetBoostVersionString().c_str());
    
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__)
    printf(", libc@%s", GetVersionString(__GLIBC__, __GLIBC_MINOR__).c_str());
#if defined(__MUSL__)
    printf("/musl");
#else
    printf("/glibc");
#endif
#endif
    
#if defined(LIBCURL_VERSION_MAJOR)
    printf(", curl@%s", GetVersionString(LIBCURL_VERSION_MAJOR, LIBCURL_VERSION_MINOR, LIBCURL_VERSION_PATCH).c_str());
#endif
    
#if defined(OPENSSL_VERSION_MAJOR)
    printf(", openssl@%s", GetVersionString(OPENSSL_VERSION_MAJOR, OPENSSL_VERSION_MINOR, OPENSSL_VERSION_PATCH).c_str());
#else
    printf(", openssl@1.1.1");
#endif
    
#if defined(JEMALLOC_VERSION_MAJOR)
    printf(", jemalloc@%s", GetVersionString(JEMALLOC_VERSION_MAJOR, JEMALLOC_VERSION_MINOR, JEMALLOC_VERSION_BUGFIX).c_str());
#endif
    
    printf("\n");
}

// Parse IP address or netmask from command line
boost::asio::ip::address PppApplication::GetNetworkAddress(const char* name, int MIN_PREFIX_ADDRESS, int MAX_PREFIX_ADDRESS, int argc, const char* argv[]) noexcept
{
    ppp::string address_string = ppp::GetCommandArgument(name, argc, argv);
    if (address_string.empty())
    {
        return boost::asio::ip::address_v4::any();
    }

    address_string = ppp::LTrim<ppp::string>(address_string);
    address_string = ppp::RTrim<ppp::string>(address_string);
    if (address_string.empty())
    {
        return boost::asio::ip::address_v4::any();
    }

    boost::asio::ip::address address;
    if (StringAuxiliary::WhoisIntegerValueString(address_string))
    {
        // Handle netmask prefix notation (e.g., "24")
        int prefix = atoll(address_string.data());
        if (prefix < 1 || prefix > MAX_PREFIX_ADDRESS)
        {
            prefix = MAX_PREFIX_ADDRESS;
        }
        elif(MIN_PREFIX_ADDRESS > 0 && prefix < MIN_PREFIX_ADDRESS)
        {
            prefix = MIN_PREFIX_ADDRESS;
        }

        auto prefix_to_netmask = IPEndPoint::PrefixToNetmask(prefix);
        address = IPEndPoint::WrapAddressV4<boost::asio::ip::tcp>(prefix_to_netmask, 0).address();
    }
    else
    {
        // Handle dotted-decimal notation
        address = Ipep::ToAddress(address_string, true);
    }

    if (IPEndPoint::IsInvalid(address))
    {
        return boost::asio::ip::address_v4::any();
    }

    return address;
}

// Parse IP address with default value
boost::asio::ip::address PppApplication::GetNetworkAddress(const char* name, int MIN_PREFIX_ADDRESS, int MAX_PREFIX_ADDRESS, const char* default_address_string, int argc, const char* argv[]) noexcept
{
    boost::asio::ip::address address = GetNetworkAddress(name, MIN_PREFIX_ADDRESS, MAX_PREFIX_ADDRESS, argc, argv);
    if (IPEndPoint::IsInvalid(address))
    {
        address = boost::asio::ip::address_v4::any();
    }

    if (IPEndPoint::IsInvalid(address))
    {
        if (NULLPTR == default_address_string)
        {
            default_address_string = "";
        }

        return Ipep::ToAddress(default_address_string, false);
    }
    else
    {
        return address;
    }
}

// Parse DNS server addresses from command line
void PppApplication::GetDnsAddresses(ppp::vector<boost::asio::ip::address>& addresses, int argc, const char* argv[]) noexcept
{
#if defined(_WIN32)
    bool at_least_two = true;
    if (!client_mode_) {
        at_least_two = false;
    }

#else
    bool at_least_two = false;
#endif

    ppp::string dns = ppp::GetCommandArgument("--dns", argc, argv);
    if (Ipep::ToDnsAddresses(dns, addresses, at_least_two) < 1) {
        boost::system::error_code ec;
        addresses.emplace_back(ppp::StringToAddress(PPP_PREFERRED_DNS_SERVER_1, ec));
        addresses.emplace_back(ppp::StringToAddress(PPP_PREFERRED_DNS_SERVER_2, ec));

        // Read IPv6 DNS from config file instead of hardcoded defaults,
        // avoiding INADDR_NONE (255.255.255.255) in TAP display.
        if (configuration_ != NULLPTR) {
            if (!configuration_->server.ipv6.dns1.empty()) {
                boost::system::error_code ec6;
                auto addr6 = ppp::StringToAddress(configuration_->server.ipv6.dns1, ec6);
                if (!ec6 && addr6.is_v6()) {
                    addresses.emplace_back(addr6);
                }
            }
            if (!configuration_->server.ipv6.dns2.empty()) {
                boost::system::error_code ec6;
                auto addr6 = ppp::StringToAddress(configuration_->server.ipv6.dns2, ec6);
                if (!ec6 && addr6.is_v6()) {
                    addresses.emplace_back(addr6);
                }
            }
        }
    }
}

// Parse network interface configuration from command line arguments
std::shared_ptr<NetworkInterface> PppApplication::GetNetworkInterface(int argc, const char* argv[]) noexcept
{
    std::shared_ptr<NetworkInterface> ni = ppp::make_shared_object<NetworkInterface>();
    if (NULLPTR != ni)
    {
#if defined(_WIN32)
        ppp::string tun_driver = ToLower(ppp::LTrim(ppp::RTrim(ppp::GetCommandArgument("--tun-driver", argc, argv, "auto"))));
        if (tun_driver == "wintun")
        {
            ppp::tap::TapWindows::SetDriverMode(ppp::tap::TapWindows::DriverMode::Wintun);
        }
        elif(tun_driver == "tap")
        {
            ppp::tap::TapWindows::SetDriverMode(ppp::tap::TapWindows::DriverMode::Tap);
        }
        else
        {
            if (tun_driver != "auto")
            {
                fprintf(stdout, "Unknown --tun-driver value '%s'; using auto.\r\n", tun_driver.data());
            }
            ppp::tap::TapWindows::SetDriverMode(ppp::tap::TapWindows::DriverMode::Auto);
        }

        ni->Lwip = ppp::ToBoolean(ppp::GetCommandArgument("--lwip", argc, argv, ppp::tap::TapWindows::IsWintun() ? ppp::string() : "y").data());
#else
        ni->Lwip = ppp::ToBoolean(ppp::GetCommandArgument("--lwip", argc, argv).data());
#endif

        ni->Nic = ppp::RTrim(ppp::LTrim(ppp::GetCommandArgument("--nic", argc, argv)));
        ni->BlockQUIC = ppp::ToBoolean(ppp::GetCommandArgument("--block-quic", argc, argv).data());

        // Parse DNS servers
        GetDnsAddresses(ni->DnsAddresses, argc, argv);
        if (!ni->DnsAddresses.empty()) {
            auto dns_servers = ppp::net::asio::vdns::servers;
            dns_servers->clear();

            for (const boost::asio::ip::address& dns_server : ni->DnsAddresses) {
                dns_servers->emplace_back(boost::asio::ip::udp::endpoint(dns_server, PPP_DNS_SYS_PORT));
            }
        }

        // Parse network addresses
        ni->Ngw = GetNetworkAddress("--ngw", 0, 32, "0.0.0.0", argc, argv);
        ni->IPAddress = GetNetworkAddress("--tun-ip", 0, 32, "10.0.0.2", argc, argv);
        ni->SubmaskAddress = GetNetworkAddress("--tun-mask", 16, 32, "255.255.255.252", argc, argv);

        // Suggested Ethernet card address setting.
        ni->GatewayServer = GetNetworkAddress("--tun-gw", 0, 32, "10.0.0.1", argc, argv);

#if defined(_WIN32)
        // DHCP-MASQ lease time in seconds.
        ni->LeaseTimeInSeconds = strtoul(ppp::GetCommandArgument("--tun-lease-time-in-seconds", argc, argv).data(), NULLPTR, 10);
        if (ni->LeaseTimeInSeconds < 1)
        {
            ni->LeaseTimeInSeconds = 7200;
        }
#endif

        // Calculate valid IP address based on gateway and subnet
        ni->IPAddress = Ipep::FixedIPAddress(ni->IPAddress, ni->GatewayServer, ni->SubmaskAddress);
        ni->StaticMode = ppp::ToBoolean(ppp::GetCommandArgument("--tun-static", argc, argv).data());
        ni->HostedNetwork = ppp::ToBoolean(ppp::GetCommandArgument("--tun-host", argc, argv, "y").data());
        ni->VNet = ppp::ToBoolean(ppp::GetCommandArgument("--tun-vnet", argc, argv, "y").data());

        ppp::string bypass_mode = ToLower(ppp::LTrim(ppp::RTrim(
            ppp::GetCommandArgument("--bypass-mode", argc, argv, "ip"))));
        if (bypass_mode == "ip") {
            ni->SplitMode = NetworkInterface::BypassMode::Ip;
        }
        elif(bypass_mode == "geo") {
            ni->SplitMode = NetworkInterface::BypassMode::Geo;
        }
        elif(bypass_mode == "no") {
            ni->SplitMode = NetworkInterface::BypassMode::No;
        }
        else {
            fprintf(stdout, "Invalid --bypass-mode '%s'; expected ip, geo, or no.\r\n", bypass_mode.data());
            return NULLPTR;
        }

        ni->GeoRules = File::GetFullPath(File::RewritePath(ppp::GetCommandArgument(
            "--geo-rules", argc, argv, "./geo-rules.yaml").data()).data());
        ni->GeoSite = File::GetFullPath(File::RewritePath(ppp::GetCommandArgument(
            "--geosite", argc, argv, "./geosite.dat").data()).data());
        ni->GeoIP = File::GetFullPath(File::RewritePath(ppp::GetCommandArgument(
            "--geoip", argc, argv, "./geoip.dat").data()).data());

#if defined(_LINUX)
        ni->BypassNic = ppp::RTrim(ppp::LTrim(ppp::GetCommandArgument("--bypass-nic", argc, argv)));
#endif
        ni->BypassNgw = GetNetworkAddress("--bypass-ngw", 0, 32, "0.0.0.0", argc, argv);
        ni->BypassLoadList(File::GetFullPath(File::RewritePath(ppp::LTrim(ppp::RTrim(ppp::GetCommandArgument("--bypass", argc, argv, "./ip.txt"))).data()).data()));

#if defined(_LINUX)
        ni->BypassNic6 = ppp::RTrim(ppp::LTrim(ppp::GetCommandArgument("--bypass-nic6", argc, argv)));
#endif
        ni->BypassNgw6 = GetNetworkAddress("--bypass-ngw6", 0, 128, "::", argc, argv);
        ni->BypassLoadList6(File::GetFullPath(File::RewritePath(ppp::LTrim(ppp::RTrim(ppp::GetCommandArgument("--bypass6", argc, argv, "./ipv6.txt"))).data()).data()));

        // Parse configuration files
        ni->DNSRules = ppp::GetCommandArgument("--dns-rules", argc, argv, "./dns-rules.txt");
        ni->FirewallRules = ppp::GetCommandArgument("--firewall-rules", argc, argv, "./firewall-rules.txt");
        
        // Parse MUX settings
        ni->Mux = (uint16_t)std::max<int>(0, atoi(ppp::GetCommandArgument("--tun-mux", argc, argv).data()));
        ni->MuxAcceleration = (uint8_t)std::max<int>(0, atoi(ppp::GetCommandArgument("--tun-mux-acceleration", argc, argv).data()));
        if (ni->MuxAcceleration > PPP_MUX_ACCELERATION_MAX) 
        {
            ni->MuxAcceleration = 0;
        }

#if defined(_WIN32)
        ni->SetHttpProxy = ppp::ToBoolean(ppp::GetCommandArgument("--set-http-proxy", argc, argv).data());
        ni->Wintun = ppp::GetCommandArgument("--tun", argc, argv, NetworkInterface::GetDefaultTun());
        ni->ComponentId = ppp::tap::TapWindows::FindComponentId(ni->Wintun);
#else
        ni->ComponentId = ppp::GetCommandArgument("--tun", argc, argv, NetworkInterface::GetDefaultTun());

#if defined(_LINUX)
        // Enable route compatibility mode if requested
        if (ppp::ToBoolean(ppp::GetCommandArgument("--tun-route", argc, argv).data())) 
        {
            ppp::tap::TapLinux::CompatibleRoute(true);
        }

        // Linux requires network protection services to be turned on, but this may not be compatible on some Linux devices.
        ni->ProtectNetwork = ppp::ToBoolean(ppp::GetCommandArgument("--tun-protect", argc, argv, "y").data());
        ni->Ssmt = 0;
        ni->SsmtMQ = false;

        // Parse SSMT configuration
        if (ppp::string ssmt = ppp::GetCommandArgument("--tun-ssmt", argc, argv); !ssmt.empty()) 
        {
            char ssmt_mq_keys[] = { 'm', 'q' };
            for (int j = 0; j < arraysizeof(ssmt_mq_keys); j++) 
            { 
                if (ssmt.find(ssmt_mq_keys[j]) != ppp::string::npos) 
                {
                    ni->SsmtMQ = true;
                    break;
                }
            }

            ni->Ssmt = std::max<int>(0, atoi(ssmt.data()));
        }
#elif defined(_MACOS)
        ni->Ssmt = std::max<int>(0, atoi(ppp::GetCommandArgument("--tun-ssmt", argc, argv).data()));
#endif

#if defined(_MACOS) || defined(_LINUX)
        // MacOS/Linux Virtual Ethernet is set to the promiscuous NIC mode by default.
        ni->Promisc = ppp::ToBoolean(ppp::GetCommandArgument("--tun-promisc", argc, argv, "y").data());
#endif
#endif

        // Clean up component ID
        ni->ComponentId = ppp::LTrim<ppp::string>(ni->ComponentId);
        ni->ComponentId = ppp::RTrim<ppp::string>(ni->ComponentId);
    }
    return ni;
}

// Determine if application should run in client or server mode
bool PppApplication::IsModeClientOrServer(int argc, const char* argv[]) noexcept
{
    static constexpr const char* keys[] = { "--mode", "--m", "-mode", "-m" };

    ppp::string mode_string;
    for (const char* key : keys)
    {
        mode_string = ppp::GetCommandArgument(key, argc, argv);
        if (mode_string.size() > 0)
        {
            break;
        }
    }

    if (mode_string.empty())
    {
        mode_string = "server";
    }

    mode_string = ppp::ToLower<ppp::string>(mode_string);
    mode_string = ppp::LTrim<ppp::string>(mode_string);
    mode_string = ppp::RTrim<ppp::string>(mode_string);
    return mode_string.empty() ? false : mode_string[0] == 'c';
}

// Clean up resources
void PppApplication::Dispose() noexcept
{
    // Clean up server
    std::shared_ptr<VirtualEthernetSwitcher> server = std::move(server_);
    if (NULLPTR != server)
    {
        server->Dispose();
    }

    // Clean up client
    std::shared_ptr<VEthernetNetworkSwitcher> client = std::move(client_);
    if (NULLPTR != client)
    {
#if defined(_WIN32)
        // Restore original QUIC settings
        ppp::net::proxies::HttpProxy::SetSupportExperimentalQuicProtocol(quic_);

        // Clear system proxy settings
        if (network_interface_->SetHttpProxy)
        {
            client->ClearHttpProxyToSystemEnv();
        }
#endif

        client->Dispose();
    }

    ClearTickAlwaysTimeout();
}

// Get transmission statistics from current switcher
bool PppApplication::GetTransmissionStatistics(uint64_t& incoming_traffic, uint64_t& outgoing_traffic, std::shared_ptr<ppp::transmissions::ITransmissionStatistics>& statistics_snapshot) noexcept
{
    // Initialization requires the initial value of the FAR outgoing parameter.
    statistics_snapshot = NULLPTR;
    incoming_traffic = 0;
    outgoing_traffic = 0;

    // Get statistics from active switcher
    std::shared_ptr<VirtualEthernetSwitcher> server = server_;
    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    if ((NULLPTR != server && !server->IsDisposed()) || (NULLPTR != client && !client->IsDisposed()))
    {
        // Obtain transport layer traffic statistics from the client switch or server switch management object.
        std::shared_ptr<ppp::transmissions::ITransmissionStatistics> transmission_statistics;
        if (NULLPTR != client)
        {
            transmission_statistics = client->GetStatistics();
        }
        elif(NULLPTR != server)
        {
            transmission_statistics = server->GetStatistics();
        }

        if (NULLPTR != transmission_statistics)
        {
            return ppp::transmissions::ITransmissionStatistics::GetTransmissionStatistics(transmission_statistics, transmission_statistics_, incoming_traffic, outgoing_traffic, statistics_snapshot);
        }
    }

    return false;
}

// Main periodic tick handler
bool PppApplication::OnTick(uint64_t now) noexcept
{
    using RouteIPListTablePtr = VEthernetNetworkSwitcher::RouteIPListTablePtr;
    using NetworkState        = VEthernetExchanger::NetworkState;

    // Handle console keyboard input for tab switching
    HandleConsoleInput();

    // Update console display
    PrintEnvironmentInformation();

    // Check auto-restart timer
    if (GLOBAL_.auto_restart > 0)
    {
        int64_t elapsed_milliseconds = stopwatch_.ElapsedMilliseconds() / 1000;
        if (elapsed_milliseconds > 0 && elapsed_milliseconds >= GLOBAL_.auto_restart)
        {
            return ShutdownApplication(true);
        }
    }

    // Client-specific periodic tasks
    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    if (NULLPTR == client) 
    {
        return false;
    }

    // Check whether the current VPN exchanger exists.
    std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger(); 
    if (NULLPTR == exchanger)
    {
        return false;
    }

    // Check link status
    NetworkState network_state = exchanger->GetNetworkState();
    if (network_state == NetworkState::NetworkState_Established) 
    {
        // Handle link restart count
        if (GLOBAL_.link_restart > 0) 
        {
            // If the number of link reconnections exceeds a certain number, the program needs to be restarted immediately.
            if (exchanger->GetReconnectionCount() >= GLOBAL_.link_restart)
            {
                return ShutdownApplication(true);
            }
        }
    }
    else 
    {
        return false;
    }

    return true;
}

// Start/stop periodic tick timer
bool PppApplication::NextTickAlwaysTimeout(bool next) noexcept
{
    std::shared_ptr<boost::asio::io_context> context = Executors::GetDefault();
    if (NULLPTR == context)
    {
        return false;
    }

    std::shared_ptr<PppApplication> app = DEFAULT_;
    if (NULLPTR == app)
    {
        return false;
    }

    std::shared_ptr<VirtualEthernetSwitcher> server = app->server_;
    std::shared_ptr<VEthernetNetworkSwitcher> client = app->client_;
    if (NULLPTR == server && NULLPTR == client)
    {
        return false;
    }

    // Create periodic timer
    std::shared_ptr<Timer> timeout = Timer::Timeout(context, 1000, 
        [](Timer*) noexcept
        {
            std::shared_ptr<PppApplication> app = DEFAULT_;
            if (NULLPTR != app)
            {
                app->NextTickAlwaysTimeout(true);
            }
        });
    if (NULLPTR == timeout)
    {
        return false;
    }
    elif(!next)
    {
        ppp::ClearConsoleOutputCharacter();
    }

    app->timeout_ = std::move(timeout);
    app->OnTick(Executors::GetTickCount());
    return true;
}

// Stop periodic tick timer
void PppApplication::ClearTickAlwaysTimeout() noexcept
{
    std::shared_ptr<Timer> timeout = std::move(timeout_);
    if (NULLPTR != timeout)
    {
        timeout->Dispose();
    }
}

// Get server switcher instance
std::shared_ptr<VirtualEthernetSwitcher> PppApplication::GetServer() noexcept
{
    return server_;
}

// Get client switcher instance
std::shared_ptr<VEthernetNetworkSwitcher> PppApplication::GetClient() noexcept
{
    return client_;
}

// Get application singleton instance
std::shared_ptr<PppApplication> PppApplication::GetDefault() noexcept
{
    return DEFAULT_;
}

// Get application configuration
std::shared_ptr<AppConfiguration> PppApplication::GetConfiguration() noexcept
{
    return configuration_;
}

bool PppApplication::LoadServerConfigurations(int argc, const char* argv[],
    const std::shared_ptr<AppConfiguration>& primary) noexcept
{
    server_directory_.clear();
    ppp::string value = ppp::ATrim<ppp::string>(
        ppp::GetCommandArgument("--server-dir", argc, argv));
    if (value.empty())
    {
        return true;
    }
    if (NULLPTR == primary)
    {
        return false;
    }

    bool has_main = false;
    for (const ClientOutboundConfiguration& outbound : outbound_configurations_)
    {
        if (ppp::ToLower<ppp::string>(outbound.tag) == "main")
        {
            has_main = true;
            break;
        }
    }
    if (!has_main)
    {
        outbound_configurations_.insert(outbound_configurations_.begin(),
            ClientOutboundConfiguration{ "main", primary, "main", false, ppp::string(), false });
    }

    server_directory_ = File::GetFullPath(File::RewritePath(value.data()).data());
    ppp::vector<ppp::string> files;
    if (server_directory_.empty() ||
        !File::GetAllFileNames(server_directory_.data(), false, files))
    {
        fprintf(stdout, "Server configuration directory cannot be read: %s\r\n",
            value.data());
        return false;
    }
    std::sort(files.begin(), files.end());

    ppp::unordered_set<ppp::string> tags;
    std::size_t loaded = 0;
    for (const ppp::string& file : files)
    {
        ppp::string file_name = File::GetFileName(file.data());
        ppp::string lower_name = ppp::ToLower<ppp::string>(file_name);
        if (lower_name.size() < 6 ||
            lower_name.compare(lower_name.size() - 5, 5, ".json") != 0)
        {
            continue;
        }

        ppp::string display_name = file_name.substr(0, file_name.size() - 5);
        ppp::string tag_base = "server:" + ppp::ToLower<ppp::string>(display_name);
        ppp::string tag = tag_base;
        for (std::size_t suffix = 2; !tags.emplace(tag).second; ++suffix)
        {
            tag = tag_base + "-" + stl::to_string<ppp::string>(suffix);
        }

        std::shared_ptr<AppConfiguration> configuration =
            ppp::make_shared_object<AppConfiguration>();
        if (NULLPTR == configuration || !configuration->Load(file))
        {
            fprintf(stdout, "Failed to load server configuration: %s\r\n", file.data());
            return false;
        }

        // Do not open the primary configuration twice when its JSON also lives
        // in --server-dir. Duplicate sessions with the same GUID/server can make
        // the control (main) handshake lose to its menu copy, leaving network
        // takeover waiting forever even though secondary links are connected.
        if (configuration->client.guid == primary->client.guid &&
            configuration->client.server == primary->client.server)
        {
            for (ClientOutboundConfiguration& outbound : outbound_configurations_)
            {
                if (ppp::ToLower<ppp::string>(outbound.tag) == "main")
                {
                    outbound.display_name = display_name;
                    outbound.server_menu = true;
                    outbound.source_path = file;
                    break;
                }
            }
            ++loaded;
            continue;
        }

#if defined(_WIN32)
        bool initialize_allocator = configuration->vmem.size > 0;
#else
        bool initialize_allocator = configuration->vmem.path.size() > 0 &&
            configuration->vmem.size > 0;
#endif
        if (initialize_allocator)
        {
            std::shared_ptr<BufferswapAllocator> allocator =
                ppp::make_shared_object<BufferswapAllocator>(
                    configuration->vmem.path,
                    std::max<int64_t>((int64_t)1LL << (int64_t)25LL,
                        (int64_t)configuration->vmem.size << (int64_t)20LL));
            if (NULLPTR != allocator && allocator->IsVaild())
            {
                configuration->SetBufferAllocator(allocator);
            }
        }

        outbound_configurations_.emplace_back(ClientOutboundConfiguration{
            tag, configuration, display_name, true, file, false });
        ++loaded;
    }

    if (loaded == 0)
    {
        fprintf(stdout, "Server configuration directory contains no JSON files: %s\r\n",
            server_directory_.data());
        return false;
    }
    return true;
}

bool PppApplication::LoadGeoOutboundConfigurations(
    const std::shared_ptr<NetworkInterface>& network_interface,
    const std::shared_ptr<AppConfiguration>& primary) noexcept
{
    using GeoRuleEngine = ppp::app::client::geo::GeoRuleEngine;
    if (NULLPTR == network_interface || NULLPTR == primary ||
        network_interface->SplitMode != NetworkInterface::BypassMode::Geo ||
        network_interface->GeoRules.empty())
    {
        return true;
    }

    ppp::vector<GeoRuleEngine::OutboundConfiguration> declarations;
    ppp::string final_outbound;
    ppp::string error;
    if (!GeoRuleEngine::ParseOutboundConfigurations(
        network_interface->GeoRules, declarations, final_outbound, error))
    {
        fprintf(stdout, "Geo rules error: %s\r\n", error.data());
        return false;
    }
    if (declarations.empty())
    {
        return true;
    }

    bool has_main = false;
    for (ClientOutboundConfiguration& outbound : outbound_configurations_)
    {
        if (ppp::ToLower<ppp::string>(outbound.tag) == "main")
        {
            has_main = true;
            break;
        }
    }
    if (!has_main)
    {
        outbound_configurations_.insert(outbound_configurations_.begin(),
            ClientOutboundConfiguration{ "main", primary, "main", false, ppp::string(), false });
    }

    for (const GeoRuleEngine::OutboundConfiguration& declaration : declarations)
    {
        if (declaration.primary)
        {
            continue;
        }

        std::shared_ptr<AppConfiguration> configuration =
            ppp::make_shared_object<AppConfiguration>();
        if (NULLPTR == configuration || !configuration->Load(declaration.path))
        {
            fprintf(stdout, "Failed to load geo outbound '%s': %s\r\n",
                declaration.tag.data(), declaration.path.data());
            return false;
        }

        ClientOutboundConfiguration* matched = NULLPTR;
        for (ClientOutboundConfiguration& outbound : outbound_configurations_)
        {
            if (NULLPTR != outbound.configuration &&
                outbound.configuration->client.guid == configuration->client.guid &&
                outbound.configuration->client.server == configuration->client.server)
            {
                matched = &outbound;
                break;
            }
        }

        if (NULLPTR != matched)
        {
            // Reuse the server-directory connection under the stable YAML tag.
            // This avoids opening the same GUID/server twice and makes a rule
            // such as "geosite,openai,us" select the visible zgo menu entry.
            matched->tag = declaration.tag;
            matched->source_path = declaration.path;
            matched->route_used = true;
            continue;
        }

        outbound_configurations_.emplace_back(ClientOutboundConfiguration{
            declaration.tag, configuration, declaration.tag, false, declaration.path, true });
    }
    return true;
}

// Load configuration from file
std::shared_ptr<AppConfiguration> PppApplication::LoadConfiguration(int argc, const char* argv[], ppp::string& path) noexcept
{
    static constexpr const char* argument_keys[] = { "-c", "--c", "-config", "--config" };
    outbound_configurations_.clear();
    geo_configuration_path_.clear();

    // Find configuration file from command line
    for (const char* argument_key : argument_keys)
    {
        ppp::string argument_value = ppp::GetCommandArgument(argument_key, argc, argv);
        if (argument_value.empty())
        {
            continue;
        }

        argument_value = File::RewritePath(argument_value.data());
        argument_value = File::GetFullPath(argument_value.data());
        if (argument_value.empty())
        {
            continue;
        }

        ppp::string argument_lower = ppp::ToLower<ppp::string>(argument_value);
        bool requested_geo_manifest = argument_lower.size() >= 4 &&
            argument_lower.compare(argument_lower.size() - 4, 4, ".txt") == 0;
        if (requested_geo_manifest && !File::CanAccess(argument_value.data(), FileAccess::Read))
        {
            fprintf(stdout, "Geo configuration cannot be read: %s\r\n", argument_value.data());
            return NULLPTR;
        }

        if (File::CanAccess(argument_value.data(), FileAccess::Read))
        {
            path = std::move(argument_value);
            break;
        }
    }

    // A .txt file passed as --config is a geo multi-outbound manifest.  It is
    // parsed before the normal JSON configuration because its main= entry
    // identifies the primary AppConfiguration used for all system-level state.
    ppp::string selected_lower = ppp::ToLower<ppp::string>(path);
    bool geo_manifest = selected_lower.size() >= 4 &&
        selected_lower.compare(selected_lower.size() - 4, 4, ".txt") == 0;
    if (geo_manifest)
    {
        using GeoRuleEngine = ppp::app::client::geo::GeoRuleEngine;
        ppp::vector<GeoRuleEngine::OutboundConfiguration> declarations;
        ppp::string final_outbound;
        ppp::string error;
        if (!GeoRuleEngine::ParseOutboundConfigurations(path, declarations, final_outbound, error) || declarations.empty())
        {
            if (error.empty()) error = "geo configuration contains no outbound declarations";
            fprintf(stdout, "Geo configuration error: %s\r\n", error.data());
            path.clear();
            return NULLPTR;
        }

        std::shared_ptr<AppConfiguration> primary;
        for (const GeoRuleEngine::OutboundConfiguration& declaration : declarations)
        {
            std::shared_ptr<AppConfiguration> configuration = ppp::make_shared_object<AppConfiguration>();
            if (NULLPTR == configuration || !configuration->Load(declaration.path))
            {
                fprintf(stdout, "Failed to load outbound '%s' configuration: %s\r\n",
                    declaration.tag.data(), declaration.path.data());
                outbound_configurations_.clear();
                path.clear();
                return NULLPTR;
            }

#if defined(_WIN32)
            bool initialize_allocator = configuration->vmem.size > 0;
#else
            bool initialize_allocator = configuration->vmem.path.size() > 0 && configuration->vmem.size > 0;
#endif
            if (initialize_allocator)
            {
                std::shared_ptr<BufferswapAllocator> allocator = ppp::make_shared_object<BufferswapAllocator>(configuration->vmem.path,
                    std::max<int64_t>((int64_t)1LL << (int64_t)25LL, (int64_t)configuration->vmem.size << (int64_t)20LL));
                if (NULLPTR != allocator && allocator->IsVaild()) configuration->SetBufferAllocator(allocator);
            }

            outbound_configurations_.emplace_back(ClientOutboundConfiguration{
                declaration.tag, configuration, declaration.tag, false, declaration.path, true });
            if (declaration.primary) primary = configuration;
        }

        if (NULLPTR == primary)
        {
            fprintf(stdout, "%s\r\n", "Geo configuration does not define a valid main outbound.");
            outbound_configurations_.clear();
            path.clear();
            return NULLPTR;
        }

        geo_configuration_path_ = path;
        return primary;
    }

    // Try default configuration file locations
    ppp::string configuration_paths[] =
    {
        path,
        "./config.json",
        "./appsettings.json",
    };
    for (ppp::string& configuration_path : configuration_paths)
    {
        configuration_path = File::GetFullPath(File::RewritePath(configuration_path.data()).data());
        if (!File::Exists(configuration_path.data()))
        {
            continue;
        }

        std::shared_ptr<AppConfiguration> configuration = ppp::make_shared_object<AppConfiguration>();
        if (NULLPTR == configuration)
        {
            continue;
        }

        if (!configuration->Load(configuration_path))
        {
            continue;
        }

        // Initialize buffer allocator if configured
#if defined(_WIN32)
        if (configuration->vmem.size > 0)
#else
        if (configuration->vmem.path.size() > 0 && configuration->vmem.size > 0)
#endif
        {
            std::shared_ptr<BufferswapAllocator> allocator = ppp::make_shared_object<BufferswapAllocator>(configuration->vmem.path,
                std::max<int64_t>((int64_t)1LL << (int64_t)25LL, (int64_t)configuration->vmem.size << (int64_t)20LL));
            if (NULLPTR != allocator && allocator->IsVaild())
            {
                configuration->SetBufferAllocator(allocator);
            }
        }

        path = configuration_path;
        return configuration;
    }

    path.clear();
    return NULLPTR;
}

// Shutdown application handler
bool PppApplication::OnShutdownApplication() noexcept 
{
    return ShutdownApplication(false);
}

// Trigger application shutdown or restart
bool PppApplication::ShutdownApplication(bool restart) noexcept 
{
    std::shared_ptr<boost::asio::io_context> context = Executors::GetDefault();
    if (NULLPTR == context)
    {
        return false;
    }
    else
    {
        GLOBAL_.restart |= restart;
        boost::asio::post(*context, 
            [restart, context]() noexcept
            {
                // References to move app application domains.
                std::shared_ptr<PppApplication> APP = std::move(DEFAULT_);
                if (NULLPTR == APP)
                {
                    return false;
                }

                // Output a prompt message that the current app is exiting.
                fprintf(stdout, "%s\r\n", restart ? "Application is restarting..." : "Application is shutting down...");

                // Release app instances.
                APP->Dispose();

                // Delay before exit to allow clean shutdown
                std::shared_ptr<Timer> timeout = Timer::Timeout(context, 1000, 
                    [](Timer*) noexcept
                    {
                        // Exit all the app loops.
                        Executors::Exit();
                    });
                return NULLPTR != timeout;
            });
        return true;
    }
}

// Register OS-specific shutdown handlers
bool PppApplication::AddShutdownApplicationEventHandler() noexcept
{
#if defined(_WIN32)
    return ppp::win32::Win32Native::AddShutdownApplicationEventHandler(PppApplication::OnShutdownApplication);
#else
    return ppp::unix__::UnixAfx::AddShutdownApplicationEventHandler(PppApplication::OnShutdownApplication);
#endif
}

// Windows-specific TAP driver installation
#if defined(_WIN32)
static bool Windows_PreparedEthernetEnvironment(const std::shared_ptr<NetworkInterface>& network_interface) noexcept
{
    fprintf(stdout, "[PrepEth] ComponentId='%s' Wintun='%s'\r\n", network_interface->ComponentId.data(), network_interface->Wintun.data());

    ppp::tap::TapWindows::DriverMode driver_mode = ppp::tap::TapWindows::GetDriverMode();
    bool use_wintun = driver_mode == ppp::tap::TapWindows::DriverMode::Wintun ||
        (driver_mode == ppp::tap::TapWindows::DriverMode::Auto && ppp::tap::TapWindows::IsWintun());
    if (use_wintun)
    {
        network_interface->ComponentId = network_interface->Wintun;
        fprintf(stdout, "[PrepEth] Wintun selected, preserving adapter name '%s' and skipping TAP installation\r\n",
            network_interface->ComponentId.data());
        return !network_interface->ComponentId.empty();
    }
    
    // Determine if we need to install: empty ComponentId, or raw name (not GUID) that needs TAP fallback
    bool is_guid = (network_interface->ComponentId.size() == 38 && network_interface->ComponentId[0] == '{');
    bool need_install = network_interface->ComponentId.empty() || !is_guid;
    
    if (need_install)
    {
        // First, check if a TAP adapter with this name already exists
        ppp::string existingGuid;
        ppp::string targetName = ppp::ToLower<ppp::string>(network_interface->Wintun);
        targetName = ppp::LTrim<ppp::string>(ppp::RTrim<ppp::string>(targetName));
        if (!targetName.empty())
        {
            ppp::unordered_set<ppp::string> allTapIds;
            if (ppp::tap::TapWindows::FindAllComponentIds(allTapIds) && !allTapIds.empty())
            {
                ppp::vector<ppp::win32::network::NetworkInterfacePtr> interfaces;
                if (ppp::win32::network::GetAllNetworkInterfaces(interfaces))
                {
                    for (const auto& ni : interfaces)
                    {
                        ppp::string connId = ppp::ToLower<ppp::string>(ni->ConnectionId);
                        connId = ppp::LTrim<ppp::string>(ppp::RTrim<ppp::string>(connId));
                        if (connId == targetName)
                        {
                            ppp::string guidLower = ppp::ToLower<ppp::string>(ni->Guid);
                            for (const auto& cid : allTapIds)
                            {
                                if (ppp::ToLower<ppp::string>(cid) == guidLower)
                                {
                                    existingGuid = cid;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }

        if (!existingGuid.empty())
        {
            fprintf(stdout, "[PrepEth] Found existing TAP '%s' with GUID %s, reusing\r\n",
                network_interface->Wintun.data(), existingGuid.data());
            network_interface->ComponentId = existingGuid;
        }
        else
        {
            fprintf(stdout, "[PrepEth] Need install (empty=%d, isGuid=%d), triggering InstallDriver...\r\n",
                network_interface->ComponentId.empty(), is_guid);
            LOG_INFO("%s", "Installing TAP-Windows driver.");

            // Install the TAP-Windows vNIC in the Windows operating system.
            ppp::string driverPath = File::GetFullPath((ppp::GetApplicationStartupPath() + "\\Driver\\").data());
            fprintf(stdout, "[PrepEth] driverPath=%s\r\n", driverPath.data());
            ppp::string newTapGuid = ppp::tap::TapWindows::InstallDriver(driverPath.data(), network_interface->Wintun);
            if (!newTapGuid.empty())
            {
                fprintf(stdout, "[PrepEth] InstallDriver OK, new TAP GUID: %s\r\n", newTapGuid.data());
                network_interface->ComponentId = newTapGuid;
            }
            else
            {
                fprintf(stdout, "[PrepEth] InstallDriver FAILED!\r\n");
            }
        }

        // The virtual Ethernet card device was not successfully deployed on your computer.
        if (network_interface->ComponentId.empty())
        {
            fprintf(stdout, "[PrepEth] FAIL: ComponentId still empty after install attempt\r\n");
            LOG_INFO("%s", "Failed to install TAP-Windows driver.");
            return false;
        }
        else
        {
            fprintf(stdout, "[PrepEth] OK: ComponentId='%s'\r\n", network_interface->ComponentId.data());
            LOG_INFO("%s", "Success to install TAP-Windows driver.");
        }
    }
    else
    {
        fprintf(stdout, "[PrepEth] ComponentId is a valid GUID, skipping install\r\n");
    }
    return true;
}

// Disable LSP for specific program
static bool Windows_NoLsp(int argc, const char* argv[]) noexcept
{
    char key[] = "--no-lsp";
    if (!ppp::HasCommandArgument(key, argc, argv))
    {
        return false;
    }

    bool ok = false;
    do
    {
        ppp::string line = ppp::GetCommandArgument(argc, argv);
        if (line.empty())
        {
            break;
        }

        std::size_t index = line.find(key);
        if (index == ppp::string::npos)
        {
            break;
        }

        line = line.substr(index + sizeof(key) - 1);
        if (line.empty())
        {
            break;
        }

        int ch = line[0];
        if (ch != '=' && ch != ' ')
        {
            break;
        }

        line = ppp::RTrim(ppp::LTrim(line.substr(1)));
        if (line.empty())
        {
            break;
        }

        ok = ppp::app::client::lsp::PaperAirplaneController::NoLsp(line);
    } while (false);

    fprintf(stdout, "[%s]%s\r\n", chnroutes2_gettime(chnroutes2_gettime()).data(), ok ? "OK" : "FAIL");
    return true;
}

// Windows network configuration commands
static bool Windows_PreferredNetwork(int argc, const char* argv[]) noexcept 
{
    bool ok = false;
    if (ppp::HasCommandArgument("--system-network-preferred-ipv4", argc, argv))
    {
        ok = ppp::net::proxies::HttpProxy::PreferredNetwork(true);
    }
    elif(ppp::HasCommandArgument("--system-network-preferred-ipv6", argc, argv))
    {
        ok = ppp::net::proxies::HttpProxy::PreferredNetwork(false);
    }
    elif(ppp::HasCommandArgument("--system-network-reset", argc, argv))
    {
        ok = ppp::win32::network::ResetNetworkEnvironment();
    }
    else
    {
        return false;
    }

    fprintf(stdout, "[%s]%s\r\n", chnroutes2_gettime(chnroutes2_gettime()).data(), ok ? "OK" : "FAIL");
    return true;
}
#endif

// Main application entry point
int PppApplication::Main(int argc, const char* argv[]) noexcept
{
    // Require administrator/root privileges
    if (!proxy_mode_ && !ppp::IsUserAnAdministrator()) // $ROOT is 0.
    {
        fprintf(stdout, "%s\r\n", "Non-administrators are not allowed to run.");
        return -1;
    }

    // Prevent multiple instances
    ppp::string rerun_name = (proxy_mode_ ? "proxy://" : (client_mode_ ? "client://" : "server://")) + configuration_path_;
    if (prevent_rerun_.Exists(rerun_name.data()))
    {
        fprintf(stdout, "%s\r\n", "Repeat runs are not allowed.");
        return -1;
    }

    // Create instance lock
    if (!prevent_rerun_.Open(rerun_name.data()))
    {
        fprintf(stdout, "%s\r\n", "Failed to open the repeat run lock.");
        return -1;
    }

#if defined(_WIN32)
    // Windows-specific setup
    if (client_mode_ && !proxy_mode_)
    {
        // Prepare the environment for the virtual Ethernet network device card.
        if (!Windows_PreparedEthernetEnvironment(network_interface_))
        {
            return -1;
        }
    }

    // Save original QUIC setting
    quic_ = ppp::net::proxies::HttpProxy::IsSupportExperimentalQuicProtocol();
#endif

    // Initialize network environment
    if (!PreparedLoopbackEnvironment(network_interface_))
    {
        return -1;
    }

    // Initialize timers and statistics
    stopwatch_.Restart();
    transmission_statistics_.Clear();

    // Configure client if running in client mode
    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    if (NULLPTR != client)
    {
#if defined(_WIN32)
        // Configure QUIC blocking
        if (!proxy_mode_)
        {
            ppp::net::proxies::HttpProxy::SetSupportExperimentalQuicProtocol(!network_interface_->BlockQUIC);
        }
#endif

        // Set up http-proxy and whether to block QUIC traffic!
        if (!proxy_mode_)
        {
            client->BlockQUIC(network_interface_->BlockQUIC);
        }

#if defined(_WIN32)
        // Linux does not support global Settings of the http proxy server on the operating system.   
        // This is because you can only change the /etc/profile configuration file.   
        // If the current user is the user, you can change the ~/.  bashrc configuration files implement.

        // The configuration proxy syntax is approximately:
        // export http_proxy="http://proxy.example.com:8080"
        // export https_proxy="http://proxy.example.com:8080"

        // However, there is a big flaw here, if the _tty terminal window that has been opened cannot take effect, 
        // And the Windows platform can take effect globally is different, so directly cancel the function support 
        // Of setting http proxy on Linux above the operating system.
        if (!proxy_mode_ && network_interface_->SetHttpProxy)
        {
            client->SetHttpProxyToSystemEnv();
        }
#endif

        // Configure the bypass list
        if (!proxy_mode_)
        {
            GLOBAL_.bypass = network_interface_->Bypass;
        }
    }

    // Parse restart configuration
    GLOBAL_.auto_restart = std::max<int>(0, atoi(ppp::GetCommandArgument("--auto-restart", argc, argv).data()));
    GLOBAL_.link_restart = (uint8_t)std::max<int>(0, atoi(ppp::GetCommandArgument("--link-restart", argc, argv).data()));

    // Start periodic updates
    return NextTickAlwaysTimeout(false) ? 0 : -1;
}

// Application runner function
static int Run(const std::shared_ptr<PppApplication>& APP, int prepared_status, int argc, const char* argv[]) noexcept
{
    // Handle IP list download command
    if (ppp::HasCommandArgument("--pull-iplist", argc, argv))
    {
        APP->PullIPList(ppp::GetCommandArgument("--pull-iplist", argc, argv));
        return -1;
    }

#if defined(_WIN32)
    // Handle Windows-specific commands
    if (Windows_PreferredNetwork(argc, argv))
    {
        return -1;
    }

    // Set the EXE program of the specified PE file path not to load LSPS. If some EXE programs load LSPS, the network cannot be accessed, for example, WSL.
    if (Windows_NoLsp(argc, argv))
    {
        return -1;
    }

    // Handle network optimization command
    if (ppp::HasCommandArgument("--system-network-optimization", argc, argv))
    {
        ppp::string datetime = chnroutes2_gettime(chnroutes2_gettime());
        fprintf(stdout, "[%s]%s\r\n", datetime.data(), ppp::win32::Win32Native::OptimizationSystemNetworkSettings() ? "OK" : "FAIL");
        return -1;
    }
#endif

    // Show help if arguments invalid
    if (prepared_status != 0)
    {
        APP->PrintHelpInformation();
        return prepared_status > 0 ? 0 : -1;
    }

    // Register shutdown handlers
    PppApplication::AddShutdownApplicationEventHandler();

    // Register restart signal handler on Unix-like systems
#if SIGRESTART
    signal(SIGRESTART, // SIG_DFL
        [](int) noexcept
        {
            PppApplication::ShutdownApplication(true);
        });
#endif

    // Run main application
    return APP->Main(argc, argv);
}

// Program entry point
int main(int argc, const char* argv[]) noexcept
{
    // Configure real-time mode
    ppp::RT = ppp::ToBoolean(ppp::GetCommandArgument("--rt", argc, argv, "y").data());

#if defined(_WIN32)
    // Switch console to UTF-8 code page so Unicode box-drawing characters
    // (├ ─ │ └ ┬ ┐ ┘ ┤ etc.) render correctly instead of showing as '?'.
    ::SetConsoleOutputCP(65001);
#endif

    // Initialize global state
    ppp::global::cctor();

    // Create application instance
    std::shared_ptr<PppApplication> APP = ppp::make_shared_object<PppApplication>();
    DEFAULT_ = APP;

    // Prepare environment and run
    int prepared_status = APP->PreparedArgumentEnvironment(argc, argv);

    // Help and argument errors do not need the event loop or kernel features.
    if (prepared_status != 0)
    {
        APP->PrintHelpInformation();
        int result_code = prepared_status > 0 ? 0 : -1;
        APP->Release();
        return result_code;
    }

    // Check io_uring compatibility on Linux after command-line handling.
#if BOOST_ASIO_HAS_IO_URING != 0
    if (!ppp::diagnostics::IfIOUringKernelVersion()) 
    {
        fprintf(stdout, "%s\r\n", "Enable io-uring, the kernel version must be 5.10.0 or higher.");
        APP->Release();
        return -1;
    }
#endif

    // When --log-file is specified, redirect LOG_TAG output to file instead of stdout.
    // The dashboard UI (fprintf to stdout) stays on console; only debug/info logs go to file.
#if defined(PPP_LOG_VERBOSE)
    if (LOG_FILE_PATH_.size() > 0)
    {
        FILE* log_file = fopen(LOG_FILE_PATH_.data(), "a");
        if (NULLPTR != log_file)
        {
            setvbuf(log_file, NULLPTR, _IONBF, 0);
            ppp::g_log_stream = log_file;
            fprintf(stdout, "Log file opened: %s\r\n", LOG_FILE_PATH_.data());
        }
    }
#endif

#if defined(_MACOS)
    // A full-tunnel client can legitimately own hundreds of TCP sockets even
    // with --tun-mux=0. macOS commonly starts command-line processes at 256,
    // which causes EMFILE and eventually stalls the event loop.
    ConfigureOpenFileDescriptorLimit();
#endif

    int result_code = Executors::Run(APP->GetBufferAllocator(), 
        [APP, prepared_status](int argc, const char* argv[]) noexcept -> int
        {
            int result_code = Run(APP, prepared_status, argc, argv);
#if defined(_WIN32)
            if (result_code != 0)
            {
                ppp::win32::Win32Native::PauseWindowsConsole();
            }
#endif
            return result_code;
        }, argc, argv);
    
    // Clean up and optionally restart
    APP->Release();

    // Restart application if requested
    if (GLOBAL_.restart)
    {
#if defined(_WIN32)
        // Build command line for restart
        ppp::string command_line = "\"" + ppp::string(*argv) + "\"";
        for (int i = 1; i < argc; ++i) 
        {
            command_line += " \"" + ppp::string(argv[i]) + "\""; 
        }

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
    
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        // Launch new instance
        if (CreateProcessA(NULLPTR, command_line.data(), NULLPTR, NULLPTR, FALSE, 0, NULLPTR, NULLPTR, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
#else
        // Unix exec restart
        execvp(*argv, (char**)argv);
#endif
    }

    return result_code;
}
