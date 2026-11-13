/* SquirrelBIOS common/fw: flattened device tree reader and writer.
 * Written from the Devicetree Specification v0.4, chapter 5 (FDT format).
 * Original code.
 *
 * Node handles are offsets into the structure block. Works in either
 * byte order (the blob is always big-endian). */
#include "fwlib.h"

#define HDR_MAGIC        0
#define HDR_TOTALSIZE    4
#define HDR_OFF_STRUCT   8
#define HDR_OFF_STRINGS  12
#define HDR_OFF_RSVMAP   16
#define HDR_VERSION      20
#define HDR_LAST_COMP    24
#define HDR_BOOT_CPU     28
#define HDR_SIZE_STRINGS 32
#define HDR_SIZE_STRUCT  36
#define HDR_LEN          40

static uint32_t hdr(const void *fdt, int off)
{
    return fw_get_be32((const uint8_t *)fdt + off);
}

static const uint8_t *sblock(const void *fdt)
{
    return (const uint8_t *)fdt + hdr(fdt, HDR_OFF_STRUCT);
}

static const char *strblock(const void *fdt)
{
    return (const char *)fdt + hdr(fdt, HDR_OFF_STRINGS);
}

static uint32_t align4(uint32_t v)
{
    return (v + 3) & ~3u;
}

int fdt_check(const void *fdt)
{
    if (!fdt || hdr(fdt, HDR_MAGIC) != FDT_MAGIC)
        return -1;
    uint32_t total = hdr(fdt, HDR_TOTALSIZE);
    if (hdr(fdt, HDR_VERSION) < 16 || total < HDR_LEN || total > 0x4000000)
        return -1;
    if (hdr(fdt, HDR_OFF_STRUCT) >= total || hdr(fdt, HDR_OFF_STRINGS) > total)
        return -1;
    return 0;
}

uint32_t fdt_total_size(const void *fdt)
{
    return hdr(fdt, HDR_TOTALSIZE);
}

/* Tag at off; *next is the offset just past it (and its payload) */
static uint32_t tag_at(const void *fdt, int off, int *next)
{
    const uint8_t *s = sblock(fdt);
    uint32_t tag = fw_get_be32(s + off);
    int n = off + 4;
    switch (tag) {
    case FDT_BEGIN_NODE:
        n += align4((uint32_t)strlen((const char *)s + n) + 1);
        break;
    case FDT_PROP:
        n += 8 + align4(fw_get_be32(s + n));
        break;
    }
    *next = n;
    return tag;
}

int fdt_root(const void *fdt)
{
    int off = 0, next;
    while (tag_at(fdt, off, &next) == FDT_NOP)
        off = next;
    return tag_at(fdt, off, &next) == FDT_BEGIN_NODE ? off : -1;
}

const char *fdt_name(const void *fdt, int node)
{
    return (const char *)sblock(fdt) + node + 4;
}

/* Offset just past node's END_NODE */
static int node_end(const void *fdt, int node)
{
    int off = node, next, depth = 0;
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t == FDT_BEGIN_NODE)
            depth++;
        else if (t == FDT_END_NODE && --depth == 0)
            return next;
        else if (t == FDT_END)
            return -1;
        off = next;
    }
}

/* First token after node's name and properties */
static int after_props(const void *fdt, int node)
{
    int off, next;
    tag_at(fdt, node, &off);
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t != FDT_PROP && t != FDT_NOP)
            return off;
        off = next;
    }
}

int fdt_first_child(const void *fdt, int node)
{
    int off = after_props(fdt, node), next;
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t == FDT_BEGIN_NODE)
            return off;
        if (t != FDT_NOP)
            return -1;
        off = next;
    }
}

int fdt_next_sibling(const void *fdt, int node)
{
    int off = node_end(fdt, node), next;
    if (off < 0)
        return -1;
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t == FDT_BEGIN_NODE)
            return off;
        if (t != FDT_NOP)
            return -1;
        off = next;
    }
}

int fdt_next_node(const void *fdt, int node, int *depth)
{
    int off, next;
    tag_at(fdt, node, &off);
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t == FDT_BEGIN_NODE) {
            if (depth)
                (*depth)++;
            return off;
        }
        if (t == FDT_END_NODE) {
            if (depth)
                (*depth)--;
        } else if (t == FDT_END) {
            return -1;
        }
        off = next;
    }
}

