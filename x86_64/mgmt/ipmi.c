/* SquirrelBIOS x86_64-uefi: a BMC on the IPMI KCS interface (roadmap F3).
 * Original code, written from the IPMI 2.0 specification: the KCS
 * interface (section 9), Get Device ID and Get Device GUID (section 20),
 * and the SPMI table (appendix C3).
 *
 * KCS is two I/O ports: data at 0xCA2 and command/status at 0xCA3. A
 * request is written one byte at a time between WRITE_START and
 * WRITE_END, each byte once the BMC has taken the last one (IBF clear);
 * the response is read one byte at a time, each acknowledged with READ,
 * until the BMC goes idle. QEMU's ipmi-bmc-sim behind isa-ipmi-kcs is
 * such a BMC. Nothing here uses an interrupt. */
#include "mgmt.h"

#define KCS_DATA        IPMI_KCS_BASE
#define KCS_CMD         (IPMI_KCS_BASE + 1)

#define ST_OBF          0x01            /* a byte for the host */
#define ST_IBF          0x02            /* the BMC has not taken ours */
#define ST_STATE(s)     ((s) >> 6)
#define S_IDLE          0
#define S_READ          1
#define S_WRITE         2
#define S_ERROR         3

#define KCS_ABORT       0x60
#define KCS_WRITE_START 0x61
#define KCS_WRITE_END   0x62
#define KCS_READ        0x68

#define SPMI_SIZE       65

static int present;
static u8 version;

static int wait_ibf_clear(void)
{
    u64 end = arch_counter() + arch_counter_hz() / 1000 * 1000;
    while (inb(KCS_CMD) & ST_IBF)
        if (arch_counter() > end)
            return 0;
    return 1;
}

static int wait_obf(void)
{
    u64 end = arch_counter() + arch_counter_hz() / 1000 * 1000;
    while (!(inb(KCS_CMD) & ST_OBF))
        if (arch_counter() > end)
            return 0;
    return 1;
}

static void clear_obf(void)
{
    if (inb(KCS_CMD) & ST_OBF)
        (void)inb(KCS_DATA);
}

/* Section 9.15's error exit: GET_STATUS/ABORT, then read the status
 * code back, which leaves the interface idle */
static void kcs_abort(void)
{
    if (!wait_ibf_clear())
        return;
    outb(KCS_CMD, KCS_ABORT);
    if (!wait_ibf_clear())
        return;
    clear_obf();
    outb(KCS_DATA, 0);
    if (!wait_ibf_clear() || ST_STATE(inb(KCS_CMD)) != S_READ || !wait_obf())
        return;
    (void)inb(KCS_DATA);
    outb(KCS_DATA, KCS_READ);
    wait_ibf_clear();
    clear_obf();
}

static int kcs_write(const u8 *msg, u32 len)
{
    if (!wait_ibf_clear())
        return 0;
    clear_obf();
    outb(KCS_CMD, KCS_WRITE_START);
    for (u32 i = 0; i < len; i++) {
        if (!wait_ibf_clear() || ST_STATE(inb(KCS_CMD)) != S_WRITE)
            return 0;
        clear_obf();
        if (i == len - 1) {
            outb(KCS_CMD, KCS_WRITE_END);
            if (!wait_ibf_clear() || ST_STATE(inb(KCS_CMD)) != S_WRITE)
                return 0;
            clear_obf();
        }
        outb(KCS_DATA, msg[i]);
    }
    return 1;
}

static u32 kcs_read(u8 *buf, u32 cap)
{
    u32 got = 0;
    for (;;) {
        if (!wait_ibf_clear())
            return 0;
        u8 st = ST_STATE(inb(KCS_CMD));
        if (st == S_IDLE) {
            if (wait_obf())
                (void)inb(KCS_DATA);    /* the dummy byte */
            return got;
        }
        if (st != S_READ || !wait_obf())
            return 0;
        u8 b = inb(KCS_DATA);
        if (got < cap)
            buf[got] = b;
        got++;
        if (got > 64)                   /* longer than any KCS message */
            return 0;
        outb(KCS_DATA, KCS_READ);
    }
}

