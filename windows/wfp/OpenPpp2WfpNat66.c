/*
 * OpenPPP2 signed WFP NAT66 callout.
 *
 * This is a WDM/WFP driver (not a user-mode filter).  The user-mode server
 * only supplies the current interface/address snapshot through the IOCTL ABI;
 * all packet parsing, flow ownership and rewriting happens below the WFP
 * network layers.  The driver is intentionally fail-closed when a packet is
 * fragmented, malformed, non-contiguous or uses an unsupported extension
 * header: it never forwards a packet whose NAT state cannot be proven.
 */
#include <ntddk.h>
#include <ndis.h>
#include <ws2def.h>
#include <fwpsk.h>
#include <fwpmk.h>
#include <wdmsec.h>
#include <ntstrsafe.h>

#define _KERNEL_MODE 1
#include "OpenPpp2WfpProtocol.h"

static const GUID OPENPPP2_WFP_SUBLAYER =
    { 0x93f1d9d6, 0x5b64, 0x4a6d, { 0x9c, 0x8a, 0x35, 0x60, 0x64, 0x68, 0x2f, 0x20 } };
static const GUID OPENPPP2_WFP_OUTBOUND_CALLOUT =
    { 0x0b1d3c7e, 0x10d0, 0x4b4a, { 0x8b, 0x42, 0x31, 0x7d, 0x11, 0x35, 0x88, 0x01 } };
static const GUID OPENPPP2_WFP_INBOUND_CALLOUT =
    { 0x4c0a8d2b, 0x1f79, 0x4f19, { 0x9b, 0x0a, 0x9f, 0x11, 0x5d, 0x3e, 0x4c, 0x02 } };
static const GUID OPENPPP2_WFP_DEVICE_CLASS =
    { 0x5e005ac5, 0x48e0, 0x4b11, { 0x8e, 0xb6, 0x5e, 0x52, 0x62, 0x51, 0x52, 0x68 } };

#define OPENPPP2_NAT_MAX_FLOWS 2048
#define OPENPPP2_NAT_TIMEOUT_TICKS (5ULL * 60ULL * 10000000ULL)

typedef struct _OPENPPP2_NAT_FLOW {
    BOOLEAN Used;
    UCHAR Protocol;
    USHORT OriginalPort;
    USHORT TranslatedPort;
    UCHAR OriginalSource[16];
    UCHAR RemoteAddress[16];
    UCHAR OriginalPrefixLength;
    UINT64 LastSeen;
} OPENPPP2_NAT_FLOW;

typedef struct _OPENPPP2_DRIVER_STATE {
    PDEVICE_OBJECT DeviceObject;
    UNICODE_STRING SymbolicLink;
    HANDLE Engine;
    HANDLE InjectionHandle;
    UINT32 OutboundCalloutId;
    UINT32 InboundCalloutId;
    UINT64 OutboundFilterId;
    UINT64 InboundFilterId;
    KSPIN_LOCK Lock;
    OPENPPP2_WFP_NAT66_CONFIG Config;
    OPENPPP2_NAT_FLOW Flows[OPENPPP2_NAT_MAX_FLOWS];
    volatile LONG ActiveFlows;
    volatile LONG DroppedPackets;
    volatile LONG UnsupportedPackets;
    volatile LONG LastError;
} OPENPPP2_DRIVER_STATE;

static OPENPPP2_DRIVER_STATE g_State;

static VOID OpenPpp2SetError(_In_ ULONG Error) {
    InterlockedExchange(&g_State.LastError, (LONG)Error);
}

static BOOLEAN OpenPpp2PrefixMatch(_In_reads_(16) const UCHAR* Address,
    _In_reads_(16) const UCHAR* Prefix, _In_ UCHAR PrefixLength) {
    UCHAR full = (UCHAR)(PrefixLength / 8);
    UCHAR bits = (UCHAR)(PrefixLength % 8);
    if (full > 16) {
        return FALSE;
    }
    if (full != 0 && RtlCompareMemory(Address, Prefix, full) != full) {
        return FALSE;
    }
    if (bits != 0 && full < 16) {
        UCHAR mask = (UCHAR)(0xffu << (8 - bits));
        if ((Address[full] & mask) != (Prefix[full] & mask)) {
            return FALSE;
        }
    }
    return TRUE;
}