int fdt_parent(const void *fdt, int node)
{
    /* Walk from the root keeping a stack of open nodes */
    int stack[32], depth = 0, off = fdt_root(fdt), next;
    if (off == node)
        return -1;
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t == FDT_BEGIN_NODE) {
            if (off == node)
                return depth ? stack[depth - 1] : -1;
            if (depth == 32)
                return -1;
            stack[depth++] = off;
        } else if (t == FDT_END_NODE) {
            if (--depth < 0)
                return -1;
        } else if (t == FDT_END) {
            return -1;
        }
        off = next;
    }
}

int fdt_next_prop(const void *fdt, int node, int prop, const char **name,
                  const void **data, int *len)
{
    int off, next;
    if (prop < 0)
        tag_at(fdt, node, &off);
    else
        tag_at(fdt, prop, &off);
    for (;;) {
        uint32_t t = tag_at(fdt, off, &next);
        if (t == FDT_PROP) {
            const uint8_t *s = sblock(fdt) + off;
            if (len)
                *len = (int)fw_get_be32(s + 4);
            if (name)
                *name = strblock(fdt) + fw_get_be32(s + 8);
            if (data)
                *data = s + 12;
            return off;
        }
        if (t != FDT_NOP)
            return -1;
        off = next;
    }
}

const void *fdt_prop(const void *fdt, int node, const char *name, int *len)
{
    const char *n;
    const void *d;
    int l;
    if (node < 0)
        return 0;
    for (int p = fdt_next_prop(fdt, node, -1, &n, &d, &l); p >= 0;
         p = fdt_next_prop(fdt, node, p, &n, &d, &l)) {
        if (!strcmp(n, name)) {
            if (len)
                *len = l;
            return d;
        }
    }
    return 0;
}

/* Does a node name match a path component? "cpu" matches "cpu@0" */
static int name_matches(const char *name, const char *comp, int clen)
{
    if (strncmp(name, comp, clen))
        return 0;
    return name[clen] == 0 || (name[clen] == '@' && !strchr(comp, '@'));
}

int fdt_path(const void *fdt, const char *path)
{
    int node = fdt_root(fdt);
    if (node < 0 || *path != '/')
        return -1;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        int clen = 0;
        while (path[clen] && path[clen] != '/')
            clen++;
        int c;
        for (c = fdt_first_child(fdt, node); c >= 0; c = fdt_next_sibling(fdt, c)) {
            const char *n = fdt_name(fdt, c);
            /* name_matches needs comp NUL- or '/'-bounded: compare a copy */
            char comp[64];
            if (clen >= (int)sizeof(comp))
                return -1;
            memcpy(comp, path, clen);
            comp[clen] = 0;
            if (name_matches(n, comp, clen))
                break;
        }
        if (c < 0)
            return -1;
        node = c;
        path += clen;
    }
    return node;
}

int fdt_compatible(const void *fdt, int node, const char *compat)
{
    int len;
    const char *p = fdt_prop(fdt, node, "compatible", &len);
    if (!p)
        return 0;
    for (int i = 0; i < len; i += (int)strlen(p + i) + 1)
        if (!strcmp(p + i, compat))
            return 1;
    return 0;
}

int fdt_find_compatible(const void *fdt, int after, const char *compat)
{
    int node = after < 0 ? fdt_root(fdt) : fdt_next_node(fdt, after, 0);
    for (; node >= 0; node = fdt_next_node(fdt, node, 0))
        if (fdt_compatible(fdt, node, compat))
            return node;
    return -1;
}

int fdt_find_device_type(const void *fdt, int after, const char *type)
{
    int node = after < 0 ? fdt_root(fdt) : fdt_next_node(fdt, after, 0);
    for (; node >= 0; node = fdt_next_node(fdt, node, 0)) {
        const char *t = fdt_prop(fdt, node, "device_type", 0);
        if (t && !strcmp(t, type))
            return node;
    }
    return -1;
}

uint32_t fdt_prop_u32(const void *fdt, int node, const char *name, uint32_t dflt)
{
    int len;
    const void *p = fdt_prop(fdt, node, name, &len);
    return p && len >= 4 ? fw_get_be32(p) : dflt;
}

