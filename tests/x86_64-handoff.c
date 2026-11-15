/* The machine an OS gets from the x86_64 firmware (bugs found booting
 * Squirrel OS/2's BOOTX64.EFI). Original code, written from the Intel
 * SDM (local APIC LVT, IDT), the 8259A and 8254 data sheets, the 8042
 * command byte and the VGA attribute controller. Built like
 * tests/x86_64-einj.c. Reports on COM1, before ExitBootServices:
 *
 *   APP handoff vga=on
 *
 * vga   the VGA attribute controller's palette address source bit is set
 *       (QEMU's standard VGA shows nothing without it, in any mode)
 *
 * and after it:
 *
 *   APP handoff lint0=extint lint1=nmi pic=ok irq0=ok kbc=xlate
 *
 * lint0/lint1  the BSP's local APIC is in virtual wire mode
 * pic          8259 masks: all but the cascade (IRQ 2)
 * irq0         with an IDT of its own, the PIT's IRQ 0 unmasked and
 *              interrupts on, vector 08h (a BIOS's IRQ 0 base) arrives
 * kbc          the 8042 command byte has translation (bit 6) and the
 *              keyboard interrupt (bit 0) on
 *
 * then writes 0 to isa-debug-exit at 0xF4, so QEMU exits with status 1. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u64 status;
typedef void *handle;
#define EFIAPI __attribute__((ms_abi))
typedef status (EFIAPI *fn)();

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } guid;
typedef struct { u64 signature; u32 revision, header_size, crc32, reserved; } table_header;
typedef struct {
    table_header hdr;
    fn raise_tpl, restore_tpl, allocate_pages, free_pages, get_memory_map, allocate_pool,
       free_pool, create_event, set_timer, wait_for_event, signal_event, close_event,
       check_event, install_protocol, reinstall_protocol, uninstall_protocol, handle_protocol,
       reserved, register_protocol_notify, locate_handle, locate_device_path,
       install_configuration_table, load_image, start_image, exit, unload_image,
       exit_boot_services, get_next_monotonic_count, stall, set_watchdog_timer,
       connect_controller, disconnect_controller, open_protocol, close_protocol,
       open_protocol_information, protocols_per_handle, locate_handle_buffer, locate_protocol;
} boot_services;
typedef struct { guid id; void *table; } config_table;
typedef struct {
    table_header hdr;
    void *vendor;
    u32 revision;
    handle con_in_handle;
    void *con_in;
    handle con_out_handle;
    void *con_out;
    handle stderr_handle;
    void *std_err;
    void *rt;
    boot_services *bs;
    u64 ntables;
    config_table *tables;
} system_table;

static void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static void putc(char c)
{
    while (!(inb(0x3FD) & 0x20))
        ;
    outb(0x3F8, (u8)c);
}

static void puts(const char *s)
{
    while (*s)
        putc(*s++);
}

static u64 rdmsr(u32 m)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return (u64)hi << 32 | lo;
}

volatile u32 irq0_ticks;
void irq0_stub(void);
void other_stub(void);
__asm__(".text\n"
        "irq0_stub:\n"
        "    push %rax\n"
        "    lock incl irq0_ticks(%rip)\n"
        "    movb $0x20, %al\n"
        "    outb %al, $0x20\n"
        "    pop %rax\n"
        "    iretq\n"
        "other_stub:\n"
        "    iretq\n");

static u8 idt[256 * 16] __attribute__((aligned(16)));

static void gate(int v, void (*h)(void), u16 cs)
{
    u64 a = (u64)h;
    u8 *g = idt + v * 16;
    g[0] = (u8)a; g[1] = (u8)(a >> 8);
    g[2] = (u8)cs; g[3] = (u8)(cs >> 8);
    g[4] = 0; g[5] = 0x8E;              /* present, interrupt gate */
    g[6] = (u8)(a >> 16); g[7] = (u8)(a >> 24);
    for (int i = 0; i < 4; i++)
        g[8 + i] = (u8)(a >> (32 + 8 * i));
    g[12] = g[13] = g[14] = g[15] = 0;
}

static int kbc_wait(u8 bit, int set)
{
    for (u32 i = 0; i < 10000000; i++)
        if (!!(inb(0x64) & bit) == set)
            return 1;
    return 0;
}

static u8 map[16384];

EFIAPI status efi_main(handle image, system_table *st)
{
    (void)inb(0x3DA);
    u8 ar = inb(0x3C0);
    puts(ar & 0x20 ? "APP handoff vga=on\r\n" : "APP handoff vga=off\r\n");

    u64 size, key = 0, dsize = 0;
    u32 ver = 0;
    status s;
    int tries = 0;
    do {
        size = sizeof map;
        s = st->bs->get_memory_map(&size, map, &key, &dsize, &ver);
        if (!s)
            s = st->bs->exit_boot_services(image, key);
    } while (s && ++tries < 4);
    if (s) {
        puts("APP handoff ebs=bad\r\n");
        return 1;
    }

    puts("APP handoff");
    u32 l0, l1;
    if ((rdmsr(0x1B) & 0xC00) == 0xC00) {
        l0 = (u32)rdmsr(0x835);
        l1 = (u32)rdmsr(0x836);
    } else {
        volatile u32 *apic = (volatile u32 *)(rdmsr(0x1B) & 0xFFFFFF000ull);
        l0 = apic[0x350 / 4];
        l1 = apic[0x360 / 4];
    }
    puts((l0 & 0x1F700) == 0x700 ? " lint0=extint" : " lint0=bad");
    puts((l1 & 0x1F700) == 0x400 ? " lint1=nmi" : " lint1=bad");
    puts(inb(0x21) == 0xFB && inb(0xA1) == 0xFF ? " pic=ok" : " pic=bad");

    u16 cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    for (int v = 0; v < 256; v++)
        gate(v, v == 0x08 ? irq0_stub : other_stub, cs);
    struct __attribute__((packed)) { u16 limit; u64 base; } idtr = { sizeof idt - 1, (u64)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
    outb(0x43, 0x34);                   /* PIT channel 0, rate generator */
    outb(0x40, 0x9C);                   /* 11932: 100 Hz */
    outb(0x40, 0x2E);
    outb(0x21, 0xFA);                   /* IRQ 0 too */
    __asm__ volatile("sti");
    for (u64 i = 0; i < 200000000ull && irq0_ticks < 3; i++)
        __asm__ volatile("pause");
    __asm__ volatile("cli");
    outb(0x21, 0xFB);
    puts(irq0_ticks >= 3 ? " irq0=ok" : " irq0=bad");

    const char *k = " kbc=bad";
    if (kbc_wait(2, 0)) {
        outb(0x64, 0x20);
        if (kbc_wait(1, 1)) {
            u8 cb = inb(0x60);
            k = (cb & 0x41) == 0x41 && !(cb & 0x10) ? " kbc=xlate" : " kbc=bad";
        }
    }
    puts(k);
    puts("\r\n");
    outb(0xF4, 0);
    for (;;)
        __asm__ volatile("hlt");
}
