/* Smoke-test kernel for SquirrelBIOS sparc64-sun4u: stands in for an
 * IEEE 1275 client such as Linux. Checks the entry state, then talks
 * to the firmware only through the client interface: finds devices,
 * walks the tree, reads properties, follows an alias, uses the stdout
 * instance, the MMU's translate method, claim and milliseconds, and
 * returns with exit. Original code. */
#include "fwlib.h"

uint64_t get_pstate(void), get_tl(void), get_tba(void), get_lsu(void);
uint64_t cif_keeps_globals(void *cif, uint64_t *args);
uint64_t dtlb_data(uint64_t i), dtlb_tag(uint64_t i);
extern uint8_t hdrs[];

static int64_t (*cif_fn)(uint64_t *);
static uint64_t stdout_ih;

/* cif("name", nargs, nret, args..., &ret0): returns the handler's status */
static int64_t cif(const char *svc, int nargs, int nret, const uint64_t *in, uint64_t *out)
{
    uint64_t a[16];
    a[0] = (uint64_t)(uintptr_t)svc;
    a[1] = (uint64_t)nargs;
    a[2] = (uint64_t)nret;
    for (int i = 0; i < nargs; i++)
        a[3 + i] = in[i];
    for (int i = 0; i < nret; i++)
        a[3 + nargs + i] = 0xdeadbeef;
    int64_t st = cif_fn(a);
    for (int i = 0; i < nret; i++)
        out[i] = a[3 + nargs + i];
    return st;
}

static uint64_t cif1(const char *svc, int nargs, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t in[4] = { a0, a1, a2, a3 }, out[1] = { 0 };
    cif(svc, nargs, 1, in, out);
    return out[0];
}

#define U(p) ((uint64_t)(uintptr_t)(p))

static char line[160];
static int line_len;

void fw_putc(char c)
{
    line[line_len++] = c;
    if (c == '\n' || line_len == (int)sizeof(line)) {
        cif1("write", 3, stdout_ih, U(line), (uint64_t)line_len, 0);
        line_len = 0;
    }
}

int fw_getc(void)
{
    return -1;
}

static const char *ok(int good)
{
    return good ? "ok" : "bad";
}

static int nodes, props;

static void walk(uint64_t ph, int depth)
{
    char name[32] = "";
    nodes++;
    for (;;) {
        char next[32];
        if ((int64_t)cif1("nextprop", 3, ph, U(name), U(next), 0) != 1)
            break;
        props++;
        memcpy(name, next, sizeof(name));
    }
    if (depth < 8)
        for (uint64_t c = cif1("child", 1, ph, 0, 0, 0); c && c != (uint64_t)-1;
             c = cif1("peer", 1, c, 0, 0, 0))
            walk(c, depth + 1);
}

