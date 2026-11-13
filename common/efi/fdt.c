/* SquirrelBIOS UEFI core (common/efi): reading the flattened device tree.
 * Original code, written from the Devicetree Specification (v0.4),
 * chapter 5: a header, a structure block of big-endian tokens
 * (BEGIN_NODE name, PROP len nameoff value, END_NODE, NOP, END) and a
 * strings block. A node is named by the offset of its BEGIN_NODE token
 * in the structure block; the root is offset 0 when the tree is sane. */
#include "sq.h"

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9
#define MAX_DEPTH      32

static const u8 *blob;
static const u8 *dt_struct;
static const char *dt_strings;
static u32 struct_size, strings_size, total;

int fdt_init(const void *p)
{
    const u8 *b = p;
    if (!b || be32(b) != FDT_MAGIC)
        return 0;
    total = be32(b + 4);
    u32 off_struct = be32(b + 8), off_strings = be32(b + 12);
    u32 version = be32(b + 20);
    strings_size = be32(b + 32);
    struct_size = version >= 17 ? be32(b + 36) : total - off_struct;
    if (total < 40 || off_struct >= total || off_strings >= total ||
        (u64)off_struct + struct_size > total || (u64)off_strings + strings_size > total)
        return 0;
    blob = b;
    dt_struct = b + off_struct;
    dt_strings = (const char *)b + off_strings;
    return 1;
}

const void *fdt_blob(void) { return blob; }
u32 fdt_size(void) { return blob ? total : 0; }
u32 fdt_u32(const void *p) { return be32(p); }

static u32 tok(u32 off) { return off + 4 <= struct_size ? be32(dt_struct + off) : FDT_END; }
static u32 align4(u32 v) { return (v + 3) & ~3u; }

/* The offset just past the token at off */
static u32 skip(u32 off)
{
    switch (tok(off)) {
    case FDT_BEGIN_NODE:
        return align4(off + 4 + (u32)strlen((const char *)dt_struct + off + 4) + 1);
    case FDT_PROP:
        return align4(off + 12 + be32(dt_struct + off + 4));
    default:
        return off + 4;
    }
}

const char *fdt_node_name(int node)
{
    return (const char *)dt_struct + node + 4;
}

/* The next BEGIN_NODE after node (node -1: the root), with *depth moved
 * by the nodes entered and left on the way. -1 at the end. */
int fdt_next_node(int node, int *depth)
{
    if (!blob)
        return -1;
    u32 off = 0;
    if (node >= 0) {
        off = skip((u32)node);
        if (depth)
            ++*depth;
    }
    for (;;) {
        switch (tok(off)) {
        case FDT_BEGIN_NODE:
            return (int)off;
        case FDT_END_NODE:
            if (depth)
                --*depth;
            break;
        case FDT_PROP:
        case FDT_NOP:
            break;
        default:
            return -1;
        }
        off = skip(off);
    }
}

const void *fdt_prop(int node, const char *name, u32 *len)
{
    if (!blob || node < 0)
        return NULL;
    for (u32 off = skip((u32)node);; off = skip(off)) {
        u32 t = tok(off);
        if (t == FDT_NOP)
            continue;
        if (t != FDT_PROP)
            return NULL;
        u32 plen = be32(dt_struct + off + 4), nameoff = be32(dt_struct + off + 8);
        if (nameoff < strings_size && !strcmp(dt_strings + nameoff, name)) {
            if (len)
                *len = plen;
            return dt_struct + off + 12;
        }
    }
}

/* Does name match the path component c (length n)? "memory" matches
 * "memory@40000000" when the component has no unit address. */
static int name_match(const char *name, const char *c, usize n)
{
    if (strncmp(name, c, n))
        return 0;
    if (name[n] == 0)
        return 1;
    for (usize i = 0; i < n; i++)
        if (c[i] == '@')
            return 0;
    return name[n] == '@';
}

int fdt_path(const char *path)
{
    if (!blob || *path != '/')
        return -1;
    int node = fdt_next_node(-1, NULL), depth = 0;
    for (const char *p = path + 1; *p;) {
        usize n = 0;
        while (p[n] && p[n] != '/')
            n++;
        /* look among node's children (depth + 1) */
        int want = depth + 1, d = depth, found = -1;
        for (int c = fdt_next_node(node, &d); c >= 0 && d >= want; c = fdt_next_node(c, &d))
            if (d == want && name_match(fdt_node_name(c), p, n)) {
                found = c;
                break;
            }
        if (found < 0)
            return -1;
        node = found;
        depth = want;
        p += n;
        while (*p == '/')
            p++;
    }
    return node;
}

/* Is the string list s (len bytes) holding str? */
static int list_has(const char *s, u32 len, const char *str)
{
    for (u32 i = 0; i < len;) {
        if (!strcmp(s + i, str))
            return 1;
        i += (u32)strlen(s + i) + 1;
    }
    return 0;
}

int fdt_find_compatible(const char *compat, int start)
{
    for (int n = fdt_next_node(start, NULL); n >= 0; n = fdt_next_node(n, NULL)) {
        u32 len;
        const char *c = fdt_prop(n, "compatible", &len);
        if (c && list_has(c, len, compat))
            return n;
    }
    return -1;
}

/* The parent of node, found by walking from the root */
static int parent_of(int node)
{
    int stack[MAX_DEPTH], depth = 0;
    for (int n = fdt_next_node(-1, &depth); n >= 0; n = fdt_next_node(n, &depth)) {
        if (depth < 0 || depth >= MAX_DEPTH)
            return -1;
        stack[depth] = n;
        if (n == node)
            return depth ? stack[depth - 1] : -1;
    }
    return -1;
}

static u32 cells(int node, const char *name, u32 dflt)
{
    u32 len;
    const u8 *p = fdt_prop(node, name, &len);
    return p && len == 4 ? be32(p) : dflt;
}

int fdt_reg(int node, int index, u64 *base, u64 *size)
{
    int parent = parent_of(node);
    if (parent < 0)
        return 0;
    u32 ac = cells(parent, "#address-cells", 2), sc = cells(parent, "#size-cells", 1);
    u32 len;
    const u8 *p = fdt_prop(node, "reg", &len);
    u32 entry = (ac + sc) * 4;
    if (!p || ac > 2 || sc > 2 || !entry || (u64)(index + 1) * entry > len)
        return 0;
    p += index * entry;
    u64 b = 0, s = 0;
    for (u32 i = 0; i < ac; i++, p += 4)
        b = b << 32 | be32(p);
    for (u32 i = 0; i < sc; i++, p += 4)
        s = s << 32 | be32(p);
    *base = b;
    if (size)
        *size = s;
    return 1;
}
