/* SquirrelBIOS x86_64-uefi: common types and helpers.
 * Original code. */
#ifndef SQ_H
#define SQ_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed long long   s64;
typedef signed int         s32;
typedef signed short       s16;
typedef unsigned long      uptr;
typedef unsigned long      usize;

#define NULL ((void *)0)

#ifdef SQ_CONFIG
#include "config.h"
#endif
#ifndef CONFIG_COM1_BASE
#define CONFIG_COM1_BASE 0x3F8
#endif

#define SQ_VERSION "2.0.0-alpha"

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

static inline u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

/* lib.c */
void *memset(void *d, int c, usize n);
void *memcpy(void *d, const void *s, usize n);
int memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int strcmp(const char *a, const char *b);

/* console.c: COM1, 115200 8N1 */
void con_init(void);
void con_putc(char c);
void con_raw(char c);
int con_getc(void);
void con_puts(const char *s);
void printf(const char *fmt, ...);
void halt(void) __attribute__((noreturn));

/* fwcfg.c: QEMU's firmware configuration device */
int fwcfg_present(void);
int fwcfg_find(const char *name, u16 *select, u32 *size);
void fwcfg_read(u16 select, void *buf, u32 len);

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

/* virtio_blk.c: one transitional virtio-blk disk */
int vblk_probe(void);
int vblk_present(void);
u64 vblk_sectors(void);
u8 vblk_bus(void);
u8 vblk_dev(void);
u8 vblk_fn(void);
int vblk_read(u64 lba, void *buf, u32 bytes);
int vblk_write(u64 lba, void *buf, u32 bytes);

/* memmap.c */
struct mem_range {
    u64 base;
    u64 length;
    u32 type;           /* 1 usable, 2 reserved, 3 ACPI, 4 NVS */
};
#define MEM_MAX 32
extern struct mem_range mem_map[MEM_MAX];
extern int mem_count;
void memmap_init(void);

#endif