static ULONG OpenPpp2FoldChecksum(_In_ ULONG Sum) {
    while ((Sum >> 16) != 0) {
        Sum = (Sum & 0xffff) + (Sum >> 16);
    }
    return Sum;
}

static ULONG OpenPpp2AddBytes(_In_ ULONG Sum, _In_reads_bytes_(Length) const UCHAR* Data, _In_ ULONG Length) {
    ULONG i;
    for (i = 0; i + 1 < Length; i += 2) {
        Sum += ((ULONG)Data[i] << 8) | Data[i + 1];
    }
    if ((Length & 1) != 0) {
        Sum += (ULONG)Data[Length - 1] << 8;
    }
    return Sum;
}

static USHORT OpenPpp2Checksum(_In_reads_bytes_(PacketLength) const UCHAR* Packet,
    _In_ ULONG PacketLength, _In_ ULONG L4Offset, _In_ UCHAR NextHeader) {
    ULONG sum = 0;
    const UCHAR* source = Packet + 8;
    const UCHAR* destination = Packet + 24;
    ULONG l4Length = PacketLength - L4Offset;
    sum = OpenPpp2AddBytes(sum, source, 16);
    sum = OpenPpp2AddBytes(sum, destination, 16);
    sum += (l4Length >> 16) & 0xffff;
    sum += l4Length & 0xffff;
    sum += NextHeader;
    sum = OpenPpp2AddBytes(sum, Packet + L4Offset, l4Length);
    sum = OpenPpp2FoldChecksum(sum);
    return (USHORT)~sum;
}

static BOOLEAN OpenPpp2LocateTransport(_In_reads_bytes_(Length) const UCHAR* Packet,
    _In_ ULONG Length, _Out_ ULONG* Offset, _Out_ UCHAR* NextHeader) {
    ULONG cursor = 40;
    UCHAR next = Packet[6];
    ULONG guard = 0;

    while (next == 0 || next == 43 || next == 60 || next == 51) {
        ULONG headerLength;
        if (++guard > 8 || cursor + 2 > Length) {
            return FALSE;
        }
        headerLength = ((next == 51) ? ((ULONG)Packet[cursor + 1] + 2) * 4 : ((ULONG)Packet[cursor + 1] + 1) * 8);
        if (cursor + headerLength > Length) {
            return FALSE;
        }
        next = Packet[cursor];
        cursor += headerLength;
    }

    if (next == 44) {
        USHORT fragment;
        if (cursor + 8 > Length) {
            return FALSE;
        }
        fragment = (USHORT)(((USHORT)Packet[cursor + 2] << 8) | Packet[cursor + 3]);
        // NAT state is not shared with later fragments in v1.  Reject the
        // entire fragment group rather than translating only its first part.
        if ((fragment & 0xfff9) != 0) {
            return FALSE;
        }
        next = Packet[cursor];
        cursor += 8;
    }

    if (next != IPPROTO_TCP && next != IPPROTO_UDP && next != IPPROTO_ICMPV6) {
        return FALSE;
    }
    if (cursor >= Length) {
        return FALSE;
    }
    *Offset = cursor;
    *NextHeader = next;
    return TRUE;
}

static BOOLEAN OpenPpp2ReadTuple(_In_reads_bytes_(Length) const UCHAR* Packet,
    _In_ ULONG Length, _Out_ ULONG* L4Offset, _Out_ UCHAR* Protocol,
    _Out_ USHORT* SourcePort, _Out_ USHORT* DestinationPort) {
    ULONG offset;
    UCHAR protocol;
    if (Length < 40 || (Packet[0] >> 4) != 6 || !OpenPpp2LocateTransport(Packet, Length, &offset, &protocol)) {
        return FALSE;
    }
    if (protocol == IPPROTO_TCP || protocol == IPPROTO_UDP) {
        if (offset + 8 > Length) {
            return FALSE;
        }
        *SourcePort = (USHORT)(((USHORT)Packet[offset] << 8) | Packet[offset + 1]);
        *DestinationPort = (USHORT)(((USHORT)Packet[offset + 2] << 8) | Packet[offset + 3]);
    }
    else {
        if (offset + 8 > Length) {
            return FALSE;
        }
        *SourcePort = (USHORT)(((USHORT)Packet[offset + 4] << 8) | Packet[offset + 5]);
        // ICMPv6 echo request/reply uses the identifier as the stable
        // flow key.  Other ICMPv6 messages do not have a NAT-safe port
        // equivalent and are rejected below.
        if (Packet[offset] != 128 && Packet[offset] != 129) {
            return FALSE;
        }
        *DestinationPort = *SourcePort;
    }
    *L4Offset = offset;
    *Protocol = protocol;
    return TRUE;
}