uint64_t fdt_cells(const void *p, int cells)
{
    return cells == 2 ? fw_get_be64(p) : cells == 1 ? fw_get_be32(p) : 0;
}

int fdt_addr_cells(const void *fdt, int node)
{
    return (int)fdt_prop_u32(fdt, node, "#address-cells", 2);
}

int fdt_size_cells(const void *fdt, int node)
{
    return (int)fdt_prop_u32(fdt, node, "#size-cells", 1);
}

/* Does an address on bus start with a space cell? PCI-style buses (3
 * cells) do, and so do ISA/LPC buses with 2 (space, offset) */
static int space_cell(const void *fdt, int bus, int ac)
{
    if (ac > 2)
        return 1;
    if (ac != 2 || bus < 0)
        return 0;
    const char *n = fdt_name(fdt, bus);
    return !strncmp(n, "isa", 3) || !strncmp(n, "lpc", 3) || !strncmp(n, "ebus", 4);
}

/* An address of 'ac' cells on bus: the space cell if any, then the offset */
static void split_addr(const void *fdt, int bus, const uint8_t *p, int ac,
                       uint32_t *space, uint64_t *off)
{
    if (space_cell(fdt, bus, ac)) {
        *space = fw_get_be32(p);
        *off = fdt_cells(p + 4, ac - 1 > 2 ? 2 : ac - 1);
    } else {
        *space = 0;
        *off = fdt_cells(p, ac);
    }
}

/* Translates (space, addr) on node's child bus to node's parent bus */
static int translate(const void *fdt, int bus, uint32_t *space, uint64_t *addr)
{
    int len;
    const uint8_t *r = fdt_prop(fdt, bus, "ranges", &len);
    if (!r)
        return -1;
    if (len == 0)
        return 0;           /* identity */
    int parent = fdt_parent(fdt, bus);
    int cac = fdt_addr_cells(fdt, bus);
    int pac = parent >= 0 ? fdt_addr_cells(fdt, parent) : 2;
    int cs = fdt_size_cells(fdt, bus);
    int step = 4 * (cac + pac + cs);
    for (int i = 0; i + step <= len; i += step) {
        uint32_t csp, psp;
        uint64_t cbase, pbase;
        split_addr(fdt, bus, r + i, cac, &csp, &cbase);
        split_addr(fdt, parent, r + i + 4 * cac, pac, &psp, &pbase);
        uint64_t size = fdt_cells(r + i + 4 * (cac + pac), cs);
        if (csp == *space && *addr >= cbase && *addr - cbase < size) {
            *space = psp;
            *addr = *addr - cbase + pbase;
            return 0;
        }
    }
    return -1;
}

int fdt_reg(const void *fdt, int node, int i, uint64_t *addr, uint64_t *size)
{
    int len, bus = fdt_parent(fdt, node);
    if (bus < 0)
        return -1;
    const uint8_t *r = fdt_prop(fdt, node, "reg", &len);
    int ac = fdt_addr_cells(fdt, bus), sc = fdt_size_cells(fdt, bus);
    int step = 4 * (ac + sc);
    if (!r || (i + 1) * step > len)
        return -1;
    uint32_t space;
    uint64_t a;
    split_addr(fdt, bus, r + i * step, ac, &space, &a);
    if (size)
        *size = fdt_cells(r + i * step + 4 * ac, sc);
    /* Up the tree to the root's bus */
    while (fdt_parent(fdt, bus) >= 0) {
        if (translate(fdt, bus, &space, &a))
            return -1;
        bus = fdt_parent(fdt, bus);
    }
    *addr = a;
    return 0;
}

uint32_t fdt_phandle(const void *fdt, int node)
{
    uint32_t ph = fdt_prop_u32(fdt, node, "phandle", 0);
    return ph ? ph : fdt_prop_u32(fdt, node, "linux,phandle", 0);
}

int fdt_by_phandle(const void *fdt, uint32_t ph)
{
    for (int n = fdt_root(fdt); n >= 0; n = fdt_next_node(fdt, n, 0))
        if (fdt_phandle(fdt, n) == ph)
            return n;
    return -1;
}

/* --- writer --------------------------------------------------------- */

void fdtw_init(struct fdt_writer *w, void *buf, uint32_t size)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->size = size;
    /* Structure block after the header and a reserve map of up to 16
     * entries (+ terminator); strings at the end, growing down */
    w->struct_off = HDR_LEN + 17 * 16;
    w->strings = w->buf + size;
    if (size < w->struct_off + 64)
        w->error = 1;
}

