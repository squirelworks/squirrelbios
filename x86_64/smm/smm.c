/* SquirrelBIOS x86_64-uefi: System Management Mode and EINJ (roadmap F2).
 * Original code, written from the Intel 3 Series / ICH9 datasheets
 * (SMRAM and ESMRAMC in the MCH, SMI_EN and APM_CNT in the LPC power
 * management block), the Intel SDM's description of SMM entry, SMBASE
 * relocation and the AMD64 save state map that QEMU uses, and the ACPI
 * 6.5 specification (18.6 EINJ, 18.3.2.7 generic hardware error source,
 * 18.3.2.7.1 generic error data) with the UEFI CPER memory error section.
 *
 * On q35 the top 1 MB of RAM below 4 GB becomes TSEG, which only SMM can
 * reach once T_EN is set. It holds a GDT, one 32-bit SMI handler, a stack
 * for each CPU and, for each CPU, an SMBASE whose entry stub switches to
 * protected mode and jumps to the handler. Every CPU's SMBASE is moved
 * there from the default 0x30000 by one SMI each at boot. SMIs come only
 * from writes to the APM port 0xB2 (APMC_EN); at ExitBootServices the
 * SMRAM controls are locked (D_LCK).
 *
 * EINJ: the OS writes the error type to a mailbox and writes SMI_EINJ to
 * port 0xB2. The handler reads only that type from outside SMRAM; for a
 * correctable memory error it copies a CPER record, built at boot and
 * kept in TSEG, into the error status block of a polled GHES, which the
 * OS reads and clears. Nothing the OS writes is used as an address. */
#include "smm.h"
#include "smp.h"
#include "acpi.h"

#define SMRAM_REG       0x9D
#define ESMRAMC_REG     0x9E
#define D_LCK           0x10
#define G_SMRAME        0x08
#define C_BASE_SEG      0x02
#define T_EN            0x01
#define ESMRAMC_CACHE   0x38            /* SM_CACHE, SM_L1, SM_L2 */
#define TSEG_SIZE       0x100000
#define SMI_EN          0x30            /* from PMBASE */
#define SMI_STS         0x34
#define GBL_SMI_EN      0x01
#define APMC_EN         0x20
#define APM_CNT         0xB2

#define DEFAULT_SMBASE  0x30000
#define SAVE_SMBASE     0xFF00          /* SMBASE in the AMD64 save state map */
#define CPU_AREA        0x10000         /* SMBASEs from TSEG + this, 8 KB apart */
#define CPU_STRIDE      0x2000
#define STACKS          0x2000          /* stacks from TSEG + this, 512 bytes */
#define STACK_SIZE      0x200
#define MAX_SMM_CPUS    ((CPU_AREA - STACKS) / STACK_SIZE)
#define HANDLER         0x1000

/* The EINJ mailbox (in its own reserved page) */
#define MB_TYPE         0x00            /* SET_ERROR_TYPE writes it */
#define MB_TYPES        0x08            /* GET_ERROR_TYPE reads it */
#define MB_BUSY         0x10
#define MB_STATUS       0x18
#define MB_TRIGGER      0x20            /* the trigger action table's address */
#define MB_PING         0x28
#define MB_TRIGGER_TAB  0x100
#define EINJ_MEM_CORRECTABLE 0x08

#define GHES_BLOCK      0x100           /* in the GHES page, after the pointer */
#define GHES_BLOCK_LEN  0x400

static int available, locked;
static u64 tseg;
static u8 *mailbox, *ghes_page;
static u16 pmbase;
static int relocated;

/* ---------------------------------------------------------------- code */

/* Templates copied into SMRAM and patched. They run with paging off, so
 * every address in them is physical. */
