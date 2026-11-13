/* SquirrelBIOS UEFI core (common/efi): common types and helpers.
 * Original code.
 *
 * The core is built once per architecture, with that architecture's
 * directory (arm/uefi, riscv/uefi) on the include path for arch.h, which
 * defines the ARCH_* constants below and may add inline helpers. The
 * arch_* functions are written per architecture. */
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

#define SQ_VERSION "2.1.0"

/* arch.h defines:
 *   ARCH_NAME            platform name in the banner ("aarch64-uefi")
 *   ARCH_PE_MACHINE      PE/COFF machine type of images it runs
 *   ARCH_ALLOC_TOP       allocations stay below this address
 *   ARCH_TEST_ADDR       a free RAM address the memory self-test may take
 *   ARCH_FW_MEMORY_TYPE  UEFI memory type of the firmware image
 *   ARCH_FW_MEMORY_ATTR  extra attribute bits for it (0 or EFI_MEMORY_RUNTIME)
 *   ARCH_BOOT_FILE       \EFI\BOOT\BOOT<arch>.EFI */
#include "arch.h"

/* Optional: arch.h may define these too */
#ifndef ARCH_RT_DATA                /* data the OS keeps after ExitBootServices */
#define ARCH_RT_DATA(name)
#endif
#ifndef arch_check_image            /* LoadImage: may this image run? */
#define arch_check_image(src, size) EFI_SUCCESS
#endif
#ifndef arch_boot_options           /* a disk boot image's LoadOptions */
#define arch_boot_options(image) ((void)(image))
#endif

static inline u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
/* By hand: without Zbb, the builtins become libgcc calls on RISC-V */
static inline u16 bswap16(u16 v) { return (u16)(v << 8 | v >> 8); }
static inline u32 bswap32(u32 v)
{
    return v << 24 | (v & 0xFF00) << 8 | (v >> 8 & 0xFF00) | v >> 24;
}
static inline u64 bswap64(u64 v) { return (u64)bswap32((u32)v) << 32 | bswap32((u32)(v >> 32)); }

static inline u8 mmio_read8(uptr a) { return *(volatile u8 *)a; }
static inline u16 mmio_read16(uptr a) { return *(volatile u16 *)a; }
static inline u32 mmio_read32(uptr a) { return *(volatile u32 *)a; }
static inline u64 mmio_read64(uptr a) { return *(volatile u64 *)a; }
static inline void mmio_write8(uptr a, u8 v) { *(volatile u8 *)a = v; }
static inline void mmio_write16(uptr a, u16 v) { *(volatile u16 *)a = v; }
static inline void mmio_write32(uptr a, u32 v) { *(volatile u32 *)a = v; }
static inline void mmio_write64(uptr a, u64 v) { *(volatile u64 *)a = v; }

/* lib.c */
void *memset(void *d, int c, usize n);
void *memcpy(void *d, const void *s, usize n);
void *memmove(void *d, const void *s, usize n);
int memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, usize n);

/* console.c: printf on the architecture's serial port */
void con_putc(char c);
void con_raw(char c);
int con_getc(void);
void con_puts(const char *s);
void printf(const char *fmt, ...);
/* A console mirrored beside the serial port (net_sol.c), when set */
extern void (*con_mirror_putc)(u8 c);
extern int (*con_mirror_getc)(void);
void halt(void) __attribute__((noreturn));

/* Written per architecture (arch.c / start.S) */
void arch_con_init(void);
void arch_con_putc(u8 c);           /* raw byte, waits for room */
int arch_con_getc(void);            /* a received byte, or -1 */
u64 arch_counter(void);             /* free-running, monotonic */
u64 arch_counter_hz(void);
void arch_relax(void);
void arch_halt(void) __attribute__((noreturn));
void arch_sync_icache(void *p, usize n);
int arch_rtc_seconds(u64 *secs);    /* 1 and seconds since 1970, or 0 */
void arch_reset(int type, u64 status) __attribute__((noreturn));
void arch_memory_reserve(void);     /* efi_memory_mark the firmware's areas */
int arch_disk_probe(void);          /* disks found (efi_disk_add each) */
struct EFI_SYSTEM_TABLE;
/* image_call(entry, handle, system table, &saved_sp): call an image's
 * entry point, return what it returns. image_exit(saved_sp, status):
 * return from that image_call with status, from anywhere inside it. */
u64 image_call(u64 entry, void *handle, void *st, u64 *saved_sp);
void image_exit(u64 saved_sp, u64 status) __attribute__((noreturn));

/* fwcfg.c: QEMU's firmware configuration device, MMIO */
int fwcfg_init(void);
int fwcfg_present(void);
int fwcfg_find(const char *name, u16 *select, u32 *size);
void fwcfg_read(u16 select, void *buf, u32 len);

/* fdt.c: the flattened device tree QEMU hands the firmware */
int fdt_init(const void *blob);
const void *fdt_blob(void);
u32 fdt_size(void);
/* The first node after start (-1: from the root) whose compatible list
 * holds compat, or -1 */
int fdt_find_compatible(const char *compat, int start);
int fdt_path(const char *path);     /* node offset of "/a/b", or -1 */
const void *fdt_prop(int node, const char *name, u32 *len);
int fdt_reg(int node, int index, u64 *base, u64 *size);
u32 fdt_u32(const void *p);
int fdt_next_node(int node, int *depth);   /* -1 at the end */
const char *fdt_node_name(int node);

/* memmap: the machine's memory, from the device tree (x86: fw_cfg's e820) */
struct mem_range {
    u64 base;
    u64 length;
    u32 type;           /* e820: 1 usable, 2 reserved, 3 ACPI, 4 NVS */
};
#define MEM_MAX 32
extern struct mem_range mem_map[MEM_MAX];
extern int mem_count;
void memmap_init(void);

#endif
