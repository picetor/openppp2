#if !defined(OPENPPP2_DNS_LIFECYCLE_TEST)
#include <windows/ppp/win32/network/NetworkInterface.h>
#include <windows/ppp/win32/Win32Native.h>
#endif
#include <netioapi.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ppp { namespace win32 { namespace network {
    namespace {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
        // The native regression harness uses an isolated HKCU journal and fake
        // adapter/netsh callbacks; it never changes a real adapter's DNS.
        const HKEY JOURNAL_HIVE = HKEY_CURRENT_USER;
        const std::wstring TEST_JOURNAL_ROOT = L"SOFTWARE\\OpenPPP2\\DnsRecoveryTests\\" +
            std::to_wstring(::GetCurrentProcessId());
        const wchar_t* const JOURNAL_ROOT = TEST_JOURNAL_ROOT.c_str();
#else
        const HKEY JOURNAL_HIVE = HKEY_LOCAL_MACHINE;
        const wchar_t* const JOURNAL_ROOT = L"SOFTWARE\\OpenPPP2\\DnsRecovery";
#endif
        // Use the same recovery records from x86 and x64 cores.
        const REGSAM JOURNAL_VIEW = KEY_WOW64_64KEY;

        struct RegistryKey {
            HKEY handle = NULL;
            ~RegistryKey() { if (handle) ::RegCloseKey(handle); }
        };

        bool DeleteRecord(const std::wstring& path) {
            return ::RegDeleteKeyExW(JOURNAL_HIVE, path.c_str(), JOURNAL_VIEW, 0) == ERROR_SUCCESS;
        }

        // Serialize read/save/apply/restore across CLI and embedded-core processes.
        struct JournalLock {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
            HANDLE handle = ::CreateMutexW(NULL, FALSE, L"Local\\OpenPPP2.DnsRecoveryTests");
#else
            HANDLE handle = ::CreateMutexW(NULL, FALSE, L"Global\\OpenPPP2.DnsRecovery");
#endif
            bool acquired = false;
            JournalLock() {
                if (handle) {
                    DWORD result = ::WaitForSingleObject(handle, 30000);
                    acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
                }
            }
            ~JournalLock() {
                if (acquired) ::ReleaseMutex(handle);
                if (handle) ::CloseHandle(handle);
            }
        };

        bool AdapterGuid(int index, std::wstring& text) {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
            return TestAdapterGuid(index, text);
#else
            NET_LUID luid = {};
            GUID guid = {};
            wchar_t buffer[40] = {};
            if (index < 0 || ::ConvertInterfaceIndexToLuid(index, &luid) != NO_ERROR ||
                ::ConvertInterfaceLuidToGuid(&luid, &guid) != NO_ERROR ||
                ::StringFromGUID2(guid, buffer, 40) == 0) return false;
            text = buffer;
            return true;
#endif
        }

        bool AdapterIndex(const std::wstring& text, int& index) {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
            return TestAdapterIndex(text, index);
#else
            GUID guid = {};
            NET_LUID luid = {};
            NET_IFINDEX value = 0;
            if (FAILED(::CLSIDFromString(text.c_str(), &guid)) ||
                ::ConvertInterfaceGuidToLuid(&guid, &luid) != NO_ERROR ||
                ::ConvertInterfaceLuidToIndex(&luid, &value) != NO_ERROR) return false;
            index = static_cast<int>(value);
            return true;
#endif
        }

        bool ReadString(HKEY key, const wchar_t* name, std::wstring& value, bool optional = false) {
            DWORD type = 0, size = 0;
            LSTATUS status = ::RegQueryValueExW(key, name, NULL, &type, NULL, &size);
            if (status == ERROR_FILE_NOT_FOUND && optional) { value.clear(); return true; }
            if (status != ERROR_SUCCESS || type != REG_SZ || size > 65536 ||
                size % sizeof(wchar_t) != 0) return false;
            std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
            status = ::RegQueryValueExW(key, name, NULL, &type,
                reinterpret_cast<BYTE*>(&buffer[0]), &size);
            if (status != ERROR_SUCCESS || type != REG_SZ) return false;
            value.assign(buffer.c_str());
            return true;
        }

        bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
            return ::RegSetValueExW(key, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }

        template<class T> bool ReadNumber(HKEY key, const wchar_t* name, DWORD type, T& value) {
            DWORD actual_type = 0, size = sizeof(value);
            return ::RegQueryValueExW(key, name, NULL, &actual_type,
                reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                actual_type == type && size == sizeof(value);
        }

        template<class T> bool WriteNumber(HKEY key, const wchar_t* name, DWORD type, T value) {
            return ::RegSetValueExW(key, name, 0, type,
                reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
        }

        bool ProcessStamp(HANDLE process, ULONGLONG& stamp) {
            FILETIME created = {}, exited = {}, kernel = {}, user = {};
            if (!::GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
            stamp = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            return true;
        }

        bool OwnerAlive(DWORD pid, ULONGLONG stamp) {
            HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return ::GetLastError() != ERROR_INVALID_PARAMETER;
            ULONGLONG actual = 0;
            bool alive = !ProcessStamp(process, actual) ||
                (actual == stamp && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT);
            ::CloseHandle(process);
            return alive;
        }

        std::wstring JournalPath(const std::wstring& guid, int family) {
            return std::wstring(JOURNAL_ROOT) + L"\\" + guid +
                (family == AF_INET ? L"-ipv4" : L"-ipv6");
        }

        bool ReadOverride(const std::wstring& guid, int family, std::wstring& servers) {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
            return TestReadOverride(guid, family, servers);
#else
            std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\";
            path += family == AF_INET ? L"Tcpip" : L"Tcpip6";
            path += L"\\Parameters\\Interfaces\\" + guid;
            RegistryKey key;
            if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE,
                    &key.handle) != ERROR_SUCCESS) return false;
            // NameServer is the manual override; DhcpNameServer is deliberately
            // excluded. IP DHCPEnabled says nothing about DNS automatic/static.
            return ReadString(key.handle, L"NameServer", servers, true);
#endif
        }

        bool ParseServers(const std::wstring& raw, int family, ppp::vector<ppp::string>& servers) {
            servers.clear();
            std::wstring token;
            auto append = [&]() {
                if (token.empty()) return true;
                ppp::string text;
                for (wchar_t c : token) {
                    if (c > 127) return false;
                    text += static_cast<char>(c);
                }
                // Numeric addresses only: no hostname resolution or untrusted
                // text is ever passed to the netsh command line.
                std::size_t scope = text.find('%');
                ppp::string suffix;
                if (scope != ppp::string::npos) {
                    if (family != AF_INET6 || scope + 1 == text.size() || text.size() - scope > 11) return false;
                    suffix = text.substr(scope);
                    for (std::size_t i = 1; i < suffix.size(); ++i)
                        if (suffix[i] < '0' || suffix[i] > '9') return false;
                    text.resize(scope);
                }
                IN6_ADDR bytes = {};
                char normalized[INET6_ADDRSTRLEN] = {};
                if (::inet_pton(family, text.c_str(), &bytes) != 1 ||
                    !::inet_ntop(family, &bytes, normalized, sizeof(normalized))) return false;
                servers.emplace_back(ppp::string(normalized) + suffix);
                token.clear();
                return true;
            };
            for (wchar_t c : raw) {
                if (c == L',' || c == L';' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                    if (!append()) return false;
                }
                else token += c;
            }
            return append();
        }

        bool ExecuteDnsCommand(const ppp::string& command) {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
            return TestExecuteDnsCommand(command);
#else
            int result = INFINITE;
            return ppp::win32::Win32Native::Execute(false, "netsh.exe", command.data(),
                &result, 10000) && result == ERROR_SUCCESS;
#endif
        }

        bool ApplyConfiguration(int index, int family, const ppp::vector<ppp::string>& servers) {
            const char* protocol = family == AF_INET ? "ipv4" : "ipv6";
            char command[1024] = {};
            if (servers.empty()) {
                ::snprintf(command, sizeof(command),
                    "interface %s set dnsservers name=%d source=dhcp", protocol, index);
                return ExecuteDnsCommand(command);
            }
            for (std::size_t i = 0; i < servers.size(); ++i) {
                if (i == 0) ::snprintf(command, sizeof(command),
                    "interface %s set dnsservers name=%d source=static address=%s validate=no",
                    protocol, index, servers[i].c_str());
                else ::snprintf(command, sizeof(command),
                    "interface %s add dnsservers name=%d address=%s index=%u validate=no",
                    protocol, index, servers[i].c_str(), static_cast<unsigned>(i + 1));
                if (!ExecuteDnsCommand(command)) return false;
            }
            return true;
        }

        bool ApplyLoopback(int index, int family, const std::wstring& guid) {
            const ppp::vector<ppp::string> servers{ family == AF_INET ? "127.0.0.1" : "::1" };
            if (!ApplyConfiguration(index, family, servers)) return false;
            std::wstring current;
            ppp::vector<ppp::string> actual;
            return ReadOverride(guid, family, current) && ParseServers(current, family, actual) && actual == servers;
        }

        bool IsLoopbackOverride(const std::wstring& raw, int family) {
            ppp::vector<ppp::string> servers;
            return ParseServers(raw, family, servers) && servers.size() == 1 &&
                servers.front() == (family == AF_INET ? "127.0.0.1" : "::1");
        }

        bool ConfigurationMatches(const std::wstring& guid, int family,
            const ppp::vector<ppp::string>& expected) {
            std::wstring raw;
            ppp::vector<ppp::string> actual;
            return ReadOverride(guid, family, raw) && ParseServers(raw, family, actual) && actual == expected;
        }

        bool DnsPortUnused(int family) {
#if defined(OPENPPP2_DNS_LIFECYCLE_TEST)
            return TestDnsPortUnused(family);
#else
            // Do not recover a dead owner's record while another local resolver
            // has taken over port 53. Check both UDP and TCP without sending data.
            for (int type : { SOCK_DGRAM, SOCK_STREAM }) {
                SOCKET socket = ::socket(family, type, 0);
                if (socket == INVALID_SOCKET) return false;
                BOOL exclusive = TRUE;
                bool unused = ::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0;
                sockaddr_storage endpoint = {};
                int size = 0;
                if (family == AF_INET) {
                    auto* v4 = reinterpret_cast<sockaddr_in*>(&endpoint);
                    v4->sin_family = AF_INET; v4->sin_port = htons(53);
                    v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); size = sizeof(*v4);
                }
                else {
                    auto* v6 = reinterpret_cast<sockaddr_in6*>(&endpoint);
                    v6->sin6_family = AF_INET6; v6->sin6_port = htons(53);
                    v6->sin6_addr = in6addr_loopback; size = sizeof(*v6);
                }
                unused = unused && ::bind(socket, reinterpret_cast<sockaddr*>(&endpoint), size) == 0;
                ::closesocket(socket);
                if (!unused) return false;
            }
            return true;
#endif
        }

        bool RestoreRecord(const std::wstring& path, const std::wstring& guid, int family, bool recovery) {
            RegistryKey key;
            LSTATUS opened = ::RegOpenKeyExW(JOURNAL_HIVE, path.c_str(), 0,
                KEY_READ | KEY_SET_VALUE | JOURNAL_VIEW, &key.handle);
            if (opened == ERROR_FILE_NOT_FOUND) return true;
            if (opened != ERROR_SUCCESS) return false;
            DWORD ready = 0, pid = 0, automatic = 0, restoring = 0;
            ULONGLONG stamp = 0, our_stamp = 0;
            std::wstring original;
            if (!ReadNumber(key.handle, L"Ready", REG_DWORD, ready) || ready != 1) {
                // No override is applied until the complete journal has been
                // flushed. An interrupted snapshot is safe to discard.
                return DeleteRecord(path);
            }
            if (!ReadNumber(key.handle, L"OwnerPid", REG_DWORD, pid) ||
                !ReadNumber(key.handle, L"OwnerCreated", REG_QWORD, stamp) ||
                !ReadNumber(key.handle, L"OriginalAutomatic", REG_DWORD, automatic) || automatic > 1 ||
                !ReadString(key.handle, L"OriginalNameServer", original)) return false;
            ReadNumber(key.handle, L"Restoring", REG_DWORD, restoring);
            if (recovery) {
                if (OwnerAlive(pid, stamp)) return true;
            }
            else if (pid != ::GetCurrentProcessId() ||
                !ProcessStamp(::GetCurrentProcess(), our_stamp) || stamp != our_stamp) return false;

            int index = -1;
            if (!AdapterIndex(guid, index)) {
                // Keep the record for a removed/reinstalled adapter, keyed by GUID.
                LOG_WARN("Windows DNS recovery: adapter unavailable; preserving journal");
                return true;
            }
            std::wstring current;
            if (!ReadOverride(guid, family, current)) return false;
            ppp::vector<ppp::string> servers, current_servers;
            if (!ParseServers(original, family, servers) || (servers.empty() != (automatic == 1))) return false;
            if (!ParseServers(current, family, current_servers)) return false;
            // If a static list restore failed after its first command, allow a
            // retry of that exact prefix. Otherwise respect a user's newer DNS.
            bool partial_restore = restoring == 1 && !current_servers.empty() &&
                current_servers.size() < servers.size() &&
                std::equal(current_servers.begin(), current_servers.end(), servers.begin());
            if (IsLoopbackOverride(current, family) || partial_restore) {
                if (recovery && IsLoopbackOverride(current, family) && !DnsPortUnused(family)) {
                    LOG_WARN("Windows DNS recovery: local resolver active; preserving journal, ifIndex=%d family=%d", index, family);
                    return false;
                }
                if (!WriteNumber(key.handle, L"Restoring", REG_DWORD, static_cast<DWORD>(1)) ||
                    ::RegFlushKey(key.handle) != ERROR_SUCCESS ||
                    !ApplyConfiguration(index, family, servers) || !ConfigurationMatches(guid, family, servers)) {
                    LOG_ERROR("Windows DNS restore failed; preserving journal, ifIndex=%d family=%d", index, family);
                    return false;
                }
                LOG_INFO("Windows DNS restored: ifIndex=%d family=%d mode=%s recovery=%d",
                    index, family, servers.empty() ? "automatic" : "static", recovery ? 1 : 0);
            }
            else {
                // Crash before apply, or a user/another app changed DNS since then.
                // Do not overwrite that newer setting with our old snapshot.
                LOG_INFO("Windows DNS restore: override no longer owned, ifIndex=%d family=%d", index, family);
            }
            return DeleteRecord(path);
        }
    }

    bool PinDnsToLoopback(int interface_index, int family) noexcept {
        if (family != AF_INET && family != AF_INET6) return false;
        JournalLock lock;
        std::wstring guid, original;
        ULONGLONG stamp = 0;
        if (!lock.acquired || !AdapterGuid(interface_index, guid) ||
            !ReadOverride(guid, family, original) || !ProcessStamp(::GetCurrentProcess(), stamp)) return false;
        ppp::vector<ppp::string> original_servers;
        if (!ParseServers(original, family, original_servers)) return false;
        RegistryKey key;
        DWORD disposition = 0;
        std::wstring path = JournalPath(guid, family);
        if (::RegCreateKeyExW(JOURNAL_HIVE, path.c_str(), 0, NULL, 0,
                KEY_READ | KEY_WRITE | JOURNAL_VIEW, NULL, &key.handle, &disposition) != ERROR_SUCCESS) return false;
        // A reconnect after a failed restore may reuse our original journal, but
        // must never snapshot 127.0.0.1 as the original DNS or claim another owner.
        if (disposition != REG_CREATED_NEW_KEY) {
            DWORD pid = 0, ready = 0;
            ULONGLONG owner_stamp = 0;
            if (!ReadNumber(key.handle, L"Ready", REG_DWORD, ready) || ready != 1 ||
                !ReadNumber(key.handle, L"OwnerPid", REG_DWORD, pid) || pid != ::GetCurrentProcessId() ||
                !ReadNumber(key.handle, L"OwnerCreated", REG_QWORD, owner_stamp) || owner_stamp != stamp) return false;
            return ApplyLoopback(interface_index, family, guid);
        }
        bool saved = WriteNumber(key.handle, L"OwnerPid", REG_DWORD, ::GetCurrentProcessId()) &&
            WriteNumber(key.handle, L"OwnerCreated", REG_QWORD, stamp) &&
            WriteString(key.handle, L"OriginalNameServer", original) &&
            WriteNumber(key.handle, L"OriginalAutomatic", REG_DWORD, static_cast<DWORD>(original_servers.empty() ? 1 : 0)) &&
            WriteNumber(key.handle, L"Ready", REG_DWORD, static_cast<DWORD>(1)) &&
            ::RegFlushKey(key.handle) == ERROR_SUCCESS;
        if (!saved) { DeleteRecord(path); return false; }
        LOG_INFO("Windows DNS snapshot: ifIndex=%d family=%d mode=%s", interface_index, family,
            original_servers.empty() ? "automatic" : "static");
        // Keep the record on apply failure: netsh may have partially succeeded.
        return ApplyLoopback(interface_index, family, guid);
    }

    bool RefreshDnsLoopback(int interface_index, int family) noexcept {
        if (family != AF_INET && family != AF_INET6) return false;
        JournalLock lock;
        std::wstring guid;
        if (!lock.acquired || !AdapterGuid(interface_index, guid)) return false;
        RegistryKey key;
        std::wstring path = JournalPath(guid, family);
        DWORD pid = 0, ready = 0;
        ULONGLONG stamp = 0, our_stamp = 0;
        if (::RegOpenKeyExW(JOURNAL_HIVE, path.c_str(), 0, KEY_READ | JOURNAL_VIEW, &key.handle) != ERROR_SUCCESS ||
            !ReadNumber(key.handle, L"Ready", REG_DWORD, ready) || ready != 1 ||
            !ReadNumber(key.handle, L"OwnerPid", REG_DWORD, pid) || pid != ::GetCurrentProcessId() ||
            !ReadNumber(key.handle, L"OwnerCreated", REG_QWORD, stamp) ||
            !ProcessStamp(::GetCurrentProcess(), our_stamp) || stamp != our_stamp) return false;
        return ApplyLoopback(interface_index, family, guid);
    }

    bool RestoreDnsTakeover(int interface_index, int family) noexcept {
        if (family != AF_INET && family != AF_INET6) return false;
        JournalLock lock;
        std::wstring guid;
        return lock.acquired && AdapterGuid(interface_index, guid) &&
            RestoreRecord(JournalPath(guid, family), guid, family, false);
    }

    bool RecoverDnsTakeovers() noexcept {
        JournalLock lock;
        if (!lock.acquired) return false;
        RegistryKey root;
        LSTATUS opened = ::RegOpenKeyExW(JOURNAL_HIVE, JOURNAL_ROOT, 0, KEY_READ | JOURNAL_VIEW, &root.handle);
        if (opened == ERROR_FILE_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS) return false;
        std::vector<std::wstring> names;
        for (DWORD i = 0;; ++i) {
            wchar_t name[128] = {};
            DWORD length = 128;
            LSTATUS status = ::RegEnumKeyExW(root.handle, i, name, &length, NULL, NULL, NULL, NULL);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) return false;
            names.emplace_back(name, length);
        }
        bool restored = true;
        for (const auto& name : names) {
            // GUID text is exactly 38 characters; reject unknown/corrupt entries.
            if (name.size() != 43 || (name.substr(38) != L"-ipv4" && name.substr(38) != L"-ipv6")) {
                restored = false; continue;
            }
            std::wstring guid = name.substr(0, 38);
            int family = name.substr(38) == L"-ipv4" ? AF_INET : AF_INET6;
            if (!RestoreRecord(JournalPath(guid, family), guid, family, true)) restored = false;
        }
        return restored;
    }
}}}
