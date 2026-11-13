/* SquirrelBIOS: the flattened device tree reader and writer shared by
 * every firmware written in C: the Phase E ports (common/fw/) and the
 * UEFI core (common/efi/fdt.c wraps it). Stand-alone: needs only
 * memcpy, memmove, memset, strlen, strcmp and strncmp from the firmware.
 * Written from the Devicetree Specification v0.4, chapter 5.
 * Original code.
 *
 * Every reader takes the blob, so a firmware can read QEMU's tree while
 * writing its own. A node is named by the offset of its FDT_BEGIN_NODE
 * token in the structure block; -1 means "not found".
 */
#ifndef SQ_FDT_H
#define SQ_FDT_H

#include <stddef.h>
#include <stdint.h>

static inline uint32_t dtb_be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static inline uint64_t dtb_be64(const void *p)
{
    return (uint64_t)dtb_be32(p) << 32 | dtb_be32((const uint8_t *)p + 4);
}

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

/* Reading. A node is named by the offset of its FDT_BEGIN_NODE token in
 * the structure block; -1 means "not found". */
int dtb_check(const void *fdt);                  /* 0 when the header is sane */
uint32_t dtb_total_size(const void *fdt);
int dtb_root(const void *fdt);
int dtb_path(const void *fdt, const char *path); /* "/cpus/cpu@0"; a unit address may be left out */
const char *dtb_name(const void *fdt, int node);
const void *dtb_prop(const void *fdt, int node, const char *name, int *len);
int dtb_first_child(const void *fdt, int node);
int dtb_next_sibling(const void *fdt, int node);
int dtb_parent(const void *fdt, int node);
/* Next node in document order after 'node' (any depth), -1 at the end */
int dtb_next_node(const void *fdt, int node, int *depth);
/* Property iteration: 'prop' is -1 to start; returns the next property's
 * offset (or -1), and sets *name / *data / *len */
int dtb_next_prop(const void *fdt, int node, int prop, const char **name,
                  const void **data, int *len);
int dtb_compatible(const void *fdt, int node, const char *compat);
int dtb_find_compatible(const void *fdt, int after, const char *compat);
int dtb_find_device_type(const void *fdt, int after, const char *type);
uint32_t dtb_prop_u32(const void *fdt, int node, const char *name, uint32_t dflt);
/* Reads cells (1 or 2) at p as one number */
uint64_t dtb_cells(const void *p, int cells);
/* #address-cells / #size-cells that apply to node's children */
int dtb_addr_cells(const void *fdt, int node);
int dtb_size_cells(const void *fdt, int node);
/* reg entry i of node, translated through the parents' ranges to a CPU
 * address; 0 on success. Address cells above 2 (e.g. ISA's space cell)
 * are handled as the space plus a 1- or 2-cell offset. */
int dtb_reg(const void *fdt, int node, int i, uint64_t *addr, uint64_t *size);
uint32_t dtb_phandle(const void *fdt, int node);
int dtb_by_phandle(const void *fdt, uint32_t ph);

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

/* Prints the tree under node as text with fw_printf (fdt_print.c) */
void dtb_print(const void *fdt, int node);

#endif
