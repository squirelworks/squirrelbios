/* SquirrelBIOS x86_64-uefi: what the shared UEFI core (common/efi) needs
 * to know about x86-64 on QEMU's pc and q35 machines, and the x86-only
 * helpers (port I/O, CPUID, the TSC, PCI configuration mechanism 1, the
 * chipset, the CSM). Original code. */
#ifndef ARCH_H
#define ARCH_H

#ifndef CONFIG_COM1_BASE
#define CONFIG_COM1_BASE 0x3F8
#endif

#define ARCH_NAME            "x86_64-uefi"
#define ARCH_PE_MACHINE      0x8664
#define ARCH_ALLOC_TOP       0x100000000ULL     /* the 4 GB identity map */
#define ARCH_TEST_ADDR       0x1000000ULL       /* 16 MB */
/* Only the .rt pages stay for the OS (link.ld, runtime.c) */
#define ARCH_FW_MEMORY_TYPE  EfiBootServicesCode
#define ARCH_FW_MEMORY_ATTR  0
#define ARCH_BOOT_FILE       u"\\EFI\\BOOT\\BOOTX64.EFI"

static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port)  { u8 v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
}

/* console.c: COM1, 115200 8N1 */
void con_init(void);

/* fwcfg.c: QEMU's firmware configuration device, I/O ports */
void fwcfg_read_at(u16 select, u32 offset, void *buf, u32 len);

/* pci.c */
u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off);
u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 v);
void pci_write8(u8 bus, u8 dev, u8 fn, u8 off, u8 v);
u16 pci_assign_io_bar(u8 bus, u8 dev, u8 fn, u8 bar_off);
u32 pci_assign_mem_bar(u8 bus, u8 dev, u8 fn, u8 bar_off);
int pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn);
int pci_extra_roots(u8 *roots, int max);     /* pxb / pxb-pcie root bus numbers */

/* virtio_blk.c: one transitional virtio-blk disk, published */
int vblk_probe(void);
/* pcidisk.c: the other PCI disks, through common/efi's drivers */
int pci_disk_probe(void);

/* csm/csm.c: the legacy BIOS, when nothing boots the UEFI way */
int csm_boot(void);
/* chipset.c: PIIX4 (i440fx, -M pc) or ICH9 (-M q35) */
struct chipset {
    int q35;            /* ICH9 and the 3 Series MCH, else PIIX3/PIIX4 */
    u16 pm_base;        /* ACPI PM1 event block; PM1a_CNT +4, PM timer +8 */
    u16 gpe0;           /* GPE0 block and its length in bytes */
    u8 gpe0_len;
    u64 mcfg_base;      /* PCI Express ECAM window (q35 only), else 0 */
    u32 mcfg_buses;
};
void chipset_init(void);
const struct chipset *chipset(void);
u8 chipset_pirq(u8 slot, u8 pin);
u8 chipset_pirq_irq(u8 pirq);
void chipset_legacy_handoff(void);

/* arch.c, rt.c */
#define arch_check_image(src, size) x86_check_image(src, size)
#define arch_boot_options(image) efi_set_load_options(image)
#define ARCH_RT_DATA(name) __attribute__((section(".rt.data." name)))
u64 x86_check_image(const void *src, u64 size);   /* an EFI_STATUS */
void x86_rt_init(u16 pm_base);      /* the PM block ResetSystem uses */

/* bootopt.c: QEMU's -initrd and -append for a -kernel image */
void efi_initrd_init(void);
void efi_set_load_options(void *image);

#endif
