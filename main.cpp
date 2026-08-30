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
#include <ppp/app/rpc/LocalRpcServer.h>
#include <ppp/core/CoreApi.h>

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
#include <darwin/ppp/net/proxies/HttpProxy.h>
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

#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    bool                                                SetHttpProxy       = false; // Enable system HTTP/HTTPS proxy
    bool                                                LocalDns           = true;  // Listen on loopback for system DNS
#else   
#if defined(_MACOS)
    bool                                                SetHttpProxy       = false; // Enable system HTTP/HTTPS proxy (desktop)
#endif
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

public:
    // Headless mode: no dashboard rendering, no keyboard listener.
    bool                                            IsHeadless() const noexcept { return headless_; }
    // Build the runtime snapshot JSON consumed by the Rust TUI front-end.
    bool                                            BuildRuntimeSnapshot(Json::Value& snapshot) noexcept;
    // Execute one RPC command (methods are dispatched on the io_context thread).
    bool                                            ExecuteRpcCommand(const ppp::string& method, const Json::Value& params, Json::Value& result, ppp::string& error) noexcept;
    // Preserve the concrete startup stage for the in-process host. The
    // standalone executable still prints its historical messages, while the
    // Rust TUI must not collapse every Main() failure into one generic error.
    const ppp::string&                              GetStartupFailure() const noexcept { return startup_failure_; }
    void                                            SetStartupFailure(const char* message) noexcept { startup_failure_ = message != NULLPTR ? message : "core startup failed"; }

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
    bool                                            proxy_mode_                 = false; // Proxy/control mode; no TUN/routes/DNS
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
    ppp::string                                     startup_failure_;                    // Last startup-stage failure
    Stopwatch                                       stopwatch_;                          // Application uptime
    PreventReturn                                   prevent_rerun_;                      // Prevent multiple instances
    ppp::transmissions::ITransmissionStatistics     transmission_statistics_;            // Traffic statistics

    bool                                            headless_                   = false; // Headless mode (no TUI)
    bool                                            owns_console_presentation_  = true;  // Rust owns it for in-process hosting
    bool                                            catalog_only_               = false; // Load server directory without starting a VPN link
    ppp::string                                     rpc_listen_;                         // RPC listen address "ip:port"
    ppp::string                                     rpc_token_;                          // RPC authentication token
    int                                             rpc_max_clients_            = 1;     // Max concurrent RPC clients
    std::shared_ptr<ppp::app::rpc::LocalRpcServer>  rpc_server_;                         // Local RPC server
};

// Global variables
static std::shared_ptr<PppApplication>              DEFAULT_;                            // Application instance
static ppp::string                                   LOG_FILE_PATH_;                      // Log file path from --log-file argument
static ppp::string                                   LOG_LEVEL_ = "error";                // Runtime log level from --log-level
FILE*                                                ppp::g_log_stream = stdout;          // Log output stream, redirected by --log-file

// RPC log ring buffer: fed by the desktop log sink hook (any thread),
// drained by OnTick (io_context thread) and get_logs (RPC requests).
struct RpcLogEntry {
    uint64_t                                            seq = 0;
    ppp::string                                         level;
    ppp::string                                         line;
    uint64_t                                            timestamp_ms = 0;
};
static ppp::vector<RpcLogEntry>                      g_rpc_logs;
static std::mutex                                    g_rpc_logs_syncobj;
static std::atomic<uint64_t>                         g_rpc_log_seq = 0;                   // Next seq to assign
static std::atomic<uint64_t>                         g_rpc_logs_last_pushed_seq = 0;      // Seq pushed to RPC clients
static constexpr std::size_t                         RPC_LOG_CAPACITY = 2000;
static std::atomic<ppp_core_log_callback>            g_core_log_callback = nullptr;
static std::atomic<void*>                             g_core_log_user_data = nullptr;
// An in-process front-end owns the console/window lifecycle.  The standalone
// core keeps its native console handlers and presentation behavior, while an
// embedded core leaves those responsibilities to Rust.
static std::atomic<bool>                               g_core_in_process_host = false;
// There is one process-wide executor/default application. Reject a second
// embedded core instead of allowing two front-ends to overwrite DEFAULT_.
static std::atomic<bool>                               g_core_api_instance_active = false;
// The PPP global constructor starts process-wide services (lwIP, DNS and
// the executor tick thread). A TUI can stop and restart the owned core in the
// same process, so those services must be initialized exactly once.
static std::once_flag                                 g_core_runtime_initializer;
static FILE*                                          g_core_log_file = nullptr;

// A Rust TUI launches the compatibility core with stdout connected to a
// pipe. CRT stdout is then fully buffered, which can hide the exact startup
// stage until the process is killed on timeout. Keep startup diagnostics
// observable even when the core fails before RPC_LISTEN is printed.
static void ConfigureCoreOutputStreams() noexcept
{
    ::fflush(stdout);
    ::setvbuf(stdout, NULLPTR, _IONBF, 0);
    ::fflush(stderr);
    ::setvbuf(stderr, NULLPTR, _IONBF, 0);
}

static void CloseCoreLogFile() noexcept
{
    // The asynchronous desktop logger must be drained before the file is
    // closed. Otherwise a detached log worker from the previous in-process
    // core can write into a closed FILE* during the next TUI restart.
    ppp::diagnostics::FlushLogs();
    if (NULLPTR != g_core_log_file)
    {
        fclose(g_core_log_file);
        g_core_log_file = NULLPTR;
    }
    ppp::diagnostics::SetLogStream(stdout);
}

static void OpenCoreLogFile() noexcept
{
    if (LOG_FILE_PATH_.empty())
    {
        return;
    }

    FILE* log_file = fopen(LOG_FILE_PATH_.data(), "a");
    if (NULLPTR == log_file)
    {
        return;
    }

    setvbuf(log_file, NULLPTR, _IONBF, 0);
    g_core_log_file = log_file;
    ppp::diagnostics::SetLogStream(log_file);
    fprintf(stdout, "Log file opened: %s\r\n", LOG_FILE_PATH_.data());
}

static void RpcLogSink(const char* tag, const char* text) noexcept {
    if (NULLPTR == tag || NULLPTR == text || text[0] == '\x0') return;

    RpcLogEntry entry;
    entry.seq = g_rpc_log_seq.fetch_add(1) + 1;
    entry.level = tag;
    entry.line = text;
    entry.timestamp_ms = ppp::threading::Executors::GetTickCount();

    {
        std::lock_guard<std::mutex> scope(g_rpc_logs_syncobj);
        if (g_rpc_logs.size() >= RPC_LOG_CAPACITY)
        {
            g_rpc_logs.erase(g_rpc_logs.begin());
        }
        g_rpc_logs.emplace_back(std::move(entry));
    }

    // Never invoke a host callback while holding the ring-buffer mutex. A
    // callback may enqueue another core/UI action and must not deadlock the
    // diagnostics path.
    ppp_core_log_callback callback = g_core_log_callback.load(std::memory_order_acquire);
    if (NULLPTR != callback) {
        callback(g_core_log_user_data.load(std::memory_order_acquire), tag, text);
    }
}

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
    owns_console_presentation_ = !g_core_in_process_host.load(std::memory_order_acquire);
    if (owns_console_presentation_)
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
}

// Destructor
PppApplication::~PppApplication() noexcept
{
    Release();
}

// Clean up resources
void PppApplication::Release() noexcept 
{
    // Restore console presentation only when this application owns it.  In
    // the in-process build Rust/egui/ratatui owns the terminal and window;
    // the C++ destructor must not reset its cursor or close-button state.
    if (owns_console_presentation_)
    {
        ppp::HideConsoleCursor(false);

#if defined(_WIN32)
        // Re-enable console close button
        ppp::win32::Win32Native::EnabledConsoleWindowClosedButton(true);
#endif
    }

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

// The route summary is about network prefixes, not the number of source
// files.  Parse the configured files independently of the active split mode so
// GEO mode reports the same IP-segment counts as IP mode.
struct RouteSegmentCounts final
{
    std::size_t ipv4 = 0;
    std::size_t ipv6 = 0;
};

static RouteSegmentCounts CountConfiguredRouteSegments(const NetworkInterface& network_interface) noexcept
{
    ppp::unordered_set<ppp::string> ipv4;
    ppp::unordered_set<ppp::string> ipv6;

    auto load_file = [&ipv4, &ipv6](const ppp::string& path) noexcept {
        if (path.empty()) {
            return;
        }

        ppp::vector<Ipep::AddressRange> ranges;
        Ipep::ParseAllCidrsFromFileName(path, ranges);
        for (const Ipep::AddressRange& range : ranges) {
            ppp::string key = Ipep::ToAddressString<ppp::string>(range.Address) + "/" +
                stl::to_string<ppp::string>(range.Cidr);
            if (range.Address.is_v4()) {
                ipv4.emplace(std::move(key));
            }
            elif(range.Address.is_v6()) {
                ipv6.emplace(std::move(key));
            }
        }
    };

    if (network_interface.Bypass) {
        for (const ppp::string& path : *network_interface.Bypass) {
            load_file(path);
        }
    }
    if (network_interface.Bypass6) {
        for (const ppp::string& path : *network_interface.Bypass6) {
            load_file(path);
        }
    }

    return RouteSegmentCounts{ ipv4.size(), ipv6.size() };
}

static std::size_t CountConfiguredDnsRules(const ppp::string& path) noexcept
{
    if (path.empty()) {
        return 0;
    }

    using DnsRule = ppp::app::client::dns::Rule;
    ppp::unordered_map<ppp::string, DnsRule::Ptr> rules;
    ppp::unordered_map<ppp::string, DnsRule::Ptr> full_rules;
    ppp::unordered_map<ppp::string, DnsRule::Ptr> regexp_rules;
    DnsRule::LoadFile(path, rules, full_rules, regexp_rules);
    return rules.size() + full_rules.size() + regexp_rules.size();
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
        // Always offer the primary configuration ("main") so the user can hot
        // switch back after moving to a --server-dir server, even when the
        // primary JSON is not duplicated inside the server directory.
        if (status.server_menu || status.tag == "main")
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
        const auto& selected = servers[server_selection_];
        if (selected.active)
        {
            client->SwitchPrimaryOutboundToRankedFirst(selected.tag);
        }
        else
        {
            client->SwitchPrimaryOutbound(selected.tag);
        }
    }
}

