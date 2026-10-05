// Native regression tests of the real DNS lifecycle implementation. Only the
// adapter/command boundary is faked; journal IO uses a process-specific HKCU key.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define OPENPPP2_DNS_LIFECYCLE_TEST
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)

namespace ppp {
    using string = std::string;
    template<class T> using vector = std::vector<T>;
    namespace win32 { namespace network {
        struct FakeAdapter {
            std::wstring guid = L"{701B1FD5-BA8C-4910-BC41-01AD591E5BDD}";
            int index = 7;
            bool present = true;
            bool port_unused = true;
            int command_count = 0;
            int fail_command = -1;
            bool apply_then_fail = false;
            std::map<int, std::wstring> overrides;
        } adapter;

        bool TestAdapterGuid(int index, std::wstring& guid) {
            if (!adapter.present || index != adapter.index) return false;
            guid = adapter.guid; return true;
        }
        bool TestAdapterIndex(const std::wstring& guid, int& index) {
            if (!adapter.present || guid != adapter.guid) return false;
            index = adapter.index; return true;
        }
        bool TestReadOverride(const std::wstring& guid, int family, std::wstring& raw) {
            if (!adapter.present || guid != adapter.guid) return false;
            raw = adapter.overrides[family]; return true;
        }
        bool TestDnsPortUnused(int) { return adapter.port_unused; }
        bool TestExecuteDnsCommand(const std::string& command) {
            ++adapter.command_count;
            bool fail = adapter.command_count == adapter.fail_command;
            if (fail && !adapter.apply_then_fail) return false;
            int family = command.find("interface ipv4 ") == 0 ? AF_INET : AF_INET6;
            if (command.find("source=dhcp") != std::string::npos) adapter.overrides[family].clear();
            else {
                auto start = command.find("address=");
                if (start == std::string::npos) throw std::runtime_error("missing DNS address");
                start += 8;
                auto end = command.find(' ', start);
                auto text = command.substr(start, end - start);
                std::wstring raw(text.begin(), text.end());
                if (command.find(" add dnsservers ") != std::string::npos)
                    adapter.overrides[family] += L"," + raw;
                else adapter.overrides[family] = raw;
            }
            return !fail;
        }
    }}
}

#include "../windows/ppp/win32/network/DnsLifecycle.cpp"

using namespace ppp::win32::network;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

static void Reset() {
    {
        RegistryKey key;
        if (::RegOpenKeyExW(JOURNAL_HIVE, JOURNAL_ROOT, 0,
                KEY_READ | KEY_WRITE | DELETE | JOURNAL_VIEW, &key.handle) == ERROR_SUCCESS)
            ::RegDeleteTreeW(key.handle, NULL);
    }
    DeleteRecord(JOURNAL_ROOT);
    adapter = FakeAdapter{};
}

static bool HasJournal(int family) {
    RegistryKey key;
    return ::RegOpenKeyExW(JOURNAL_HIVE, JournalPath(adapter.guid, family).c_str(), 0,
        KEY_READ | JOURNAL_VIEW, &key.handle) == ERROR_SUCCESS;
}

static void MarkDeadOwner(int family, bool reused_pid = false) {
    RegistryKey key;
    CHECK(::RegOpenKeyExW(JOURNAL_HIVE, JournalPath(adapter.guid, family).c_str(), 0,
        KEY_WRITE | JOURNAL_VIEW, &key.handle) == ERROR_SUCCESS);
    CHECK(WriteNumber(key.handle, L"OwnerPid", REG_DWORD,
        reused_pid ? ::GetCurrentProcessId() : static_cast<DWORD>(0)));
    CHECK(WriteNumber(key.handle, L"OwnerCreated", REG_QWORD, static_cast<ULONGLONG>(0)));
}

static void TestAutomaticAndStatic() {
    for (int family : { AF_INET, AF_INET6 }) {
        for (bool automatic : { true, false }) {
            Reset();
            std::wstring original = automatic ? L"" : family == AF_INET ?
                L"1.1.1.1,8.8.8.8" : L"2001:4860:4860::8888,fe80::1%7";
            adapter.overrides[family] = original;
            CHECK(PinDnsToLoopback(adapter.index, family));
            CHECK(IsLoopbackOverride(adapter.overrides[family], family));
            RegistryKey key;
            CHECK(::RegOpenKeyExW(JOURNAL_HIVE, JournalPath(adapter.guid, family).c_str(), 0,
                KEY_READ | JOURNAL_VIEW, &key.handle) == ERROR_SUCCESS);
            DWORD mode = 0;
            CHECK(ReadNumber(key.handle, L"OriginalAutomatic", REG_DWORD, mode));
            CHECK(mode == (automatic ? 1u : 0u));
            CHECK(RefreshDnsLoopback(adapter.index, family));
            CHECK(RestoreDnsTakeover(adapter.index, family));
            CHECK(adapter.overrides[family] == original);
            CHECK(!HasJournal(family));
            CHECK(!RefreshDnsLoopback(adapter.index, family));
            CHECK(adapter.overrides[family] == original);
        }
    }
    Reset();
    adapter.overrides[AF_INET6] = L"2001:4860:4860::8888";
    CHECK(PinDnsToLoopback(7, AF_INET));
    CHECK(PinDnsToLoopback(7, AF_INET6));
    CHECK(RestoreDnsTakeover(7, AF_INET));
    CHECK(RestoreDnsTakeover(7, AF_INET6));
    CHECK(adapter.overrides[AF_INET].empty());
    CHECK(adapter.overrides[AF_INET6] == L"2001:4860:4860::8888");
}

