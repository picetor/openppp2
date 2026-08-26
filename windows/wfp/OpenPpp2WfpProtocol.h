#pragma once

/*
 * User/kernel ABI for the OpenPPP2 IPv6 NAT66 callout.
 *
 * Keep this header free of C++ and WDK-only types: it is consumed by the
 * signed kernel driver and by the desktop server controller.  Every field is
 * fixed-width so x86/x64/ARM64 builds share the same IOCTL contract.
 */
#include <stdint.h>
#if defined(_WIN32) && !defined(_KERNEL_MODE)
#include <winioctl.h>
#endif

#define OPENPPP2_WFP_PROTOCOL_VERSION 1u
#define OPENPPP2_WFP_DEVICE_NT        L"\\Device\\OpenPpp2WfpNat66"
#define OPENPPP2_WFP_DEVICE_DOS       L"\\DosDevices\\OpenPpp2WfpNat66"
#define OPENPPP2_WFP_DEVICE_WIN32     L"\\\\.\\OpenPpp2WfpNat66"

#define OPENPPP2_WFP_FLAG_ENABLED     0x00000001u
#define OPENPPP2_WFP_FLAG_FAIL_CLOSED 0x00000002u

#define OPENPPP2_WFP_IOCTL_CONFIGURE \
    CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define OPENPPP2_WFP_IOCTL_CLEAR \
    CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define OPENPPP2_WFP_IOCTL_QUERY \
    CTL_CODE(FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _OPENPPP2_WFP_NAT66_CONFIG {
    uint32_t Version;
    uint32_t Flags;
    uint32_t TransitIfIndex;
    uint32_t UplinkIfIndex;
    uint8_t  UlaPrefix[16];
    uint8_t  UlaPrefixLength;
    uint8_t  ExternalAddress[16];
    uint8_t  Reserved[7];
} OPENPPP2_WFP_NAT66_CONFIG;

typedef struct _OPENPPP2_WFP_NAT66_STATUS {
    uint32_t Version;
    uint32_t Flags;
    uint32_t ActiveFlows;
    uint32_t DroppedPackets;
    uint32_t UnsupportedPackets;
    uint32_t LastError;
} OPENPPP2_WFP_NAT66_STATUS;

#if defined(__cplusplus)
static_assert(sizeof(OPENPPP2_WFP_NAT66_CONFIG) == 56, "WFP config ABI drift");
static_assert(sizeof(OPENPPP2_WFP_NAT66_STATUS) == 24, "WFP status ABI drift");
#endif