static uint64_t ld_phys(uint64_t a)
{
    uint64_t v;
    __asm__ volatile("ldxa [%1] 0x15, %0" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static void st_phys(uint64_t a, uint64_t v)
{
    __asm__ volatile("stxa %0, [%1] 0x15\n\tmembar #Sync" : : "r"(v), "r"(a) : "memory");
}

static void st_phys8(uint64_t a, uint8_t v)
{
    __asm__ volatile("stba %0, [%1] 0x15\n\tmembar #Sync" : : "r"(v), "r"(a) : "memory");
}

/* The CPU's interrupt receive register (busy bit 0x20) and the first
 * word of the incoming interrupt vector */
static uint64_t intr_status(void)
{
    uint64_t v;
    __asm__ volatile("ldxa [%%g0] 0x49, %0" : "=r"(v) : : "memory");
    return v;
}

static uint64_t intr_data0(void)
{
    uint64_t v, a = 0x40;
    __asm__ volatile("ldxa [%1] 0x7f, %0" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static void intr_ack(void)
{
    __asm__ volatile("stxa %%g0, [%%g0] 0x49\n\tmembar #Sync" : : : "memory");
}

static int deep(int n)
{
    volatile int x = n;
    return n ? deep(n - 1) + x : 0;
}

void kmain(uint64_t cif_entry, uint64_t initrd, uint64_t initrd_size)
{
    cif_fn = (int64_t (*)(uint64_t *))(uintptr_t)cif_entry;
    int entry_ok = cif_entry >= 0xf0000000 && cif_entry < 0xf0400000;
    int state = (get_pstate() & 0x04) && !(get_pstate() & 0x02) && get_tl() == 0;
    int mmu = (get_lsu() & 0xc) == 0xc;
    int tba = get_tba() >= 0xf0000000 && get_tba() < 0xf0400000;

    uint64_t chosen = cif1("finddevice", 1, U("/chosen"), 0, 0, 0);
    uint32_t ih = 0;
    cif1("getprop", 4, chosen, U("stdout"), U(&ih), 4);
    stdout_ih = ih;
    fw_printf("OS-ENTRY sparc64 cif=%s state=%s mmu=%s tba=%s windows=%s\n", ok(entry_ok),
              ok(state), ok(mmu), ok(tba), ok(deep(60) == 1830));

    int test = cif1("test", 1, U("getprop"), 0, 0, 0) == 0 &&
               cif1("test", 1, U("no-such-service"), 0, 0, 0) != 0;
    uint64_t a[1] = { 0 }, o[1];
    int unknown = cif("no-such-service", 0, 1, a, o) == -1;

    /* /memory: reg, and available leaves out the kernel and the firmware */
    uint64_t mem = cif1("finddevice", 1, U("/memory"), 0, 0, 0);
    uint64_t reg[2] = { 0 }, avail[32] = { 0 };
    int rl = (int)cif1("getprop", 4, mem, U("reg"), U(reg), sizeof(reg));
    int al = (int)cif1("getprop", 4, mem, U("available"), U(avail), sizeof(avail));
    int mem_ok = rl == 16 && reg[0] == 0 && reg[1] >= (64u << 20) && al >= 16 &&
                 (int)cif1("getproplen", 2, mem, U("reg"), 0, 0) == 16;
    for (int i = 0; i + 1 < al / 8; i += 2)
        if ((0x404000 >= avail[i] && 0x404000 < avail[i] + avail[i + 1]) ||
            avail[i] + avail[i + 1] > reg[1])
            mem_ok = 0;

    nodes = props = 0;
    walk(cif1("peer", 1, 0, 0, 0, 0), 0);
    int missing = (int64_t)cif1("finddevice", 1, U("/no/such/node"), 0, 0, 0) == -1 &&
                  (int64_t)cif1("getproplen", 2, chosen, U("no-such-prop"), 0, 0) == -1;

    char path[64] = "";
    uint64_t tty = cif1("finddevice", 1, U("ttya"), 0, 0, 0);
    int plen = (int)cif1("package-to-path", 3, tty, U(path), sizeof(path), 0);
    /* ttya: the 16550 on the EBus, behind a Simba bridge of the PBM */
    uint64_t ebus = cif1("parent", 1, tty, 0, 0, 0);
    uint64_t simba = cif1("parent", 1, ebus, 0, 0, 0);
    uint64_t pbm = cif1("parent", 1, simba, 0, 0, 0);
    int alias = plen == 35 && !strcmp(path, "/pci@1fe,0/pci@1,1/ebus@1/su@14,3f8") &&
                cif1("instance-to-package", 1, stdout_ih, 0, 0, 0) == tty &&
                cif1("parent", 1, pbm, 0, 0, 0) == cif1("peer", 1, 0, 0, 0, 0);
    char dtype[16] = "";
    cif1("getprop", 4, tty, U("device_type"), U(dtype), sizeof(dtype));
    alias &= !strcmp(dtype, "serial");

    fw_printf("CIF test=%s unknown=%s mem=%s walk=%s nodes=%d missing=%s alias=%s\n", ok(test),
              ok(unknown), ok(mem_ok), ok(nodes >= 9 && props >= 30), nodes, ok(missing),
              ok(alias));

    /* MMU: translate the kernel's own address; the firmware's mappings
     * are in /virtual-memory translations */
    uint32_t mmu_ih = 0;
    cif1("getprop", 4, chosen, U("mmu"), U(&mmu_ih), 4);
    uint64_t tin[3] = { U("translate"), mmu_ih, 0x404000 }, tout[5] = { 0 };
    cif("call-method", 3, 5, tin, tout);
    int translate = tout[0] == 0 && tout[1] == (uint64_t)-1 &&
                    (tout[3] << 32 | tout[4]) == 0x404000;
    uint64_t vm = cif1("finddevice", 1, U("/virtual-memory"), 0, 0, 0);
    uint64_t tr[3 * 16] = { 0 };
    int tl = (int)cif1("getprop", 4, vm, U("translations"), U(tr), sizeof(tr));
    int fw_mapped = 0;
    for (int i = 0; i < tl / 24; i++)
        fw_mapped |= tr[3 * i] == 0xf0000000 && tr[3 * i + 1] == 0x400000 &&
                     (tr[3 * i + 2] >> 63);
    uint64_t bad[3] = { U("no-such-method"), mmu_ih, 0 }, bo[1];
    cif("call-method", 3, 1, bad, bo);
    int method_fail = bo[0] != 0;

    /* claim: an aligned block, used, then the same range again is refused */
    uint64_t blk = cif1("claim", 3, 0, 0x10000, 0x10000, 0);
    int claim = blk != (uint64_t)-1 && blk && !(blk & 0xffff);
    if (claim) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)blk;
        p[0] = 0x5153;
        p[0x1fff] = 0x2042;
        claim = p[0] == 0x5153 && p[0x1fff] == 0x2042 &&
                cif1("claim", 3, blk, 0x10000, 0, 0) == (uint64_t)-1;
    }
    int64_t t0 = (int64_t)cif1("milliseconds", 0, 0, 0, 0, 0);
    int64_t t1 = t0;
    for (long i = 0; i < 400000000 && t1 < t0 + 20; i++)
        t1 = (int64_t)cif1("milliseconds", 0, 0, 0, 0, 0);
    uint64_t iin[1] = { U("1 2 + .") }, iout[1];
    cif("interpret", 1, 1, iin, iout);

    fw_printf("MMU translate=%s translations=%s method=%s claim=%s ms=%s interpret=%s\n",
              ok(translate), ok(fw_mapped), ok(method_fail), ok(claim), ok(t1 >= t0 + 20),
              ok(iout[0] != 0));
    /* What Linux needs besides: the client's %g1-%g7 survive a call */
    uint64_t gargs[5] = { U("test"), 1, 1, U("getprop"), 0 };
    uint64_t gbad = cif_keeps_globals((void *)(uintptr_t)cif_entry, gargs);

    /* The 1:1 RAM mappings are not locked (they would shadow user
     * addresses once an OS runs processes); only the firmware's is */
    int unlocked = 1;
    for (uint64_t i = 0; i < 64; i++) {
        uint64_t d = dtlb_data(i), t = dtlb_tag(i) & ~0x1fffull;
        if ((d >> 63) && (d & 0x40) && t < 0xf0000000)
            unlocked = 0;
    }
    /* SUNW,dtlb-load writes the entry the OS names (args: virt, data,
     * index, as Linux passes them) */
    uint64_t tte = (1ull << 63) | (3ull << 61) | 0x1c00000 | 0x40 | 0x20 | 0x10 | 0x4 | 0x2;
    uint64_t lin[5] = { U("SUNW,dtlb-load"), mmu_ih, 0x1c00000, tte, 7 }, lout[1] = { 1 };
    cif("call-method", 5, 1, lin, lout);
    int tlbload = lout[0] == 0 && (dtlb_data(7) & 0x1ffffffe000ull) == 0x1c00000 &&
                  (dtlb_tag(7) & ~0x1fffull) == 0x1c00000;

    /* -initrd: the firmware filled the HdrS header the way Linux reads it
     * (image64 - 0x400000 + lowest available address = the initrd) */
    const char *hd = "none";
    if (initrd_size) {
        uint64_t low = ~0ull;
        for (int i = 0; i + 1 < al / 8; i += 2)
            if (avail[i] < low)
                low = avail[i];
        uint64_t img64 = fw_get_be64(hdrs + 40);
        hd = ok(fw_get_be32(hdrs + 16) == 0 && fw_get_be32(hdrs + 20) == initrd_size &&
                img64 - 0x400000 + low == initrd);
    }
    fw_printf("LINUX globals=%s unlocked=%s tlbload=%s hdrs=%s\n", ok(gbad == 0), ok(unlocked),
              ok(tlbload), hd);

    /* The console's interrupt, the way Linux finds and uses it: su's
     * "interrupts" is an INO of the PBM above it; the PBM's IMAP register
     * for an on-board INO (regs + 0x1000 + (ino & 0x1f) * 8) reads back
     * the group and number the CPU receives. Made valid, the UART's
     * transmitter-empty interrupt arrives as that interrupt vector (with
     * interrupts off in PSTATE it waits in the receive register). */
    uint32_t ino = 0, portid = 0, sreg[3] = { 0 }, er[6] = { 0 }, pr[14] = { 0 };
    uint64_t preg[2] = { 0 };
    char model[16] = "";
    cif1("getprop", 4, tty, U("interrupts"), U(&ino), 4);
    cif1("getprop", 4, tty, U("reg"), U(sreg), sizeof(sreg));
    cif1("getprop", 4, ebus, U("ranges"), U(er), sizeof(er));
    cif1("getprop", 4, pbm, U("ranges"), U(pr), sizeof(pr));
    cif1("getprop", 4, pbm, U("reg"), U(preg), sizeof(preg));
    cif1("getprop", 4, pbm, U("upa-portid"), U(&portid), 4);
    cif1("getprop", 4, pbm, U("model"), U(model), sizeof(model));
    /* reg (space 0x14, port) -> PCI I/O space (EBus ranges) -> physical
     * (the PBM's I/O range) */
    uint64_t uart = ((uint64_t)pr[3] << 32 | pr[4]) + (sreg[1] - er[1]) + er[4];
    int reg_ok = !strcmp(model, "SUNW,sabre") && sreg[0] == er[0] &&
                 (er[2] & 0x03000000) == 0x01000000 && (pr[0] & 0x03000000) == 0x01000000 &&
                 uart == 0x1fe020003f8ull;
    uint64_t imap = preg[0] + 0x1000 + ((ino & 0x1f) << 3);
    uint64_t iclr = preg[0] + 0x1800 + ((ino & 0x1f) << 3);
    uint64_t m = ino >= 0x20 && reg_ok ? ld_phys(imap) : 0;
    int imap_ok = ino >= 0x20 && (m & 0x7ff) == (portid << 6 | ino);
    uint64_t vec = 0;
    int got = 0;
    if (imap_ok) {
        st_phys(iclr, 0);
        st_phys(imap, m | 1ull << 31);              /* valid, to CPU 0 */
        st_phys8(uart + 1, 0x02);                   /* IER: transmitter empty */
        for (long i = 0; i < 1000000 && !got; i++)
            got = (intr_status() & 0x20) != 0;
        vec = intr_data0();
        st_phys8(uart + 1, 0);
        intr_ack();
        st_phys(iclr, 0);
        st_phys(imap, m);
    }
    fw_printf("IRQ console ino=0x%x reg=%s imap=%s vector=%s\n", ino, ok(reg_ok), ok(imap_ok),
              ok(got && vec == (m & 0x7ff)));

    fw_printf("OS-DONE sparc64, exiting to the firmware\n");
    cif("exit", 0, 0, a, o);
    for (;;)
        ;
}
