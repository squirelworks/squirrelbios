/* SquirrelBIOS x86_64-uefi: application processor bring-up and the MP
 * services protocol (roadmap C4). Original code, written from the Intel
 * SDM (MP initialization: INIT-SIPI-SIPI) and the UEFI PI specification's
 * description of EFI_MP_SERVICES_PROTOCOL.
 *
 * The BSP copies a trampoline into a page below 1 MB and broadcasts
 * INIT, STARTUP, STARTUP to all other processors. Each AP starts in real
 * mode at that page, loads the trampoline's GDT, switches to protected
 * mode, takes the BSP's page tables, enters long mode, claims a slot and
 * a stack and calls ap_main, where it parks: it spins on its mailbox
 * until StartupAllAPs or StartupThisAP gives it a procedure. When boot
 * services end, the APs get an INIT and wait for the OS's STARTUP.
 *
 * Processor numbers: 0 is the BSP, the APs follow in APIC ID order.
 * Only blocking calls are supported (WaitEvent must be NULL); SwitchBSP
 * answers EFI_UNSUPPORTED. */
#include "smp.h"

#define STACK_SIZE   0x4000
#define WAIT_US      2000000         /* for every AP to arrive */
#define FWCFG_NB_CPUS 0x05

#ifndef CONFIG_X2APIC
#define CONFIG_X2APIC 0                 /* 0: only when a CPU needs it */
#endif

/* ---------------------------------------------------------------------
 * The trampoline. Copied to a page-aligned address B below 1 MB; an AP
 * gets STARTUP vector B >> 12 and starts at CS = B >> 4, IP = 0. EBX
 * holds B all the way to long mode, so the code addresses its data as
 * B + offset; the BSP fills the data at the end before the IPIs.
 */
__asm__(
    "    .section .text.smp_tramp, \"ax\"\n"
    "    .globl smp_tramp_start, smp_tramp_data, smp_tramp_end\n"
    "    .code16\n"
    "smp_tramp_start:\n"
    "    cli\n"
    "    cld\n"
    "    movw %cs, %ax\n"
    "    movw %ax, %ds\n"
    "    xorl %ebx, %ebx\n"
    "    movw %cs, %bx\n"
    "    shll $4, %ebx\n"
    "    leal (t_pm32 - smp_tramp_start)(%ebx), %eax\n"
    "    movl %eax, (t_far32 - smp_tramp_start)\n"
    "    leal (t_gdt - smp_tramp_start)(%ebx), %eax\n"
    "    movl %eax, (t_gdtr + 2 - smp_tramp_start)\n"
    "    lgdtl (t_gdtr - smp_tramp_start)\n"
    "    movl %cr0, %eax\n"
    "    orl $1, %eax\n"
    "    movl %eax, %cr0\n"
    "    ljmpl *(t_far32 - smp_tramp_start)\n"
    "    .code32\n"
    "t_pm32:\n"
    "    movw $0x10, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %ss\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    movl %cr4, %eax\n"
    "    orl $0x620, %eax\n"                /* PAE, OSFXSR, OSXMMEXCPT */
    "    movl %eax, %cr4\n"
    "    movl (t_cr3 - smp_tramp_start)(%ebx), %eax\n"
    "    movl %eax, %cr3\n"
    "    movl $0xC0000080, %ecx\n"          /* EFER.LME */
    "    rdmsr\n"
    "    orl $0x100, %eax\n"
    "    wrmsr\n"
    "    movl %cr0, %eax\n"
    "    andl $0xFFFFFFFB, %eax\n"          /* EM off */
    "    orl $0x80000002, %eax\n"           /* PG, MP */
    "    movl %eax, %cr0\n"
    "    leal (t_lm64 - smp_tramp_start)(%ebx), %eax\n"
    "    movl %eax, (t_far64 - smp_tramp_start)(%ebx)\n"
    "    ljmpl *(t_far64 - smp_tramp_start)(%ebx)\n"
    "    .code64\n"
    "t_lm64:\n"
    "    xorl %eax, %eax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %ss\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    movl $1, %eax\n"
    "    lock xaddl %eax, (t_next - smp_tramp_start)(%rbx)\n"
    "    cmpl (t_max - smp_tramp_start)(%rbx), %eax\n"
    "    ja 1f\n"                           /* more APs than slots: park */
    "    movl %eax, %edi\n"
    "    movq (t_stack_size - smp_tramp_start)(%rbx), %rax\n"
    "    imulq %rdi, %rax\n"
    "    addq (t_stacks - smp_tramp_start)(%rbx), %rax\n"
    "    movq %rax, %rsp\n"
    "    movq (t_entry - smp_tramp_start)(%rbx), %rax\n"
    "    callq *%rax\n"
    "1:  cli\n"
    "    hlt\n"
    "    jmp 1b\n"
    "    .balign 8\n"
    "t_gdt:\n"
    "    .quad 0\n"
    "    .quad 0x00CF9A000000FFFF\n"        /* 08h: 32-bit code */
    "    .quad 0x00CF92000000FFFF\n"        /* 10h: data */
    "    .quad 0x00AF9A000000FFFF\n"        /* 18h: 64-bit code */
    "t_gdtr:\n"
    "    .word 31\n"
    "    .long 0\n"
    "t_far32:\n"
    "    .long 0\n"
    "    .word 0x08\n"
    "t_far64:\n"
    "    .long 0\n"
    "    .word 0x18\n"
    "    .balign 8\n"
    "smp_tramp_data:\n"
    "t_cr3:        .quad 0\n"
    "t_stacks:     .quad 0\n"
    "t_stack_size: .quad 0\n"
    "t_entry:      .quad 0\n"
    "t_next:       .long 0\n"
    "t_max:        .long 0\n"
    "smp_tramp_end:\n"
    "    .text\n");

