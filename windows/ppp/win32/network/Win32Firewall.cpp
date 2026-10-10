#include <windows/ppp/win32/network/Firewall.h>
#include <windows/ppp/win32/Win32Native.h>
#include <windows/ppp/win32/Win32Variant.h>
#include <ppp/text/Encoding.h>

#include <Windows.h>
#include <atlbase.h>
#include <netfw.h>
#include <fwpmu.h>
#include <netioapi.h>
#include <comutil.h>
#include <atomic>

#pragma comment(lib, "ole32.lib")          /* netfw32.lib */
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "fwpuclnt.lib")

namespace ppp
{
    namespace win32
    {
        namespace network
        {
            static bool FW_NetFirewallAddApplication(const wchar_t* name, const wchar_t* executablePath, NET_FW_PROFILE_TYPE netFwType) noexcept
            {
                if (!name || !executablePath)
                {
                    return false;
                }

                if (GetFileAttributes(executablePath) == INVALID_FILE_ATTRIBUTES)
                {
                    return false;
                }

                CComPtr<INetFwMgr> pNetFwMgr;
                HRESULT hr = CoCreateInstance(__uuidof(NetFwMgr), NULLPTR, CLSCTX_INPROC_SERVER, __uuidof(INetFwMgr), (void**)&pNetFwMgr);
                if (FAILED(hr))
                {
                    return false;
                }

                CComPtr<INetFwPolicy> pNetFwPolicy;
                hr = pNetFwMgr->get_LocalPolicy(&pNetFwPolicy);
                if (FAILED(hr))
                {
                    return false;
                }

                CComPtr<INetFwAuthorizedApplication> pApp;
                hr = CoCreateInstance(__uuidof(NetFwAuthorizedApplication), NULLPTR, CLSCTX_INPROC_SERVER, __uuidof(INetFwAuthorizedApplication), (void**)&pApp);
                if (FAILED(hr))
                {
                    return false;
                }

                // �������б��������ʾ������
                BSTR bstrName = SysAllocString(name);
                pApp->put_Name(bstrName);
                SysFreeString(bstrName);

                // �����·�����ļ���
                BSTR bstrExecutablePath = SysAllocString(executablePath);
                pApp->put_ProcessImageFileName(bstrExecutablePath);
                SysFreeString(bstrExecutablePath);

                // �Ƿ����øù���
                pApp->put_Enabled(VARIANT_TRUE);

                // ���뵽����ǽ�Ĺ�������
                CComPtr<INetFwProfile> pNetFwProfile;
                hr = pNetFwPolicy->GetProfileByType(netFwType, &pNetFwProfile);
                if (FAILED(hr))
                {
                    return false;
                }

                CComPtr<INetFwAuthorizedApplications> pApps;
                hr = pNetFwProfile->get_AuthorizedApplications(&pApps);
                if (FAILED(hr))
                {
                    return false;
                }

                hr = pApps->Add(pApp);
                if (FAILED(hr))
                {
                    return false;
                }
                return true;
            }

            static bool FW_NetFirewallAddApplication(const wchar_t* name, const wchar_t* executablePath)
            {
                HRESULT hr = S_OK;

                // ����NetFwPolicy2����
                INetFwPolicy2* pPolicy = NULLPTR;
                hr = CoCreateInstance(__uuidof(NetFwPolicy2), NULLPTR, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)&pPolicy);
                if (FAILED(hr))
                {
                    return false;
                }

                // ��ȡINetFwRules����
                INetFwRules* pRules = NULLPTR;
                hr = pPolicy->get_Rules(&pRules);
                if (FAILED(hr))
                {
                    pPolicy->Release();
                    return false;
                }

