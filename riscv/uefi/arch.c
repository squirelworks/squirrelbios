/* SquirrelBIOS riscv64-uefi: the RISC-V / QEMU virt side of the UEFI
 * core, which runs in S-mode on top of the SquirrelBIOS SBI. Original
 * code, written from the RISC-V privileged specification, the RISC-V
 * SBI specification, the NS16550A data sheet, the RISC-V UEFI boot
 * protocol (RISCV_EFI_BOOT_PROTOCOL) and QEMU's goldfish RTC
 * documentation.
 *
 * Console: the NS16550A the device tree names (the SBI set it up).
 * Time: the time CSR at /cpus timebase-frequency; the goldfish RTC.
 * Reset: SBI SRST. Memory: physical, translation off. The SBI keeps the
 * first 2 MB of RAM; the core is right after it. A -kernel image QEMU
 * loaded (the a2 the SBI passes) is started as an EFI application, with
 * /chosen bootargs as its LoadOptions and the /chosen initrd for the
 * Linux initrd LoadFile2 path. */
#include "efi.h"

extern char __fw_start[], __fw_end[];

static uptr uart = CONFIG_UART_BASE;
static uptr rtc;
static u64 timebase = 10000000;
static u64 boot_hart;
static uptr kernel;
static u32 kernel_size;

#define UART_RBR 0
#define UART_THR 0
#define UART_LSR 5

/* ------------------------------------------------------------ console */

void arch_con_init(void)
{
    /* the SBI programmed it: 115200 8N1 */
}

void arch_con_putc(u8 c)
{
    for (int i = 0; i < 1000000 && !(mmio_read8(uart + UART_LSR) & 0x20); i++)
        ;
    mmio_write8(uart + UART_THR, c);
}

int arch_con_getc(void)
{
    if (!(mmio_read8(uart + UART_LSR) & 0x01))
        return -1;
    return mmio_read8(uart + UART_RBR);
}

/* ---------------------------------------------------------------- SBI */

struct sbiret { long error; long value; };

static struct sbiret sbi(u64 ext, u64 fid, u64 a0, u64 a1)
{
    register u64 r0 __asm__("a0") = a0;
    register u64 r1 __asm__("a1") = a1;
    register u64 r6 __asm__("a6") = fid;
    register u64 r7 __asm__("a7") = ext;
    __asm__ volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r6), "r"(r7) : "memory");
    struct sbiret r = { (long)r0, (long)r1 };
    return r;
}

#define SBI_EXT_BASE 0x10
#define SBI_EXT_SRST 0x53525354

/* --------------------------------------------------------- CPU, time */