static POPENPPP2_NAT_FLOW OpenPpp2FindOutbound(_In_reads_(16) const UCHAR* Source,
    _In_reads_(16) const UCHAR* Remote, _In_ UCHAR Protocol, _In_ USHORT Port) {
    ULONG i;
    for (i = 0; i < OPENPPP2_NAT_MAX_FLOWS; ++i) {
        POPENPPP2_NAT_FLOW flow = &g_State.Flows[i];
        if (flow->Used && flow->Protocol == Protocol && flow->OriginalPort == Port &&
            RtlCompareMemory(flow->OriginalSource, Source, 16) == 16 &&
            RtlCompareMemory(flow->RemoteAddress, Remote, 16) == 16) {
            return flow;
        }
    }
    return NULL;
}

static POPENPPP2_NAT_FLOW OpenPpp2FindInbound(_In_reads_(16) const UCHAR* Remote,
    _In_ UCHAR Protocol, _In_ USHORT Port) {
    ULONG i;
    for (i = 0; i < OPENPPP2_NAT_MAX_FLOWS; ++i) {
        POPENPPP2_NAT_FLOW flow = &g_State.Flows[i];
        if (flow->Used && flow->Protocol == Protocol && flow->TranslatedPort == Port &&
            RtlCompareMemory(flow->RemoteAddress, Remote, 16) == 16) {
            return flow;
        }
    }
    return NULL;
}

static POPENPPP2_NAT_FLOW OpenPpp2CreateFlow(_In_reads_(16) const UCHAR* Source,
    _In_reads_(16) const UCHAR* Remote, _In_ UCHAR Protocol, _In_ USHORT Port) {
    ULONG i;
    USHORT translated = (USHORT)(49152 + (Port % 16384));
    UINT64 now = KeQueryInterruptTime();
    for (i = 0; i < OPENPPP2_NAT_MAX_FLOWS; ++i) {
        POPENPPP2_NAT_FLOW flow = &g_State.Flows[i];
        if (flow->Used && now - flow->LastSeen > OPENPPP2_NAT_TIMEOUT_TICKS) {
            flow->Used = FALSE;
            InterlockedDecrement(&g_State.ActiveFlows);
        }
        if (!flow->Used) {
            RtlZeroMemory(flow, sizeof(*flow));
            flow->Used = TRUE;
            flow->Protocol = Protocol;
            flow->OriginalPort = Port;
            flow->TranslatedPort = translated;
            RtlCopyMemory(flow->OriginalSource, Source, 16);
            RtlCopyMemory(flow->RemoteAddress, Remote, 16);
            flow->OriginalPrefixLength = g_State.Config.UlaPrefixLength;
            flow->LastSeen = now;
            InterlockedIncrement(&g_State.ActiveFlows);
            return flow;
        }
        translated = (USHORT)(translated == 65535 ? 49152 : translated + 1);
    }
    return NULL;
}

static BOOLEAN OpenPpp2RewriteChecksum(_Inout_updates_bytes_(Length) UCHAR* Packet,
    _In_ ULONG Length, _In_ ULONG L4Offset, _In_ UCHAR Protocol) {
    ULONG checksumOffset;
    if (Protocol == IPPROTO_TCP) {
        checksumOffset = L4Offset + 16;
    }
    else if (Protocol == IPPROTO_UDP) {
        checksumOffset = L4Offset + 6;
    }
    else {
        checksumOffset = L4Offset + 2;
    }
    if (checksumOffset + 2 > Length) {
        return FALSE;
    }
    Packet[checksumOffset] = 0;
    Packet[checksumOffset + 1] = 0;
    {
        USHORT checksum = OpenPpp2Checksum(Packet, Length, L4Offset, Protocol);
        Packet[checksumOffset] = (UCHAR)(checksum >> 8);
        Packet[checksumOffset + 1] = (UCHAR)checksum;
    }
    return TRUE;
}

