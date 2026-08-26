#include <windows/ppp/net/proxies/HttpProxy.h>
#include <windows/ppp/win32/Win32Native.h>
#include <windows/ppp/win32/Win32RegistryKey.h>

#include <wininet.h>
#include <tchar.h>
#include <comdef.h>
#include <comutil.h>

#include <Windows.h>
#include <Shellapi.h>
#include <shlobj_core.h>

#pragma comment(lib, "wininet.lib")

using ppp::win32::Win32Native;

namespace ppp
{
    namespace net
    {
        namespace proxies
        {
            static constexpr const wchar_t* EXPERIMENTALQUICPROTOCOL_POLICIES_CHROME = L"Software\\Policies\\Google\\Chrome";
            static constexpr const wchar_t* EXPERIMENTALQUICPROTOCOL_POLICIES_EDGE = L"Software\\Policies\\Microsoft\\Edge";
            static constexpr const wchar_t* INTERNET_SETTINGS_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

            struct SystemProxyState final {
                bool captured = false;
                bool proxy_enable = false;
                std::wstring proxy_server;
                std::wstring proxy_override;
                std::wstring auto_config_url;
            };

            static SystemProxyState g_system_proxy_state;

            static void CaptureSystemProxyState() noexcept {
                if (g_system_proxy_state.captured) {
                    return;
                }
                bool ok = false;
                g_system_proxy_state.proxy_enable =
                    ppp::win32::GetRegistryValueDword(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"ProxyEnable", &ok) != 0;
                g_system_proxy_state.proxy_server =
                    ppp::win32::GetRegistryValueString(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"ProxyServer", &ok);
                g_system_proxy_state.proxy_override =
                    ppp::win32::GetRegistryValueString(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"ProxyOverride", &ok);
                g_system_proxy_state.auto_config_url =
                    ppp::win32::GetRegistryValueString(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"AutoConfigURL", &ok);
                g_system_proxy_state.captured = true;
            }