void fdtw_reserve(struct fdt_writer *w, uint64_t addr, uint64_t size)
{
    if (w->rsv_count == 16) {
        w->error = 1;
        return;
    }
    w->rsv[2 * w->rsv_count] = addr;
    w->rsv[2 * w->rsv_count + 1] = size;
    w->rsv_count++;
}

static uint8_t *room(struct fdt_writer *w, uint32_t n)
{
    uint8_t *p = w->buf + w->struct_off + w->struct_len;
    if (w->error || p + n > w->strings) {
        w->error = 1;
        return 0;
    }
    w->struct_len += n;
    return p;
}

static void put_tag(struct fdt_writer *w, uint32_t tag)
{
    uint8_t *p = room(w, 4);
    if (p)
        fw_put_be32(p, tag);
}

void fdtw_begin(struct fdt_writer *w, const char *name)
{
    uint32_t n = (uint32_t)strlen(name) + 1;
    put_tag(w, FDT_BEGIN_NODE);
    uint8_t *p = room(w, align4(n));
    if (p) {
        memset(p, 0, align4(n));
        memcpy(p, name, n);
    }
    w->depth++;
}

void fdtw_end(struct fdt_writer *w)
{
    put_tag(w, FDT_END_NODE);
    if (--w->depth < 0)
        w->error = 1;
}

/* Offset of name in the strings block (strings are stored back to front
 * at the end of buf and moved into place by fdtw_finish) */
static uint32_t string_off(struct fdt_writer *w, const char *name)
{
    uint32_t n = (uint32_t)strlen(name) + 1;
    /* Look for it among the strings already added */
    for (uint32_t off = 0; off < w->strings_len;) {
        const char *s = (const char *)w->strings + off;
        if (!strcmp(s, name))
            return off;
        off += (uint32_t)strlen(s) + 1;
    }
    uint8_t *end = w->buf + w->struct_off + w->struct_len;
    if (w->error || w->strings - n < end) {
        w->error = 1;
        return 0;
    }
    /* Grow down: move existing strings n bytes lower, append at the end */
    memmove(w->strings - n, w->strings, w->strings_len);
    w->strings -= n;
    memcpy(w->strings + w->strings_len, name, n);
    w->strings_len += n;
    return w->strings_len - n;
}

void fdtw_prop(struct fdt_writer *w, const char *name, const void *data, int len)
{
    uint32_t soff = string_off(w, name);
    put_tag(w, FDT_PROP);
    uint8_t *p = room(w, 8 + align4((uint32_t)len));
    if (!p)
        return;
    fw_put_be32(p, (uint32_t)len);
    fw_put_be32(p + 4, soff);
    memset(p + 8, 0, align4((uint32_t)len));
    memcpy(p + 8, data, (size_t)len);
}

void fdtw_prop_u32(struct fdt_writer *w, const char *name, uint32_t v)
{
    uint8_t b[4];
    fw_put_be32(b, v);
    fdtw_prop(w, name, b, 4);
}

void fdtw_prop_u64(struct fdt_writer *w, const char *name, uint64_t v)
{
    uint8_t b[8];
    fw_put_be64(b, v);
    fdtw_prop(w, name, b, 8);
}

void fdtw_prop_str(struct fdt_writer *w, const char *name, const char *s)
{
    fdtw_prop(w, name, s, (int)strlen(s) + 1);
}

void fdtw_prop_str2(struct fdt_writer *w, const char *name, const char *a, const char *b)
{
    char tmp[128];
    int la = (int)strlen(a) + 1, lb = (int)strlen(b) + 1;
    if (la + lb > (int)sizeof(tmp)) {
        w->error = 1;
        return;
    }
    memcpy(tmp, a, la);
    memcpy(tmp + la, b, lb);
    fdtw_prop(w, name, tmp, la + lb);
}

void fdtw_prop_empty(struct fdt_writer *w, const char *name)
{
    fdtw_prop(w, name, "", 0);
}