static BOOLEAN OpenPpp2RewritePacket(_Inout_updates_bytes_(Length) UCHAR* Packet,
    _In_ ULONG Length, _In_ BOOLEAN Outbound) {
    ULONG l4Offset;
    UCHAR protocol;
    USHORT sourcePort;
    USHORT destinationPort;
    KIRQL oldIrql;
    POPENPPP2_NAT_FLOW flow = NULL;

    if (!OpenPpp2ReadTuple(Packet, Length, &l4Offset, &protocol, &sourcePort, &destinationPort)) {
        return FALSE;
    }

    KeAcquireSpinLock(&g_State.Lock, &oldIrql);
    if (Outbound) {
        if (!OpenPpp2PrefixMatch(Packet + 8, g_State.Config.UlaPrefix, g_State.Config.UlaPrefixLength)) {
            KeReleaseSpinLock(&g_State.Lock, oldIrql);
            return TRUE;
        }
        flow = OpenPpp2FindOutbound(Packet + 8, Packet + 24, protocol, sourcePort);
        if (flow == NULL) {
            flow = OpenPpp2CreateFlow(Packet + 8, Packet + 24, protocol, sourcePort);
        }
        if (flow != NULL) {
            RtlCopyMemory(Packet + 8, g_State.Config.ExternalAddress, 16);
            if (protocol == IPPROTO_ICMPV6) {
                Packet[l4Offset + 4] = (UCHAR)(flow->TranslatedPort >> 8);
                Packet[l4Offset + 5] = (UCHAR)flow->TranslatedPort;
            }
            else {
                Packet[l4Offset] = (UCHAR)(flow->TranslatedPort >> 8);
                Packet[l4Offset + 1] = (UCHAR)flow->TranslatedPort;
            }
        }
    }
    else {
        if (RtlCompareMemory(Packet + 24, g_State.Config.ExternalAddress, 16) != 16) {
            KeReleaseSpinLock(&g_State.Lock, oldIrql);
            return TRUE;
        }
        flow = OpenPpp2FindInbound(Packet + 8, protocol, destinationPort);
        if (flow != NULL) {
            RtlCopyMemory(Packet + 24, flow->OriginalSource, 16);
            if (protocol == IPPROTO_ICMPV6) {
                Packet[l4Offset + 4] = (UCHAR)(flow->OriginalPort >> 8);
                Packet[l4Offset + 5] = (UCHAR)flow->OriginalPort;
            }
            else {
                Packet[l4Offset + 2] = (UCHAR)(flow->OriginalPort >> 8);
                Packet[l4Offset + 3] = (UCHAR)flow->OriginalPort;
            }
        }
    }

    if (flow != NULL) {
        flow->LastSeen = KeQueryInterruptTime();
    }
    KeReleaseSpinLock(&g_State.Lock, oldIrql);

    if (flow == NULL) {
        InterlockedIncrement(&g_State.DroppedPackets);
        return FALSE;
    }
    return OpenPpp2RewriteChecksum(Packet, Length, l4Offset, protocol);
}

static VOID NTAPI OpenPpp2InjectionComplete(
    VOID* Context,
    NET_BUFFER_LIST* NetBufferList,
    BOOLEAN DispatchLevel) {
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DispatchLevel);
    if (NetBufferList != NULL) {
        FwpsFreeCloneNetBufferList0(NetBufferList, 0);
    }
}

static VOID OpenPpp2Absorb(_Inout_ FWPS_CLASSIFY_OUT0* ClassifyOut) {
    ClassifyOut->actionType = FWP_ACTION_BLOCK;
    ClassifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
    ClassifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
}