extern const u8 smm_reloc_start[], smm_reloc_next[], smm_reloc_done[], smm_reloc_end[];
extern const u8 smm_entry_start[], smm_entry_esp[], smm_entry_target[], smm_entry_gdtr[], smm_entry_end[];
extern const u8 smm_h32_start[], smm_h32_data[], smm_h32_end[];
__asm__(
    ".section .rodata\n"
    /* At the default SMBASE + 0x8000 (CS base 0x30000): store the SMBASE
     * the BSP left in reloc_next into the save state, flag it, resume */
    ".code16\n"
    ".globl smm_reloc_start, smm_reloc_next, smm_reloc_done, smm_reloc_end\n"
    "smm_reloc_start:\n"
    "    movl %cs:(0x8000 + smm_reloc_next - smm_reloc_start), %eax\n"
    "    movl %eax, %cs:0xFF00\n"
    "    movl $1, %cs:(0x8000 + smm_reloc_done - smm_reloc_start)\n"
    "    rsm\n"
    "    .p2align 2\n"
    "smm_reloc_next: .long 0\n"
    "smm_reloc_done: .long 0\n"
    "smm_reloc_end:\n"
    /* At each CPU's SMBASE + 0x8000: its stack, the GDT, protected mode,
     * then the shared handler */
    ".globl smm_entry_start, smm_entry_esp, smm_entry_target, smm_entry_gdtr, smm_entry_end\n"
    "smm_entry_start:\n"
    "    .byte 0x66, 0xBC\n"                /* mov esp, imm32 */
    "smm_entry_esp: .long 0\n"
    "    lgdtl %cs:(0x8000 + smm_entry_gdtr - smm_entry_start)\n"
    "    movl %cr0, %eax\n"
    "    orb $1, %al\n"
    "    movl %eax, %cr0\n"
    "    .byte 0x66, 0xEA\n"                /* jmp dword 0x08:imm32 */
    "smm_entry_target: .long 0\n"
    "    .word 0x08\n"
    "    .p2align 3\n"
    "smm_entry_gdtr: .word 0, 0, 0, 0\n"
    "smm_entry_end:\n"
    /* The handler, 32-bit flat. Data after the code: mailbox address,
     * GHES block address, PMBASE, record length, record */
    ".code32\n"
    ".globl smm_h32_start, smm_h32_data, smm_h32_end\n"
    "smm_h32_start:\n"
    "    movw $0x10, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %ss\n"
    "    call 1f\n"
    "1:  popl %ebx\n"
    "    subl $(1b - smm_h32_start), %ebx\n"
    "    movl (smm_h32_data - smm_h32_start + 8)(%ebx), %edx\n"
    "    addl $0x34, %edx\n"                /* SMI_STS: clear APM_STS */
    "    movl $0x20, %eax\n"
    "    outl %eax, %dx\n"
    "    inb $0xB2, %al\n"
    "    movl (smm_h32_data - smm_h32_start)(%ebx), %esi\n"
    "    cmpb $0xE0, %al\n"                 /* SMI_PING */
    "    jne 2f\n"
    "    movl $0x534D4D21, 0x28(%esi)\n"    /* "!MMS" */
    "    jmp 9f\n"
    "2:  cmpb $0xE1, %al\n"                 /* SMI_EINJ */
    "    jne 9f\n"
    "    movl $1, %edx\n"                   /* status: failed */
    "    cmpl $0x08, 0x00(%esi)\n"          /* memory correctable only */
    "    jne 3f\n"
    "    cmpl $0, 0x04(%esi)\n"
    "    jne 3f\n"
    "    movl (smm_h32_data - smm_h32_start + 4)(%ebx), %edi\n"
    "    cmpl $0, (%edi)\n"                 /* the last one not read yet */
    "    jne 3f\n"
    "    pushl %esi\n"
    "    leal (smm_h32_data - smm_h32_start + 20)(%ebx), %esi\n"
    "    movl (smm_h32_data - smm_h32_start + 12)(%ebx), %ecx\n"
    "    addl $4, %esi\n"                   /* block status last */
    "    addl $4, %edi\n"
    "    subl $4, %ecx\n"
    "    cld\n"
    "    rep movsb\n"
    "    movl (smm_h32_data - smm_h32_start + 20)(%ebx), %eax\n"
    "    movl (smm_h32_data - smm_h32_start + 4)(%ebx), %edi\n"
    "    movl %eax, (%edi)\n"
    "    popl %esi\n"
    "    xorl %edx, %edx\n"                 /* success */
    "3:  movl %edx, 0x18(%esi)\n"
    "    movl $0, 0x1C(%esi)\n"
    "    movl $0, 0x10(%esi)\n"             /* not busy */
    "9:  rsm\n"
    "    .p2align 3\n"
    "smm_h32_data: .fill 20 + 256, 1, 0\n"
    "smm_h32_end:\n"
    ".code64\n"
    ".previous\n");

#define H32_DATA (u32)(smm_h32_data - smm_h32_start)
#define REC_MAX 256

/* ---------------------------------------------------------------- the record */

static void put16(u8 *p, u16 v) { memcpy(p, &v, 2); }
static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