extern char smp_tramp_start[], smp_tramp_data[], smp_tramp_end[];

struct tramp_data {
    u64 cr3;
    u64 stacks;                 /* slot n's stack top is stacks + n * size */
    u64 stack_size;
    u64 entry;
    u32 next;                   /* the slot the next AP takes */
    u32 max;                    /* the last slot with a stack */
};

/* --------------------------------------------------------------------- */

enum { AP_DOWN, AP_IDLE, AP_BUSY };

struct cpu {
    struct smp_cpu_info info;
    volatile u32 state;
    volatile EFI_AP_PROCEDURE proc;
    void *volatile arg;
};

static struct cpu cpus[SMP_MAX_CPUS];  /* by slot: 0 the BSP, APs by arrival */
static u16 order[SMP_MAX_CPUS];        /* processor number -> slot */
static int ncpus;
static int x2mode;
static volatile struct tramp_data *td;
static u32 tramp_page;

const EFI_GUID efi_mp_services_guid = EFI_MP_SERVICES_PROTOCOL_GUID;

int smp_cpu_count(void) { return ncpus; }
int smp_x2apic(void) { return x2mode; }
const struct smp_cpu_info *smp_cpu(int n)
{
    return n >= 0 && n < ncpus ? &cpus[order[n]].info : NULL;
}

/* Package, core and thread from the APIC ID and CPUID leaf 0Bh (the
 * x2APIC topology leaf), when the CPU has it */
static void topology(struct smp_cpu_info *ci)
{
    u32 a, b, c, d, max;
    u32 smt_shift = 0, pkg_shift = 0;
    cpuid(0, &max, &b, &c, &d);
    if (max >= 0x0B) {
        for (u32 level = 0; level < 8; level++) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x0B), "c"(level));
            u32 type = (c >> 8) & 0xFF;
            if (!type || !(b & 0xFFFF))
                break;
            if (type == 1)
                smt_shift = a & 0x1F;
            pkg_shift = a & 0x1F;       /* the last level's shift */
            ci->apic_id = d;            /* the full 32-bit x2APIC ID */
        }
    }
    u32 id = ci->apic_id;
    ci->thread = id & ((1u << smt_shift) - 1);
    ci->core = (id & ((1u << pkg_shift) - 1)) >> smt_shift;
    ci->package = pkg_shift ? id >> pkg_shift : id;
}

/* Restarts: the AP comes up in a spare slot and moves to its own */
static u32 spare_slot, restart_slot;
static volatile u32 restart_id;
static u64 stack_base;

static void __attribute__((noreturn)) ap_loop(u32 slot)
{
    struct cpu *c = &cpus[slot];
    c->info.apic_id = apic_id();
    topology(&c->info);
    __atomic_store_n(&c->state, AP_IDLE, __ATOMIC_RELEASE);
    for (;;) {
        while (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != AP_BUSY)
            cpu_pause();
        c->proc(c->arg);
        __atomic_store_n(&c->state, AP_IDLE, __ATOMIC_RELEASE);
    }
}

