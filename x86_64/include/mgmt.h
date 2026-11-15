/* SquirrelBIOS x86_64-uefi: platform management (roadmap F3). Original
 * code. A BMC on the IPMI KCS interface, described to the OS by SMBIOS
 * type 38 and the ACPI SPMI table, and a Redfish host interface (SMBIOS
 * type 42) to reach it over a NIC. */
#ifndef MGMT_H
#define MGMT_H

#include "acpi.h"

#define IPMI_KCS_BASE   0x0CA2          /* data; command/status at +1 */

#define IPMI_NETFN_APP  0x06
#define IPMI_GET_DEVICE_ID      0x01
#define IPMI_GET_SELFTEST       0x04
#define IPMI_GET_DEVICE_GUID    0x08

/* ipmi.c */
void ipmi_init(void);                   /* before acpi_init and smbios_init */
int ipmi_present(void);
u8 ipmi_version(void);                  /* BCD, major digit high: 0x20 for 2.0 */
/* One request: rsp[] gets the completion code, then the data. Returns
 * the number of bytes in rsp (at least 1), or 0 if the BMC did not answer. */
u32 ipmi_command(u8 netfn, u8 cmd, const u8 *data, u32 len, u8 *rsp, u32 cap);
int ipmi_acpi_tables(u8 **out, acpi_take_fn take);     /* SPMI */

/* redfish.c: the host interface record, if there is a BMC and a NIC */
struct redfish_hi {
    u16 vendor, device, sub_vendor, sub_device;     /* the NIC */
    u8 uuid[16];                        /* the service's: the BMC's GUID */
    u8 host_ip[4], service_ip[4], mask[4];
    u16 port;
    const char *hostname;
};
int redfish_host_interface(struct redfish_hi *hi);

#endif
