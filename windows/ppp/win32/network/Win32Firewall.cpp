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
        }
    }
}
