/* SquirrelBIOS x86_64-uefi: a Redfish host interface stub (roadmap F3).
 * Original code, written from DMTF DSP0270 (Redfish Host Interface
 * Specification) 1.3 and SMBIOS type 42.
 *
 * The host interface tells the OS which NIC reaches the BMC's Redfish
 * service and at which addresses. There is no Redfish service in QEMU, so
 * this describes the first network controller on bus 0 with fixed
 * link-local addresses (host 169.254.0.2, service 169.254.0.1:443) and the
 * BMC's GUID as the service UUID, as a BMC with a host-side NIC would. It
 * is offered only when a BMC answered on KCS. */
#include "mgmt.h"

static const u8 host_ip[4]    = { 169, 254, 0, 2 };
static const u8 service_ip[4] = { 169, 254, 0, 1 };
static const u8 mask[4]       = { 255, 255, 0, 0 };

int redfish_host_interface(struct redfish_hi *hi)
{
    if (!ipmi_present())
        return 0;
    for (u8 dev = 0; dev < 32; dev++) {
        u32 id = pci_read32(0, dev, 0, 0);
        if ((u16)id == 0xFFFF || pci_read8(0, dev, 0, 0x0B) != 0x02)
            continue;
        memset(hi, 0, sizeof *hi);
        hi->vendor = (u16)id;
        hi->device = (u16)(id >> 16);
        u32 sub = pci_read32(0, dev, 0, 0x2C);
        hi->sub_vendor = (u16)sub;
        hi->sub_device = (u16)(sub >> 16);
        u8 rsp[17];
        if (ipmi_command(IPMI_NETFN_APP, IPMI_GET_DEVICE_GUID, NULL, 0, rsp, sizeof rsp) == 17 &&
            !rsp[0])
            memcpy(hi->uuid, rsp + 1, 16);
        memcpy(hi->host_ip, host_ip, 4);
        memcpy(hi->service_ip, service_ip, 4);
        memcpy(hi->mask, mask, 4);
        hi->port = 443;
        hi->hostname = "squirrel-bmc";
        return 1;
    }
    return 0;
}