u32 ipmi_command(u8 netfn, u8 cmd, const u8 *data, u32 len, u8 *rsp, u32 cap)
{
    u8 msg[40], in[64];
    if (len > sizeof msg - 2)
        return 0;
    msg[0] = (u8)(netfn << 2);          /* LUN 0 */
    msg[1] = cmd;
    if (len)
        memcpy(msg + 2, data, len);
    u32 n = 0;
    if (kcs_write(msg, len + 2))
        n = kcs_read(in, sizeof in);
    if (n < 3 || n > sizeof in || in[0] != (u8)((netfn | 1) << 2) || in[1] != cmd) {
        kcs_abort();
        return 0;
    }
    n -= 2;                             /* completion code and data */
    if (n > cap)
        n = cap;
    memcpy(rsp, in + 2, n);
    return n;
}

int ipmi_present(void) { return present; }
u8 ipmi_version(void) { return version; }

void ipmi_init(void)
{
    u8 st = inb(KCS_CMD);
    if (st == 0xFF) {                   /* nothing decodes the port */
        printf("ipmi: none\n");
        return;
    }
    if (ST_STATE(st) != S_IDLE)
        kcs_abort();
    u8 id[16];
    u32 n = ipmi_command(IPMI_NETFN_APP, IPMI_GET_DEVICE_ID, NULL, 0, id, sizeof id);
    if (n < 12 || id[0]) {
        printf("ipmi: no answer on KCS at 0x%x\n", KCS_DATA);
        return;
    }
    present = 1;
    /* Get Device ID has the minor digit in bits 7:4 (02h is 2.0); keep it
     * the usual way round, as SMBIOS and SPMI want it */
    version = (u8)((id[5] & 0x0F) << 4 | id[5] >> 4);
    u32 mfr = id[7] | (u32)id[8] << 8 | (u32)(id[9] & 0x0F) << 16;
    u16 product = (u16)(id[10] | id[11] << 8);
    u8 test[3];
    n = ipmi_command(IPMI_NETFN_APP, IPMI_GET_SELFTEST, NULL, 0, test, sizeof test);
    printf("ipmi: BMC on KCS at 0x%x, device 0x%x rev %u, firmware %u.%x, IPMI %u.%u, "
           "manufacturer %u product 0x%x, self test %s\n",
           KCS_DATA, id[1], id[2] & 0x0F, id[3] & 0x7F, id[4], version >> 4, version & 0x0F,
           mfr, product, n < 2 || test[0] ? "unsupported" : test[1] == 0x55 ? "passed" :
           test[1] == 0x56 ? "not implemented" : "failed");
}

/* SPMI (IPMI 2.0 appendix C3, revision 5): the KCS ports as a byte-wide
 * system I/O register, polled, not a PCI device */
int ipmi_acpi_tables(u8 **out, acpi_take_fn take)
{
    if (!present)
        return 0;
    u8 *t = take(SPMI_SIZE);
    if (!t)
        return 0;
    memset(t, 0, SPMI_SIZE);
    acpi_header(t, "SPMI", SPMI_SIZE, 5);
    t[36] = 1;                          /* interface type: KCS */
    t[37] = 1;                          /* reserved, must be 1 */
    t[38] = version & 0x0F;             /* specification revision, BCD */
    t[39] = version >> 4;
    t[40] = 0;                          /* no SCI, no I/O APIC interrupt */
    t[48] = 1;                          /* base address: system I/O */
    t[49] = 8;                          /* registers a byte wide and apart */
    t[50] = 0;
    t[51] = 1;                          /* byte access */
    t[52] = (u8)KCS_DATA;
    t[53] = (u8)(KCS_DATA >> 8);
    t[60] = 1;                          /* UID, as no PCI device */
    acpi_seal(t);
    out[0] = t;
    return 1;
}