static const u8 mem_err_guid[16] = {      /* A5BC1114-6F64-4EDE-B863-3E83ED7C83B1 */
    0x14, 0x11, 0xBC, 0xA5, 0x64, 0x6F, 0xDE, 0x4E, 0xB8, 0x63, 0x3E, 0x83, 0xED, 0x7C, 0x83, 0xB1,
};

/* A Generic Error Status Block: one corrected memory error (single-bit
 * ECC) at addr, as a revision 3 generic error data entry */
static u32 build_record(u8 *r, u64 addr)
{
    memset(r, 0, REC_MAX);
    u32 sec = 80, entry = 72 + sec;
    put32(r, 1u << 1 | 1u << 4);        /* correctable valid, one entry */
    put32(r + 4, 0);                    /* raw data offset, length */
    put32(r + 8, 0);
    put32(r + 12, entry);               /* data length */
    put32(r + 16, 2);                   /* corrected */
    u8 *e = r + 20;
    memcpy(e, mem_err_guid, 16);
    put32(e + 16, 2);                   /* corrected */
    put16(e + 20, 0x0300);
    e[22] = 0;                          /* no FRU id, text or time stamp */
    put32(e + 24, sec);
    u8 *m = e + 72;
    put64(m, 1u << 1 | 1u << 14);       /* physical address, error type */
    put64(m + 16, addr);
    m[72] = 2;                          /* single-bit ECC */
    return 20 + entry;
}

/* ---------------------------------------------------------------- setup */

static u64 low_ram_top(void)
{
    u64 top = 0;
    for (int i = 0; i < mem_count; i++) {
        u64 e = mem_map[i].base + mem_map[i].length;
        if (mem_map[i].type == 1 && e <= 0x100000000ull && e > top)
            top = e;
    }
    return top;
}

static int is_q35(void)
{
    return pci_read16(0, 0, 0, 0x00) == 0x8086 && pci_read16(0, 0, 0, 0x02) == 0x29C0;
}

void smm_reserve(void)
{
    u64 top = low_ram_top();
    if (!is_q35() || top < 0x4000000)       /* 64 MB at least */
        return;
    tseg = top - TSEG_SIZE;
    efi_memory_mark(tseg, TSEG_SIZE / 4096, EfiReservedMemoryType);
}

static volatile u32 reloc_lock;
static volatile u32 *reloc_next, *reloc_done;
static volatile int reloc_ok[MAX_SMM_CPUS];

static int my_index(void)
{
    u32 id = apic_id();
    for (int i = 0; i < smp_cpu_count(); i++)
        if (smp_cpu(i)->apic_id == id)
            return i;
    return -1;
}

/* On each CPU in turn: one SMI at the default SMBASE moves it */
static void EFIAPI relocate_this(void *arg)
{
    (void)arg;
    while (__atomic_exchange_n(&reloc_lock, 1, __ATOMIC_ACQUIRE))
        cpu_pause();
    int i = my_index();
    if (i >= 0 && i < MAX_SMM_CPUS) {
        *reloc_next = (u32)(tseg + CPU_AREA + (u64)i * CPU_STRIDE);
        *reloc_done = 0;
        outb(APM_CNT, 0);
        reloc_ok[i] = *reloc_done == 1;
    }
    __atomic_store_n(&reloc_lock, 0, __ATOMIC_RELEASE);
}

static void smi_enable(int on)
{
    u32 en = inl((u16)(pmbase + SMI_EN));
    en = on ? en | GBL_SMI_EN | APMC_EN : en & ~(u32)APMC_EN;
    outl((u16)(pmbase + SMI_EN), en);
}

static void EFIAPI on_exit_boot_services(EFI_EVENT ev, void *ctx)
{
    (void)ev, (void)ctx;
    if (available && !locked) {
        pci_write8(0, 0, 0, SMRAM_REG, G_SMRAME | C_BASE_SEG | D_LCK);
        locked = 1;
    }
}