                // �����������
                INetFwRule* pRule = NULLPTR;
                hr = CoCreateInstance(__uuidof(NetFwRule), NULLPTR, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), (void**)&pRule);
                if (FAILED(hr))
                {
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                // ���ù�������
                _bstr_t bstrName(name);
                _bstr_t bstrExecutablePath(executablePath);

                hr = pRule->put_Name(bstrName);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                hr = pRule->put_Description(bstrName);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                hr = pRule->put_ApplicationName(bstrExecutablePath);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                hr = pRule->put_Direction(NET_FW_RULE_DIR_IN);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                hr = pRule->put_Action(NET_FW_ACTION_ALLOW);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                hr = pRule->put_Enabled(VARIANT_TRUE);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                // ����Ƿ��Ѵ���ͬ������
                VARIANT_BOOL bFound = VARIANT_FALSE;
                IUnknown* pEnumeratorUnk = NULLPTR;
                hr = pRules->get__NewEnum(&pEnumeratorUnk);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                IEnumVARIANT* pEnumerator = NULLPTR;
                hr = pEnumeratorUnk->QueryInterface(__uuidof(IEnumVARIANT), (void**)&pEnumerator);
                pEnumeratorUnk->Release();
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                VARIANT var;
                ULONG cElems;
                while (pEnumerator->Next(1, &var, &cElems) == S_OK)
                {
                    IUnknown* pUnknown = var.punkVal;
                    INetFwRule* pExistingRule = NULLPTR;
                    hr = pUnknown->QueryInterface(__uuidof(INetFwRule), (void**)&pExistingRule);
                    if (hr == S_OK)
                    {
                        _bstr_t bstrExistingName;
                        hr = pExistingRule->get_Name(bstrExistingName.GetAddress());
                        if (FAILED(hr))
                        {
                            continue;
                        }

                        _bstr_t bstrExistingAppPath;
                        hr = pExistingRule->get_ApplicationName(bstrExistingAppPath.GetAddress());
                        if (FAILED(hr))
                        {
                            continue;
                        }

                        if (bstrExistingName == bstrName && bstrExistingAppPath == bstrExecutablePath) {
                            bFound = VARIANT_TRUE;
                            break;
                        }
                        else
                        {
                            pExistingRule->Release();
                        }
                    }
                    VariantClear(&var);
                }

                // ����Ѵ���ͬ���������ͷ���Դ������
                pEnumerator->Release();
                if (bFound)
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return true;
                }

                // ���ӹ���
                hr = pRules->Add(pRule);
                if (FAILED(hr))
                {
                    pRule->Release();
                    pRules->Release();
                    pPolicy->Release();
                    return false;
                }

                // �ͷ���Դ
                pRule->Release();
                pRules->Release();
                pPolicy->Release();

                return true;
            }

            static bool FW_NetFirewallAddAllApplication(const wchar_t* name, const wchar_t* executablePath) noexcept
            {
                if (FW_NetFirewallAddApplication(name, executablePath))
                {
                    return true;
                }

                bool b = true;
                b &= FW_NetFirewallAddApplication(name, executablePath, NET_FW_PROFILE_STANDARD); // 1
                b &= FW_NetFirewallAddApplication(name, executablePath, NET_FW_PROFILE_CURRENT);  // 2
                return b;
            }

            static bool FW_require(const char* name, const char* executablePath, NET_FW_PROFILE_TYPE netFwType, bool(*f)(_bstr_t&, _bstr_t&, NET_FW_PROFILE_TYPE)) noexcept
            {
                if (NULLPTR == name)
                {
                    name = "";
                }

                if (NULLPTR == executablePath)
                {
                    executablePath = "";
                }

                _bstr_t bstr_name(name);
                _bstr_t bstr_executablePath(executablePath);

                return f(bstr_name, bstr_executablePath, netFwType);
            }

            bool Fw::NetFirewallAddApplication(const char* name, const char* executablePath, NetFirewallType netFwType) noexcept
            {
                NET_FW_PROFILE_TYPE netFwProfileType = NET_FW_PROFILE_DOMAIN; // ��������
                if (netFwType == NetFirewallType_PrivateNetwork)   // ר������
                {
                    netFwProfileType = NET_FW_PROFILE_STANDARD;
                }
                elif(netFwType == NetFirewallType_PublicNetwork) // ��������
                {
                    netFwProfileType = NET_FW_PROFILE_CURRENT;
                }

                return FW_require(name, executablePath, netFwProfileType, [](_bstr_t& name, _bstr_t& executablePath, NET_FW_PROFILE_TYPE netFwType) noexcept
                    {
                        return FW_NetFirewallAddApplication(name, executablePath, netFwType);
                    });
            }

            bool Fw::NetFirewallAddApplication(const char* name, const char* executablePath) noexcept
            {
                return FW_require(name, executablePath, NET_FW_PROFILE_TYPE_MAX, [](_bstr_t& name, _bstr_t& executablePath, NET_FW_PROFILE_TYPE netFwType) noexcept
                    {
                        return FW_NetFirewallAddApplication(name, executablePath);
                    });
            }