            static bool RestoreSystemProxyState() noexcept {
                if (!g_system_proxy_state.captured) {
                    return true;
                }
                bool ok =
                    ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"ProxyServer", g_system_proxy_state.proxy_server) &&
                    ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"ProxyOverride", g_system_proxy_state.proxy_override) &&
                    ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"AutoConfigURL", g_system_proxy_state.auto_config_url) &&
                    ppp::win32::SetRegistryValueDword(HKEY_CURRENT_USER, INTERNET_SETTINGS_PATH,
                        L"ProxyEnable", g_system_proxy_state.proxy_enable ? 1 : 0);
                HttpProxy::RefreshSystemProxy();
                g_system_proxy_state = SystemProxyState();
                return ok;
            }

            static bool STATIC_IsSupportExperimentalQuicProtocol(LPCWSTR path) noexcept
            {
                bool bOK = false;
                DWORD dwQuicAllowed = ppp::win32::GetRegistryValueDword(HKEY_CURRENT_USER, path, L"QuicAllowed", &bOK);
                if (!bOK)
                {
                    return true;
                }
                else
                {
                    return dwQuicAllowed != 0;
                }
            }

            static bool STATIC_SetSupportExperimentalQuicProtocol(LPCWSTR path, bool value) noexcept
            {
                bool bOK = ppp::win32::SetRegistryValueDword(HKEY_CURRENT_USER, path, L"QuicAllowed", value ? 1 : 0);
                return bOK;
            }

            bool HttpProxy::RefreshSystemProxy() noexcept
            {
                INTERNET_PROXY_INFO ipi;
                RtlZeroMemory(&ipi, sizeof(ipi));

                bool b =
                    InternetSetOption(NULLPTR, INTERNET_OPTION_PROXY_SETTINGS_CHANGED, NULLPTR, 0) &&
                    InternetSetOption(NULLPTR, INTERNET_OPTION_SETTINGS_CHANGED, &ipi, sizeof(INTERNET_PROXY_INFO)) &&
                    InternetSetOption(NULLPTR, INTERNET_OPTION_REFRESH, NULLPTR, 0);
                return b;
            }

            bool HttpProxy::SetSystemProxy(const ppp::string& server, const ppp::string& bypass) noexcept
            {
                INTERNET_PROXY_INFO ipi;
                RtlZeroMemory(&ipi, sizeof(ipi));

                _bstr_t bypass_bstr(bypass.data());
                _bstr_t server_bstr(server.data());

                ipi.dwAccessType = INTERNET_OPEN_TYPE_PROXY;
                ipi.lpszProxy = server_bstr;
                ipi.lpszProxyBypass = bypass_bstr;

                bool b = InternetSetOption(NULLPTR, INTERNET_OPTION_PROXY, &ipi, sizeof(INTERNET_PROXY_INFO));
                return b;
            }

            bool HttpProxy::SetSystemProxy(const ppp::string& server) noexcept
            {
                // Do not bypass private networks here. The local proxy must
                // see them so --bypass-mode=geo/ip/no controls the split.
                return SetSystemProxy(server, "localhost;127.*;[::1]");
            }

            bool HttpProxy::SetSystemProxy(const ppp::string& server, const ppp::string& pac, bool enable) noexcept
            {
                _bstr_t server_bstr(server.data());
                _bstr_t pac_bstr(pac.data());

                std::wstring server_wcs = server_bstr.GetBSTR();
                std::wstring pac_wcs = pac_bstr.GetBSTR();
                return SetSystemProxy(server_wcs, pac_wcs, enable);
            }

            bool HttpProxy::SetSystemProxy(const std::wstring& server, const std::wstring& pac, bool enable) noexcept
            {
                if (!enable) {
                    return RestoreSystemProxyState();
                }

                CaptureSystemProxyState();
                LPCWSTR PATH = INTERNET_SETTINGS_PATH;
                ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, PATH, L"ProxyServer", server);
                ppp::win32::SetRegistryValueDword(HKEY_CURRENT_USER, PATH, L"ProxyEnable", 1);
                ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, PATH, L"AutoConfigURL", pac);

                RefreshSystemProxy();
                ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, PATH, L"ProxyOverride", L"localhost;127.*;[::1]");
                RefreshSystemProxy();

                // Compare literally.  Treating an IPv6 proxy such as
                // [::1]:8080 as a regular expression can throw or produce a
                // false mismatch because of the brackets and colons.
                if (ppp::win32::GetRegistryValueString(HKEY_CURRENT_USER, PATH, L"ProxyServer") != server)
                {
                    ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, PATH, L"ProxyServer", server);
                    ppp::win32::SetRegistryValueDword(HKEY_CURRENT_USER, PATH, L"ProxyEnable", 1);
                    ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, PATH, L"AutoConfigURL", pac);

                    RefreshSystemProxy();
                    ppp::win32::SetRegistryValueString(HKEY_CURRENT_USER, PATH, L"ProxyOverride", L"localhost;127.*;[::1]");
                    RefreshSystemProxy();
                }
                return true;
            }

            bool HttpProxy::OpenProxySettingsWindow() noexcept
            {
                ULONG dwMajor;
                ULONG dwMinor;
                ULONG dwBuildNumber;
                if (!Win32Native::RtlGetNtVersionNumbers(&dwMajor, &dwMinor, &dwBuildNumber))
                {
                    return false;
                }

                if (dwMajor >= 10) // How-to: Quickly open control panel applets with ms-settings
                {                  // https://ss64.com/nt/syntax-settings.html
                    if (ShellExecuteA(NULLPTR, "open", "ms-settings:network-proxy", NULLPTR, NULLPTR, SW_SHOWNORMAL) != 0)
                    {
                        return true;
                    }
                }
                return OpenControlWindow(4);
            }

            bool HttpProxy::OpenControlWindow() noexcept
            {
                // control.exe inetcpl.cpl
                return ShellExecute(NULLPTR, TEXT("open"), TEXT("rundll32"), TEXT("shell32.dll,Control_RunDLL inetcpl.cpl"), NULLPTR, SW_SHOWNORMAL);
            }

            bool HttpProxy::OpenControlWindow(int TabIndex) noexcept
            {
                if (TabIndex < 0)
                {
                    TabIndex = 0;
                }

                ppp::string cmd = "shell32,Control_RunDLL inetcpl.cpl";
                if (TabIndex > 0)
                {
                    cmd += ",," + stl::to_string<ppp::string>(TabIndex);
                }

                // control.exe inetcpl.cpl
                return ShellExecuteA(NULLPTR, "open", "rundll32", cmd.data(), NULLPTR, SW_SHOWNORMAL);
            }

            bool HttpProxy::IsSupportExperimentalQuicProtocol() noexcept
            {
                bool b = STATIC_IsSupportExperimentalQuicProtocol(EXPERIMENTALQUICPROTOCOL_POLICIES_EDGE) ||
                    STATIC_IsSupportExperimentalQuicProtocol(EXPERIMENTALQUICPROTOCOL_POLICIES_CHROME);
                return b;
            }

            bool HttpProxy::SetSupportExperimentalQuicProtocol(bool value) noexcept
            {
                bool b = STATIC_SetSupportExperimentalQuicProtocol(EXPERIMENTALQUICPROTOCOL_POLICIES_EDGE, value);
                b |= STATIC_SetSupportExperimentalQuicProtocol(EXPERIMENTALQUICPROTOCOL_POLICIES_CHROME, value);
                return b;
            }

            bool HttpProxy::PreferredNetwork(bool in4or6) noexcept
            {
                DWORD dwFlags = in4or6 ? 0x20 : 0x00;
                return ppp::win32::SetRegistryValueDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters", L"DisabledComponents", dwFlags);
            }
        }
    }
}