static void TestRecoveryOwnership() {
    for (int family : { AF_INET, AF_INET6 }) {
        Reset();
        CHECK(PinDnsToLoopback(7, family));
        CHECK(RecoverDnsTakeovers()); // Live owner must not be disrupted.
        CHECK(IsLoopbackOverride(adapter.overrides[family], family));
        MarkDeadOwner(family);
        adapter.port_unused = false;
        CHECK(!RecoverDnsTakeovers()); // A different local DNS service is active.
        CHECK(HasJournal(family));
        adapter.port_unused = true;
        adapter.index = 41; // Adapter GUID is stable across index changes.
        CHECK(RecoverDnsTakeovers());
        CHECK(adapter.overrides[family].empty());
        CHECK(!HasJournal(family));

        Reset();
        CHECK(PinDnsToLoopback(7, family));
        MarkDeadOwner(family, true); // PID reused, but process creation time differs.
        CHECK(!RefreshDnsLoopback(7, family));
        CHECK(!PinDnsToLoopback(7, family));
        CHECK(RecoverDnsTakeovers());
        CHECK(adapter.overrides[family].empty());

        Reset();
        CHECK(PinDnsToLoopback(7, family));
        MarkDeadOwner(family);
        adapter.present = false;
        CHECK(RecoverDnsTakeovers());
        CHECK(HasJournal(family));
        adapter.present = true;
        CHECK(RecoverDnsTakeovers());
        CHECK(!HasJournal(family));
    }
}

static void TestPartialFailuresAndUserChanges() {
    Reset();
    adapter.overrides[AF_INET] = L"1.1.1.1,8.8.8.8";
    CHECK(PinDnsToLoopback(7, AF_INET));
    adapter.fail_command = adapter.command_count + 2;
    CHECK(!RestoreDnsTakeover(7, AF_INET)); // Second static DNS command fails.
    CHECK(adapter.overrides[AF_INET] == L"1.1.1.1");
    CHECK(HasJournal(AF_INET));
    MarkDeadOwner(AF_INET); // Recovery must finish that partial static restore.
    CHECK(RecoverDnsTakeovers());
    CHECK(adapter.overrides[AF_INET] == L"1.1.1.1,8.8.8.8");
    CHECK(!HasJournal(AF_INET));

    Reset();
    adapter.fail_command = 1;
    adapter.apply_then_fail = true;
    CHECK(!PinDnsToLoopback(7, AF_INET)); // Simulate netsh failing after a change.
    CHECK(HasJournal(AF_INET));
    CHECK(RestoreDnsTakeover(7, AF_INET));
    CHECK(adapter.overrides[AF_INET].empty());

    Reset();
    adapter.overrides[AF_INET] = L"1.1.1.1,8.8.8.8";
    CHECK(PinDnsToLoopback(7, AF_INET));
    CHECK(PinDnsToLoopback(7, AF_INET)); // Reconnect must keep the first snapshot.
    CHECK(RestoreDnsTakeover(7, AF_INET));
    CHECK(adapter.overrides[AF_INET] == L"1.1.1.1,8.8.8.8");

    Reset();
    CHECK(PinDnsToLoopback(7, AF_INET));
    adapter.overrides[AF_INET] = L"9.9.9.9"; // User changed DNS while VPN was up.
    MarkDeadOwner(AF_INET);
    CHECK(RecoverDnsTakeovers());
    CHECK(adapter.overrides[AF_INET] == L"9.9.9.9");
    CHECK(!HasJournal(AF_INET));

    Reset();
    RegistryKey key;
    CHECK(::RegCreateKeyExW(JOURNAL_HIVE, JournalPath(adapter.guid, AF_INET).c_str(), 0,
        NULL, 0, KEY_WRITE | JOURNAL_VIEW, NULL, &key.handle, NULL) == ERROR_SUCCESS);
    CHECK(RecoverDnsTakeovers()); // Crash during snapshot, before Ready was written.
    CHECK(!HasJournal(AF_INET));

    Reset();
    adapter.overrides[AF_INET] = L"1.1.1.1 & arbitrary-command";
    CHECK(!PinDnsToLoopback(7, AF_INET));
    CHECK(adapter.command_count == 0);
    CHECK(!HasJournal(AF_INET));
}

static void TestGuidConversion() {
    // Adapter callbacks are mocked above, so exercise the real COM APIs too.
    // Their declarations must come from DnsLifecycle.cpp's own includes.
    const wchar_t* text = L"{701B1FD5-BA8C-49D5-8A32-040144000007}";
    GUID guid = {};
    wchar_t buffer[40] = {};
    CHECK(SUCCEEDED(::CLSIDFromString(text, &guid)));
    CHECK(::StringFromGUID2(guid, buffer, 40) == 39);
    CHECK(std::wstring(buffer) == text);
}

int main() {
    WSADATA data = {};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    try {
        TestGuidConversion();
        TestAutomaticAndStatic();
        TestRecoveryOwnership();
        TestPartialFailuresAndUserChanges();
        Reset();
        ::WSACleanup();
        std::cout << "Windows DNS lifecycle regression tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error) {
        Reset();
        ::WSACleanup();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
