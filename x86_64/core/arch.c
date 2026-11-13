/* SquirrelBIOS x86_64-uefi: the architecture interface of the shared
 * UEFI core (common/efi, see sq.h) at boot time: COM1, the TSC measured
 * against the PIT, the firmware's memory, PE entry calls (the Microsoft
 * x64 convention) and the disks. Original code. The services the OS may
 * call after ExitBootServices are in rt.c. */
#include "efi.h"
#include "secure.h"

#define COM1 CONFIG_COM1_BASE

void con_init(void)
{
    outb(COM1 + 1, 0x00);       /* no interrupts */
    outb(COM1 + 3, 0x80);       /* DLAB */
    outb(COM1 + 0, 0x01);       /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8N1 */
    outb(COM1 + 2, 0x00);       /* no FIFO: turning it on would drop a byte
                                   received before we got here */
    outb(COM1 + 4, 0x03);       /* DTR, RTS */
}

void arch_con_putc(u8 c)
{
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, c);
}

int arch_con_getc(void)
{
    return (inb(COM1 + 5) & 0x01) ? inb(COM1) : -1;
}

u64 arch_counter(void)
{
    return rdtsc();
}

/* PIT channel 2, gated through port 61h: count down 10 ms, see how far
 * the TSC moved (0: no answer) */
u64 arch_counter_hz(void)
{
    u8 gate = inb(0x61);
    outb(0x61, (u8)((gate & ~0x02) | 0x01));    /* gate on, speaker off */
    outb(0x43, 0xB0);                           /* ch2, lo/hi, mode 0 */
    outb(0x42, 11932 & 0xFF);                   /* 1193182 Hz / 100 */
    outb(0x42, 11932 >> 8);
    u64 t0 = rdtsc();
    u64 n = 0;
    while (!(inb(0x61) & 0x20) && ++n < 100000000)
        ;
    u64 t1 = rdtsc();
    outb(0x61, gate);
    return (n < 100000000 && t1 > t0) ? (t1 - t0) * 100 : 0;
}

void arch_relax(void)
{
    __asm__ volatile("pause");
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void arch_sync_icache(void *p, usize n)
{
    (void)p;                    /* x86 keeps instruction fetches coherent */
    (void)n;
}

/* The image is boot services code (memory.c) except for the runtime
 * pages (link.ld): the runtime services, the variable store, the system
 * and runtime services tables and the configuration tables, which the OS
 * keeps after ExitBootServices (Linux calls GetVariable and SetVariable
 * later). The rest is the OS's to reuse. */
extern char __rt_code_start[], __rt_code_end[], __rt_data_start[], __rt_data_end[];

void arch_memory_reserve(void)
{
    efi_memory_mark(0, 1, EfiBootServicesData);                         /* page 0 */
    efi_memory_mark(0x1000, 6, EfiBootServicesData);                    /* page tables */
    efi_memory_mark(0xA0000, 0x60, EfiReservedMemoryType);              /* VGA, ROMs */
    efi_memory_mark(0x100000, 0x100, EfiBootServicesData);              /* stack */
    u64 cs = (u64)(uptr)__rt_code_start, ce = (u64)(uptr)__rt_code_end;
    u64 ds = (u64)(uptr)__rt_data_start, de = (u64)(uptr)__rt_data_end;
    if (ce > cs)
        efi_memory_mark_attr(cs, (ce - cs) / 4096, EfiRuntimeServicesCode,
                             EFI_MEMORY_WB | EFI_MEMORY_RUNTIME);
    if (de > ds)
        efi_memory_mark_attr(ds, (de - ds) / 4096, EfiRuntimeServicesData,
                             EFI_MEMORY_WB | EFI_MEMORY_RUNTIME);
}

/* Secure Boot decides whether an image may run (secure.c) */
u64 x86_check_image(const void *src, u64 size)
{
    return secure_check_image(src, size);
}

/* The transitional virtio-blk on PCI (virtio_blk.c) */
int arch_disk_probe(void)
{
    return vblk_probe();
}

/* image_call(entry, handle, system table, &saved_rsp): call the entry
 * point with the Microsoft x64 convention, return what it returns.
 * image_exit(saved_rsp, status): return from that image_call with
 * status, from anywhere inside the image. */
__asm__(
    ".text\n"
    ".globl image_call\n"
    "image_call:\n"
    "    push %rbp\n"
    "    push %rbx\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    mov %rsp, (%rcx)\n"
    "    mov %rsp, %rbp\n"
    "    mov %rdi, %rax\n"
    "    mov %rsi, %rcx\n"
    "    and $-16, %rsp\n"
    "    sub $32, %rsp\n"
    "    cld\n"
    "    call *%rax\n"
    "    mov %rbp, %rsp\n"
    "image_call_out:\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rbx\n"
    "    pop %rbp\n"
    "    ret\n"
    ".globl image_exit\n"
    "image_exit:\n"
    "    mov %rdi, %rsp\n"
    "    mov %rsi, %rax\n"
    "    jmp image_call_out\n");