void smm_init(void)
{
    if (!tseg)
        return;
    int cpus = smp_cpu_count();
    if (cpus < 1)
        cpus = 1;
    if (cpus > MAX_SMM_CPUS) {
        printf("smm: %d CPUs, more than the %d TSEG has room for: no SMM\n", cpus, MAX_SMM_CPUS);
        return;
    }
    pmbase = (u16)(pci_read32(0, 0x1F, 0, 0x40) & 0xFF80);
    EFI_PHYSICAL_ADDRESS mb = 0xFFFFFFFF, gp = 0xFFFFFFFF;
    if (efi_allocate_pages(AllocateMaxAddress, EfiReservedMemoryType, 1, &mb) ||
        efi_allocate_pages(AllocateMaxAddress, EfiReservedMemoryType, 1, &gp)) {
        printf("smm: no memory for the EINJ mailbox\n");
        return;
    }
    mailbox = (u8 *)(uptr)mb;
    ghes_page = (u8 *)(uptr)gp;
    memset(mailbox, 0, 4096);
    memset(ghes_page, 0, 4096);
    put64(mailbox + MB_TYPES, EINJ_MEM_CORRECTABLE);
    put64(mailbox + MB_TRIGGER, mb + MB_TRIGGER_TAB);
    u8 *tt = mailbox + MB_TRIGGER_TAB;  /* one NOOP trigger action */
    put32(tt, 16);
    put32(tt + 4, 1);
    put32(tt + 8, 16 + 32);
    put32(tt + 12, 1);
    tt[16] = 0xFF;                      /* TRIGGER_ERROR */
    tt[17] = 4;                         /* NOOP */
    put64(ghes_page, gp + GHES_BLOCK);  /* the error status address cell */

    /* TSEG while it is still plain RAM: GDT, handler, entry stubs */
    u8 *t = (u8 *)(uptr)tseg;
    memset(t, 0, TSEG_SIZE);
    static const u64 gdt[3] = { 0, 0x00CF9A000000FFFFull, 0x00CF92000000FFFFull };
    memcpy(t, gdt, sizeof gdt);
    u32 hlen = (u32)(smm_h32_end - smm_h32_start);
    u8 *h = t + HANDLER;
    memcpy(h, smm_h32_start, hlen);
    put32(h + H32_DATA, (u32)mb);
    put32(h + H32_DATA + 4, (u32)(gp + GHES_BLOCK));
    put32(h + H32_DATA + 8, pmbase);
    put32(h + H32_DATA + 12, build_record(h + H32_DATA + 20, gp));
    u32 elen = (u32)(smm_entry_end - smm_entry_start);
    for (int i = 0; i < cpus; i++) {
        u8 *e = t + CPU_AREA + (u64)i * CPU_STRIDE + 0x8000;
        memcpy(e, smm_entry_start, elen);
        put32(e + (smm_entry_esp - smm_entry_start), (u32)(tseg + STACKS + (u64)(i + 1) * STACK_SIZE));
        put32(e + (smm_entry_target - smm_entry_start), (u32)(tseg + HANDLER));
        u8 *g = e + (smm_entry_gdtr - smm_entry_start);
        put16(g, sizeof gdt - 1);
        put32(g + 2, (u32)tseg);
    }

    /* The relocation stub where every CPU enters SMM first; what was
     * there comes back afterwards */
    static u8 saved_stub[4096], saved_state[4096];
    u8 *stub = (u8 *)(uptr)(DEFAULT_SMBASE + 0x8000), *state = (u8 *)(uptr)(DEFAULT_SMBASE + 0xF000);
    memcpy(saved_stub, stub, 4096);
    memcpy(saved_state, state, 4096);
    memcpy(stub, smm_reloc_start, (u32)(smm_reloc_end - smm_reloc_start));
    reloc_next = (volatile u32 *)(stub + (smm_reloc_next - smm_reloc_start));
    reloc_done = (volatile u32 *)(stub + (smm_reloc_done - smm_reloc_start));

    pci_write8(0, 0, 0, SMRAM_REG, G_SMRAME | C_BASE_SEG);
    pci_write8(0, 0, 0, ESMRAMC_REG, ESMRAMC_CACHE | T_EN);
    smi_enable(1);
    smp_run_all(relocate_this, NULL);
    memcpy(stub, saved_stub, 4096);
    memcpy(state, saved_state, 4096);
    for (int i = 0; i < cpus; i++)
        relocated += reloc_ok[i];

    /* Is it really SMRAM now, and does the handler answer? */
    u32 seen = *(volatile u32 *)(uptr)(tseg + HANDLER);
    put32(mailbox + MB_PING, 0);
    outb(APM_CNT, SMI_PING);
    u32 ping = *(volatile u32 *)(mailbox + MB_PING);
    if (relocated != cpus || ping != 0x534D4D21 || seen == *(const u32 *)smm_h32_start) {
        printf("smm: not available (%d of %d CPUs relocated, handler %s, TSEG %s)\n", relocated, cpus,
               ping == 0x534D4D21 ? "answers" : "silent",
               seen == *(const u32 *)smm_h32_start ? "readable outside SMM" : "closed");
        smi_enable(0);
        pci_write8(0, 0, 0, ESMRAMC_REG, ESMRAMC_CACHE);
        return;
    }
    available = 1;
    EFI_EVENT ev;
    efi_create_event(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK, on_exit_boot_services, NULL, &ev);
    printf("smm: TSEG %u KB at 0x%lx, SMBASE moved there on %d CPU%s, SMIs from port 0xB2; "
           "locked at ExitBootServices\n", TSEG_SIZE / 1024, tseg, relocated, relocated == 1 ? "" : "s");
}

