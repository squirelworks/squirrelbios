/* SquirrelBIOS: small arch-neutral C library for the firmware ports that
 * are written in C (ppc64le-powernv, sparc64-sun4u, ...). Freestanding:
 * no libc, no allocation beyond what the caller hands in.
 * Original code.
 *
 * The port provides fw_putc() and fw_getc() (the console) and links in
 * the files of common/fw/ it uses.
 */
#ifndef SQ_FWLIB_H
#define SQ_FWLIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* --- console, provided by the port ----------------------------------- */
void fw_putc(char c);
int fw_getc(void);          /* -1 when nothing is waiting */

/* --- string.c -------------------------------------------------------- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
int fw_parse_u64(const char *s, uint64_t *out);     /* decimal or 0x hex */

/* --- printf.c -------------------------------------------------------- */
/* %c %s %d %i %u %x %X %p, with 0 / width / l / ll / z; %p prints 0x and
 * 16 hex digits */
int fw_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int fw_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int fw_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fw_puts(const char *s);    /* no newline added */

/* --- byte order ------------------------------------------------------ */
static inline uint16_t fw_bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t fw_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t fw_bswap64(uint64_t v) { return __builtin_bswap64(v); }
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FW_BIG_ENDIAN 1
static inline uint32_t fw_be32(uint32_t v) { return v; }
static inline uint64_t fw_be64(uint64_t v) { return v; }
static inline uint16_t fw_le16(uint16_t v) { return fw_bswap16(v); }
static inline uint32_t fw_le32(uint32_t v) { return fw_bswap32(v); }
static inline uint64_t fw_le64(uint64_t v) { return fw_bswap64(v); }
#else
#define FW_BIG_ENDIAN 0
static inline uint32_t fw_be32(uint32_t v) { return fw_bswap32(v); }
static inline uint64_t fw_be64(uint64_t v) { return fw_bswap64(v); }
static inline uint16_t fw_le16(uint16_t v) { return v; }
static inline uint32_t fw_le32(uint32_t v) { return v; }
static inline uint64_t fw_le64(uint64_t v) { return v; }
#endif
/* Unaligned big-endian reads, for device-tree properties */
uint32_t fw_get_be32(const void *p);
uint64_t fw_get_be64(const void *p);
void fw_put_be32(void *p, uint32_t v);
void fw_put_be64(void *p, uint64_t v);

/* --- fdt.c: flattened device tree, see fdt.h --------------------------- */
#include "fdt.h"

/* --- elf.c ----------------------------------------------------------- */
struct elf_image {
    int big_endian;
    int machine;
    uint64_t entry;         /* e_entry as linked */
    uint64_t entry_phys;    /* where to jump, once placed */
    uint64_t low, high;     /* physical range the image occupies, once placed */
    int in_place;           /* 1: runs where it was loaded (p_paddr 0, as Linux vmlinux) */
};
/* Checks an ELF64 executable at 'file' (of 'size' bytes, or 0 when not
 * known) for machine 'machine'. Returns 0 or a negative reason:
 *  -1 not ELF, -2 not ELF64 executable, -3 other machine, -4 no PT_LOAD,
 *  -5 a segment runs past the file */
int elf_probe(const void *file, uint64_t size, int machine, struct elf_image *img);
/* Copies the PT_LOAD segments to their p_paddr and zeroes their BSS
 * (only when !img->in_place) */
void elf_place(const void *file, const struct elf_image *img);
const char *elf_error(int err);

/* --- prompt.c -------------------------------------------------------- */
struct fw_cmd {
    const char *name;
    const char *help;
    void (*run)(int argc, char **argv);
};
/* Reads lines with "sq> " and runs commands until one of them does not
 * return. 'cmds' ends with a { 0 } entry; "help" is built in. */
void fw_prompt(const struct fw_cmd *cmds);
/* Reads one line (echoing, with backspace); returns its length */
int fw_readline(char *buf, int size);

#endif
