/* SquirrelBIOS x86_64-uefi: the MultiProcessor Specification 1.4 table,
 * installed as the MPS EFI configuration table. Original code, written
 * from the MP specification's description of the floating pointer, the
 * configuration table header and its entries.
 *
 * Entries: one per processor (APIC IDs up to 254, the format's limit),
 * the ISA bus, the I/O APIC, the 16 ISA interrupts (IRQ 0 on pin 2, the
 * others on their own pins) and LINT0/LINT1 as ExtINT/NMI. PCI interrupt
 * routing is left to ACPI. */
#include "smp.h"

#define IOAPIC_BASE 0xFEC00000u
#define LAPIC_BASE  0xFEE00000u

struct mp_float {
    char sig[4];
    u32 config;
    u8 length, rev, checksum, feature1;
    u8 feature2, reserved[3];
} __attribute__((packed));

struct mp_header {
    char sig[4];
    u16 length;
    u8 rev, checksum;
    char oem[8], product[12];
    u32 oem_table;
    u16 oem_size, count;
    u32 lapic;
    u16 ext_length;
    u8 ext_checksum, reserved;
} __attribute__((packed));

struct mp_cpu {
    u8 type, apic_id, apic_ver, flags;
    u32 signature, features;
    u32 reserved[2];
} __attribute__((packed));

struct mp_bus {
    u8 type, id;
    char name[6];
} __attribute__((packed));

struct mp_ioapic {
    u8 type, id, ver, flags;
    u32 addr;
} __attribute__((packed));

struct mp_irq {
    u8 type, kind;
    u16 flags;
    u8 bus, bus_irq, apic, pin;
} __attribute__((packed));

const EFI_GUID efi_mps_guid = MPS_TABLE_GUID;

static u8 sum(const void *p, usize n)
{
    const u8 *b = p;
    u8 s = 0;
    while (n--)
        s += *b++;
    return s;
}

static u32 ioapic_read(u32 reg)
{
    *(volatile u32 *)(uptr)IOAPIC_BASE = reg;
    return *(volatile u32 *)(uptr)(IOAPIC_BASE + 0x10);
}

void mptable_install(void)
{
    int n = smp_cpu_count();
    usize size = sizeof(struct mp_float) + sizeof(struct mp_header) + n * sizeof(struct mp_cpu) +
                 sizeof(struct mp_bus) + sizeof(struct mp_ioapic) + 18 * sizeof(struct mp_irq);
    u8 *buf;
    if (efi_allocate_pool(EfiACPIMemoryNVS, size, (void **)&buf) != EFI_SUCCESS)
        return;
    memset(buf, 0, size);
    struct mp_float *fp = (struct mp_float *)buf;
    struct mp_header *h = (struct mp_header *)(fp + 1);
    u8 *e = (u8 *)(h + 1);
    u16 count = 0;

    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    u8 lapic_ver = (u8)*(volatile u32 *)(uptr)(LAPIC_BASE + 0x30);
    if (smp_x2apic())
        lapic_ver = 0x14;               /* the version register is an MSR then */
    for (int i = 0; i < n; i++) {
        const struct smp_cpu_info *ci = smp_cpu(i);
        if (ci->apic_id > 254)
            continue;
        struct mp_cpu *pc = (struct mp_cpu *)e;
        pc->type = 0;
        pc->apic_id = (u8)ci->apic_id;
        pc->apic_ver = lapic_ver;
        pc->flags = (ci->enabled ? 1 : 0) | (i == 0 ? 2 : 0);
        pc->signature = a;
        pc->features = d;
        e += sizeof *pc;
        count++;
    }

    struct mp_bus *bus = (struct mp_bus *)e;
    bus->type = 1;
    bus->id = 0;
    memcpy(bus->name, "ISA   ", 6);
    e += sizeof *bus;
    count++;

    u8 io_id = (ioapic_read(0) >> 24) & 0x0F;
    struct mp_ioapic *io = (struct mp_ioapic *)e;
    io->type = 2;
    io->id = io_id;
    io->ver = (u8)ioapic_read(1);
    io->flags = 1;
    io->addr = IOAPIC_BASE;
    e += sizeof *io;
    count++;

    for (u8 irq = 0; irq < 16; irq++) {
        if (irq == 2)
            continue;                   /* the cascade: IRQ 0 takes pin 2 */
        struct mp_irq *ir = (struct mp_irq *)e;
        ir->type = 3;
        ir->kind = 0;                   /* INT */
        ir->flags = 0;                  /* conforms to the bus */
        ir->bus = 0;
        ir->bus_irq = irq;
        ir->apic = io_id;
        ir->pin = irq == 0 ? 2 : irq;
        e += sizeof *ir;
        count++;
    }
    for (u8 lint = 0; lint < 2; lint++) {
        struct mp_irq *li = (struct mp_irq *)e;
        li->type = 4;
        li->kind = lint ? 1 : 3;        /* NMI on LINT1, ExtINT on LINT0 */
        li->bus = 0;
        li->bus_irq = 0;
        li->apic = 0xFF;                /* every local APIC */
        li->pin = lint;
        e += sizeof *li;
        count++;
    }

    memcpy(h->sig, "PCMP", 4);
    h->length = (u16)(e - (u8 *)h);
    h->rev = 4;
    memcpy(h->oem, "SQUIRREL", 8);
    memcpy(h->product, "SquirrelBIOS", 12);
    h->count = count;
    h->lapic = LAPIC_BASE;
    h->checksum = (u8)-sum(h, h->length);

    memcpy(fp->sig, "_MP_", 4);
    fp->config = (u32)(uptr)h;
    fp->length = 1;
    fp->rev = 4;
    fp->checksum = (u8)-sum(fp, sizeof *fp);

    efi_install_configuration_table((EFI_GUID *)&efi_mps_guid, fp);
    printf("smp: MP table at %p, %u entries\n", fp, count);
}
