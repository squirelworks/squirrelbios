/* SquirrelBIOS UEFI core (common/efi): reading the flattened device tree.
 * The tree QEMU hands over is kept here as "the" tree; the parsing is
 * the shared reader in common/fw/fdt.c (fdt.h), so the UEFI core and the
 * Phase E ports read device trees with the same code.
 * Original code. */
#include "sq.h"
#include "../fw/fdt.h"

static const void *blob;

int fdt_init(const void *p)
{
    if (!p || dtb_check(p) || dtb_root(p) < 0)
        return 0;
    blob = p;
    return 1;
}

const void *fdt_blob(void) { return blob; }
u32 fdt_size(void) { return blob ? dtb_total_size(blob) : 0; }
u32 fdt_u32(const void *p) { return be32(p); }

const char *fdt_node_name(int node)
{
    return dtb_name(blob, node);
}

/* The next node after node (node -1: the root), with *depth moved by the
 * nodes entered and left on the way. -1 at the end. */
int fdt_next_node(int node, int *depth)
{
    if (!blob)
        return -1;
    if (node < 0)
        return dtb_root(blob);
    return dtb_next_node(blob, node, depth);
}

const void *fdt_prop(int node, const char *name, u32 *len)
{
    int l;
    if (!blob || node < 0)
        return NULL;
    const void *p = dtb_prop(blob, node, name, &l);
    if (p && len)
        *len = (u32)l;
    return p;
}

/* "memory" matches "memory@40000000" when the component has no unit address */
int fdt_path(const char *path)
{
    return blob ? dtb_path(blob, path) : -1;
}

int fdt_find_compatible(const char *compat, int start)
{
    return blob ? dtb_find_compatible(blob, start, compat) : -1;
}

/* reg entry index as written (#address-cells and #size-cells of the
 * parent, at most 2 each), not translated through ranges: the UEFI
 * ports' devices sit on buses that map 1:1 */
int fdt_reg(int node, int index, u64 *base, u64 *size)
{
    int parent = blob && node >= 0 ? dtb_parent(blob, node) : -1;
    if (parent < 0)
        return 0;
    u32 ac = dtb_prop_u32(blob, parent, "#address-cells", 2);
    u32 sc = dtb_prop_u32(blob, parent, "#size-cells", 1);
    int len;
    const u8 *p = dtb_prop(blob, node, "reg", &len);
    u32 entry = (ac + sc) * 4;
    if (!p || ac > 2 || sc > 2 || !entry || (u64)(index + 1) * entry > (u64)len)
        return 0;
    p += index * entry;
    *base = dtb_cells(p, (int)ac);
    if (size)
        *size = dtb_cells(p + 4 * ac, (int)sc);
    return 1;
}