u64 arch_counter(void)
{
    u64 t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

u64 arch_counter_hz(void)
{
    return timebase;
}

void arch_relax(void)
{
    __asm__ volatile("nop");
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("csrw sie, zero; wfi");
}

void arch_sync_icache(void *p, usize n)
{
    (void)p;
    (void)n;
    __asm__ volatile("fence.i" ::: "memory");   /* the other harts are parked */
}

int arch_rtc_seconds(u64 *secs)
{
    if (!rtc)
        return 0;
    u64 lo = mmio_read32(rtc + 0x00);           /* TIME_LOW latches TIME_HIGH */
    u64 hi = mmio_read32(rtc + 0x04);
    *secs = (hi << 32 | lo) / 1000000000ULL;
    return 1;
}

void arch_reset(int type, u64 status)
{
    /* SRST: type 0 shutdown, 1 cold reboot, 2 warm reboot; reason 1 = failure */
    u64 t = type == EfiResetShutdown ? 0 : type == EfiResetWarm ? 2 : 1;
    sbi(SBI_EXT_SRST, 0, t, status ? 1 : 0);
    if (!efi_boot_services_exited())
        printf("fw: SBI system reset failed, halting\n");
    arch_halt();
}

void arch_exception(u64 cause, u64 epc, u64 tval)
{
    printf("exception: S-mode scause=%016lx sepc=%016lx stval=%016lx\n", cause, epc, tval);
    printf("boot halted\n");
    arch_halt();
}

/* ------------------------------------------------------------ platform */

/* The file size of the PE image at p: the end of its last section */
static u32 pe_file_size(const u8 *p)
{
    if (p[0] != 'M' || p[1] != 'Z')
        return 0;
    u32 pe = (u32)p[0x3C] | (u32)p[0x3D] << 8 | (u32)p[0x3E] << 16 | (u32)p[0x3F] << 24;
    if (pe > 0x1000 || memcmp(p + pe, "PE\0\0", 4))
        return 0;
    const u8 *coff = p + pe + 4;
    u32 nsect = coff[2] | coff[3] << 8, optsize = coff[16] | coff[17] << 8;
    const u8 *opt = coff + 20;
    u32 end = (u32)opt[60] | (u32)opt[61] << 8 | (u32)opt[62] << 16 | (u32)opt[63] << 24;
    const u8 *s = opt + optsize;
    for (u32 i = 0; i < nsect && i < 96; i++, s += 40) {
        u32 raw = (u32)s[16] | (u32)s[17] << 8 | (u32)s[18] << 16 | (u32)s[19] << 24;
        u32 at = (u32)s[20] | (u32)s[21] << 8 | (u32)s[22] << 16 | (u32)s[23] << 24;
        if (raw && at + raw > end)
            end = at + raw;
    }
    return end;
}

int arch_kernel(void **image, u32 *size)
{
    if (!kernel_size)
        return 0;
    *image = (void *)kernel;
    *size = kernel_size;
    return 1;
}

void riscv_main(u64 hart, const void *dtb, uptr k)
{
    boot_hart = hart;
    kernel = k;
    efi_main(dtb);
}

static u64 dt_u64(const u8 *p, u32 len)
{
    return len == 8 ? be64(p) : len == 4 ? be32(p) : 0;
}

void arch_init(void)
{
    u64 b;
    int n = fdt_find_compatible("ns16550a", -1);
    if (n >= 0 && fdt_reg(n, 0, &b, NULL))
        uart = (uptr)b;
    n = fdt_find_compatible("google,goldfish-rtc", -1);
    if (n >= 0 && fdt_reg(n, 0, &b, NULL))
        rtc = (uptr)b;
    u32 len;
    const u8 *tb = fdt_prop(fdt_path("/cpus"), "timebase-frequency", &len);
    if (tb && (len == 4 || len == 8))
        timebase = dt_u64(tb, len);

    struct sbiret v = sbi(SBI_EXT_BASE, 0, 0, 0), id = sbi(SBI_EXT_BASE, 1, 0, 0);
    printf("cpu: hart %lu, S-mode, SBI %lu.%lu implementation 0x%lx\n", boot_hart,
           (u64)(v.value >> 24) & 0x7F, (u64)v.value & 0xFFFFFF, (u64)id.value);
    printf("uart: NS16550A at %p\n", (void *)uart);
    printf("timer: time CSR at %lu Hz\n", timebase);
    if (rtc)
        printf("rtc: goldfish at %p\n", (void *)rtc);
    if (kernel) {
        kernel_size = pe_file_size((const u8 *)kernel);
        if (kernel_size)
            printf("fw: -kernel PE image at %p, %u bytes\n", (void *)kernel, kernel_size);
        else
            printf("fw: -kernel image at %p is not PE (an EFI stub), ignored\n", (void *)kernel);
    }
}

void arch_memory_reserve(void)
{
    u64 dt = (uptr)fdt_blob(), size = fdt_size();
    efi_memory_mark(dt & ~0xFFFULL, ((dt & 0xFFF) + size + 0xFFF) / 0x1000, EfiBootServicesData);
    if (kernel_size)
        efi_memory_mark(kernel & ~0xFFFULL, ((kernel & 0xFFF) + kernel_size + 0xFFF) / 0x1000,
                        EfiBootServicesData);
    u32 len;
    int chosen = fdt_path("/chosen");
    const u8 *s = fdt_prop(chosen, "linux,initrd-start", &len);
    u32 elen;
    const u8 *e = fdt_prop(chosen, "linux,initrd-end", &elen);
    if (s && e) {
        u64 is = dt_u64(s, len), ie = dt_u64(e, elen);
        if (ie > is)
            efi_memory_mark(is & ~0xFFFULL, ((is & 0xFFF) + (ie - is) + 0xFFF) / 0x1000, EfiBootServicesData);
    }
    if (rtc)
        efi_memory_mark_attr(rtc & ~0xFFFULL, 1, EfiMemoryMappedIO, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
}

/* The memory self-test's fixed address: 8 MB-aligned, half way into RAM */
unsigned long long arch_test_addr(void)
{
    for (int i = 0; i < mem_count; i++)
        if (mem_map[i].type == 1)
            return (mem_map[i].base + mem_map[i].length / 2) & ~0x7FFFFFULL;
    return 0x84000000ULL;
}

void arch_virtual_map(void)
{
    efi_convert_pointer(&rtc);
}

/* RISCV_EFI_BOOT_PROTOCOL: the boot hart, for Linux's EFI stub */
#define RISCV_EFI_BOOT_PROTOCOL_GUID \
    { 0xccd15fec, 0x6f73, 0x4eec, { 0x83, 0x95, 0x3e, 0x69, 0xe4, 0xb9, 0x40, 0xbf } }

typedef struct RISCV_EFI_BOOT_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *get_boot_hartid)(struct RISCV_EFI_BOOT_PROTOCOL *, UINTN *);
} RISCV_EFI_BOOT_PROTOCOL;

static EFI_STATUS EFIAPI get_boot_hartid(RISCV_EFI_BOOT_PROTOCOL *self, UINTN *id)
{
    (void)self;
    if (!id)
        return EFI_INVALID_PARAMETER;
    *id = boot_hart;
    return EFI_SUCCESS;
}

static RISCV_EFI_BOOT_PROTOCOL boot_protocol = { 0x00010000, get_boot_hartid };
static const EFI_GUID boot_protocol_guid = RISCV_EFI_BOOT_PROTOCOL_GUID;

void arch_efi_init(void)
{
    EFI_HANDLE h = NULL;
    efi_install_protocol_interface(&h, (EFI_GUID *)&boot_protocol_guid, 0, &boot_protocol);
    printf("efi: RISCV_EFI_BOOT_PROTOCOL installed (boot hart %lu)\n", boot_hart);
}

/* Disks: virtio-mmio transports and the PCIe host bridge in the device tree */
int arch_disk_probe(void)
{
    return virtio_mmio_probe() + pci_probe();
}