static VOID NTAPI OpenPpp2Classify(
    const FWPS_INCOMING_VALUES0* Values,
    const FWPS_INCOMING_METADATA_VALUES0* Metadata,
    VOID* LayerData,
    const VOID* ClassifyContext,
    const FWPS_FILTER2* Filter,
    UINT64 FlowContext,
    FWPS_CLASSIFY_OUT0* ClassifyOut) {
    BOOLEAN outbound;
    UINT32 sourceIfIndex;
    UINT32 destinationIfIndex;
    UINT32 injectIfIndex;
    PNET_BUFFER_LIST nbl;
    PNET_BUFFER nb;
    ULONG length;
    PUCHAR packet;
    NET_BUFFER_LIST* clone = NULL;
    PUCHAR clonePacket;
    NTSTATUS status;
    COMPARTMENT_ID compartment = UNSPECIFIED_COMPARTMENT_ID;

    UNREFERENCED_PARAMETER(ClassifyContext);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FlowContext);

    ClassifyOut->actionType = FWP_ACTION_PERMIT;
    if ((g_State.Config.Flags & OPENPPP2_WFP_FLAG_ENABLED) == 0 || LayerData == NULL) {
        return;
    }
    if ((ClassifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) {
        return;
    }

    // The forward layer exposes both the arrival and next-hop interface.
    // Only the transit->uplink and uplink->transit directions belong to NAT66.
    sourceIfIndex = Values->incomingValue[FWPS_FIELD_IPFORWARD_V6_SOURCE_INTERFACE_INDEX].value.uint32;
    destinationIfIndex = Values->incomingValue[FWPS_FIELD_IPFORWARD_V6_DESTINATION_INTERFACE_INDEX].value.uint32;
    outbound = sourceIfIndex == g_State.Config.TransitIfIndex &&
        destinationIfIndex == g_State.Config.UplinkIfIndex;
    if (!outbound && !(sourceIfIndex == g_State.Config.UplinkIfIndex &&
        destinationIfIndex == g_State.Config.TransitIfIndex)) {
        return;
    }
    injectIfIndex = destinationIfIndex;

    nbl = (PNET_BUFFER_LIST)LayerData;
    if (nbl->Next != NULL) {
        InterlockedIncrement(&g_State.UnsupportedPackets);
        ClassifyOut->actionType = (g_State.Config.Flags & OPENPPP2_WFP_FLAG_FAIL_CLOSED) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
        if (ClassifyOut->actionType == FWP_ACTION_BLOCK) OpenPpp2Absorb(ClassifyOut);
        return;
    }
    nb = NET_BUFFER_LIST_FIRST_NB(nbl);
    if (nb == NULL || NET_BUFFER_NEXT_NB(nb) != NULL) {
        InterlockedIncrement(&g_State.UnsupportedPackets);
        ClassifyOut->actionType = (g_State.Config.Flags & OPENPPP2_WFP_FLAG_FAIL_CLOSED) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
        if (ClassifyOut->actionType == FWP_ACTION_BLOCK) OpenPpp2Absorb(ClassifyOut);
        return;
    }
    length = nb != NULL ? NET_BUFFER_DATA_LENGTH(nb) : 0;
    packet = nb != NULL ? NdisGetDataBuffer(nb, length, NULL, 1, 0) : NULL;
    if (packet == NULL || length < 40) {
        InterlockedIncrement(&g_State.UnsupportedPackets);
        ClassifyOut->actionType = (g_State.Config.Flags & OPENPPP2_WFP_FLAG_FAIL_CLOSED) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
        if (ClassifyOut->actionType == FWP_ACTION_BLOCK) OpenPpp2Absorb(ClassifyOut);
        return;
    }

    // Do not clone unrelated forwarded IPv6 traffic.  This also avoids
    // blocking ordinary forwarding when NAT66 is enabled on a shared host.
    if ((outbound && !OpenPpp2PrefixMatch(packet + 8, g_State.Config.UlaPrefix, g_State.Config.UlaPrefixLength)) ||
        (!outbound && RtlCompareMemory(packet + 24, g_State.Config.ExternalAddress, 16) != 16)) {
        return;
    }

    if (g_State.InjectionHandle == NULL) {
        OpenPpp2SetError((ULONG)STATUS_INVALID_HANDLE);
        InterlockedIncrement(&g_State.DroppedPackets);
        OpenPpp2Absorb(ClassifyOut);
        return;
    }
    {
        FWPS_PACKET_INJECTION_STATE injectionState =
            FwpsQueryPacketInjectionState0(g_State.InjectionHandle, nbl, NULL);
        if (injectionState == FWPS_PACKET_INJECTED_BY_SELF ||
            injectionState == FWPS_PACKET_PREVIOUSLY_INJECTED_BY_SELF) {
            return;
        }
    }

    status = FwpsAllocateCloneNetBufferList0(nbl, NULL, NULL, 0, &clone);
    if (!NT_SUCCESS(status) || clone == NULL) {
        OpenPpp2SetError((ULONG)status);
        InterlockedIncrement(&g_State.DroppedPackets);
        OpenPpp2Absorb(ClassifyOut);
        return;
    }

    clonePacket = NdisGetDataBuffer(NET_BUFFER_LIST_FIRST_NB(clone), length, NULL, 1, 0);
    if (clonePacket == NULL || !OpenPpp2RewritePacket(clonePacket, length, outbound)) {
        FwpsFreeCloneNetBufferList0(clone, 0);
        InterlockedIncrement(&g_State.UnsupportedPackets);
        ClassifyOut->actionType = (g_State.Config.Flags & OPENPPP2_WFP_FLAG_FAIL_CLOSED) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
        if (ClassifyOut->actionType == FWP_ACTION_BLOCK) {
            OpenPpp2Absorb(ClassifyOut);
        }
    }
    else {
        if (Metadata != NULL && (Metadata->currentMetadataValues & FWPS_METADATA_FIELD_COMPARTMENT_ID) != 0) {
            compartment = Metadata->compartmentId;
        }
        status = FwpsInjectForwardAsync0(g_State.InjectionHandle, NULL, 0, AF_INET6,
            compartment, injectIfIndex, clone, OpenPpp2InjectionComplete, NULL);
        if (!NT_SUCCESS(status)) {
            OpenPpp2SetError((ULONG)status);
            FwpsFreeCloneNetBufferList0(clone, 0);
            InterlockedIncrement(&g_State.DroppedPackets);
        }
        // The original packet is always absorbed.  The clone is the only
        // packet allowed to continue after translation.
        OpenPpp2Absorb(ClassifyOut);
    }
}

