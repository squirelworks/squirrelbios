/* Smoke-test kernel for SquirrelBIOS ppc64le-powernv: stands in for a
 * little-endian Linux. Checks the hand-off (registers, MSR, device tree,
 * the /ibm,opal node and the reserve map), calls the OPAL-style services,
 * starts the other threads with OPAL_START_CPU, has them come back with
 * OPAL_RETURN_CPU and starts them again, then powers the machine off.
 * Everything it prints goes through OPAL_CONSOLE_WRITE.
 * Original code.
 */
#include "fwlib.h"

#define OPAL_CONSOLE_WRITE              1
#define OPAL_CONSOLE_READ               2
#define OPAL_RTC_READ                   3
#define OPAL_CEC_POWER_DOWN             5
#define OPAL_POLL_EVENTS                10
#define OPAL_CONSOLE_WRITE_BUFFER_SPACE 25
#define OPAL_START_CPU                  41
#define OPAL_QUERY_CPU_STATUS           42
#define OPAL_RETURN_CPU                 69
#define OPAL_CHECK_TOKEN                80

#define MSR_SF (1ull << 63)
#define MSR_HV (1ull << 60)
#define MSR_EE (1ull << 15)
#define MSR_IR (1ull << 5)
#define MSR_DR (1ull << 4)
#define MSR_LE 1ull

