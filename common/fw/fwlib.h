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

/* --- fdt.c: flattened device tree (devicetree spec v0.4, version 17) -- */
#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

/* Reading. A node is named by the offset of its FDT_BEGIN_NODE token in
 * the structure block; -1 means "not found". */
int fdt_check(const void *fdt);                  /* 0 when the header is sane */
uint32_t fdt_total_size(const void *fdt);
int fdt_root(const void *fdt);
int fdt_path(const void *fdt, const char *path); /* "/cpus/cpu@0"; a unit address may be left out */
const char *fdt_name(const void *fdt, int node);
const void *fdt_prop(const void *fdt, int node, const char *name, int *len);
int fdt_first_child(const void *fdt, int node);
int fdt_next_sibling(const void *fdt, int node);
int fdt_parent(const void *fdt, int node);
/* Next node in document order after 'node' (any depth), -1 at the end */
int fdt_next_node(const void *fdt, int node, int *depth);
/* Property iteration: 'prop' is -1 to start; returns the next property's
 * offset (or -1), and sets *name / *data / *len */
int fdt_next_prop(const void *fdt, int node, int prop, const char **name,
                  const void **data, int *len);
int fdt_compatible(const void *fdt, int node, const char *compat);
int fdt_find_compatible(const void *fdt, int after, const char *compat);
int fdt_find_device_type(const void *fdt, int after, const char *type);
uint32_t fdt_prop_u32(const void *fdt, int node, const char *name, uint32_t dflt);
/* Reads cells (1 or 2) at p as one number */
uint64_t fdt_cells(const void *p, int cells);
/* #address-cells / #size-cells that apply to node's children */
int fdt_addr_cells(const void *fdt, int node);
int fdt_size_cells(const void *fdt, int node);
/* reg entry i of node, translated through the parents' ranges to a CPU
 * address; 0 on success. Address cells above 2 (e.g. ISA's space cell)
 * are handled as the space plus a 1- or 2-cell offset. */
int fdt_reg(const void *fdt, int node, int i, uint64_t *addr, uint64_t *size);
uint32_t fdt_phandle(const void *fdt, int node);
int fdt_by_phandle(const void *fdt, uint32_t ph);

/* Writing: a tree is built into a caller buffer, one node at a time. */
struct fdt_writer {
    uint8_t *buf;
    uint32_t size;      /* of buf */
    uint32_t struct_off, struct_len;
    uint8_t *strings;   /* built at the end of buf, growing down */
    uint32_t strings_len;
    uint32_t rsv_count;
    uint64_t rsv[2 * 16];
    int depth;
    int error;          /* set when buf ran out or nodes are unbalanced */
};
void fdtw_init(struct fdt_writer *w, void *buf, uint32_t size);
void fdtw_reserve(struct fdt_writer *w, uint64_t addr, uint64_t size);  /* before the first node */
void fdtw_begin(struct fdt_writer *w, const char *name);
void fdtw_end(struct fdt_writer *w);
void fdtw_prop(struct fdt_writer *w, const char *name, const void *data, int len);
void fdtw_prop_u32(struct fdt_writer *w, const char *name, uint32_t v);
void fdtw_prop_u64(struct fdt_writer *w, const char *name, uint64_t v);
void fdtw_prop_str(struct fdt_writer *w, const char *name, const char *s);
/* Two strings, for "compatible" lists */
void fdtw_prop_str2(struct fdt_writer *w, const char *name, const char *a, const char *b);
void fdtw_prop_empty(struct fdt_writer *w, const char *name);
/* Copies node (from another tree, with its properties and children);
 * hook (may be 0) is called after a node's own properties are written,
 * with the source node, so it can add properties and child nodes. A
 * property for which skip returns non-zero is not copied. */
typedef void (*fdtw_hook)(struct fdt_writer *w, const void *src, int node, void *ctx);
typedef int (*fdtw_skip)(const void *src, int node, const char *prop, void *ctx);
void fdtw_copy(struct fdt_writer *w, const void *src, int node, fdtw_hook hook,
               fdtw_skip skip, void *ctx);
/* Finishes the blob: returns its size, or 0 when it did not fit */
uint32_t fdtw_finish(struct fdt_writer *w, uint32_t boot_cpu);

/* Prints the tree under node as text (for the prompt's "dt" command) */
void fdt_print(const void *fdt, int node);

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