static NTSTATUS NTAPI OpenPpp2Notify(FWPS_CALLOUT_NOTIFY_TYPE NotifyType, const GUID* FilterKey, FWPS_FILTER2* Filter) {
    UNREFERENCED_PARAMETER(NotifyType);
    UNREFERENCED_PARAMETER(FilterKey);
    UNREFERENCED_PARAMETER(Filter);
    return STATUS_SUCCESS;
}

static VOID NTAPI OpenPpp2FlowDelete(UINT16 LayerId, UINT32 CalloutId, UINT64 FlowContext) {
    UNREFERENCED_PARAMETER(LayerId);
    UNREFERENCED_PARAMETER(CalloutId);
    UNREFERENCED_PARAMETER(FlowContext);
}

static NTSTATUS OpenPpp2AddWfpObjects(PDEVICE_OBJECT DeviceObject) {
    NTSTATUS status;
    FWPM_SESSION0 session = { 0 };
    FWPM_SUBLAYER0 sublayer = { 0 };
    FWPM_CALLOUT0 callout = { 0 };
    FWPM_FILTER0 filter = { 0 };
    FWPS_CALLOUT2 kernelCallout = { 0 };

    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    status = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &session, &g_State.Engine);
    if (!NT_SUCCESS(status)) return status;

    sublayer.subLayerKey = OPENPPP2_WFP_SUBLAYER;
    sublayer.displayData.name = L"OpenPPP2 NAT66";
    sublayer.weight = 0x8000;
    status = FwpmSubLayerAdd0(g_State.Engine, &sublayer, NULL);
    if (!NT_SUCCESS(status)) return status;

    status = FwpsInjectionHandleCreate0(AF_INET6, FWPS_INJECTION_TYPE_FORWARD, &g_State.InjectionHandle);
    if (!NT_SUCCESS(status)) return status;

    kernelCallout.calloutKey = OPENPPP2_WFP_OUTBOUND_CALLOUT;
    kernelCallout.classifyFn = OpenPpp2Classify;
    kernelCallout.notifyFn = OpenPpp2Notify;
    kernelCallout.flowDeleteFn = OpenPpp2FlowDelete;
    status = FwpsCalloutRegister2(DeviceObject, &kernelCallout, &g_State.OutboundCalloutId);
    if (!NT_SUCCESS(status)) return status;

    kernelCallout.calloutKey = OPENPPP2_WFP_INBOUND_CALLOUT;
    status = FwpsCalloutRegister2(DeviceObject, &kernelCallout, &g_State.InboundCalloutId);
    if (!NT_SUCCESS(status)) return status;

    callout.calloutKey = OPENPPP2_WFP_OUTBOUND_CALLOUT;
    callout.applicableLayer = FWPM_LAYER_IPFORWARD_V6;
    callout.displayData.name = L"OpenPPP2 NAT66 outbound";
    status = FwpmCalloutAdd0(g_State.Engine, &callout, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    callout.calloutKey = OPENPPP2_WFP_INBOUND_CALLOUT;
    callout.applicableLayer = FWPM_LAYER_IPFORWARD_V6;
    callout.displayData.name = L"OpenPPP2 NAT66 inbound";
    status = FwpmCalloutAdd0(g_State.Engine, &callout, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;

    filter.subLayerKey = OPENPPP2_WFP_SUBLAYER;
    filter.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filter.weight.type = FWP_EMPTY;
    filter.layerKey = FWPM_LAYER_IPFORWARD_V6;
    filter.action.calloutKey = OPENPPP2_WFP_OUTBOUND_CALLOUT;
    filter.displayData.name = L"OpenPPP2 NAT66 outbound filter";
    status = FwpmFilterAdd0(g_State.Engine, &filter, NULL, &g_State.OutboundFilterId);
    if (!NT_SUCCESS(status)) return status;
    filter.layerKey = FWPM_LAYER_IPFORWARD_V6;
    filter.action.calloutKey = OPENPPP2_WFP_INBOUND_CALLOUT;
    filter.displayData.name = L"OpenPPP2 NAT66 inbound filter";
    status = FwpmFilterAdd0(g_State.Engine, &filter, NULL, &g_State.InboundFilterId);
    return status;
}

static VOID OpenPpp2RemoveWfpObjects(VOID) {
    if (g_State.InjectionHandle != NULL) {
        FwpsInjectionHandleDestroy0(g_State.InjectionHandle);
        g_State.InjectionHandle = NULL;
    }
    if (g_State.Engine != NULL) {
        if (g_State.OutboundFilterId != 0) FwpmFilterDeleteById0(g_State.Engine, g_State.OutboundFilterId);
        if (g_State.InboundFilterId != 0) FwpmFilterDeleteById0(g_State.Engine, g_State.InboundFilterId);
        FwpmCalloutDeleteByKey0(g_State.Engine, &OPENPPP2_WFP_OUTBOUND_CALLOUT);
        FwpmCalloutDeleteByKey0(g_State.Engine, &OPENPPP2_WFP_INBOUND_CALLOUT);
        FwpmSubLayerDeleteByKey0(g_State.Engine, &OPENPPP2_WFP_SUBLAYER);
        FwpmEngineClose0(g_State.Engine);
        g_State.Engine = NULL;
    }
    if (g_State.OutboundCalloutId != 0) FwpsCalloutUnregisterById0(g_State.OutboundCalloutId);
    if (g_State.InboundCalloutId != 0) FwpsCalloutUnregisterById0(g_State.InboundCalloutId);
}

static NTSTATUS NTAPI OpenPpp2CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI OpenPpp2DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG_PTR information = 0;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (code == OPENPPP2_WFP_IOCTL_CONFIGURE && inputLength >= sizeof(OPENPPP2_WFP_NAT66_CONFIG)) {
        OPENPPP2_WFP_NAT66_CONFIG* config = (OPENPPP2_WFP_NAT66_CONFIG*)Irp->AssociatedIrp.SystemBuffer;
        if (config->Version == OPENPPP2_WFP_PROTOCOL_VERSION &&
            (config->Flags & OPENPPP2_WFP_FLAG_ENABLED) != 0 &&
            (config->Flags & ~(OPENPPP2_WFP_FLAG_ENABLED | OPENPPP2_WFP_FLAG_FAIL_CLOSED)) == 0 &&
            config->TransitIfIndex != 0 && config->UplinkIfIndex != 0 &&
            config->TransitIfIndex != config->UplinkIfIndex && config->UlaPrefixLength <= 128) {
            KIRQL oldIrql;
            KeAcquireSpinLock(&g_State.Lock, &oldIrql);
            RtlCopyMemory(&g_State.Config, config, sizeof(g_State.Config));
            RtlZeroMemory(g_State.Flows, sizeof(g_State.Flows));
            InterlockedExchange(&g_State.ActiveFlows, 0);
            InterlockedExchange(&g_State.DroppedPackets, 0);
            InterlockedExchange(&g_State.UnsupportedPackets, 0);
            InterlockedExchange(&g_State.LastError, 0);
            KeReleaseSpinLock(&g_State.Lock, oldIrql);
            status = STATUS_SUCCESS;
        }
        else status = STATUS_INVALID_PARAMETER;
    }
    else if (code == OPENPPP2_WFP_IOCTL_CLEAR) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_State.Lock, &oldIrql);
        RtlZeroMemory(&g_State.Config, sizeof(g_State.Config));
        RtlZeroMemory(g_State.Flows, sizeof(g_State.Flows));
        InterlockedExchange(&g_State.ActiveFlows, 0);
        KeReleaseSpinLock(&g_State.Lock, oldIrql);
        status = STATUS_SUCCESS;
    }
    else if (code == OPENPPP2_WFP_IOCTL_QUERY && outputLength >= sizeof(OPENPPP2_WFP_NAT66_STATUS)) {
        OPENPPP2_WFP_NAT66_STATUS* query = (OPENPPP2_WFP_NAT66_STATUS*)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(query, sizeof(*query));
        query->Version = OPENPPP2_WFP_PROTOCOL_VERSION;
        query->Flags = g_State.Config.Flags;
        query->ActiveFlows = (ULONG)g_State.ActiveFlows;
        query->DroppedPackets = (ULONG)g_State.DroppedPackets;
        query->UnsupportedPackets = (ULONG)g_State.UnsupportedPackets;
        query->LastError = (ULONG)g_State.LastError;
        information = sizeof(*query);
        status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID NTAPI OpenPpp2Unload(PDRIVER_OBJECT DriverObject) {
    OpenPpp2RemoveWfpObjects();
    IoDeleteSymbolicLink(&g_State.SymbolicLink);
    if (g_State.DeviceObject != NULL) IoDeleteDevice(g_State.DeviceObject);
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    NTSTATUS status;
    UNICODE_STRING deviceName;
    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&g_State, sizeof(g_State));
    KeInitializeSpinLock(&g_State.Lock);
    RtlInitUnicodeString(&deviceName, OPENPPP2_WFP_DEVICE_NT);
    RtlInitUnicodeString(&g_State.SymbolicLink, OPENPPP2_WFP_DEVICE_DOS);

    {
        UNICODE_STRING sddl;
        RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        status = IoCreateDeviceSecure(DriverObject, 0, &deviceName, FILE_DEVICE_NETWORK,
            FILE_DEVICE_SECURE_OPEN, FALSE, &sddl, &OPENPPP2_WFP_DEVICE_CLASS, &g_State.DeviceObject);
    }
    if (!NT_SUCCESS(status)) return status;
    status = IoCreateSymbolicLink(&g_State.SymbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_State.DeviceObject);
        return status;
    }

    DriverObject->DriverUnload = OpenPpp2Unload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = OpenPpp2CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = OpenPpp2CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = OpenPpp2DeviceControl;
    status = OpenPpp2AddWfpObjects(g_State.DeviceObject);
    if (!NT_SUCCESS(status)) {
        OpenPpp2Unload(DriverObject);
    }
    return status;
}