int64_t opal_call(uint64_t token, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t get_msr(void);
uint32_t get_pir(void);
extern uint64_t opal_entry_addr, opal_base_addr;
extern char secondary_entry[];

/* OPAL's arguments are big-endian; this kernel is little-endian */
static char line[256];
static int line_len;

static void flush(void)
{
    uint64_t len = fw_bswap64((uint64_t)line_len);
    if (line_len)
        opal_call(OPAL_CONSOLE_WRITE, 0, (uint64_t)(uintptr_t)&len,
                  (uint64_t)(uintptr_t)line, 0);
    line_len = 0;
}

void fw_putc(char c)
{
    line[line_len++] = c;
    if (c == '\n' || line_len == (int)sizeof(line))
        flush();
}

int fw_getc(void)
{
    return -1;
}

static const char *ok(int good)
{
    return good ? "ok" : "bad";
}

static volatile uint32_t started;
static volatile uint32_t seen_pir[64];

void ksecondary(uint32_t pir)
{
    uint32_t n = __atomic_fetch_add(&started, 1, __ATOMIC_SEQ_CST);
    if (n < 64)
        seen_pir[n] = pir;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    opal_call(OPAL_RETURN_CPU, 0, 0, 0, 0);
    for (;;)
        ;       /* OPAL_RETURN_CPU does not come back */
}

static int wait_started(uint32_t want)
{
    for (long i = 0; i < 200000000; i++)
        if (started >= want)
            return 1;
    return 0;
}

static int all_inactive(const uint32_t *pirs, int n)
{
    for (long tries = 0; tries < 1000000; tries++) {
        int inactive = 0;
        for (int i = 0; i < n; i++) {
            uint8_t st = 0xff;
            opal_call(OPAL_QUERY_CPU_STATUS, pirs[i], (uint64_t)(uintptr_t)&st, 0, 0);
            inactive += st == 0;
        }
        if (inactive == n)
            return 1;
    }
    return 0;
}

static int rsv_covers(const void *fdt, uint64_t addr)
{
    const uint8_t *r = (const uint8_t *)fdt + fw_get_be32((const uint8_t *)fdt + 16);
    for (; fw_get_be64(r + 8); r += 16)
        if (addr >= fw_get_be64(r) && addr - fw_get_be64(r) < fw_get_be64(r + 8))
            return 1;
    return 0;
}

void kmain(const void *fdt, uint64_t r4, uint64_t r5, uint64_t r8, uint64_t r9, uint64_t msr)
{
    int dtb = fdt && !fdt_check(fdt);
    int regs = r4 == 0 && r5 == 0;
    int msr_ok = (msr & (MSR_SF | MSR_HV)) == (MSR_SF | MSR_HV) &&
                 !(msr & (MSR_EE | MSR_IR | MSR_DR));
    int le = (get_msr() & MSR_LE) != 0;

    int opal = dtb ? fdt_path(fdt, "/ibm,opal") : -1;
    int len;
    const void *base_p = fdt_prop(fdt, opal, "opal-base-address", &len);
    const void *entry_p = fdt_prop(fdt, opal, "opal-entry-address", 0);
    int opal_dt = opal >= 0 && fdt_compatible(fdt, opal, "ibm,opal-v3") && base_p && entry_p &&
                  len == 8 && fw_get_be64(base_p) == r8 && fw_get_be64(entry_p) == r9;
    opal_base_addr = r8;
    opal_entry_addr = r9;
    int con = dtb && fdt_compatible(fdt, fdt_path(fdt, "/ibm,opal/consoles/serial@0"),
                                    "ibm,opal-console-raw");
    int rsv = dtb && rsv_covers(fdt, r8);

    fw_printf("OS-ENTRY ppc64 dtb=%s regs=%s msr=%s le=%s opal-dt=%s console=%s rsv=%s\n",
              ok(dtb), ok(regs), ok(msr_ok), ok(le), ok(opal_dt), ok(con), ok(rsv));

    /* Services */
    int token = opal_call(OPAL_CHECK_TOKEN, OPAL_CONSOLE_WRITE, 0, 0, 0) == 1 &&
                opal_call(OPAL_CHECK_TOKEN, OPAL_START_CPU, 0, 0, 0) == 1 &&
                opal_call(OPAL_CHECK_TOKEN, 0x7fff, 0, 0, 0) == 0 &&
                opal_call(0x7fff, 0, 0, 0, 0) == -1;
    uint32_t ymd = 0;
    uint64_t hms = 0;
    int rtc = opal_call(OPAL_RTC_READ, (uint64_t)(uintptr_t)&ymd, (uint64_t)(uintptr_t)&hms,
                        0, 0) == 0;
    ymd = fw_bswap32(ymd);
    hms = fw_bswap64(hms);
    rtc = rtc && (ymd >> 16) >= 0x2000 && (ymd >> 16) < 0x2100 && ((ymd >> 8) & 0xff) >= 1 &&
          ((ymd >> 8) & 0xff) <= 0x12 && (hms >> 56) <= 0x23;
    uint64_t events = ~0ull;
    int poll = opal_call(OPAL_POLL_EVENTS, (uint64_t)(uintptr_t)&events, 0, 0, 0) == 0 &&
               events == 0;
    uint64_t space = 0;
    int sp = opal_call(OPAL_CONSOLE_WRITE_BUFFER_SPACE, 0, (uint64_t)(uintptr_t)&space, 0, 0) == 0 &&
             fw_bswap64(space) > 0;
    uint64_t rlen = fw_bswap64(16);
    char rbuf[16];
    int rd = opal_call(OPAL_CONSOLE_READ, 0, (uint64_t)(uintptr_t)&rlen,
                       (uint64_t)(uintptr_t)rbuf, 0) == 0 && rlen == 0;
    int bad_term = opal_call(OPAL_CONSOLE_WRITE, 5, (uint64_t)(uintptr_t)&rlen,
                             (uint64_t)(uintptr_t)rbuf, 0) == -1;
    fw_printf("OPAL token=%s rtc=%s poll=%s space=%s read=%s term=%s\n", ok(token), ok(rtc),
              ok(poll), ok(sp), ok(rd), ok(bad_term));

    /* Other threads: every server number in the device tree but ours */
    uint32_t pirs[64] = { 0 };
    int n = 0, total = 0;
    uint32_t me = get_pir();
    for (int c = fdt_find_device_type(fdt, -1, "cpu"); c >= 0;
         c = fdt_find_device_type(fdt, c, "cpu")) {
        const uint8_t *s = fdt_prop(fdt, c, "ibm,ppc-interrupt-server#s", &len);
        for (int i = 0; s && i + 4 <= len; i += 4) {
            total++;
            if (fw_get_be32(s + i) != me && n < 64)
                pirs[n++] = fw_get_be32(s + i);
        }
    }
    int smp = all_inactive(pirs, n);
    int wrong = n ? opal_call(OPAL_START_CPU, me, (uint64_t)(uintptr_t)secondary_entry, 0, 0) == -14
                  : 1;     /* ourselves: OPAL_WRONG_STATE */
    for (int i = 0; i < n; i++)
        smp &= opal_call(OPAL_START_CPU, pirs[i], (uint64_t)(uintptr_t)secondary_entry, 0, 0) == 0;
    smp &= wait_started((uint32_t)n);
    /* They called OPAL_RETURN_CPU: start them once more */
    int again = all_inactive(pirs, n);
    for (int i = 0; i < n; i++)
        again &= opal_call(OPAL_START_CPU, pirs[i], (uint64_t)(uintptr_t)secondary_entry, 0, 0) == 0;
    again &= wait_started(2u * (uint32_t)n) && all_inactive(pirs, n);
    int pir_ok = 1;
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < 2 * n && j < 64; j++)
            found |= seen_pir[j] == pirs[i];
        pir_ok &= found;
    }
    fw_printf("SMP cpus=%d started=%u smp=%s return=%s pir=%s self=%s\n", total, started,
              ok(smp), ok(again), ok(pir_ok), ok(wrong));

    fw_printf("OS-DONE ppc64, powering off\n");
    opal_call(OPAL_CEC_POWER_DOWN, 0, 0, 0, 0);
    for (;;)
        opal_call(OPAL_POLL_EVENTS, 0, 0, 0, 0);
}