int smm_einj_available(void)
{
    return available;
}

void smm_legacy_handoff(void)
{
    if (!available || locked)
        return;
    smi_enable(0);
    pci_write8(0, 0, 0, ESMRAMC_REG, ESMRAMC_CACHE);
    pci_write8(0, 0, 0, SMRAM_REG, C_BASE_SEG);
    available = 0;
}

/* ---------------------------------------------------------------- tables */

static void gas(u8 *g, u8 space, u8 width, u8 access, u64 addr)
{
    g[0] = space, g[1] = width, g[2] = 0, g[3] = access;
    put64(g + 4, addr);
}

/* HEST type 9: the generic hardware error source the handler writes,
 * polled by the OS */
u32 smm_hest_ghes(u8 *p, u16 id)
{
    if (!available)
        return 0;
    memset(p, 0, 64);
    put16(p, 9);
    put16(p + 2, id);
    put16(p + 4, 0xFFFF);               /* no related source */
    p[7] = 1;                           /* enabled */
    put32(p + 8, 1);                    /* records to pre-allocate */
    put32(p + 12, 1);                   /* sections per record */
    put32(p + 16, GHES_BLOCK_LEN);      /* max raw data length */
    gas(p + 20, 0, 64, 4, (u64)(uptr)ghes_page);
    u8 *nt = p + 32;
    nt[0] = 0;                          /* polled */
    nt[1] = 28;
    put32(nt + 4, 500);                 /* every 500 ms */
    put32(p + 60, GHES_BLOCK_LEN);
    return 64;
}

/* One EINJ instruction entry */
static u8 *einj_entry(u8 *e, u8 action, u8 ins, u8 space, u8 width, u8 access, u64 addr, u64 value, u64 mask)
{
    memset(e, 0, 32);
    e[0] = action, e[1] = ins;
    gas(e + 4, space, width, access, addr);
    put64(e + 16, value);
    put64(e + 24, mask);
    return e + 32;
}

enum { BEGIN, GET_TRIGGER, SET_TYPE, GET_TYPE, END, EXECUTE, CHECK_BUSY, GET_STATUS };
enum { READ_REG, READ_REG_VALUE, WRITE_REG, WRITE_REG_VALUE, NOOP };

u8 *smm_einj_table(void *(*take)(u32 len))
{
    if (!available)
        return NULL;
    const int n = 8;
    u32 len = 48 + 32 * n;
    u8 *t = take(len);
    if (!t)
        return NULL;
    memset(t, 0, len);
    put32(t + 36, 12);                  /* injection header size */
    put32(t + 44, (u32)n);
    u64 m = (u64)(uptr)mailbox;
    u8 *e = t + 48;
    e = einj_entry(e, BEGIN, NOOP, 0, 64, 4, m + MB_TYPE, 0, 0);
    e = einj_entry(e, GET_TRIGGER, READ_REG, 0, 64, 4, m + MB_TRIGGER, 0, ~0ull);
    e = einj_entry(e, SET_TYPE, WRITE_REG, 0, 64, 4, m + MB_TYPE, 0, 0xFFFFFFFF);
    e = einj_entry(e, GET_TYPE, READ_REG, 0, 64, 4, m + MB_TYPES, 0, 0xFFFFFFFF);
    e = einj_entry(e, END, NOOP, 0, 64, 4, m + MB_TYPE, 0, 0);
    e = einj_entry(e, EXECUTE, WRITE_REG_VALUE, 1, 8, 1, APM_CNT, SMI_EINJ, 0xFF);
    e = einj_entry(e, CHECK_BUSY, READ_REG, 0, 64, 4, m + MB_BUSY, 0, 1);
    einj_entry(e, GET_STATUS, READ_REG, 0, 64, 4, m + MB_STATUS, 0, 0xFF);
    acpi_header(t, "EINJ", len, 1);
    acpi_seal(t);
    return t;
}