void fdtw_copy(struct fdt_writer *w, const void *src, int node, fdtw_hook hook,
               fdtw_skip skip, void *ctx)
{
    const char *name;
    const void *data;
    int len;
    fdtw_begin(w, fdt_name(src, node));
    for (int p = fdt_next_prop(src, node, -1, &name, &data, &len); p >= 0;
         p = fdt_next_prop(src, node, p, &name, &data, &len))
        if (!skip || !skip(src, node, name, ctx))
            fdtw_prop(w, name, data, len);
    if (hook)
        hook(w, src, node, ctx);
    for (int c = fdt_first_child(src, node); c >= 0; c = fdt_next_sibling(src, c))
        fdtw_copy(w, src, c, hook, skip, ctx);
    fdtw_end(w);
}

uint32_t fdtw_finish(struct fdt_writer *w, uint32_t boot_cpu)
{
    put_tag(w, FDT_END);
    if (w->error || w->depth != 0)
        return 0;
    uint8_t *b = w->buf;
    uint32_t rsv_off = HDR_LEN;
    uint32_t str_off = w->struct_off + w->struct_len;
    uint32_t total = align4(str_off + w->strings_len);
    memmove(b + str_off, w->strings, w->strings_len);
    for (uint32_t i = 0; i < w->rsv_count; i++) {
        fw_put_be64(b + rsv_off + 16 * i, w->rsv[2 * i]);
        fw_put_be64(b + rsv_off + 16 * i + 8, w->rsv[2 * i + 1]);
    }
    /* Terminator, and zeros over the unused reserve slots */
    memset(b + rsv_off + 16 * w->rsv_count, 0, 16 * (17 - w->rsv_count));
    memset(b + str_off + w->strings_len, 0, total - str_off - w->strings_len);
    fw_put_be32(b + HDR_MAGIC, FDT_MAGIC);
    fw_put_be32(b + HDR_TOTALSIZE, total);
    fw_put_be32(b + HDR_OFF_STRUCT, w->struct_off);
    fw_put_be32(b + HDR_OFF_STRINGS, str_off);
    fw_put_be32(b + HDR_OFF_RSVMAP, rsv_off);
    fw_put_be32(b + HDR_VERSION, 17);
    fw_put_be32(b + HDR_LAST_COMP, 16);
    fw_put_be32(b + HDR_BOOT_CPU, boot_cpu);
    fw_put_be32(b + HDR_SIZE_STRINGS, w->strings_len);
    fw_put_be32(b + HDR_SIZE_STRUCT, w->struct_len);
    return total;
}

/* --- printing --------------------------------------------------------- */

static int printable(const char *p, int len)
{
    if (len == 0 || p[len - 1])
        return 0;
    for (int i = 0; i < len - 1; i++)
        if ((p[i] < 32 || p[i] > 126) && p[i] != 0)
            return 0;
    return p[0] != 0;
}

static void print_node(const void *fdt, int node, int depth)
{
    const char *name;
    const void *data;
    int len;
    fw_printf("%*s%s {\n", depth * 2, "", depth == 0 && !*fdt_name(fdt, node) ? "/" : fdt_name(fdt, node));
    for (int p = fdt_next_prop(fdt, node, -1, &name, &data, &len); p >= 0;
         p = fdt_next_prop(fdt, node, p, &name, &data, &len)) {
        const uint8_t *d = data;
        fw_printf("%*s%s", depth * 2 + 2, "", name);
        if (len && printable(data, len)) {
            fw_puts(" = \"");
            for (int i = 0; i < len - 1; i++)
                fw_putc(d[i] ? (char)d[i] : ',');
            fw_puts("\"");
        } else if (len && len % 4 == 0) {
            fw_puts(" = <");
            for (int i = 0; i < len && i < 64; i += 4)
                fw_printf("%s0x%x", i ? " " : "", fw_get_be32(d + i));
            fw_puts(len > 64 ? " ...>" : ">");
        } else if (len) {
            fw_puts(" = [");
            for (int i = 0; i < len && i < 32; i++)
                fw_printf("%s%02x", i ? " " : "", d[i]);
            fw_puts(len > 32 ? " ...]" : "]");
        }
        fw_puts(";\n");
    }
    for (int c = fdt_first_child(fdt, node); c >= 0; c = fdt_next_sibling(fdt, c))
        print_node(fdt, c, depth + 1);
    fw_printf("%*s};\n", depth * 2, "");
}

void fdt_print(const void *fdt, int node)
{
    print_node(fdt, node, 0);
}