            bool Fw::NetFirewallAddAllApplication(const char* name, const char* executablePath) noexcept
            {
                return FW_require(name, executablePath, NET_FW_PROFILE_TYPE_MAX, [](_bstr_t& name, _bstr_t& executablePath, NET_FW_PROFILE_TYPE netFwType) noexcept
                    {
                        return FW_NetFirewallAddAllApplication(name, executablePath);
                    });
            }

            bool Fw::SetIPv6LeakBlock(const char* rule_name, const char* interface_name, bool enabled) noexcept
            {
                if (NULLPTR == rule_name || *rule_name == '\0')
                {
                    return false;
                }

                const std::wstring name = ppp::text::Encoding::utf8_to_wstring(rule_name);
                if (name.empty())
                {
                    return false;
                }

                CComPtr<INetFwPolicy2> policy;
                HRESULT hr = ::CoCreateInstance(__uuidof(NetFwPolicy2), NULLPTR, CLSCTX_INPROC_SERVER,
                    __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
                if (FAILED(hr) || NULLPTR == policy)
                {
                    return false;
                }

                CComPtr<INetFwRules> rules;
                hr = policy->get_Rules(&rules);
                if (FAILED(hr) || NULLPTR == rules)
                {
                    return false;
                }

                // Always remove an old rule first. This also recovers a rule left
                // behind after an abnormal process termination.
                rules->Remove(CComBSTR(name.c_str()));
                if (!enabled)
                {
                    return true;
                }
                if (NULLPTR == interface_name || *interface_name == '\0')
                {
                    return false;
                }

                const std::wstring interface_w = ppp::text::Encoding::utf8_to_wstring(interface_name);
                if (interface_w.empty())
                {
                    return false;
                }

                CComPtr<INetFwRule> rule;
                hr = ::CoCreateInstance(__uuidof(NetFwRule), NULLPTR, CLSCTX_INPROC_SERVER,
                    __uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
                if (FAILED(hr) || NULLPTR == rule)
                {
                    return false;
                }

                hr = rule->put_Name(CComBSTR(name.c_str()));
                if (SUCCEEDED(hr)) hr = rule->put_Description(CComBSTR(L"Blocks physical-interface public IPv6 while the VPN server has no IPv6 dataplane."));
                if (SUCCEEDED(hr)) hr = rule->put_Direction(NET_FW_RULE_DIR_OUT);
                if (SUCCEEDED(hr)) hr = rule->put_Action(NET_FW_ACTION_BLOCK);
                if (SUCCEEDED(hr)) hr = rule->put_Protocol(NET_FW_IP_PROTOCOL_ANY);
                if (SUCCEEDED(hr)) hr = rule->put_Profiles(NET_FW_PROFILE2_ALL);
                if (SUCCEEDED(hr)) hr = rule->put_RemoteAddresses(CComBSTR(L"2000::/3"));
                if (FAILED(hr))
                {
                    return false;
                }

                SAFEARRAYBOUND bound = {};
                bound.cElements = 1;
                bound.lLbound = 0;
                SAFEARRAY* interfaces = ::SafeArrayCreate(VT_VARIANT, 1, &bound);
                if (NULLPTR == interfaces)
                {
                    return false;
                }

                VARIANT item;
                ::VariantInit(&item);
                item.vt = VT_BSTR;
                item.bstrVal = ::SysAllocString(interface_w.c_str());
                LONG index = 0;
                hr = item.bstrVal ? ::SafeArrayPutElement(interfaces, &index, &item) : E_OUTOFMEMORY;
                ::VariantClear(&item);
                if (FAILED(hr))
                {
                    ::SafeArrayDestroy(interfaces);
                    return false;
                }

                VARIANT interface_list;
                ::VariantInit(&interface_list);
                interface_list.vt = VT_ARRAY | VT_VARIANT;
                interface_list.parray = interfaces;
                hr = rule->put_Interfaces(interface_list);
                ::VariantClear(&interface_list);
                if (FAILED(hr))
                {
                    return false;
                }

                hr = rule->put_Enabled(VARIANT_TRUE);
                if (FAILED(hr) || FAILED(rules->Add(rule)))
                {
                    rules->Remove(CComBSTR(name.c_str()));
                    return false;
                }

                // Verify that the rule can be retrieved after insertion. A COM
                // setter succeeding does not guarantee the policy store accepted it.
                CComPtr<INetFwRule> stored;
                return SUCCEEDED(rules->Item(CComBSTR(name.c_str()), &stored)) && NULLPTR != stored;
            }

            static bool FW_NetFirewallRemoveNamedRule(const wchar_t* name) noexcept
            {
                if (NULLPTR == name || *name == L'\0')
                {
                    return false;
                }

                CComPtr<INetFwPolicy2> policy;
                HRESULT hr = ::CoCreateInstance(__uuidof(NetFwPolicy2), NULLPTR, CLSCTX_INPROC_SERVER,
                    __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
                if (FAILED(hr) || NULLPTR == policy)
                {
                    return false;
                }

                CComPtr<INetFwRules> rules;
                hr = policy->get_Rules(&rules);
                if (FAILED(hr) || NULLPTR == rules)
                {
                    return false;
                }

                return SUCCEEDED(rules->Remove(CComBSTR(name)));
            }

            static bool FW_NetFirewallAddTunnelInboundRule(const wchar_t* name, const wchar_t* interface_w,
                const wchar_t* local_w, LONG protocol) noexcept
            {
                if (NULLPTR == name || *name == L'\0')
                {
                    return false;
                }

                CComPtr<INetFwPolicy2> policy;
                HRESULT hr = ::CoCreateInstance(__uuidof(NetFwPolicy2), NULLPTR, CLSCTX_INPROC_SERVER,
                    __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
                if (FAILED(hr) || NULLPTR == policy)
                {
                    return false;
                }

                CComPtr<INetFwRules> rules;
                hr = policy->get_Rules(&rules);
                if (FAILED(hr) || NULLPTR == rules)
                {
                    return false;
                }

                // Always replace an existing rule so a stale copy from an older
                // build cannot keep the old protocol scope.
                rules->Remove(CComBSTR(name));

                CComPtr<INetFwRule> rule;
                hr = ::CoCreateInstance(__uuidof(NetFwRule), NULLPTR, CLSCTX_INPROC_SERVER,
                    __uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
                if (FAILED(hr) || NULLPTR == rule)
                {
                    return false;
                }

                hr = rule->put_Name(CComBSTR(name));
                if (SUCCEEDED(hr)) hr = rule->put_Description(CComBSTR(L"Allows replies injected by the openppp2 tunnel adapter to reach the host network stack."));
                if (SUCCEEDED(hr)) hr = rule->put_Direction(NET_FW_RULE_DIR_IN);
                if (SUCCEEDED(hr)) hr = rule->put_Action(NET_FW_ACTION_ALLOW);
                if (SUCCEEDED(hr)) hr = rule->put_Profiles(NET_FW_PROFILE2_ALL);
                // The local address is the inbound scope that actually works for
                // inbound rules; without it the rule would allow the protocol on
                // every interface.
                if (SUCCEEDED(hr) && NULLPTR != local_w && *local_w != L'\0')
                {
                    hr = rule->put_LocalAddresses(CComBSTR(local_w));
                }
                if (FAILED(hr))
                {
                    return false;
                }

                if (NULLPTR != interface_w && *interface_w != L'\0')
                {
                    SAFEARRAYBOUND bound = {};
                    bound.cElements = 1;
                    bound.lLbound = 0;
                    SAFEARRAY* interfaces = ::SafeArrayCreate(VT_VARIANT, 1, &bound);
                    if (NULLPTR == interfaces)
                    {
                        return false;
                    }

                    VARIANT item;
                    ::VariantInit(&item);
                    item.vt = VT_BSTR;
                    item.bstrVal = ::SysAllocString(interface_w);
                    LONG index = 0;
                    HRESULT hr_interfaces =
                        item.bstrVal ? ::SafeArrayPutElement(interfaces, &index, &item) : E_OUTOFMEMORY;
                    ::VariantClear(&item);
                    if (SUCCEEDED(hr_interfaces))
                    {
                        VARIANT interface_list;
                        ::VariantInit(&interface_list);
                        interface_list.vt = VT_ARRAY | VT_VARIANT;
                        interface_list.parray = interfaces;
                        hr_interfaces = rule->put_Interfaces(interface_list);
                        ::VariantClear(&interface_list);
                    }
                    else
                    {
                        ::SafeArrayDestroy(interfaces);
                    }

                    // The interface condition only tightens the scope: the
                    // INetFwRule documentation states that Interfaces is meant for
                    // outbound rules, so some systems reject it here.  A failure
                    // must not drop the rule, because LocalAddresses already scopes
                    // it to the tunnel subnet.
                    if (FAILED(hr_interfaces))
                    {
                        LOG_WARN("Fw::AllowTunnelInbound: put_Interfaces failed, hr=0x%08X",
                            (unsigned int)hr_interfaces);
                    }
                }

                // The INetFwRule documentation requires an ICMP rule to have its
                // protocol set before the rule is added; changing other conditions
                // afterwards can be rejected and silently lose the rule.  Set the
                // protocol last, immediately before enabling and adding.
                hr = rule->put_Protocol(protocol);
                if (FAILED(hr))
                {
                    return false;
                }

                hr = rule->put_Enabled(VARIANT_TRUE);
                if (FAILED(hr) || FAILED(rules->Add(rule)))
                {
                    rules->Remove(CComBSTR(name));
                    return false;
                }

                // A successful COM setter does not prove the policy store kept
                // the rule; verify it is retrievable before reporting success.
                CComPtr<INetFwRule> stored;
                return SUCCEEDED(rules->Item(CComBSTR(name), &stored)) && NULLPTR != stored;
            }

            bool Fw::AllowTunnelInbound(const char* rule_name, const char* interface_name,
                const char* local_addresses, bool enabled) noexcept
            {
                if (NULLPTR == rule_name || *rule_name == '\0')
                {
                    return false;
                }

                // ICMP has no NET_FW_IP_PROTOCOL constant; the policy store keeps
                // the raw IP protocol number (1 = ICMPv4).
                static const char* const suffixes[] = { " UDP", " ICMPv4" };
                static const LONG protocols[] = {
                    NET_FW_IP_PROTOCOL_UDP, /* 17 */
                    1,                      /* ICMPv4 */
                };
                static const int rule_count = (int)(sizeof(suffixes) / sizeof(suffixes[0]));

                const std::wstring base = ppp::text::Encoding::utf8_to_wstring(rule_name);
                if (base.empty())
                {
                    return false;
                }

                std::wstring interface_w;
                if (enabled && NULLPTR != interface_name && *interface_name != '\0')
                {
                    interface_w = ppp::text::Encoding::utf8_to_wstring(interface_name);
                    if (interface_w.empty())
                    {
                        return false;
                    }
                }

                std::wstring local_w;
                if (enabled && NULLPTR != local_addresses && *local_addresses != '\0')
                {
                    // Without a local scope the rule would allow the protocol on
                    // every interface, which is not what a tunnel adapter needs.
                    local_w = ppp::text::Encoding::utf8_to_wstring(local_addresses);
                    if (local_w.empty())
                    {
                        return false;
                    }
                }

                if (enabled && local_w.empty())
                {
                    return false;
                }

                bool ok = false;
                for (int i = 0; i < rule_count; i++)
                {
                    const std::wstring name = base + ppp::text::Encoding::utf8_to_wstring(suffixes[i]);
                    if (enabled)
                    {
                        ok |= FW_NetFirewallAddTunnelInboundRule(
                            name.c_str(), interface_w.c_str(), local_w.c_str(), protocols[i]);
                    }
                    else
                    {
                        ok |= FW_NetFirewallRemoveNamedRule(name.c_str());
                    }
                }
                return ok;
            }

            bool Fw::AddIPv6LeakBlockWfp(int interface_index, HANDLE& engine_handle) noexcept
            {
                RemoveIPv6LeakBlockWfp(engine_handle);
                if (interface_index < 0)
                {
                    return false;
                }

                NET_LUID interface_luid = {};
                DWORD result = ::ConvertInterfaceIndexToLuid(
                    static_cast<NET_IFINDEX>(interface_index), &interface_luid);
                if (result != NO_ERROR)
                {
                    LOG_ERROR("Fw::AddIPv6LeakBlockWfp: ConvertInterfaceIndexToLuid failed, result=%lu, ifindex=%d",
                        result, interface_index);
                    return false;
                }

                FWPM_SESSION0 session = {};
                session.displayData.name = const_cast<wchar_t*>(L"openppp2 IPv6 leak block");
                session.displayData.description = const_cast<wchar_t*>(L"Dynamic IPv6 kill switch for an IPv4-only VPN server");
                session.flags = FWPM_SESSION_FLAG_DYNAMIC;

                HANDLE engine = NULLPTR;
                result = ::FwpmEngineOpen0(NULLPTR, RPC_C_AUTHN_WINNT, NULLPTR, &session, &engine);
                if (result != ERROR_SUCCESS || NULLPTR == engine)
                {
                    LOG_ERROR("Fw::AddIPv6LeakBlockWfp: FwpmEngineOpen0 failed, result=%lu", result);
                    return false;
                }

                FWP_V6_ADDR_AND_MASK remote_public = {};
                remote_public.addr[0] = 0x20;
                remote_public.prefixLength = 3;

                UINT64 luid_value = interface_luid.Value;
                FWPM_FILTER_CONDITION0 conditions[2] = {};
                conditions[0].fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
                conditions[0].matchType = FWP_MATCH_EQUAL;
                conditions[0].conditionValue.type = FWP_UINT64;
                conditions[0].conditionValue.uint64 = &luid_value;
                conditions[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
                conditions[1].matchType = FWP_MATCH_EQUAL;
                conditions[1].conditionValue.type = FWP_V6_ADDR_MASK;
                conditions[1].conditionValue.v6AddrMask = &remote_public;

                FWPM_FILTER0 filter = {};
                filter.displayData.name = const_cast<wchar_t*>(L"openppp2 block physical public IPv6");
                filter.displayData.description = const_cast<wchar_t*>(L"Fail closed when the VPN server has no IPv6 dataplane");
                filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V6;
                filter.subLayerKey = FWPM_SUBLAYER_UNIVERSAL;
                filter.action.type = FWP_ACTION_BLOCK;
                filter.weight.type = FWP_EMPTY;
                filter.numFilterConditions = ARRAYSIZE(conditions);
                filter.filterCondition = conditions;

                UINT64 filter_id = 0;
                result = ::FwpmFilterAdd0(engine, &filter, NULLPTR, &filter_id);
                if (result != ERROR_SUCCESS)
                {
                    LOG_ERROR("Fw::AddIPv6LeakBlockWfp: FwpmFilterAdd0 failed, result=%lu, ifindex=%d",
                        result, interface_index);
                    ::FwpmEngineClose0(engine);
                    return false;
                }

                engine_handle = engine;
                LOG_INFO("Fw::AddIPv6LeakBlockWfp: installed dynamic filter id=%llu, ifindex=%d",
                    static_cast<unsigned long long>(filter_id), interface_index);
                return true;
            }

            void Fw::RemoveIPv6LeakBlockWfp(HANDLE& engine_handle) noexcept
            {
                HANDLE engine = engine_handle;
                engine_handle = NULLPTR;
                if (NULLPTR != engine)
                {
                    // The dynamic session removes all owned filters atomically.
                    ::FwpmEngineClose0(engine);
                }
            }

            // ---------------------------------------------------------------------
            //  WFP drop diagnostics
            //
            //  Measured problem this exists for: on a real machine every tunnel
            //  connection was aborted from the host side (WSAECONNABORTED, ecv=10053,
            //  reported as "software on your host aborted an established connection")
            //  within a second of connecting, and the packets the client injected into
            //  the tunnel adapter never reached the host stack. Neither the firewall
            //  rules nor the WFP filter inventory explained it, so the client now
            //  subscribes to the platform's own drop notifications and prints the
            //  filter, provider and service behind each one.
            // ---------------------------------------------------------------------
            namespace
            {
                HANDLE                              g_drop_engine = NULLPTR;
                HANDLE                              g_drop_subscription = NULLPTR;
                std::atomic<bool>                   g_drop_active = false;
                std::atomic<bool>                   g_drop_audit_enabled = false;

                // The platform refuses a net-event subscription until the matching audit
                // subcategory is on (measured: 0x80320013 = FWP_E_NET_EVENTS_DISABLED).
                // Flip exactly that one subcategory with the platform's own tool; no
                // other policy is read or written.
                bool WFP_RunAuditPol(const wchar_t* switch_text) noexcept {
                    std::wstring line = L"cmd.exe /c auditpol /set /subcategory:\"{0CCE9225-69AE-11D9-BED3-505054503030}\" ";
                    if (NULLPTR != switch_text) {
                        line += switch_text;
                    }

                    line += L" > nul 2>&1";

                    STARTUPINFOW startup = {};
                    startup.cb = sizeof(startup);
                    startup.dwFlags = STARTF_USESHOWWINDOW;
                    startup.wShowWindow = SW_HIDE;

                    PROCESS_INFORMATION process = {};
                    if (!::CreateProcessW(NULLPTR, &line[0], NULLPTR, NULLPTR, FALSE, CREATE_NO_WINDOW,
                        NULLPTR, NULLPTR, &startup, &process)) {
                        return false;
                    }

                    ::WaitForSingleObject(process.hProcess, 20000);
                    DWORD exit_code = 1;
                    ::GetExitCodeProcess(process.hProcess, &exit_code);
                    ::CloseHandle(process.hThread);
                    ::CloseHandle(process.hProcess);
                    return exit_code == 0;
                }

                std::string WFP_Utf8(const wchar_t* text) noexcept {
                    if (NULLPTR == text || *text == L'\0') {
                        return std::string();
                    }

                    return ppp::text::Encoding::wstring_to_utf8(text);
                }

                void WFP_LogDropDetails(HANDLE engine, UINT64 filter_id, UINT32 layer_id) noexcept {
                    std::string filter_name;
                    std::string provider_name;
                    std::string service_name;

                    FWPM_FILTER0* filter = NULLPTR;
                    if (NULLPTR != engine && ERROR_SUCCESS == ::FwpmFilterGetById0(engine, filter_id, &filter) && NULLPTR != filter) {
                        filter_name = WFP_Utf8(filter->displayData.name);

                        if (NULLPTR != filter->providerKey) {
                            FWPM_PROVIDER0* provider = NULLPTR;
                            if (ERROR_SUCCESS == ::FwpmProviderGetByKey0(engine, filter->providerKey, &provider) && NULLPTR != provider) {
                                provider_name = WFP_Utf8(provider->displayData.name);
                                service_name = WFP_Utf8(provider->serviceName);
                                ::FwpmFreeMemory0(reinterpret_cast<void**>(&provider));
                            }
                        }

                        ::FwpmFreeMemory0(reinterpret_cast<void**>(&filter));
                    }

                    LOG_WARN("WFP drop event: filterId=%llu layerId=%u filter='%s' provider='%s' service='%s'",
                        static_cast<unsigned long long>(filter_id), static_cast<unsigned>(layer_id),
                        filter_name.data(), provider_name.data(), service_name.data());
                }

                void NTAPI WFP_DropEventCallback(void* context, const FWPM_NET_EVENT1* event) noexcept {
                    if (NULLPTR == event) {
                        return;
                    }

                    try {
                        switch (event->type) {
                        case FWPM_NET_EVENT_TYPE_CLASSIFY_DROP:
                            // The classify-drop payload is a pointer to a versioned
                            // struct, not an inline member.
                            if (NULLPTR != event->classifyDrop) {
                                WFP_LogDropDetails(g_drop_engine, event->classifyDrop->filterId,
                                    event->classifyDrop->layerId);
                            }
                            break;
                        default:
                            LOG_DEBUG("WFP net event: type=%d", static_cast<int>(event->type));
                            break;
                        }
                    }
                    catch (...) {
                    }
                }
            }

            bool Fw::StartDropDiagnostics() noexcept {
                if (g_drop_active.load()) {
                    return true;
                }

                FWPM_SESSION0 session = {};
                session.displayData.name = const_cast<wchar_t*>(L"openppp2 drop diagnostics");
                session.displayData.description = const_cast<wchar_t*>(L"Names the filter behind dropped or aborted tunnel traffic");
                session.flags = FWPM_SESSION_FLAG_DYNAMIC;

                HANDLE engine = NULLPTR;
                DWORD result = ::FwpmEngineOpen0(NULLPTR, RPC_C_AUTHN_WINNT, NULLPTR, &session, &engine);
                if (result != ERROR_SUCCESS || NULLPTR == engine) {
                    LOG_ERROR("Fw::StartDropDiagnostics: FwpmEngineOpen0 failed, result=%lu", result);
                    return false;
                }

                FWPM_NET_EVENT_SUBSCRIPTION0 subscription = {};
                subscription.enumTemplate = NULLPTR;

                HANDLE subscription_handle = NULLPTR;
                result = ::FwpmNetEventSubscribe0(engine, &subscription, &WFP_DropEventCallback, NULLPTR, &subscription_handle);

                // Windows refuses the subscription while the matching audit subcategory
                // is off (measured on a real machine: 0x80320013 =
                // FWP_E_NET_EVENTS_DISABLED). Enable exactly that one subcategory
                // through the platform's own tool when that is the reason, and put it
                // back when the diagnostic stops. An audit policy that was already on
                // is never touched: the subscription succeeds on the first attempt.
                bool enabled_audit = false;
                if (result == static_cast<DWORD>(0x80320013)) {
                    if (WFP_RunAuditPol(L"/failure:enable")) {
                        LOG_INFO("WFP drop diagnostics: enabled the Filtering Platform Packet Drop audit subcategory to "
                            "receive drop events; it is turned back off when the diagnostic stops");
                        enabled_audit = true;

                        // A live run showed the subscription still refused right after
                        // the policy was set: the change takes a moment to reach the
                        // filtering platform. Retry briefly instead of giving up on the
                        // first refusal, which is what left the drop source unnamed.
                        for (int attempt = 0; attempt < 10 && result == static_cast<DWORD>(0x80320013); attempt++) {
                            ::Sleep(300);
                            result = ::FwpmNetEventSubscribe0(engine, &subscription, &WFP_DropEventCallback, NULLPTR,
                                &subscription_handle);
                        }
                    }
                }

                if (result != ERROR_SUCCESS || NULLPTR == subscription_handle) {
                    if (result == static_cast<DWORD>(0x80320013)) {
                        LOG_WARN("Fw::StartDropDiagnostics: FwpmNetEventSubscribe0 refused the subscription because the "
                            "Filtering Platform Packet Drop audit is off (0x80320013) and it could not be enabled; run "
                            "auditpol /set /subcategory:\"{0CCE9225-69AE-11D9-BED3-505054503030}\" /failure:enable as an "
                            "administrator to get the drop source named in this log");
                    }
                    else {
                        LOG_ERROR("Fw::StartDropDiagnostics: FwpmNetEventSubscribe0 failed, result=%lu", result);
                    }

                    if (enabled_audit) {
                        WFP_RunAuditPol(L"/failure:disable");
                    }

                    ::FwpmEngineClose0(engine);
                    return false;
                }

                g_drop_engine = engine;
                g_drop_subscription = subscription_handle;
                g_drop_audit_enabled.store(enabled_audit);
                g_drop_active.store(true);
                LOG_INFO("WFP drop diagnostics: subscribed to net events; drops and aborts are reported as "
                    "'WFP drop event: filterId=... provider=... service=...' while they happen");
                return true;
            }

            void Fw::StopDropDiagnostics() noexcept {
                if (!g_drop_active.exchange(false)) {
                    return;
                }

                HANDLE engine = g_drop_engine;
                HANDLE subscription = g_drop_subscription;
                g_drop_engine = NULLPTR;
                g_drop_subscription = NULLPTR;
                if (NULLPTR != engine && NULLPTR != subscription) {
                    ::FwpmNetEventUnsubscribe0(engine, subscription);
                }

                if (NULLPTR != engine) {
                    ::FwpmEngineClose0(engine);
                }

                if (g_drop_audit_enabled.exchange(false)) {
                    if (WFP_RunAuditPol(L"/failure:disable")) {
                        LOG_INFO("WFP drop diagnostics: the audit subcategory this diagnostic enabled was turned back off");
                    }
                    else {
                        LOG_WARN("WFP drop diagnostics: could not turn the Filtering Platform Packet Drop audit back off; "
                            "run auditpol /set /subcategory:\"{0CCE9225-69AE-11D9-BED3-505054503030}\" /failure:disable to restore it");
                    }
                }

                LOG_INFO("WFP drop diagnostics: stopped");
            }
        }
    }
}