static void __attribute__((noreturn)) park(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void ap_main(u32 slot)
{
    apic_enable(x2mode);
    if (slot != spare_slot)
        ap_loop(slot);
    /* A restart: only the AP being restarted goes on, on its own stack;
     * one from the first broadcast that turns up late stops here */
    if (apic_id() != restart_id)
        park();
    u64 top = stack_base + (u64)restart_slot * STACK_SIZE;
    __asm__ volatile("mov %0, %%rsp\n"
                     "call *%1\n"
                     :: "r"(top), "r"(ap_loop), "D"(restart_slot) : "memory");
    park();
}

static u32 expected_cpus(void)
{
    u16 n = 0;
    if (fwcfg_present())
        fwcfg_read(FWCFG_NB_CPUS, &n, 2);
    return n ? n : SMP_MAX_CPUS;
}

static void init_sipi_sipi(int all, u32 dest)
{
    u32 vec = tramp_page >> 12;
    apic_send_ipi(dest, 0x4500, all);            /* INIT, assert */
    efi_stall(10000);
    apic_send_ipi(dest, 0x4600 | vec, all);      /* STARTUP */
    efi_stall(200);
    apic_send_ipi(dest, 0x4600 | vec, all);
}

/* An AP that ran past its timeout: INIT it and start it again in its own
 * slot, so it is parked and ready for the next call */
static void restart_ap(int slot)
{
    struct cpu *c = &cpus[slot];
    c->state = AP_DOWN;
    restart_slot = slot;
    restart_id = c->info.apic_id;
    td->next = spare_slot;              /* one slot open: the spare */
    td->max = spare_slot;
    init_sipi_sipi(0, c->info.apic_id);
    for (int n = 0; n < 100000 && c->state == AP_DOWN; n++)
        efi_stall(10);
    td->max = 0;
    if (c->state == AP_DOWN)
        c->info.enabled = 0;
}

/* INIT every AP: they wait for the OS's STARTUP, out of the memory the
 * OS is about to take over. Called when boot services end and before
 * the CSM leaves long mode. */
void smp_park_aps(void)
{
    if (ncpus <= 1)
        return;
    apic_send_ipi(0, 0x4500, 1);
    for (int n = 1; n < ncpus; n++)
        cpus[order[n]].state = AP_DOWN;
}

static void EFIAPI park_on_exit(EFI_EVENT e, void *ctx)
{
    (void)e, (void)ctx;
    smp_park_aps();
}

/* ---------------------------------------------------------------------
 * EFI_MP_SERVICES_PROTOCOL */

static int self_number(void)
{
    u32 id = apic_id();
    for (int n = 0; n < ncpus; n++)
        if (cpus[order[n]].info.apic_id == id)
            return n;
    return -1;
}

static int on_bsp(void) { return self_number() == 0; }

/* Microseconds from the TSC. A TSC that measured below 1 MHz (a failed
 * calibration) counts one tick as a microsecond: timeouts get longer,
 * never a division by zero. */
static u64 now_us(void)
{
    u64 per_us = efi_counter_hz() / 1000000;
    return rdtsc() / (per_us ? per_us : 1);
}

static EFI_STATUS EFIAPI get_number_of_processors(EFI_MP_SERVICES_PROTOCOL *this, UINTN *total, UINTN *enabled)
{
    (void)this;
    if (!total || !enabled)
        return EFI_INVALID_PARAMETER;
    if (!on_bsp())
        return EFI_DEVICE_ERROR;
    UINTN e = 0;
    for (int n = 0; n < ncpus; n++)
        e += cpus[order[n]].info.enabled != 0;
    *total = ncpus;
    *enabled = e;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_processor_info(EFI_MP_SERVICES_PROTOCOL *this, UINTN n, EFI_PROCESSOR_INFORMATION *out)
{
    (void)this;
    if (!out)
        return EFI_INVALID_PARAMETER;
    if (!on_bsp())
        return EFI_DEVICE_ERROR;
    n &= 0xFFFFFF;                      /* bit 24 asks for the extended topology */
    if (n >= (UINTN)ncpus)
        return EFI_NOT_FOUND;
    const struct smp_cpu_info *ci = &cpus[order[n]].info;
    out->processor_id = ci->apic_id;
    out->status_flag = PROCESSOR_HEALTH_STATUS_BIT | (ci->enabled ? PROCESSOR_ENABLED_BIT : 0) |
                       (n == 0 ? PROCESSOR_AS_BSP_BIT : 0);
    out->location.package = ci->package;
    out->location.core = ci->core;
    out->location.thread = ci->thread;
    return EFI_SUCCESS;
}

static void dispatch(struct cpu *c, EFI_AP_PROCEDURE proc, void *arg)
{
    c->proc = proc;
    c->arg = arg;
    __atomic_store_n(&c->state, AP_BUSY, __ATOMIC_RELEASE);
}

/* Wait for slot s until the deadline (0: none); 1 when it finished */
static int wait_ap(int s, u64 deadline)
{
    while (__atomic_load_n(&cpus[s].state, __ATOMIC_ACQUIRE) == AP_BUSY) {
        if (deadline && now_us() >= deadline)
            return 0;
        cpu_pause();
    }
    return 1;
}

static EFI_STATUS EFIAPI startup_all_aps(EFI_MP_SERVICES_PROTOCOL *this, EFI_AP_PROCEDURE proc,
                                         BOOLEAN single, EFI_EVENT wait, UINTN timeout_us,
                                         void *arg, UINTN **failed)
{
    (void)this;
    if (failed)
        *failed = NULL;
    if (!on_bsp())
        return EFI_DEVICE_ERROR;
    if (!proc)
        return EFI_INVALID_PARAMETER;
    if (wait)
        return EFI_UNSUPPORTED;         /* non-blocking mode */
    int any = 0;
    for (int n = 1; n < ncpus; n++) {
        struct cpu *c = &cpus[order[n]];
        if (!c->info.enabled)
            continue;
        if (c->state != AP_IDLE)
            return EFI_NOT_READY;
        any = 1;
    }
    if (!any)
        return EFI_NOT_STARTED;

    u64 deadline = timeout_us ? now_us() + timeout_us : 0;
    static UINTN late[SMP_MAX_CPUS + 1];
    int nlate = 0;
    if (single) {
        for (int n = 1; n < ncpus; n++) {
            int s = order[n];
            if (!cpus[s].info.enabled)
                continue;
            if (deadline && now_us() >= deadline) {
                late[nlate++] = n;      /* never started */
                continue;
            }
            dispatch(&cpus[s], proc, arg);
            if (!wait_ap(s, deadline)) {
                late[nlate++] = n;
                restart_ap(s);
            }
        }
    } else {
        for (int n = 1; n < ncpus; n++)
            if (cpus[order[n]].info.enabled)
                dispatch(&cpus[order[n]], proc, arg);
        for (int n = 1; n < ncpus; n++) {
            int s = order[n];
            if (!cpus[s].info.enabled)
                continue;
            if (!wait_ap(s, deadline)) {
                late[nlate++] = n;
                restart_ap(s);
            }
        }
    }
    if (!nlate)
        return EFI_SUCCESS;
    late[nlate] = END_OF_CPU_LIST;
    if (failed && efi_allocate_pool(EfiBootServicesData, (nlate + 1) * sizeof(UINTN),
                                    (void **)failed) == EFI_SUCCESS)
        memcpy(*failed, late, (nlate + 1) * sizeof(UINTN));
    return EFI_TIMEOUT;
}

static EFI_STATUS EFIAPI startup_this_ap(EFI_MP_SERVICES_PROTOCOL *this, EFI_AP_PROCEDURE proc,
                                         UINTN n, EFI_EVENT wait, UINTN timeout_us, void *arg,
                                         BOOLEAN *finished)
{
    (void)this;
    if (finished)
        *finished = 0;
    if (!on_bsp())
        return EFI_DEVICE_ERROR;
    if (!proc)
        return EFI_INVALID_PARAMETER;
    if (n >= (UINTN)ncpus)
        return EFI_NOT_FOUND;
    if (n == 0 || !cpus[order[n]].info.enabled)
        return EFI_INVALID_PARAMETER;
    if (wait)
        return EFI_UNSUPPORTED;
    int s = order[n];
    if (cpus[s].state != AP_IDLE)
        return EFI_NOT_READY;
    dispatch(&cpus[s], proc, arg);
    if (!wait_ap(s, timeout_us ? now_us() + timeout_us : 0)) {
        restart_ap(s);
        return EFI_TIMEOUT;
    }
    if (finished)
        *finished = 1;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI switch_bsp(EFI_MP_SERVICES_PROTOCOL *this, UINTN n, BOOLEAN keep_old)
{
    (void)this, (void)n, (void)keep_old;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI enable_disable_ap(EFI_MP_SERVICES_PROTOCOL *this, UINTN n, BOOLEAN enable, u32 *health)
{
    (void)this, (void)health;
    if (!on_bsp())
        return EFI_DEVICE_ERROR;
    if (n >= (UINTN)ncpus)
        return EFI_NOT_FOUND;
    if (n == 0)
        return EFI_INVALID_PARAMETER;
    struct cpu *c = &cpus[order[n]];
    if (c->state == AP_DOWN)
        return EFI_UNSUPPORTED;
    c->info.enabled = enable != 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI who_am_i(EFI_MP_SERVICES_PROTOCOL *this, UINTN *n)
{
    (void)this;
    if (!n)
        return EFI_INVALID_PARAMETER;
    int me = self_number();
    if (me < 0)
        return EFI_NOT_FOUND;
    *n = me;
    return EFI_SUCCESS;
}

static EFI_MP_SERVICES_PROTOCOL mp = {
    get_number_of_processors, get_processor_info, startup_all_aps, startup_this_ap,
    switch_bsp, enable_disable_ap, who_am_i,
};

/* --------------------------------------------------------------------- */

/* Number the processors that came up: the BSP, then by APIC ID */
static void sort_order(int slots)
{
    ncpus = 0;
    for (int s = 0; s < slots; s++)
        if (cpus[s].state != AP_DOWN)
            order[ncpus++] = s;
    for (int i = 2; i < ncpus; i++) {           /* the BSP stays first */
        u16 s = order[i];
        int j = i;
        for (; j > 1 && cpus[order[j - 1]].info.apic_id > cpus[s].info.apic_id; j--)
            order[j] = order[j - 1];
        order[j] = s;
    }
}

/* Run fn on the BSP, then on every enabled AP, and wait for them */
void smp_run_all(EFI_AP_PROCEDURE fn, void *arg)
{
    fn(arg);
    if (ncpus > 1)
        startup_all_aps(&mp, fn, 0, NULL, 0, arg, NULL);
}

void smp_init(void)
{
    u32 want = expected_cpus();
    if (want > SMP_MAX_CPUS)
        want = SMP_MAX_CPUS;
    x2mode = apic_has_x2apic() && (CONFIG_X2APIC || want > 255);
    apic_enable(x2mode);

    struct cpu *bsp = &cpus[0];
    bsp->info.apic_id = apic_id();
    bsp->info.enabled = 1;
    topology(&bsp->info);
    bsp->state = AP_BUSY;               /* never dispatched to */

    /* Trampoline page below 1 MB, stacks anywhere below 4 GB */
    EFI_PHYSICAL_ADDRESS page = 0x9FFFF, stacks;
    UINTN nstacks = want - 1;
    if (want <= 1 ||
        efi_allocate_pages(AllocateMaxAddress, EfiBootServicesData, 1, &page) != EFI_SUCCESS ||
        efi_allocate_pages(AllocateAnyPages, EfiBootServicesData,
                           (nstacks + 1) * (STACK_SIZE / 4096), &stacks) != EFI_SUCCESS) {
        sort_order(1);
        goto done;
    }
    tramp_page = (u32)page;
    usize len = smp_tramp_end - smp_tramp_start;
    memcpy((void *)(uptr)page, smp_tramp_start, len);
    td = (volatile struct tramp_data *)(uptr)(page + (smp_tramp_data - smp_tramp_start));
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    td->cr3 = cr3;
    td->stacks = stacks;                /* slot 1's stack ends at stacks + size */
    td->stack_size = STACK_SIZE;
    td->entry = (u64)(uptr)ap_main;
    td->next = 1;
    td->max = nstacks;
    stack_base = stacks;
    spare_slot = nstacks + 1;           /* its stack is the extra one */

    init_sipi_sipi(1, 0);
    u64 deadline = now_us() + WAIT_US;
    u32 up = 0;
    for (;;) {
        up = 0;
        u32 started = td->next - 1;
        if (started > nstacks)
            started = nstacks;
        for (u32 s = 1; s <= started; s++)
            up += cpus[s].state == AP_IDLE;
        if (up >= nstacks || now_us() >= deadline)
            break;
        efi_stall(100);
    }
    td->max = 0;                        /* an AP still on its way parks in the trampoline */
    for (u32 s = 1; s <= nstacks; s++)
        cpus[s].info.enabled = cpus[s].state == AP_IDLE;
    sort_order(nstacks + 1);

    EFI_EVENT ev;
    efi_create_event(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, park_on_exit, NULL, &ev);
done:;
    EFI_HANDLE h = NULL;
    efi_install_protocol_interface(&h, (EFI_GUID *)&efi_mp_services_guid, 0, &mp);
    printf("smp: %d CPU%s, %s, BSP APIC ID %u\n", ncpus, ncpus == 1 ? "" : "s",
           x2mode ? "x2APIC" : CONFIG_X2APIC ? "xAPIC (the CPU has no x2APIC)" : "xAPIC",
           bsp->info.apic_id);
    mptable_install();
}