void PppApplication::HandleConsoleInput() noexcept
{
#if !defined(_ANDROID)
    if (NULLPTR == client_)
    {
        console_tab_page_ = 0;
        return;
    }

    static constexpr int console_tab_count = 4;
    static constexpr int servers_tab_page = 3;
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

#if defined(_WIN32)
    // Console writes block in Quick Edit selection mode. Skip this frame so
    // the networking event loop keeps running while arbitrary text is copied.
    CONSOLE_SELECTION_INFO selection{};
    if (GetConsoleSelectionInfo(&selection) && selection.dwFlags != CONSOLE_NO_SELECTION)
    {
        return true;
    }
#endif

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

#if defined(_WIN32)
    // Windows uses the native console API for cursor positioning.  Keep the
    // operation outside the frame buffer because WriteConsole handles it
    // independently from the ANSI sequences used by Unix terminals.
    if (console_window_size.tty && !ppp::SetConsoleCursorPosition(0, 0))
    {
        return false;
    }
#endif
    
    // Determine hosting environment
    ppp::string hosting_environment;
#if defined(_DEBUG)
    hosting_environment = "development";
#else
    hosting_environment = "production";
#endif

    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    hosting_environment = (NULLPTR != client ? "client:" : "server:") + hosting_environment;

#if defined(_WIN32)
    // The Windows console needs the native buffer clear when the next frame
    // becomes shorter.  Unix terminals can clear the old tail after the new
    // frame has been written, which avoids displaying an empty frame.
    if (console_window_size.tty)
    {
        ppp::ClearConsoleOutputCharacter();
    }
#endif

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

    // Client pages remain switchable; the server always shows Status without a tab bar.
    if (NULLPTR != client)
    {
        const char* tab_labels[] = { "Status", "Network", "Routes", "Servers" };
        const int tab_count = 4;

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
            if (link_url.empty())
            {
                printfn("Managed Server        : %s", link_state);
            }
            else
            {
                printfn("Managed Server        : %s %s", link_url.data(), link_state);
            }
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
            std::shared_ptr<AppConfiguration> active_configuration = client->GetConfiguration();
            ppp::string guid = NULLPTR != active_configuration ?
                active_configuration->client.guid : configuration_->client.guid;
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
        const RouteSegmentCounts segment_counts = CountConfiguredRouteSegments(*network_interface);
        printfn("IPv4 List Entries     : %zu", segment_counts.ipv4);
        printfn("IPv6 List Entries     : %zu", segment_counts.ipv6);
        printfn("DNS Rules File        : %s", network_interface->DNSRules.data());
        printfn("DNS Rule Count        : %zu", CountConfiguredDnsRules(network_interface->DNSRules));
        if (network_interface->SplitMode == NetworkInterface::BypassMode::Geo)
        {
            printfn("GEO Policy YAML       : %s", network_interface->GeoRules.data());
            printfn("GeoSite Database      : %s", network_interface->GeoSite.data());
            printfn("GeoIP Database        : %s", network_interface->GeoIP.data());
            if (NULLPTR != client)
            {
                auto geo_rules = client->GetGeoRules();
                auto statuses = client->GetOutboundStatuses();
                if (NULLPTR != geo_rules)
                {
                    ppp::string direct_dns;
                    if (geo_rules->UsesLocalDirectDns())
                    {
                        direct_dns = "local";
                    }
                    for (const auto& address : geo_rules->GetDirectDnsServers())
                    {
                        ppp::string value = ppp::net::Ipep::ToAddressString<ppp::string>(address);
                        if (!direct_dns.empty())
                        {
                            direct_dns += ", ";
                        }
                        direct_dns += value;
                    }
                    printfn("Direct DNS            : %s",
                        direct_dns.empty() ? "none" : direct_dns.data());
                    printfn("Rule Count            : %zu", geo_rules->GetRuleCount());

                    auto find_status = [&statuses](const ppp::string& tag) noexcept ->
                        const VEthernetNetworkSwitcher::OutboundStatus*
                    {
                        for (const auto& status : statuses)
                        {
                            if (status.tag == tag)
                            {
                                return &status;
                            }
                        }
                        return NULLPTR;
                    };

                    std::size_t split_rule_count = 0;
                    for (const auto& rule : geo_rules->GetRuleSummaries())
                    {
                        if (rule.action != ppp::app::client::geo::GeoRuleEngine::Action::Tunnel ||
                            rule.outbound.empty())
                        {
                            continue;
                        }
                        const auto* target = find_status(rule.outbound);
                        if (NULLPTR == target || !target->route_used)
                        {
                            continue;
                        }
                        if (split_rule_count == 0)
                        {
                            printfn("Split Rules           :");
                        }
                        ++split_rule_count;
                        ppp::string matcher = rule.type + "," + rule.value;
                        ppp::string display = target->display_name.empty() ? target->tag : target->display_name;
                        printfn("  %s -> %s (%s)", matcher.data(), rule.outbound.data(), display.data());
                    }
                    if (split_rule_count == 0)
                    {
                        printfn("Split Rules           : none");
                    }
                }
            }
        }
        elif(network_interface->SplitMode == NetworkInterface::BypassMode::Ip)
        {
            printfn("Bypass IPv4 File      : %s", network_interface->Bypass->size() > 0 ?
                network_interface->Bypass->begin()->data() : "(none)");
            printfn("Bypass IPv6 File      : %s", network_interface->Bypass6->size() > 0 ?
                network_interface->Bypass6->begin()->data() : "(none)");
            printfn("Bypass Gateway        : %s", network_interface->BypassNgw.to_string().data());
            printfn("Bypass Gateway IPv6   : %s", network_interface->BypassNgw6.to_string().data());
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
        printfn("%s", "Use Up/Down to select. Enter switches server; press again for Rank #1.");
        printfn("%s", "Rank #1 rebuilds a complete MUX generation on one entry.");
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
                // Same rule as HandleServerSelection: "main" (the primary
                // configuration) is always a valid hot-switch target.
                if (status.server_menu || status.tag == "main")
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
                auto probe_suffix = [](const VEthernetNetworkSwitcher::OutboundStatus& outbound) noexcept -> ppp::string
                {
                    if (!outbound.probe_enabled)
                    {
                        return ppp::string("  (probe off)");
                    }
                    ppp::string suffix;
                    if (outbound.probe_checked)
                    {
                        if (outbound.probe_reachable && outbound.probe_rtt_ms >= 0)
                        {
                            suffix = "  (";
                            suffix += stl::to_string<ppp::string>(outbound.probe_rtt_ms);
                            suffix += "ms)";
                        }
                        else
                        {
                            suffix = "  (unreachable)";
                        }
                    }
                    return suffix;
                };
                auto server_endpoint = [](const ppp::string& uri) noexcept -> ppp::string
                {
                    ppp::string value = ppp::ATrim<ppp::string>(uri);
                    const std::size_t scheme = value.find("://");
                    const std::size_t authority = scheme == ppp::string::npos ? 0 : scheme + 3;
                    const std::size_t slash = value.find('/', authority);
                    if (slash != ppp::string::npos)
                    {
                        value.erase(slash);
                    }
                    if (authority > 0 && authority < value.size())
                    {
                        value = value.substr(authority);
                    }
                    return value;
                };
                auto entry_status = [&server_endpoint](const VEthernetNetworkSwitcher::OutboundStatus& outbound) noexcept -> ppp::string
                {
                    ppp::string value;
                    if (!outbound.multiple_entries)
                    {
                        // A single-entry outbound has no ranking state.  Keep
                        // the display useful by showing its configured endpoint
                        // when it is not connected, without the multi-entry
                        // "ranking collecting" placeholder.
                        value += outbound.current_entry.empty() ?
                            server_endpoint(outbound.server) : outbound.current_entry;
                        return value;
                    }

                    const bool connected = !outbound.current_entry.empty();
                    ppp::string current = outbound.current_entry;
                    if (current.empty())
                    {
                        // An idle multi-entry outbound still has a useful
                        // candidate from the latest probe/ranking pass. Show
                        // it instead of hiding all entry information behind
                        // "(not connected)".
                        current = !outbound.ranked_first_entry.empty() ?
                            outbound.ranked_first_entry : outbound.probe_entry;
                        if (current.empty())
                        {
                            current = server_endpoint(outbound.server);
                        }
                    }
                    if (!connected)
                    {
                        value += current.empty() ? "(not connected)" : current;
                        return value;
                    }
                    value += current;
                    if (!outbound.ranked_first_entry.empty())
                    {
                        if (outbound.ranked_first_entry == outbound.current_entry)
                        {
                            value += "  [#1]";
                        }
                        else
                        {
                            value += "  -> #1 ";
                            value += outbound.ranked_first_entry;
                        }
                    }
                    else
                    {
                        value += "  [ranking collecting]";
                    }
                    return value;
                };
                for (std::size_t i = first; i < last; ++i)
                {
                    const auto& outbound = servers[i];
                    bool connected = outbound.state ==
                        VEthernetExchanger::NetworkState_Established;
                    // "main" is the live primary selection.  "split" is a
                    // static routing-rule membership and must remain visible
                    // even when another outbound is the current main.
                    ppp::string usage;
                    if (outbound.active)
                    {
                        usage += " main";
                    }
                    if (outbound.route_used)
                    {
                        usage += " split";
                    }
                    if (connected)
                    {
                        usage += " connected";
                    }
                    usage += probe_suffix(outbound);
                    ppp::string entries = entry_status(outbound);
                    if (i == server_selection_ && console_highlight)
                    {
                        printfn.Highlighted(">%-1c %-24s%s",
                            outbound.active ? '*' : ' ',
                            outbound.display_name.data(),
                            usage.data());
                        printfn.Highlighted("   %s", entries.data());
                    }
                    else
                    {
                        printfn("%c%c %-24s%s",
                            i == server_selection_ ? '>' : ' ',
                            outbound.active ? '*' : ' ',
                            outbound.display_name.data(),
                            usage.data());
                        printfn("   %s", entries.data());
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

    // Output to console.  Compose the cursor move, frame, and tail clear into
    // one write on Unix.  The old implementation called `clear` before
    // building the frame, which exposed a blank terminal for every refresh.
#if !defined(_WIN32)
    if (console_window_size.tty)
    {
        console_window_content.insert(0, "\033[H");
        // Every rendered row is padded to the terminal width.  Clear only
        // below the new frame so pages with fewer rows do not leave stale
        // content behind, without clearing the visible frame first.
        console_window_content.append("\033[0J");
    }
#endif
    fprintf(stdout, "%s", console_window_content.data());
    fflush(stdout);
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
                SetStartupFailure("Open tun/tap driver failure");
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
                SetStartupFailure("Listen tun/tap driver failure");
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
                SetStartupFailure("failed to allocate VPN client");
                break;
            }

            {
                bool proxy_only = proxy_mode_;
                ethernet->ProxyOnly(&proxy_only);
            }
            {
                bool proxy_ip_rules =
                    network_interface->SplitMode == NetworkInterface::BypassMode::Ip;
                ethernet->ProxyIpRules(&proxy_ip_rules);
            }

            if (!outbound_configurations_.empty())
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
                    SetStartupFailure("invalid multi-outbound configuration");
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
            if (network_interface->SplitMode == NetworkInterface::BypassMode::Geo) {
                if (!ethernet->LoadGeoRules(network_interface->GeoRules, network_interface->GeoSite, network_interface->GeoIP)) {
                    SetStartupFailure("failed to load geo bypass rules");
                    fprintf(stdout, "%s\r\n", "Failed to load geo bypass rules.");
                    break;
                }
            }

            // Load bypass IP lists in IP mode.
            if (network_interface->SplitMode == NetworkInterface::BypassMode::Ip) {
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

            if (network_interface->SplitMode == NetworkInterface::BypassMode::Ip) {
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
            ethernet->SetConfiguredTunDns(network_interface->DnsAddresses);
            if (!ethernet->Open(tap))
            {
                SetStartupFailure("failed to open VPN client or no usable network interface");
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
    fprintf(stdout, "[CoreStartup] stage=arguments-prepare enter argc=%d\r\n", argc);
    // Parse runtime logging before any configuration or RPC startup so early
    // startup failures also obey the selected level.  Release and Debug use
    // the same log implementation; the default is intentionally error-only.
    LOG_LEVEL_ = ppp::GetCommandArgument("--log-level", argc, argv, "error");
    ppp::diagnostics::SetLogLevel(static_cast<int>(
        ppp::diagnostics::ParseLogLevel(LOG_LEVEL_.data())));

    // Parse log file path from command line
    LOG_FILE_PATH_ = ppp::GetCommandArgument("--log-file", argc, argv);
    if (LOG_FILE_PATH_.size() > 0)
    {
        LOG_FILE_PATH_ = File::GetFullPath(File::RewritePath(LOG_FILE_PATH_.data()).data());
    }
    // A Rust TUI can stop and start the C++ core more than once in one
    // process. Reset the previous run's file-backed sink before parsing the
    // new runtime configuration, including the explicit empty-path case.
    CloseCoreLogFile();
    fprintf(stdout, "[CoreStartup] stage=arguments-prepare logging-ready log_file='%s'\r\n",
        LOG_FILE_PATH_.data());

    // Parse headless / RPC options.  The local RPC server powers the Rust
    // TUI front-end (docs/RUST_TUI_DESIGN_CN.md); --rpc-listen requires
    // --rpc-token and only loopback bindings are accepted by the server.
    headless_ = ppp::HasCommandArgument("--headless", argc, argv);
    catalog_only_ = ppp::ToBoolean(ppp::GetCommandArgument(
        "--catalog-only", argc, argv, "no").data());
    rpc_listen_ = ppp::GetCommandArgument("--rpc-listen", argc, argv);
    rpc_token_ = ppp::GetCommandArgument("--rpc-token", argc, argv);
    rpc_max_clients_ = std::max<int>(1,
        atoi(ppp::GetCommandArgument("--rpc-max-clients", argc, argv, "1").data()));
    fprintf(stdout, "[CoreStartup] stage=rpc-arguments listen='%s' headless=%d catalog_only=%d\r\n",
        rpc_listen_.data(), headless_ ? 1 : 0, catalog_only_ ? 1 : 0);
    if (rpc_listen_.size() > 0 && rpc_token_.empty())
    {
        fprintf(stdout, "%s\r\n", "--rpc-listen requires --rpc-token.");
        return -1;
    }

    // Show help if requested
    if (ppp::IsInputHelpCommand(argc, argv))
    {
        return 1;
    }

    // Load configuration
    ppp::string path;
    fprintf(stdout, "[CoreStartup] stage=configuration-load begin\r\n");
    std::shared_ptr<AppConfiguration> configuration = LoadConfiguration(argc, argv, path);
    if (NULLPTR == configuration)
    {
        fprintf(stdout, "[CoreStartup] FAIL stage=configuration-load\r\n");
        return -1;
    }
    else
    {
        fprintf(stdout, "[CoreStartup] stage=configuration-load ok path='%s'\r\n", path.data());
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
        // `client.proxy-only` is the configuration equivalent of
        // `--mode=proxy`. It must be decided before the loopback/TAP setup;
        // changing this structural mode during a hot switch would require
        // tearing down the operating-system network takeover.
        proxy_mode_ = mode == "proxy" || configuration->client.proxy_only;
        client_mode_ = proxy_mode_ || IsModeClientOrServer(argc, argv);
        fprintf(stdout, "[CoreStartup] stage=mode-resolve mode='%s' client=%d proxy=%d\r\n",
            mode.data(), client_mode_ ? 1 : 0, proxy_mode_ ? 1 : 0);

        if (client_mode_ && !LoadServerConfigurations(argc, argv, configuration))
        {
            fprintf(stdout, "[CoreStartup] FAIL stage=server-configurations\r\n");
            return -1;
        }
        fprintf(stdout, "[CoreStartup] stage=server-configurations ok count=%zu\r\n",
            outbound_configurations_.size());

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
                    // Port 0 explicitly disables that local listener.  This
                    // is used by the Rust desktop client for a proxy-like
                    // control session that loads server profiles without
                    // opening an HTTP/SOCKS port.
                    if (NULLPTR == tail || tail == text.data() || *tail != '\x0' ||
                        value < 0 || value > IPEndPoint::MaxPort)
                    {
                        fprintf(stdout, "Invalid %s value '%s'; expected 0-65535.\r\n",
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
                LOG_INFO("Proxy control mode: no HTTP/SOCKS listener; server profiles and transport loaded, TUN/routes/DNS disabled");
            }
            else
            {
                LOG_INFO("Proxy mode: HTTP=%s:%d SOCKS=%s:%d; TUN/routes/DNS disabled, system-proxy optional",
                    configuration->client.http_proxy.bind.data(), configuration->client.http_proxy.port,
                    configuration->client.socks_proxy.bind.data(), configuration->client.socks_proxy.port);
            }
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
    fprintf(stdout, "[CoreStartup] stage=network-interface parsed mode=%s ok=%d\r\n",
        proxy_mode_ ? "proxy" : "client/server", NULLPTR != network_interface ? 1 : 0);
    if (NULLPTR == network_interface)
    {
        return -1;
    }
    if (proxy_mode_)
    {
        network_interface->SplitMode = NetworkInterface::BypassMode::No;
        ppp::string proxy_bypass_mode = ToLower(ppp::LTrim(ppp::RTrim(
            ppp::GetCommandArgument("--bypass-mode", argc, argv, "no"))));
        if (proxy_bypass_mode == "geo")
        {
            network_interface->SplitMode = NetworkInterface::BypassMode::Geo;
            network_interface->GeoRules = File::GetFullPath(File::RewritePath(
                ppp::GetCommandArgument("--geo-rules", argc, argv, "./geo-rules.yaml").data()).data());
            network_interface->GeoSite = File::GetFullPath(File::RewritePath(
                ppp::GetCommandArgument("--geosite", argc, argv, "./geosite.dat").data()).data());
            network_interface->GeoIP = File::GetFullPath(File::RewritePath(
                ppp::GetCommandArgument("--geoip", argc, argv, "./geoip.dat").data()).data());
        }
        elif (proxy_bypass_mode == "ip")
        {
            network_interface->SplitMode = NetworkInterface::BypassMode::Ip;
            network_interface->BypassLoadList(File::GetFullPath(File::RewritePath(
                ppp::LTrim(ppp::RTrim(ppp::GetCommandArgument("--bypass", argc, argv, "./ip.txt"))).data()).data()));
            network_interface->BypassLoadList6(File::GetFullPath(File::RewritePath(
                ppp::LTrim(ppp::RTrim(ppp::GetCommandArgument("--bypass6", argc, argv, "./ipv6.txt"))).data()).data()));
        }
        elif (proxy_bypass_mode == "no")
        {
            network_interface->SplitMode = NetworkInterface::BypassMode::No;
        }
        else
        {
            fprintf(stdout, "Invalid --bypass-mode '%s'; expected ip, geo, or no.\r\n", proxy_bypass_mode.data());
            return -1;
        }
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
#if defined(_WIN32) || defined(_MACOS)
        network_interface->SetHttpProxy = ppp::ToBoolean(
            ppp::GetCommandArgument("--set-http-proxy", argc, argv).data());
#endif
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

    if (client_mode_ &&
        !LoadGeoOutboundConfigurations(network_interface, configuration))
    {
        fprintf(stdout, "[CoreStartup] FAIL stage=geo-outbound-configurations\r\n");
        return -1;
    }

    network_interface_ = network_interface;
    
    // Configure DNS settings
    ppp::net::asio::vdns::ttl = configuration->udp.dns.ttl;
    ppp::net::asio::vdns::enabled = configuration->udp.dns.turbo;
#if defined(_WIN32)
    const char* tun_driver =
        ppp::tap::TapWindows::GetDriverMode() == ppp::tap::TapWindows::DriverMode::Wintun ? "wintun" :
        (ppp::tap::TapWindows::GetDriverMode() == ppp::tap::TapWindows::DriverMode::Tap ? "tap" : "auto");
#else
    const char* tun_driver = "n/a";
#endif
    fprintf(stdout, "[CoreStartup] stage=arguments-prepare exit ok=1 lwip=%d tun_driver=%s\r\n",
        network_interface->Lwip ? 1 : 0, tun_driver);
    
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

#if defined(_WIN32) || defined(_MACOS)
    printf("  %-*s  %-*s  %-*s\r\n",
        col_option_width, "--set-http-proxy=[yes|no]",
        col_description_width, "Set system HTTP/HTTPS proxy; restore on exit",
        col_default_width, "no");
#endif

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
        col_description_width, "Core runtime log file (all builds)",
        col_default_width, "console only");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--log-level=[none|error|warn|info|debug]",
        col_description_width, "Runtime log level",
        col_default_width, "error");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--link-restart=<count>",
        col_description_width, "Link reconnection attempts",
        col_default_width, "0");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--block-quic=[yes|no]",
        col_description_width, "Block QUIC protocol traffic",
        col_default_width, "no");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--headless",
        col_description_width, "No dashboard rendering / keyboard input",
        col_default_width, "off");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--rpc-listen=<ip:port>",
        col_description_width, "Local JSON-RPC server (requires --rpc-token)",
        col_default_width, "disabled");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--rpc-token=<token>",
        col_description_width, "RPC authentication token (loopback only)",
        col_default_width, "required with --rpc-listen");

    printf("│ %-*s │ %-*s │ %-*s │\n",
        col_option_width, "--rpc-max-clients=<n>",
        col_description_width, "Max concurrent RPC clients",
        col_default_width, "1");

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
        "no"
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
    fprintf(stdout, "[CoreStartup] address-parse begin name='%s' raw='%s'\r\n",
        name != NULLPTR ? name : "", address_string.data());
    if (address_string.empty())
    {
        fprintf(stdout, "[CoreStartup] address-parse end name='%s' result=empty-any\r\n",
            name != NULLPTR ? name : "");
        return boost::asio::ip::address_v4::any();
    }

    address_string = ppp::LTrim<ppp::string>(address_string);
    address_string = ppp::RTrim<ppp::string>(address_string);
    if (address_string.empty())
    {
        fprintf(stdout, "[CoreStartup] address-parse end name='%s' result=trimmed-empty-any\r\n",
            name != NULLPTR ? name : "");
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
        fprintf(stdout, "[CoreStartup] address-parse end name='%s' result=invalid-any\r\n",
            name != NULLPTR ? name : "");
        return boost::asio::ip::address_v4::any();
    }

    std::string parsed_address = address.to_string();
    fprintf(stdout, "[CoreStartup] address-parse end name='%s' result='%s'\r\n",
        name != NULLPTR ? name : "", parsed_address.c_str());
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
    fprintf(stdout, "[CoreStartup] dns-parse begin raw='%s' at_least_two=%d config=%d\r\n",
        dns.data(), at_least_two ? 1 : 0, configuration_ != NULLPTR ? 1 : 0);
    int parsed = Ipep::ToDnsAddresses(dns, addresses, at_least_two);
    fprintf(stdout, "[CoreStartup] dns-parse user-result=%d count=%zu\r\n",
        parsed, addresses.size());
    if (parsed < 1) {
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
    fprintf(stdout, "[CoreStartup] dns-parse end count=%zu\r\n", addresses.size());
}

// Parse network interface configuration from command line arguments
std::shared_ptr<NetworkInterface> PppApplication::GetNetworkInterface(int argc, const char* argv[]) noexcept
{
    fprintf(stdout, "[CoreStartup] network-interface begin\r\n");
    std::shared_ptr<NetworkInterface> ni = ppp::make_shared_object<NetworkInterface>();
    if (NULLPTR != ni)
    {
#if defined(_WIN32)
        ppp::string tun_driver = ToLower(ppp::LTrim(ppp::RTrim(ppp::GetCommandArgument("--tun-driver", argc, argv, "auto"))));
        fprintf(stdout, "[CoreStartup] network-interface driver-arg='%s'\r\n", tun_driver.data());
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

        // Keep the platform default on the reliable C/TCP path. The Windows
        // TAP + lwIP path remains available through an explicit --lwip=yes,
        // but it currently cannot reach an established state on all Windows
        // configurations.
        ni->Lwip = ppp::ToBoolean(ppp::GetCommandArgument("--lwip", argc, argv).data());
        fprintf(stdout, "[CoreStartup] network-interface driver-mode-set lwip=%d\r\n", ni->Lwip ? 1 : 0);
#else
        ni->Lwip = ppp::ToBoolean(ppp::GetCommandArgument("--lwip", argc, argv).data());
#endif

        ni->Nic = ppp::RTrim(ppp::LTrim(ppp::GetCommandArgument("--nic", argc, argv)));
        ni->BlockQUIC = ppp::ToBoolean(ppp::GetCommandArgument("--block-quic", argc, argv).data());
        fprintf(stdout, "[CoreStartup] network-interface basic nic='%s' block_quic=%d\r\n",
            ni->Nic.data(), ni->BlockQUIC ? 1 : 0);

        // Parse DNS servers
        fprintf(stdout, "[CoreStartup] network-interface dns begin\r\n");
        GetDnsAddresses(ni->DnsAddresses, argc, argv);
        fprintf(stdout, "[CoreStartup] network-interface dns parsed count=%zu\r\n", ni->DnsAddresses.size());
        if (!ni->DnsAddresses.empty()) {
            auto dns_servers = ppp::net::asio::vdns::servers;
            fprintf(stdout, "[CoreStartup] network-interface dns global-reset ptr=%d\r\n",
                dns_servers != NULLPTR ? 1 : 0);
            if (dns_servers == NULLPTR)
            {
                fprintf(stdout, "[CoreStartup] FAIL stage=network-interface reason=dns-global-null\r\n");
                return NULLPTR;
            }
            dns_servers->clear();

            for (const boost::asio::ip::address& dns_server : ni->DnsAddresses) {
                dns_servers->emplace_back(boost::asio::ip::udp::endpoint(dns_server, PPP_DNS_SYS_PORT));
            }
        }
        fprintf(stdout, "[CoreStartup] network-interface dns end\r\n");

        // Parse network addresses
        fprintf(stdout, "[CoreStartup] network-interface addresses begin\r\n");
        ni->Ngw = GetNetworkAddress("--ngw", 0, 32, "0.0.0.0", argc, argv);
        ni->IPAddress = GetNetworkAddress("--tun-ip", 0, 32, "10.0.0.2", argc, argv);
        ni->SubmaskAddress = GetNetworkAddress("--tun-mask", 16, 32, "255.255.255.252", argc, argv);

        // Suggested Ethernet card address setting.
        ni->GatewayServer = GetNetworkAddress("--tun-gw", 0, 32, "10.0.0.1", argc, argv);
        fprintf(stdout, "[CoreStartup] network-interface addresses end\r\n");

#if defined(_WIN32)
        // DHCP-MASQ lease time in seconds.
        ni->LeaseTimeInSeconds = strtoul(ppp::GetCommandArgument("--tun-lease-time-in-seconds", argc, argv).data(), NULLPTR, 10);
        if (ni->LeaseTimeInSeconds < 1)
        {
            ni->LeaseTimeInSeconds = 7200;
        }
#endif

        // Calculate valid IP address based on gateway and subnet
        fprintf(stdout, "[CoreStartup] network-interface address-normalize begin\r\n");
        ni->IPAddress = Ipep::FixedIPAddress(ni->IPAddress, ni->GatewayServer, ni->SubmaskAddress);
        ni->StaticMode = ppp::ToBoolean(ppp::GetCommandArgument("--tun-static", argc, argv).data());
        ni->HostedNetwork = ppp::ToBoolean(ppp::GetCommandArgument("--tun-host", argc, argv, "y").data());
        ni->VNet = ppp::ToBoolean(ppp::GetCommandArgument("--tun-vnet", argc, argv, "y").data());
        fprintf(stdout, "[CoreStartup] network-interface address-normalize end hosted=%d vnet=%d\r\n",
            ni->HostedNetwork ? 1 : 0, ni->VNet ? 1 : 0);

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
        fprintf(stdout, "[CoreStartup] network-interface bypass-mode='%s'\r\n", bypass_mode.data());

        ni->GeoRules = File::GetFullPath(File::RewritePath(ppp::GetCommandArgument(
            "--geo-rules", argc, argv, "./geo-rules.yaml").data()).data());
        ni->GeoSite = File::GetFullPath(File::RewritePath(ppp::GetCommandArgument(
            "--geosite", argc, argv, "./geosite.dat").data()).data());
        ni->GeoIP = File::GetFullPath(File::RewritePath(ppp::GetCommandArgument(
            "--geoip", argc, argv, "./geoip.dat").data()).data());
        fprintf(stdout, "[CoreStartup] network-interface geo-paths rules='%s' site='%s' ip='%s'\r\n",
            ni->GeoRules.data(), ni->GeoSite.data(), ni->GeoIP.data());

#if defined(_LINUX)
        ni->BypassNic = ppp::RTrim(ppp::LTrim(ppp::GetCommandArgument("--bypass-nic", argc, argv)));
#endif
        ni->BypassNgw = GetNetworkAddress("--bypass-ngw", 0, 32, "0.0.0.0", argc, argv);
        ppp::string bypass_path = File::GetFullPath(File::RewritePath(ppp::LTrim(ppp::RTrim(
            ppp::GetCommandArgument("--bypass", argc, argv, "./ip.txt"))).data()).data());
        fprintf(stdout, "[CoreStartup] network-interface bypass4 begin path='%s'\r\n", bypass_path.data());
        int bypass4_count = ni->BypassLoadList(bypass_path);
        fprintf(stdout, "[CoreStartup] network-interface bypass4 end count=%d\r\n", bypass4_count);

#if defined(_LINUX)
        ni->BypassNic6 = ppp::RTrim(ppp::LTrim(ppp::GetCommandArgument("--bypass-nic6", argc, argv)));
#endif
        ni->BypassNgw6 = GetNetworkAddress("--bypass-ngw6", 0, 128, "::", argc, argv);
        ppp::string bypass6_path = File::GetFullPath(File::RewritePath(ppp::LTrim(ppp::RTrim(
            ppp::GetCommandArgument("--bypass6", argc, argv, "./ipv6.txt"))).data()).data());
        fprintf(stdout, "[CoreStartup] network-interface bypass6 begin path='%s'\r\n", bypass6_path.data());
        int bypass6_count = ni->BypassLoadList6(bypass6_path);
        fprintf(stdout, "[CoreStartup] network-interface bypass6 end count=%d\r\n", bypass6_count);
        fprintf(stdout, "[CoreStartup] network-interface bypass-lists end\r\n");

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
        fprintf(stdout, "[CoreStartup] network-interface mux mux=%u acceleration=%u\r\n",
            static_cast<unsigned int>(ni->Mux), static_cast<unsigned int>(ni->MuxAcceleration));

#if defined(_WIN32) || defined(_MACOS)
        ni->SetHttpProxy = ppp::ToBoolean(ppp::GetCommandArgument("--set-http-proxy", argc, argv).data());
#if defined(_WIN32)
        ni->Wintun = ppp::GetCommandArgument("--tun", argc, argv, NetworkInterface::GetDefaultTun());
        if (ppp::tap::TapWindows::GetDriverMode() == ppp::tap::TapWindows::DriverMode::Tap &&
            (ni->Wintun.empty() || ppp::ToLower<ppp::string>(ni->Wintun) == "ppp"))
        {
            // "PPP" is the Wintun default name. Keep TAP mode on the
            // dedicated openppp2 adapter so a third-party "PPP 1" TAP is
            // never selected by name or by suffix fallback.
            ni->Wintun = "PPP PRIVATE NETWORK 2 TAP";
        }
        // An explicit Wintun adapter is identified by its name, not by a TAP
        // component GUID. Do not enumerate Windows network interfaces during
        // argument preparation: WMI/IP Helper enumeration can block for many
        // seconds when a stale TAP adapter or a stopped Wintun service exists.
        // TapWindows::Create performs the definitive Wintun DLL/device check.
        if (ppp::tap::TapWindows::GetDriverMode() == ppp::tap::TapWindows::DriverMode::Wintun)
        {
            ni->ComponentId = ni->Wintun;
            fprintf(stdout, "[CoreStartup] network-interface wintun explicit name='%s' skip-component-enumeration\r\n",
                ni->Wintun.data());
        }
        else
        {
            fprintf(stdout, "[CoreStartup] network-interface component-query begin name='%s'\r\n",
                ni->Wintun.data());
            ni->ComponentId = ppp::tap::TapWindows::FindComponentId(ni->Wintun);
            fprintf(stdout, "[CoreStartup] network-interface component-query end component='%s'\r\n",
                ni->ComponentId.data());
        }
#else
        ni->ComponentId = ppp::GetCommandArgument("--tun", argc, argv, NetworkInterface::GetDefaultTun());
#if defined(_MACOS)
        ni->Ssmt = std::max<int>(0, atoi(ppp::GetCommandArgument("--tun-ssmt", argc, argv).data()));
        ni->Promisc = ppp::ToBoolean(ppp::GetCommandArgument("--tun-promisc", argc, argv, "y").data());
#endif
#endif
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
        fprintf(stdout, "[CoreStartup] network-interface end component='%s'\r\n",
            ni->ComponentId.data());
    }
    else
    {
        fprintf(stdout, "[CoreStartup] FAIL stage=network-interface reason=allocate\r\n");
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
    // Close the local RPC server first so no new commands can arrive while
    // the client/server objects are being torn down.
    std::shared_ptr<ppp::app::rpc::LocalRpcServer> rpc_server = std::move(rpc_server_);
    if (NULLPTR != rpc_server)
    {
        rpc_server->Dispose();
    }
    ppp::diagnostics::SetLogSink(NULLPTR);

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
#endif

#if defined(_WIN32) || defined(_MACOS)
        // Clear system proxy settings
        if (network_interface_->SetHttpProxy)
        {
            client->ClearHttpProxyToSystemEnv();
        }
#endif

        client->Dispose();
    }

    // The switcher restores DNS/routes/TUN state synchronously in Dispose.
    // Flush its final diagnostics before the application loop is released.
    ppp::diagnostics::FlushLogs();
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

// Build the runtime snapshot JSON consumed by the Rust TUI front-end.
// Field names and semantics follow the Android get_runtime_snapshot contract
// (tests/contracts/runtime-snapshot/) plus desktop-only extensions.
bool PppApplication::BuildRuntimeSnapshot(Json::Value& snapshot) noexcept
{
    snapshot = Json::Value(Json::objectValue);
    snapshot["schema_version"] = 1;
    snapshot["generation"] = (Json::UInt64)Executors::GetTickCount();
    snapshot["monotonic_ms"] = (Json::UInt64)Executors::GetTickCount();
    snapshot["phase"] = "idle";
    snapshot["role"] = proxy_mode_ ? "proxy" : (client_mode_ ? "client" : "server");
    snapshot["server"] = "";
    snapshot["guid"] = "";
    snapshot["vpn_server"] = "pending";
    snapshot["transport"] = "ppp";
    snapshot["bypass_mode"] =
        NULLPTR != network_interface_ ?
            (network_interface_->SplitMode == NetworkInterface::BypassMode::Geo ? "geo" :
             network_interface_->SplitMode == NetworkInterface::BypassMode::No ? "no" : "ip") : "ip";
    snapshot["http_proxy"] = "off";
    snapshot["socks_proxy"] = "off";
    snapshot["connection"] = "unavailable";
    snapshot["mux_state"] = "unavailable";
#if defined(_DEBUG)
    snapshot["hosting_environment"] = client_mode_ ? "client:development" : "server:development";
#else
    snapshot["hosting_environment"] = client_mode_ ? "client:production" : "server:production";
#endif
    snapshot["duration_ms"] = (Json::UInt64)stopwatch_.ElapsedMilliseconds();
    snapshot["requested_mux_mode"] = NULLPTR != configuration_ ? configuration_->mux.mode : "compat";
    snapshot["effective_mux_mode"] = "compat";
    snapshot["mux_receiver_ordering"] = "compat";
    snapshot["mux_active_links"] = 0;
    snapshot["mux_fallback_reason"] = "";
    snapshot["connected_monotonic_ms"] = 0;

    Json::Value capabilities(Json::arrayValue);
    capabilities.append("mux.compat");
    capabilities.append("mux.flow");
    capabilities.append("mux.balance");
    capabilities.append("mux.stripe");
    snapshot["capabilities"] = capabilities;

    // Derive the transport scheme from the configured server URL.
    if (NULLPTR != configuration_ && configuration_->client.server.size() > 0)
    {
        ppp::string server = configuration_->client.server;
        std::size_t scheme = server.find("://");
        if (scheme != ppp::string::npos)
        {
            ppp::string transport = ppp::ToLower<ppp::string>(server.substr(0, scheme));
            if (transport == "ppp" || transport == "ws" || transport == "wss")
            {
                snapshot["transport"] = transport;
            }
            snapshot["server"] = server;
        }
    }

    // Client-mode state: phase, live entry, MUX details.
    std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
    if (NULLPTR != client)
    {
        // Keep the runtime identity fields aligned with the fixed C++ TUI
        // status area.  These are exported through RPC for the Rust TUI;
        // the C++ TUI rendering itself remains unchanged.
        std::shared_ptr<AppConfiguration> active_configuration = client->GetConfiguration();
        ppp::string guid = NULLPTR != active_configuration ?
            active_configuration->client.guid : configuration_->client.guid;
        if (guid.size() >= 2 && guid.front() == '{' && guid.back() == '}')
        {
            guid = guid.substr(1, guid.size() - 2);
        }
        if (!guid.empty())
        {
            snapshot["guid"] = "{" + guid + "}";
        }

        if (ppp::string remote_uri = client->GetRemoteUri(); remote_uri.size() > 0)
        {
            std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger();
            ppp::string vpn_server = remote_uri;
            vpn_server += NULLPTR != exchanger && exchanger->StaticEchoAllocated() ? " [static]" : " [dynamic]";
            if (client->IsProxyOnly())
            {
                vpn_server += " [proxy]";
            }
            snapshot["vpn_server"] = vpn_server;
        }

        auto get_local_proxy = [&client](
            const std::shared_ptr<VEthernetLocalProxySwitcher>& switcher,
            const char* scheme) -> ppp::string
        {
            if (NULLPTR == switcher)
            {
                return "off";
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

            ppp::string value = IPEndPoint::ToEndPoint(
                boost::asio::ip::tcp::endpoint(localIP, localEP.port())).ToString();
            value += "/";
            value += scheme;
            return value;
        };
        snapshot["http_proxy"] = get_local_proxy(client->GetHttpProxy(), "http");
        snapshot["socks_proxy"] = get_local_proxy(client->GetSocksProxy(), "socks");

        std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger();
        if (NULLPTR != exchanger)
        {
            using NetworkState = VEthernetExchanger::NetworkState;
            NetworkState state = exchanger->GetNetworkState();
            const char* connection_states[] = { "connecting", "established", "reconnecting" };
            int connection_state = static_cast<int>(state);
            if (connection_state >= 0 && connection_state < arraysizeof(connection_states))
            {
                snapshot["connection"] = connection_states[connection_state];
            }

            if (client->IsMuxEnabled())
            {
                int mux_network_state = static_cast<int>(exchanger->GetMuxNetworkState());
                ppp::string mux_state =
                    mux_network_state >= 0 && mux_network_state < arraysizeof(connection_states) ?
                        connection_states[mux_network_state] : "unknown";
                mux_state += ", ";
                mux_state += stl::to_string<ppp::string>(client->Mux(NULLPTR));
                mux_state += "-channel";
                snapshot["mux_state"] = mux_state;
            }
            else
            {
                snapshot["mux_state"] = "disabled";
            }

            switch (state)
            {
                case NetworkState::NetworkState_Connecting:
                    snapshot["phase"] = "connecting";
                    break;
                case NetworkState::NetworkState_Established:
                    snapshot["phase"] = "connected";
                    snapshot["connected_monotonic_ms"] = (Json::UInt64)stopwatch_.ElapsedMilliseconds();
                    break;
                case NetworkState::NetworkState_Reconnecting:
                    snapshot["phase"] = "reconnecting";
                    break;
            }

            ppp::string current_entry = exchanger->GetCurrentEntry();
            if (current_entry.size() > 0)
            {
                snapshot["server"] = current_entry;
            }

            if (NULLPTR != exchanger->GetMux())
            {
                std::shared_ptr<vmux::vmux_net> mux = exchanger->GetMux();
                snapshot["effective_mux_mode"] = vmux::vmux_net::mode_name(mux->get_mode());
                snapshot["mux_receiver_ordering"] =
                    mux->get_ordering_mode() == vmux::vmux_net::ordering_flow_v2 ? "flow_v2" : "compat";
                snapshot["mux_active_links"] = mux->get_live_linklayer_count();
            }
        }

        // Traffic statistics: rx/tx are the last OnTick period deltas (they
        // ARE the current rates, mirroring the built-in TUI's TX/RX rows);
        // in/out are the cumulative totals (the built-in TUI's IN/OUT rows).
        uint64_t incoming_traffic = 0;
        uint64_t outgoing_traffic = 0;
        std::shared_ptr<ppp::transmissions::ITransmissionStatistics> statistics_snapshot;
        if (GetTransmissionStatistics(incoming_traffic, outgoing_traffic, statistics_snapshot))
        {
            snapshot["traffic"]["rx_bytes"] = (Json::UInt64)incoming_traffic;
            snapshot["traffic"]["tx_bytes"] = (Json::UInt64)outgoing_traffic;
            if (NULLPTR != statistics_snapshot)
            {
                snapshot["traffic"]["in_bytes"] = (Json::UInt64)statistics_snapshot->IncomingTraffic;
                snapshot["traffic"]["out_bytes"] = (Json::UInt64)statistics_snapshot->OutgoingTraffic;
            }
        }

        // Network interfaces (TUN + physical NIC).
        Json::Value network(Json::objectValue);
        network["mode"] = client->IsProxyOnly() ? "proxy-only" : "tun";
        network["adapter"] = client->IsProxyOnly() ? "none" : "";
        network["logical_ipv4"] = "";
        network["logical_ipv6"] = "";
        network["tunnel_dns"] = "";
        network["link_state"] = snapshot["connection"];
        network["mux_state"] = snapshot["mux_state"];
        network["tcp_ip_transport"] = "PPP tunnel";
        network["dns_transport"] = "PPP tunnel";
        network["aggligator"] = "none";
        network["proxy_interlayer"] = "none";
#if defined(SYSNAT)
        network["tcp_ip_cc"] = client->IsLwip() ? "lwip" :
            (client->IsSysnat() ? "tc" : "ctcp");
#else
        network["tcp_ip_cc"] = client->IsLwip() ? "lwip" : "ctcp";
#endif
        network["block_quic"] = client->IsBlockQUIC() ? "blocked" : "unblocked";
        auto write_interface = [](Json::Value& target, const std::shared_ptr<VEthernetNetworkSwitcher::NetworkInterface>& ni) noexcept
            {
                if (NULLPTR == ni) return;
                if (ni->Name.size() > 0) target["name"] = ni->Name;
#if defined(_WIN32)
                if (ni->Description.size() > 0) target["description"] = ni->Description;
#endif
                target["index"] = ni->Index;
#if !defined(_MACOS)
                if (ni->Id.size() > 0) target["id"] = ni->Id;
#endif
                if (ni->IPAddress.is_v4() || ni->IPAddress.is_v6())
                {
                    target["ipv4"] = ppp::net::Ipep::ToAddressString<ppp::string>(ni->IPAddress);
                }
                target["gateway"] = ni->GatewayServer.to_string().data();
                target["subnet_mask"] = ni->SubmaskAddress.to_string().data();
                if (ni->IPv6GatewayServer.is_v6() && !ni->IPv6GatewayServer.is_unspecified())
                {
                    target["ipv6"] = ppp::net::Ipep::ToAddressString<ppp::string>(ni->IPv6GatewayServer);
                    target["ipv6_gateway"] = ppp::net::Ipep::ToAddressString<ppp::string>(ni->IPv6GatewayServer);
                }
                if (!ni->DnsAddresses.empty())
                {
                    Json::Value dns(Json::arrayValue);
                    for (const boost::asio::ip::address& address : ni->DnsAddresses)
                    {
                        dns.append(ppp::net::Ipep::ToAddressString<ppp::string>(address));
                    }
                    target["dns"] = dns;
                }
            };
        write_interface(network["tun"], client->GetTapNetworkInterface());
        write_interface(network["nic"], client->GetUnderlyingNetworkInterface());
        if (std::shared_ptr<ITap> tap = client->GetTap(); NULLPTR != tap)
        {
            if (tap->IPAddress != ppp::net::IPEndPoint::AnyAddress)
            {
                network["logical_ipv4"] = ppp::net::IPEndPoint::ToAddressString(tap->IPAddress);
            }
            if (tap->IPv6Address.is_v6() && !tap->IPv6Address.is_unspecified())
            {
                network["logical_ipv6"] = tap->IPv6Address.to_string().data();
                network["tun"]["ipv6_address"] = tap->IPv6Address.to_string().data();
            }
            if (tap->IPv6GatewayServer.is_v6() && !tap->IPv6GatewayServer.is_unspecified())
            {
                network["tun"]["ipv6_gateway"] = tap->IPv6GatewayServer.to_string().data();
            }
            if (tap->IPv6SubmaskAddress.is_v6() && !tap->IPv6SubmaskAddress.is_unspecified())
            {
                network["tun"]["ipv6_subnet_mask"] = tap->IPv6SubmaskAddress.to_string().data();
            }
        }
        if (network["tun"].isObject() && network["tun"]["name"].isString())
        {
            network["adapter"] = network["tun"]["name"];
        }
        if (NULLPTR != configuration_ && configuration_->udp.dns.redirect.size() > 0)
        {
            network["tunnel_dns"] = configuration_->udp.dns.redirect;
        }
        if (std::shared_ptr<aggligator::aggligator> aggligator = client->GetAggligator(); NULLPTR != aggligator)
        {
            const char* aggligator_status[] = { "none", "unknown", "connecting", "reconnecting", "established" };
            int max_channel = 0;
            int max_servers = 0;
            aggligator->client_fetch_concurrency(max_servers, max_channel);
            int status = static_cast<int>(aggligator->status());
            if (status < 0 || status >= arraysizeof(aggligator_status)) status = 1;
            network["aggligator"] = ppp::string(aggligator_status[status]) + ", " +
                stl::to_string<ppp::string>(max_servers) + "-server, " +
                stl::to_string<ppp::string>(max_channel) + "-channel";
        }
        if (std::shared_ptr<ppp::transmissions::proxys::IForwarding> forwarding = client->GetForwarding();
            NULLPTR != forwarding)
        {
            ppp::string proxy_url = forwarding->GetProxyUrl();
            network["proxy_interlayer"] = proxy_url.empty() ? "none" : proxy_url;
        }
        snapshot["network"] = network;

        // Route inputs and counters for the Rust TUI.  Counts are parsed from
        // the configured prefix files even when GEO is the active engine.
        Json::Value routes(Json::objectValue);
        routes["bypass_ipv4_file"] = "(none)";
        routes["bypass_ipv6_file"] = "(none)";
        routes["bypass_gateway"] = "0.0.0.0";
        routes["bypass_gateway_ipv6"] = "::";
        routes["dns_rules_file"] = "";

        routes["dns_rule_count"] = (Json::UInt64)0;
        routes["geo_rules_file"] = "";
        routes["geosite_file"] = "";
        routes["geoip_file"] = "";
        if (NULLPTR != network_interface_)
        {
            if (NULLPTR != network_interface_->Bypass &&
                !network_interface_->Bypass->empty())
            {
                routes["bypass_ipv4_file"] = network_interface_->Bypass->begin()->data();
            }
            if (NULLPTR != network_interface_->Bypass6 &&
                !network_interface_->Bypass6->empty())
            {
                routes["bypass_ipv6_file"] = network_interface_->Bypass6->begin()->data();
            }
            routes["bypass_gateway"] = network_interface_->BypassNgw.to_string().data();
            routes["bypass_gateway_ipv6"] = network_interface_->BypassNgw6.to_string().data();
            routes["dns_rules_file"] = network_interface_->DNSRules.data();
            routes["geo_rules_file"] = network_interface_->GeoRules.data();
            routes["geosite_file"] = network_interface_->GeoSite.data();
            routes["geoip_file"] = network_interface_->GeoIP.data();
            const RouteSegmentCounts segment_counts = CountConfiguredRouteSegments(*network_interface_);

            routes["dns_rule_count"] = (Json::UInt64)CountConfiguredDnsRules(network_interface_->DNSRules);
        }
        snapshot["routes"] = routes;

        // Geo split summaries.
        std::shared_ptr<ppp::app::client::geo::GeoRuleEngine> geo_rules = client->GetGeoRules();
        if (NULLPTR != geo_rules)
        {
            Json::Value geo(Json::objectValue);
            Json::Value direct_dns(Json::arrayValue);
            if (geo_rules->UsesLocalDirectDns())
            {
                direct_dns.append("local");
            }
            for (const boost::asio::ip::address& address : geo_rules->GetDirectDnsServers())
            {
                direct_dns.append(ppp::net::Ipep::ToAddressString<ppp::string>(address));
            }
            geo["direct_dns"] = direct_dns;
            geo["rule_count"] = (Json::UInt64)geo_rules->GetRuleCount();
            geo["dns_rule_count"] = routes["dns_rule_count"];
            geo["static_networks"] = (Json::UInt64)geo_rules->GetStaticNetworks().size();

            Json::Value split_rules(Json::arrayValue);
            auto outbound_statuses = client->GetOutboundStatuses();
            auto find_status = [&outbound_statuses](const ppp::string& tag) noexcept ->
                const VEthernetNetworkSwitcher::OutboundStatus*
                {
                    for (const auto& status : outbound_statuses)
                    {
                        if (status.tag == tag) return &status;
                    }
                    return NULLPTR;
                };
            for (const auto& rule : geo_rules->GetRuleSummaries())
            {
                if (rule.action != ppp::app::client::geo::GeoRuleEngine::Action::Tunnel ||
                    rule.outbound.empty())
                {
                    continue;
                }
                const auto* target = find_status(rule.outbound);
                if (NULLPTR == target || !target->route_used) continue;

                Json::Value item(Json::objectValue);
                item["matcher"] = rule.type + "," + rule.value;
                item["outbound"] = rule.outbound;
                item["display"] = target->display_name.empty() ? target->tag : target->display_name;
                split_rules.append(item);
            }
            geo["split_rules"] = split_rules;
            snapshot["geo"] = geo;
        }

        // Outbound list (= GetOutboundStatuses).
        Json::Value outbounds(Json::arrayValue);
        for (const auto& status : client->GetOutboundStatuses())
        {
            Json::Value item(Json::objectValue);
            item["tag"] = status.tag;
            item["display_name"] = status.display_name;
            item["server"] = status.server;
            item["state"] = status.state;
            item["reconnects"] = status.reconnects;
            item["active"] = status.active;
            item["server_menu"] = status.server_menu;
            item["route_used"] = status.route_used;
            item["multiple_entries"] = status.multiple_entries;
            item["probe_enabled"] = status.probe_enabled;
            item["probe_checked"] = status.probe_checked;
            item["probe_reachable"] = status.probe_reachable;
            item["probe_rtt_ms"] = status.probe_rtt_ms;
            item["current_entry"] = status.current_entry;
            item["ranked_first_entry"] = status.ranked_first_entry;
            item["probe_entry"] = status.probe_entry;
            outbounds.append(item);
        }
        snapshot["outbounds"] = outbounds;
    }

    snapshot["last_error"]["code"] = 0;
    snapshot["last_error"]["severity"] = "";
    snapshot["last_error"]["retryable"] = false;
    snapshot["last_error"]["user_message_key"] = "";
    snapshot["last_error"]["diagnostic_detail"] = "";
    snapshot["log_level"] = ppp::diagnostics::LogLevelName(
        static_cast<ppp::diagnostics::LogLevel>(ppp::diagnostics::GetLogLevel()));
    return true;
}

// Execute one RPC command.  Called on the io_context thread through the
// LocalRpcServer handler; commands map 1:1 onto existing application methods.
bool PppApplication::ExecuteRpcCommand(const ppp::string& method, const Json::Value& params, Json::Value& result, ppp::string& error) noexcept
{
    if (method == "ping")
    {
        result["pong"] = true;
        return true;
    }

    if (method == "get_snapshot")
    {
        return BuildRuntimeSnapshot(result);
    }

    if (method == "get_log_level")
    {
        result["level"] = ppp::diagnostics::LogLevelName(
            static_cast<ppp::diagnostics::LogLevel>(ppp::diagnostics::GetLogLevel()));
        return true;
    }

    if (method == "set_log_level")
    {
        ppp::string value = ppp::auxiliary::JsonAuxiliary::AsString(
            params.get("level", Json::Value()));
        if (value.empty())
        {
            error = "missing level";
            return false;
        }

        ppp::diagnostics::SetLogLevel(static_cast<int>(
            ppp::diagnostics::ParseLogLevel(value.data())));
        result["level"] = ppp::diagnostics::LogLevelName(
            static_cast<ppp::diagnostics::LogLevel>(ppp::diagnostics::GetLogLevel()));
        return true;
    }

    if (method == "get_outbounds")
    {
        Json::Value snapshot;
        if (!BuildRuntimeSnapshot(snapshot)) return false;
        result = snapshot.get("outbounds", Json::Value(Json::arrayValue));
        return true;
    }

    if (method == "get_logs")
    {
        uint64_t since_seq = ppp::auxiliary::JsonAuxiliary::AsUInt64(
            params.get("since_seq", Json::Value((Json::UInt64)0)));

        Json::Value logs(Json::arrayValue);
        {
            std::lock_guard<std::mutex> scope(g_rpc_logs_syncobj);
            for (const RpcLogEntry& entry : g_rpc_logs)
            {
                if (entry.seq <= since_seq) continue;

                Json::Value item(Json::objectValue);
                item["seq"] = (Json::UInt64)entry.seq;
                item["level"] = entry.level;
                item["line"] = entry.line;
                item["timestamp_ms"] = (Json::UInt64)entry.timestamp_ms;
                logs.append(item);
            }
        }
        result["logs"] = logs;
        result["latest_seq"] = (Json::UInt64)g_rpc_log_seq.load(std::memory_order_relaxed);
        return true;
    }

    if (method == "switch_server" || method == "switch_rank1")
    {
        ppp::string tag = ppp::auxiliary::JsonAuxiliary::AsString(params.get("tag", Json::Value()));
        if (tag.empty())
        {
            error = "missing tag";
            return false;
        }

        std::shared_ptr<VEthernetNetworkSwitcher> client = client_;
        if (NULLPTR == client)
        {
            error = "no client";
            return false;
        }

        bool ok = method == "switch_rank1" ?
            client->SwitchPrimaryOutboundToRankedFirst(tag) :
            client->SwitchPrimaryOutbound(tag);
        result["accepted"] = ok;
        result["tag"] = tag;
        return true;
    }

    if (method == "shutdown")
    {
        ppp::string confirm = ppp::auxiliary::JsonAuxiliary::AsString(params.get("confirm", Json::Value()));
        if (confirm != "shutdown")
        {
            error = "confirm required";
            return false;
        }

        bool restart = ppp::auxiliary::JsonAuxiliary::AsBoolean(params.get("restart", Json::Value(false)));
        result["accepted"] = ShutdownApplication(restart);
        return true;
    }

    error = "unknown method";
    return false;
}

// Main periodic tick handler
bool PppApplication::OnTick(uint64_t now) noexcept
{
    using RouteIPListTablePtr = VEthernetNetworkSwitcher::RouteIPListTablePtr;
    using NetworkState        = VEthernetExchanger::NetworkState;

#if defined(_WIN32)
    CONSOLE_SELECTION_INFO selection{};
    bool console_selection_active =
        GetConsoleSelectionInfo(&selection) && selection.dwFlags != CONSOLE_NO_SELECTION;
#else
    constexpr bool console_selection_active = false;
#endif

    // Handle console keyboard input for tab switching
    if (!headless_ && !console_selection_active)
    {
        HandleConsoleInput();

        // Update console display
        PrintEnvironmentInformation();
    }

    // Push new log lines to RPC clients (drained at most once per tick,
    // capped so a verbose core cannot flood the RPC pipe).
    std::shared_ptr<ppp::app::rpc::LocalRpcServer> rpc_server = rpc_server_;
    if (NULLPTR != rpc_server)
    {
        uint64_t last_pushed = g_rpc_logs_last_pushed_seq.load(std::memory_order_relaxed);
        uint64_t latest = g_rpc_log_seq.load(std::memory_order_relaxed);
        if (latest > last_pushed)
        {
            ppp::vector<Json::Value> frames;
            uint64_t pushed_until = last_pushed;
            {
                std::lock_guard<std::mutex> scope(g_rpc_logs_syncobj);
                for (const RpcLogEntry& entry : g_rpc_logs)
                {
                    if (entry.seq <= last_pushed) continue;

                    Json::Value params(Json::objectValue);
                    params["seq"] = (Json::UInt64)entry.seq;
                    params["level"] = entry.level;
                    params["line"] = entry.line;
                    params["timestamp_ms"] = (Json::UInt64)entry.timestamp_ms;
                    Json::Value frame(Json::objectValue);
                    frame["event"] = "log";
                    frame["params"] = params;
                    frames.emplace_back(std::move(frame));
                    pushed_until = entry.seq;
                    if (frames.size() >= 16) break; // per-tick cap (verbose builds can be noisy)
                }
            }
            for (const Json::Value& frame : frames)
            {
                rpc_server->Broadcast(frame);
            }
            g_rpc_logs_last_pushed_seq.store(pushed_until, std::memory_order_relaxed);
        }
    }

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
    if (NULLPTR == server && NULLPTR == client && !app->catalog_only_)
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
    fprintf(stdout, "[CoreStartup] server-config directory='%s'\r\n", server_directory_.data());
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
    auto normalize_configuration_path = [](const ppp::string& value) noexcept -> ppp::string
    {
        if (value.empty()) return ppp::string();
        ppp::string path = File::GetFullPath(File::RewritePath(value.data()).data());
        return ppp::ToLower<ppp::string>(ppp::ATrim<ppp::string>(path));
    };
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
        fprintf(stdout, "[CoreStartup] server-config load begin file='%s'\r\n", file.data());
        if (NULLPTR == configuration || !configuration->Load(file))
        {
            fprintf(stdout, "Failed to load server configuration: %s\r\n", file.data());
            return false;
        }

        fprintf(stdout, "[CoreStartup] server-config load ok file='%s'\r\n", file.data());

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
                    // The primary outbound is the default path, not a split
                    // target.  A GEO manifest also declares it under
                    // `outbounds.main`, but that must not add the UI `split`
                    // marker to the main server row.
                    outbound.route_used = false;
                    break;
                }
            }
            ++loaded;
            continue;
        }

        // A GEO outbound may already have been loaded from geo-rules.yaml
        // before --server-dir is processed. Reuse that record so the server
        // menu row (for example zgo) inherits route_used=true instead of
        // creating a second, unmarked server:zgo row.
        std::size_t matched_route_index = outbound_configurations_.size();
        for (std::size_t index = 0; index < outbound_configurations_.size(); ++index)
        {
            ClientOutboundConfiguration& outbound = outbound_configurations_[index];
            if (!outbound.route_used || outbound.server_menu ||
                ppp::ToLower<ppp::string>(outbound.tag) == "main")
            {
                continue;
            }

            const bool same_source = !outbound.source_path.empty() &&
                normalize_configuration_path(outbound.source_path) ==
                normalize_configuration_path(file);
            const bool same_identity = NULLPTR != outbound.configuration &&
                outbound.configuration->client.guid == configuration->client.guid &&
                outbound.configuration->client.server == configuration->client.server;
            if (same_source || same_identity)
            {
                matched_route_index = index;
                break;
            }
        }
        if (matched_route_index < outbound_configurations_.size())
        {
            ClientOutboundConfiguration& matched_route =
                outbound_configurations_[matched_route_index];
            matched_route.display_name = display_name;
            matched_route.server_menu = true;
            matched_route.source_path = file;
            LOG_INFO("PppApplication::LoadServerConfigurations: server=%s merged_geo_tag=%s, route_used=1",
                display_name.data(), matched_route.tag.data());
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

    // The GEO manifest is loaded before --server-dir, so a split row such as
    // `us` temporarily appears near `main`. Rebuild only the presentation
    // order after all files are merged: main first, then exactly the sorted
    // server-directory order, while preserving any GEO-only rows afterward.
    if (!outbound_configurations_.empty())
    {
        ppp::vector<bool> ordered_flags(outbound_configurations_.size(), false);
        ppp::vector<ClientOutboundConfiguration> ordered;
        ordered.reserve(outbound_configurations_.size());

        for (std::size_t index = 0; index < outbound_configurations_.size(); ++index)
        {
            if (ppp::ToLower<ppp::string>(outbound_configurations_[index].tag) == "main")
            {
                ordered_flags[index] = true;
                ordered.emplace_back(std::move(outbound_configurations_[index]));
                break;
            }
        }
        for (const ppp::string& file : files)
        {
            const ppp::string normalized_file = normalize_configuration_path(file);
            for (std::size_t index = 0; index < outbound_configurations_.size(); ++index)
            {
                if (ordered_flags[index])
                {
                    continue;
                }
                const ClientOutboundConfiguration& outbound = outbound_configurations_[index];
                if (!outbound.server_menu ||
                    normalize_configuration_path(outbound.source_path) != normalized_file)
                {
                    continue;
                }
                ordered_flags[index] = true;
                ordered.emplace_back(std::move(outbound_configurations_[index]));
                break;
            }
        }
        for (std::size_t index = 0; index < outbound_configurations_.size(); ++index)
        {
            if (!ordered_flags[index])
            {
                ordered.emplace_back(std::move(outbound_configurations_[index]));
            }
        }
        outbound_configurations_ = std::move(ordered);
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

    auto normalize_configuration_path = [](const ppp::string& value) noexcept -> ppp::string
    {
        if (value.empty()) return ppp::string();
        ppp::string path = File::GetFullPath(File::RewritePath(value.data()).data());
        return ppp::ToLower<ppp::string>(ppp::ATrim<ppp::string>(path));
    };

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

        const ppp::string declaration_path = normalize_configuration_path(declaration.path);

        ClientOutboundConfiguration* matched = NULLPTR;
        bool matched_same_source = false;
        bool matched_same_identity = false;
        for (ClientOutboundConfiguration& outbound : outbound_configurations_)
        {
            const bool same_source = !declaration_path.empty() &&
                normalize_configuration_path(outbound.source_path) == declaration_path;
            const bool same_identity = NULLPTR != outbound.configuration &&
                outbound.configuration->client.guid == configuration->client.guid &&
                outbound.configuration->client.server == configuration->client.server;
            if (same_source || same_identity)
            {
                matched = &outbound;
                matched_same_source = same_source;
                matched_same_identity = same_identity;
                break;
            }
        }

        if (NULLPTR != matched)
        {
            // The outbound may already be the same GEO declaration. This is
            // common when LoadConfiguration parsed geo-rules.yaml before
            // --server-dir and the latter merely promoted the row to the
            // server menu. Keep its display/source metadata intact.
            if (matched->tag == declaration.tag && matched->route_used)
            {
                continue;
            }

            bool matched_primary =
                ppp::ToLower<ppp::string>(matched->tag) == "main" &&
                matched->configuration == primary;
            if (matched_primary)
            {
                // Keep the runtime primary under "main" and expose the GEO
                // tag as an alias. The switcher maps this alias back to main,
                // so the same GUID/server never opens a second control link.
                matched->route_used = false;
                ppp::string display_name = matched->display_name;
                bool server_menu = matched->server_menu;
                matched->display_name = "main";
                matched->server_menu = false;
                outbound_configurations_.emplace_back(ClientOutboundConfiguration{
                    declaration.tag, matched->configuration,
                    display_name.empty() ? declaration.tag : display_name,
                    server_menu, declaration.path, true });
                continue;
            }

            // Reuse the server-directory connection under the stable YAML tag.
            // This avoids opening the same GUID/server twice and makes a rule
            // such as "geosite,openai,us" select the visible zgo menu entry.
            const ppp::string previous_tag = matched->tag;
            matched->tag = declaration.tag;
            matched->source_path = declaration.path;
            matched->route_used = true;
            LOG_INFO("PppApplication::LoadGeoOutboundConfigurations: outbound=%s matched_existing=%s, route_used=1, same_source=%d, same_identity=%d",
                declaration.tag.data(), previous_tag.data(),
                (int)matched_same_source, (int)matched_same_identity);
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

    // The Rust desktop client starts this control-plane phase before the
    // user chooses a server.  Do not inspect --config or fallback files here:
    // the only configuration loaded in this phase is the server directory.
    if (catalog_only_)
    {
        fprintf(stdout, "[CoreStartup] configuration-load catalog-only branch\r\n");
        std::shared_ptr<AppConfiguration> configuration =
            ppp::make_shared_object<AppConfiguration>();
        if (NULLPTR != configuration)
        {
            LOG_INFO("Catalog-only mode: no primary configuration loaded; reading server directory");
            path.clear();
            return configuration;
        }
    }

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
        fprintf(stdout, "[CoreStartup] configuration candidate key='%s' path='%s'\r\n",
            argument_key, argument_value.data());
        if (argument_value.empty())
        {
            continue;
        }

        ppp::string argument_lower = ppp::ToLower<ppp::string>(argument_value);
        bool requested_geo_manifest =
            (argument_lower.size() >= 4 &&
             argument_lower.compare(argument_lower.size() - 4, 4, ".txt") == 0) ||
            (argument_lower.size() >= 5 &&
             argument_lower.compare(argument_lower.size() - 5, 5, ".yaml") == 0) ||
            (argument_lower.size() >= 4 &&
             argument_lower.compare(argument_lower.size() - 4, 4, ".yml") == 0);
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

    // A .txt / .yaml / .yml file passed as --config is a geo multi-outbound
    // manifest.  It is parsed before the normal JSON configuration because its
    // main= entry identifies the primary AppConfiguration used for all system-
    // level state.
    ppp::string selected_lower = ppp::ToLower<ppp::string>(path);
    bool geo_manifest =
        (selected_lower.size() >= 4 &&
         selected_lower.compare(selected_lower.size() - 4, 4, ".txt") == 0) ||
        (selected_lower.size() >= 5 &&
         selected_lower.compare(selected_lower.size() - 5, 5, ".yaml") == 0) ||
        (selected_lower.size() >= 4 &&
         selected_lower.compare(selected_lower.size() - 4, 4, ".yml") == 0);
    if (geo_manifest)
    {
        using GeoRuleEngine = ppp::app::client::geo::GeoRuleEngine;
        ppp::vector<GeoRuleEngine::OutboundConfiguration> declarations;
        ppp::string final_outbound;
        ppp::string error;
        if (!GeoRuleEngine::ParseOutboundConfigurations(
            path, declarations, final_outbound, error, true) || declarations.empty())
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
                declaration.tag, configuration, declaration.tag, false,
                declaration.path, !declaration.primary });
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
        fprintf(stdout, "[CoreStartup] configuration file check path='%s'\r\n", configuration_path.data());
        if (!File::Exists(configuration_path.data()))
        {
            continue;
        }

        std::shared_ptr<AppConfiguration> configuration = ppp::make_shared_object<AppConfiguration>();
        if (NULLPTR == configuration)
        {
            continue;
        }

        fprintf(stdout, "[CoreStartup] configuration file load begin path='%s'\r\n", configuration_path.data());
        if (!configuration->Load(configuration_path))
        {
            continue;
        }
        fprintf(stdout, "[CoreStartup] configuration file load ok path='%s'\r\n", configuration_path.data());

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
    static const char* const OPENPPP2_TAP_NAME = "PPP PRIVATE NETWORK 2 TAP";
    fprintf(stdout, "[PrepEth] ComponentId='%s' Wintun='%s'\r\n", network_interface->ComponentId.data(), network_interface->Wintun.data());

    ppp::tap::TapWindows::DriverMode driver_mode = ppp::tap::TapWindows::GetDriverMode();
    const char* driver_mode_name = driver_mode == ppp::tap::TapWindows::DriverMode::Wintun ? "wintun" :
        (driver_mode == ppp::tap::TapWindows::DriverMode::Tap ? "tap" : "auto");
    bool use_wintun = driver_mode == ppp::tap::TapWindows::DriverMode::Wintun ||
        (driver_mode == ppp::tap::TapWindows::DriverMode::Auto && ppp::tap::TapWindows::IsWintun());
    fprintf(stdout, "[PrepEth] driver_mode=%s(%d) use_wintun=%d\r\n",
        driver_mode_name, static_cast<int>(driver_mode), use_wintun ? 1 : 0);
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

            // The default Windows name is also used by the Wintun adapter.
            // If TAP mode needs to create a device, give it an explicit
            // openppp2-owned name instead of letting Windows choose "PPP 1"
            // or falling back to another application's TAP adapter.
            ppp::string tap_name = network_interface->Wintun;
            if (driver_mode == ppp::tap::TapWindows::DriverMode::Tap &&
                (tap_name.empty() || ppp::ToLower<ppp::string>(tap_name) == "ppp"))
            {
                tap_name = OPENPPP2_TAP_NAME;
                network_interface->Wintun = tap_name;
                fprintf(stdout, "[PrepEth] TAP mode using dedicated adapter name '%s'\r\n",
                    tap_name.data());
            }

            // Install the TAP-Windows vNIC in the Windows operating system.
            ppp::string driverPath = File::GetFullPath((ppp::GetApplicationStartupPath() + "\\Driver\\").data());
            fprintf(stdout, "[PrepEth] driverPath=%s\r\n", driverPath.data());
            ppp::string newTapGuid = ppp::tap::TapWindows::InstallDriver(driverPath.data(), tap_name);
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
    fprintf(stdout, "[CoreStartup] Main enter client=%d proxy=%d headless=%d catalog_only=%d config='%s'\r\n",
        client_mode_ ? 1 : 0,
        proxy_mode_ ? 1 : 0,
        headless_ ? 1 : 0,
        catalog_only_ ? 1 : 0,
        configuration_path_.data());

    // Require administrator/root privileges
    if (!proxy_mode_ && !ppp::IsUserAnAdministrator()) // $ROOT is 0.
    {
        SetStartupFailure("administrator privileges are required");
        fprintf(stdout, "%s\r\n", "Non-administrators are not allowed to run.");
        fprintf(stdout, "[CoreStartup] FAIL stage=administrator-check\r\n");
        return -1;
    }

    // Prevent multiple instances
    ppp::string rerun_name = (proxy_mode_ ? "proxy://" : (client_mode_ ? "client://" : "server://")) + configuration_path_;
    if (prevent_rerun_.Exists(rerun_name.data()))
    {
        SetStartupFailure("another core instance is already running");
        fprintf(stdout, "%s\r\n", "Repeat runs are not allowed.");
        fprintf(stdout, "[CoreStartup] FAIL stage=repeat-run-lock exists=1\r\n");
        return -1;
    }

    // Create instance lock
    if (!prevent_rerun_.Open(rerun_name.data()))
    {
        SetStartupFailure("failed to open repeat-run lock");
        fprintf(stdout, "%s\r\n", "Failed to open the repeat run lock.");
        fprintf(stdout, "[CoreStartup] FAIL stage=repeat-run-lock open=0\r\n");
        return -1;
    }

#if defined(_WIN32)
    // Windows-specific setup
    if (client_mode_ && !proxy_mode_)
    {
        fprintf(stdout, "[CoreStartup] stage=windows-prepare begin\r\n");
        // Prepare the environment for the virtual Ethernet network device card.
        if (!Windows_PreparedEthernetEnvironment(network_interface_))
        {
            SetStartupFailure("Windows tunnel driver preparation failed");
            fprintf(stdout, "[CoreStartup] FAIL stage=windows-prepare\r\n");
            return -1;
        }
        fprintf(stdout, "[CoreStartup] stage=windows-prepare ok component='%s'\r\n",
            network_interface_->ComponentId.data());
    }

    // Save original QUIC setting
    quic_ = ppp::net::proxies::HttpProxy::IsSupportExperimentalQuicProtocol();
#endif

    // Catalog-only mode starts the process/RPC control plane and reads the
    // server directory, but deliberately does not create a client, TUN, or
    // outbound connection.  The Rust UI starts a second, real core after a
    // server configuration is selected.
    if (!catalog_only_ && !PreparedLoopbackEnvironment(network_interface_))
    {
        if (startup_failure_.empty())
        {
            SetStartupFailure("VPN loopback environment preparation failed");
        }
        fprintf(stdout, "[CoreStartup] FAIL stage=loopback-prepare\r\n");
        return -1;
    }
    fprintf(stdout, "[CoreStartup] stage=loopback-prepare ok skipped=%d\r\n", catalog_only_ ? 1 : 0);

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

#if defined(_WIN32) || defined(_MACOS)
        // Configure the optional system proxy on both desktop platforms.  The
        // local HTTP proxy performs the actual per-destination split.
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
#endif
        if (network_interface_->SetHttpProxy)
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

    // Start the local RPC server (Rust TUI front-end / headless control).
    if (rpc_listen_.size() > 0)
    {
        std::shared_ptr<PppApplication> self = shared_from_this();
        std::shared_ptr<ppp::app::rpc::LocalRpcServer> rpc_server =
            ppp::make_shared_object<ppp::app::rpc::LocalRpcServer>(
                Executors::GetDefault(), rpc_token_, rpc_max_clients_,
                [self](const ppp::string& method, const Json::Value& params, Json::Value& result, ppp::string& error) noexcept -> bool
                {
                    return NULLPTR != self && self->ExecuteRpcCommand(method, params, result, error);
                });
        if (NULLPTR == rpc_server || !rpc_server->Open(rpc_listen_))
        {
            SetStartupFailure("failed to open local RPC server");
            fprintf(stdout, "%s\r\n", "Failed to open the local RPC server.");
            fprintf(stdout, "[CoreStartup] FAIL stage=rpc-open listen='%s'\r\n", rpc_listen_.data());
            return -1;
        }

        rpc_server_ = std::move(rpc_server);

        // Feed the RPC log ring buffer from every desktop log line.
        ppp::diagnostics::SetLogSink(RpcLogSink);

        // Headless mode prints a single machine-readable line so the TUI
        // front-end can discover the actual port when 0 (random) was used.
        // stdout may be a pipe (TUI launcher): pipes are fully buffered, so
        // flush explicitly or the line never reaches the parent.
        if (headless_)
        {
            boost::asio::ip::tcp::endpoint endpoint = rpc_server_->GetLocalEndPoint();
            boost::asio::ip::address address = endpoint.address();
            ppp::string address_string = ppp::net::Ipep::ToAddressString<ppp::string>(address);
            fprintf(stdout, "RPC_LISTEN=%s:%d\r\n", address_string.data(), endpoint.port());
            fflush(stdout);
            fprintf(stdout, "[CoreStartup] stage=rpc-ready endpoint=%s:%d\r\n",
                address_string.data(), endpoint.port());
        }
    }

    // Start periodic updates
    if (!NextTickAlwaysTimeout(false))
    {
        SetStartupFailure("failed to create core periodic timer");
        return -1;
    }
    return 0;
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

    // Register shutdown handlers.  An embedded host has its own synchronous
    // C ABI stop path; registering the standalone handler here would consume
    // Ctrl+C/console-close before the Rust owner can restore the terminal.
    if (!g_core_in_process_host.load(std::memory_order_acquire))
    {
        PppApplication::AddShutdownApplicationEventHandler();
    }

    // Register restart signal handler on Unix-like systems
#if SIGRESTART
    if (!g_core_in_process_host.load(std::memory_order_acquire))
    {
        signal(SIGRESTART, // SIG_DFL
            [](int) noexcept
            {
                PppApplication::ShutdownApplication(true);
            });
    }
#endif

// Run main application
    return APP->Main(argc, argv);
}

// -----------------------------------------------------------------------------
// In-process core host API
// -----------------------------------------------------------------------------
//
// The Rust TUI/CLI can link the core as a static library and call this API
// directly.  The executor still runs on a dedicated C++ thread, but there is
// no child process, temporary executable, stdout discovery or loopback RPC.
// All calls crossing the language boundary use the opaque handle and UTF-8
// JSON strings declared in ppp/core/CoreApi.h.

struct ppp_core_handle final {
    struct CommandRequest final {
        std::mutex                                          mutex;
        std::condition_variable                             completed_cv;
        bool                                                completed = false;
        bool                                                success = false;
        std::string                                         result;
        std::string                                         error;
    };

    std::mutex                                              state_mutex;
    std::condition_variable                                 state_cv;
    std::mutex                                              command_mutex;
    std::shared_ptr<boost::asio::io_context>                context;
    std::thread                                             thread;
    bool                                                    startup_completed = false;
    bool                                                    startup_success = false;
    bool                                                    finished = false;
    int                                                     result_code = -1;
    std::string                                             error;
};

static void CoreApiSetError(char* buffer, size_t buffer_size, const std::string& error) noexcept
{
    if (NULLPTR == buffer || buffer_size < 1) {
        return;
    }

    size_t length = std::min(buffer_size - 1, error.size());
    if (length > 0) {
        std::memcpy(buffer, error.data(), length);
    }
    buffer[length] = '\x0';
}

static void CoreApiSignalStartup(
    ppp_core_handle* handle,
    bool success,
    int result_code,
    const std::string& error) noexcept
{
    if (NULLPTR == handle) {
        return;
    }

    std::lock_guard<std::mutex> scope(handle->state_mutex);
    handle->startup_completed = true;
    handle->startup_success = success;
    handle->result_code = result_code;
    if (!success) {
        handle->error = error;
    }
    handle->state_cv.notify_all();
}

static void CoreApiSignalFinished(ppp_core_handle* handle, int result_code) noexcept
{
    if (NULLPTR == handle) {
        return;
    }

    std::lock_guard<std::mutex> scope(handle->state_mutex);
    handle->finished = true;
    handle->result_code = result_code;
    handle->state_cv.notify_all();
}

static void CoreApiRun(
    ppp_core_handle* handle,
    const std::shared_ptr<std::vector<std::string>>& arguments) noexcept
{
    ConfigureCoreOutputStreams();
    fprintf(stdout, "[CoreApi] runtime thread begin argc=%zu\r\n", arguments ? arguments->size() : 0u);

    struct CoreApiInstanceGuard final {
        ~CoreApiInstanceGuard() noexcept {
            g_core_api_instance_active.store(false, std::memory_order_release);
        }
    } active_guard;

    int result_code = -1;
    std::shared_ptr<PppApplication> APP;
    g_core_in_process_host.store(true, std::memory_order_release);

    try {
        std::vector<const char*> argv;
        argv.reserve(arguments->size());
        for (const std::string& argument : *arguments) {
            argv.emplace_back(argument.c_str());
        }

        int argc = static_cast<int>(argv.size());
        const char** argv_data = argv.empty() ? NULLPTR : argv.data();

        ppp::RT = ppp::ToBoolean(ppp::GetCommandArgument("--rt", argc, argv_data, "y").data());
        fprintf(stdout, "[CoreApi] stage=global-init begin\r\n");
        std::call_once(g_core_runtime_initializer,
            []() noexcept
            {
                ppp::global::cctor();
            });
        fprintf(stdout, "[CoreApi] stage=global-init ok\r\n");

        APP = ppp::make_shared_object<PppApplication>();
        DEFAULT_ = APP;
        if (NULLPTR == APP) {
            g_core_in_process_host.store(false, std::memory_order_release);
            CoreApiSignalStartup(handle, false, -1, "failed to allocate core application");
            CoreApiSignalFinished(handle, -1);
            return;
        }

        int prepared_status = APP->PreparedArgumentEnvironment(argc, argv_data);
        fprintf(stdout, "[CoreApi] stage=arguments-prepared status=%d\r\n", prepared_status);
        if (prepared_status != 0) {
            APP->PrintHelpInformation();
            g_core_in_process_host.store(false, std::memory_order_release);
            CoreApiSignalStartup(handle, false, prepared_status,
                "core arguments were rejected");
            APP->Release();
            DEFAULT_.reset();
            CoreApiSignalFinished(handle, prepared_status);
            return;
        }

#if BOOST_ASIO_HAS_IO_URING != 0
        if (!ppp::diagnostics::IfIOUringKernelVersion()) {
            g_core_in_process_host.store(false, std::memory_order_release);
            CoreApiSignalStartup(handle, false, -1,
                "io-uring requires a Linux kernel version of 5.10 or newer");
            APP->Release();
            DEFAULT_.reset();
            CoreApiSignalFinished(handle, -1);
            return;
        }
#endif

        OpenCoreLogFile();
        fprintf(stdout, "[CoreApi] stage=log-open ok\r\n");

        // The direct host receives logs through the same sink used by the
        // legacy RPC path.  The sink is harmless when no callback is set.
        ppp::diagnostics::SetLogSink(RpcLogSink);

#if defined(_MACOS)
        // The standalone entry point applies this before entering the
        // executor.  The in-process host must keep the same protection from
        // EMFILE when the desktop client opens many tunnel sockets.
        ConfigureOpenFileDescriptorLimit();
#endif

        result_code = Executors::Run(
            APP->GetBufferAllocator(),
            [APP, prepared_status, handle](int callback_argc, const char* callback_argv[]) noexcept -> int
            {
                // Executors::Run attaches the default context immediately
                // before invoking this callback. Capture it only here; it is
                // null during the host thread's pre-Run initialization.
                {
                    std::lock_guard<std::mutex> scope(handle->state_mutex);
                    handle->context = Executors::GetDefault();
                }
                if (NULLPTR == handle->context) {
                    fprintf(stdout, "[CoreApi] FAIL stage=executor-context\r\n");
                    CoreApiSignalStartup(handle, false, -1,
                        "core executor context is unavailable");
                    return -1;
                }
                fprintf(stdout, "[CoreApi] stage=run begin\r\n");
                int code = Run(APP, prepared_status, callback_argc, callback_argv);
                fprintf(stdout, "[CoreApi] stage=run end status=%d\r\n", code);
                std::string startup_error;
                if (code != 0) {
                    const ppp::string& failure = APP->GetStartupFailure();
                    startup_error = failure.empty() ?
                        "core startup failed" : failure.data();
                }
                CoreApiSignalStartup(handle, code == 0, code, startup_error);
                return code;
            },
            argc,
            argv_data);

        // Main() may fail after preparing part of the network environment
        // (for example when the periodic timer cannot be created).  The
        // standalone executable historically only released its lock here,
        // but an embedded host must never report startup failure while
        // leaving DNS/routes/TUN ownership behind.
        // Dispose is idempotent and is required even when the executor stops
        // unexpectedly; the shutdown command normally did this earlier, but
        // an event-loop failure must not skip DNS/routes/TUN restoration.
        APP->Dispose();
        APP->Release();
        if (DEFAULT_ == APP) {
            DEFAULT_.reset();
        }
        CloseCoreLogFile();
        g_core_in_process_host.store(false, std::memory_order_release);
    }
    catch (const std::exception& exception) {
        g_core_in_process_host.store(false, std::memory_order_release);
        CoreApiSignalStartup(handle, false, -1, exception.what());
        if (NULLPTR != APP) {
            APP->Dispose();
            APP->Release();
        }
        DEFAULT_.reset();
        CloseCoreLogFile();
        result_code = -1;
    }
    catch (...) {
        g_core_in_process_host.store(false, std::memory_order_release);
        CoreApiSignalStartup(handle, false, -1, "unknown exception in core runtime");
        if (NULLPTR != APP) {
            APP->Dispose();
            APP->Release();
        }
        DEFAULT_.reset();
        CloseCoreLogFile();
        result_code = -1;
    }

    CoreApiSignalFinished(handle, result_code);
}

static bool CoreApiWaitFinished(ppp_core_handle* handle, int timeout_ms) noexcept
{
    if (NULLPTR == handle) {
        return false;
    }

    std::unique_lock<std::mutex> lock(handle->state_mutex);
    if (timeout_ms <= 0) {
        handle->state_cv.wait(lock, [handle]() noexcept { return handle->finished; });
        return true;
    }

    return handle->state_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [handle]() noexcept { return handle->finished; });
}

static bool CoreApiInvoke(
    ppp_core_handle* handle,
    const char* method,
    const char* params_json,
    std::string& result,
    std::string& error) noexcept
{
    try {
        if (NULLPTR == handle || NULLPTR == method || method[0] == '\x0') {
            error = "invalid core command";
            return false;
        }

        std::shared_ptr<boost::asio::io_context> context;
        {
            std::lock_guard<std::mutex> state_scope(handle->state_mutex);
            if (handle->finished) {
                error = "core has already stopped";
                return false;
            }
            context = handle->context;
        }
        if (NULLPTR == context) {
            error = "core executor is unavailable";
            return false;
        }

        Json::Value params = params_json != NULLPTR && params_json[0] != '\x0' ?
            ppp::auxiliary::JsonAuxiliary::FromString(params_json) :
            Json::Value(Json::objectValue);
        if (params.isNull()) {
            error = "invalid command parameters JSON";
            return false;
        }

        std::shared_ptr<ppp_core_handle::CommandRequest> request =
            std::make_shared<ppp_core_handle::CommandRequest>();
        if (NULLPTR == request) {
            error = "failed to allocate command request";
            return false;
        }

        ppp::string method_copy(method);
        boost::asio::post(*context,
            [request, method_copy, params]() noexcept
            {
                Json::Value value;
                ppp::string dispatch_error;
                bool ok = false;

                try {
                    std::shared_ptr<PppApplication> APP = DEFAULT_;
                    if (NULLPTR != APP) {
                        ok = APP->ExecuteRpcCommand(method_copy, params, value, dispatch_error);
                    }
                    else {
                        dispatch_error = "core application is unavailable";
                    }
                }
                catch (...) {
                    dispatch_error = "exception while executing core command";
                    ok = false;
                }

                std::lock_guard<std::mutex> scope(request->mutex);
                request->success = ok;
                if (ok) {
                    request->result = ppp::auxiliary::JsonAuxiliary::ToString(value);
                }
                else {
                    request->error = dispatch_error.empty() ? "core command rejected" : dispatch_error;
                }
                request->completed = true;
                request->completed_cv.notify_one();
            });

        std::unique_lock<std::mutex> request_lock(request->mutex);
        if (!request->completed_cv.wait_for(request_lock, std::chrono::seconds(30),
            [request]() noexcept { return request->completed; })) {
            error = "core command timed out";
            return false;
        }

        result = request->result;
        error = request->error;
        return request->success;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    catch (...) {
        error = "exception while scheduling core command";
        return false;
    }
}

extern "C" ppp_core_handle* ppp_core_start(
    int argc,
    const char* const* argv,
    ppp_core_log_callback log_callback,
    void* user_data,
    char* error_buffer,
    size_t error_buffer_size)
{
    CoreApiSetError(error_buffer, error_buffer_size, std::string());
    if (argc < 0 || (argc > 0 && NULLPTR == argv)) {
        CoreApiSetError(error_buffer, error_buffer_size, "invalid core arguments");
        return NULLPTR;
    }

    ppp_core_handle* handle = NULLPTR;
    bool active_acquired = false;
    try {
        std::shared_ptr<std::vector<std::string>> arguments =
            std::make_shared<std::vector<std::string>>();
        arguments->reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            arguments->emplace_back(argv[i] != NULLPTR ? argv[i] : "");
        }

        bool expected = false;
        if (!g_core_api_instance_active.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
            CoreApiSetError(error_buffer, error_buffer_size,
                "another in-process core is already active");
            return NULLPTR;
        }
        active_acquired = true;

        handle = new ppp_core_handle();
        g_core_log_user_data.store(user_data, std::memory_order_release);
        g_core_log_callback.store(log_callback, std::memory_order_release);
        handle->thread = std::thread(CoreApiRun, handle, arguments);

        std::unique_lock<std::mutex> lock(handle->state_mutex);
        if (!handle->state_cv.wait_for(lock, std::chrono::seconds(20),
            [handle]() noexcept { return handle->startup_completed; })) {
            lock.unlock();
            ppp_core_stop(handle, error_buffer, error_buffer_size);
            ppp_core_destroy(handle);
            CoreApiSetError(error_buffer, error_buffer_size,
                "core did not finish startup within 20 seconds");
            return NULLPTR;
        }

        if (!handle->startup_success) {
            std::string error = handle->error.empty() ? "core startup failed" : handle->error;
            lock.unlock();
            if (handle->thread.joinable()) {
                handle->thread.join();
            }
            g_core_log_callback.store(nullptr, std::memory_order_release);
            g_core_log_user_data.store(nullptr, std::memory_order_release);
            if (active_acquired) {
                g_core_api_instance_active.store(false, std::memory_order_release);
            }
            delete handle;
            CoreApiSetError(error_buffer, error_buffer_size, error);
            return NULLPTR;
        }

        return handle;
    }
    catch (const std::exception& exception) {
        if (NULLPTR != handle) {
            if (handle->thread.joinable()) {
                handle->thread.join();
            }
            delete handle;
        }
        if (active_acquired) {
            g_core_api_instance_active.store(false, std::memory_order_release);
        }
        g_core_log_callback.store(nullptr, std::memory_order_release);
        g_core_log_user_data.store(nullptr, std::memory_order_release);
        CoreApiSetError(error_buffer, error_buffer_size, exception.what());
        return NULLPTR;
    }
    catch (...) {
        if (NULLPTR != handle) {
            if (handle->thread.joinable()) {
                handle->thread.join();
            }
            delete handle;
        }
        if (active_acquired) {
            g_core_api_instance_active.store(false, std::memory_order_release);
        }
        g_core_log_callback.store(nullptr, std::memory_order_release);
        g_core_log_user_data.store(nullptr, std::memory_order_release);
        CoreApiSetError(error_buffer, error_buffer_size, "failed to start core runtime");
        return NULLPTR;
    }
}

extern "C" int ppp_core_command(
    ppp_core_handle* handle,
    const char* method,
    const char* params_json,
    char** result_json,
    char* error_buffer,
    size_t error_buffer_size)
{
    CoreApiSetError(error_buffer, error_buffer_size, std::string());
    if (NULLPTR != result_json) {
        *result_json = NULLPTR;
    }
    if (NULLPTR == handle) {
        CoreApiSetError(error_buffer, error_buffer_size, "invalid core handle");
        return 0;
    }

    try {
        std::lock_guard<std::mutex> command_scope(handle->command_mutex);
        std::string result;
        std::string error;
        if (!CoreApiInvoke(handle, method, params_json, result, error)) {
            CoreApiSetError(error_buffer, error_buffer_size, error);
            return 0;
        }

        if (NULLPTR != result_json) {
            char* copy = static_cast<char*>(std::malloc(result.size() + 1));
            if (NULLPTR == copy) {
                CoreApiSetError(error_buffer, error_buffer_size, "failed to allocate command result");
                return 0;
            }
            std::memcpy(copy, result.data(), result.size());
            copy[result.size()] = '\x0';
            *result_json = copy;
        }
        return 1;
    }
    catch (const std::exception& exception) {
        CoreApiSetError(error_buffer, error_buffer_size, exception.what());
        return 0;
    }
    catch (...) {
        CoreApiSetError(error_buffer, error_buffer_size, "exception while invoking core command");
        return 0;
    }
}

extern "C" int ppp_core_snapshot(
    ppp_core_handle* handle,
    char** snapshot_json,
    char* error_buffer,
    size_t error_buffer_size)
{
    return ppp_core_command(handle, "get_snapshot", "{}", snapshot_json,
        error_buffer, error_buffer_size);
}

extern "C" int ppp_core_set_log_level(
    ppp_core_handle* handle,
    const char* level,
    char* error_buffer,
    size_t error_buffer_size)
{
    std::string params = "{\"level\":\"";
    if (NULLPTR != level) {
        for (const char* p = level; *p != '\x0'; ++p) {
            if (*p == '\\' || *p == '"') params.push_back('\\');
            params.push_back(*p);
        }
    }
    params += "\"}";
    return ppp_core_command(handle, "set_log_level", params.data(), NULLPTR,
        error_buffer, error_buffer_size);
}

extern "C" int ppp_core_is_running(ppp_core_handle* handle)
{
    if (NULLPTR == handle) {
        return 0;
    }

    std::lock_guard<std::mutex> scope(handle->state_mutex);
    return handle->startup_success && !handle->finished ? 1 : 0;
}

extern "C" int ppp_core_stop(
    ppp_core_handle* handle,
    char* error_buffer,
    size_t error_buffer_size)
{
    CoreApiSetError(error_buffer, error_buffer_size, std::string());
    if (NULLPTR == handle) {
        CoreApiSetError(error_buffer, error_buffer_size, "invalid core handle");
        return 0;
    }

    {
        std::lock_guard<std::mutex> scope(handle->state_mutex);
        if (handle->finished) {
            return 1;
        }
    }

    char* ignored = NULLPTR;
    if (!ppp_core_command(handle, "shutdown",
        "{\"confirm\":\"shutdown\",\"restart\":false}",
        &ignored, error_buffer, error_buffer_size)) {
        if (NULLPTR != ignored) std::free(ignored);
        return 0;
    }
    if (NULLPTR == ignored) {
        CoreApiSetError(error_buffer, error_buffer_size, "core returned no shutdown result");
        return 0;
    }
    Json::Value shutdown_result =
        ppp::auxiliary::JsonAuxiliary::FromString(ignored);
    bool accepted = shutdown_result.get("accepted", Json::Value(false)).asBool();
    if (NULLPTR != ignored) std::free(ignored);
    if (!accepted) {
        CoreApiSetError(error_buffer, error_buffer_size, "core did not accept shutdown");
        return 0;
    }

    if (!CoreApiWaitFinished(handle, 30000)) {
        CoreApiSetError(error_buffer, error_buffer_size,
            "core did not finish network cleanup within 30 seconds");
        return 0;
    }
    return 1;
}

extern "C" void ppp_core_destroy(ppp_core_handle* handle)
{
    if (NULLPTR == handle) {
        return;
    }

    char error[256] = {};
    ppp_core_stop(handle, error, sizeof(error));
    if (handle->thread.joinable()) {
        handle->thread.join();
    }
    g_core_log_callback.store(nullptr, std::memory_order_release);
    g_core_log_user_data.store(nullptr, std::memory_order_release);
    delete handle;
}

extern "C" void ppp_core_free_string(char* value)
{
    std::free(value);
}

// Program entry point
#if !defined(PPP_CORE_LIBRARY)
int main(int argc, const char* argv[]) noexcept
{
    ConfigureCoreOutputStreams();
    fprintf(stdout, "[CoreStartup] process begin argc=%d\r\n", argc);
#if defined(_WIN32)
    fprintf(stdout, "[CoreStartup] process pid=%lu\r\n",
        static_cast<unsigned long>(::GetCurrentProcessId()));
#endif

    // Configure real-time mode
    ppp::RT = ppp::ToBoolean(ppp::GetCommandArgument("--rt", argc, argv, "y").data());
    fprintf(stdout, "[CoreStartup] stage=rt-config ok\r\n");

#if defined(_WIN32)
    // Switch console to UTF-8 code page so Unicode box-drawing characters
    // (├ ─ │ └ ┬ ┐ ┘ ┤ etc.) render correctly instead of showing as '?'.
    ::SetConsoleOutputCP(65001);
    fprintf(stdout, "[CoreStartup] stage=console-codepage ok\r\n");
#endif

    // Initialize global state
    fprintf(stdout, "[CoreStartup] stage=global-init begin\r\n");
    ppp::global::cctor();
    fprintf(stdout, "[CoreStartup] stage=global-init ok\r\n");

    // Create application instance
    fprintf(stdout, "[CoreStartup] stage=application-allocate begin\r\n");
    std::shared_ptr<PppApplication> APP = ppp::make_shared_object<PppApplication>();
    DEFAULT_ = APP;
    fprintf(stdout, "[CoreStartup] stage=application-allocate ok=%d\r\n", NULLPTR != APP ? 1 : 0);

    // Prepare environment and run
    fprintf(stdout, "[CoreStartup] stage=arguments-prepare begin\r\n");
    int prepared_status = APP->PreparedArgumentEnvironment(argc, argv);
    fprintf(stdout, "[CoreStartup] stage=arguments-prepare status=%d\r\n", prepared_status);

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

    // When --log-file is specified, redirect core LOG_* output to file in all
    // builds.  The dashboard UI (fprintf to stdout) stays on console.
    OpenCoreLogFile();
    fprintf(stdout, "[CoreStartup] stage=log-open ok\r\n");

#if defined(_MACOS)
    // A full-tunnel client can legitimately own hundreds of TCP sockets even
    // with --tun-mux=0. macOS commonly starts command-line processes at 256,
    // which causes EMFILE and eventually stalls the event loop.
    ConfigureOpenFileDescriptorLimit();
#endif

    fprintf(stdout, "[CoreStartup] stage=executor-run begin\r\n");
    int result_code = Executors::Run(APP->GetBufferAllocator(), 
        [APP, prepared_status](int argc, const char* argv[]) noexcept -> int
        {
            fprintf(stdout, "[CoreStartup] stage=run-callback begin\r\n");
            int result_code = Run(APP, prepared_status, argc, argv);
            fprintf(stdout, "[CoreStartup] stage=run-callback end status=%d\r\n", result_code);
#if defined(_WIN32)
            // The Rust TUI launches the core headlessly with stdout/stderr
            // redirected.  Never invoke the legacy console pause in that
            // mode: if a console is inherited from a launcher/batch file,
            // system("pause") starts cmd.exe and appears as a flashing
            // command window on every failed/restarted core.
            if (result_code != 0 && !APP->IsHeadless())
            {
                ppp::win32::Win32Native::PauseWindowsConsole();
            }
#endif
            return result_code;
        }, argc, argv);
    fprintf(stdout, "[CoreStartup] stage=executor-run end status=%d\r\n", result_code);
    
    // Clean up and optionally restart
    APP->Dispose();
    APP->Release();
    CloseCoreLogFile();

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
#endif
