/* SquirrelBIOS common/fw: the device tree as text, for the sq> prompt's
 * "dt" command. Original code. */
#include "fwlib.h"

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
    fw_printf("%*s%s {\n", depth * 2, "", depth == 0 && !*dtb_name(fdt, node) ? "/" : dtb_name(fdt, node));
    for (int p = dtb_next_prop(fdt, node, -1, &name, &data, &len); p >= 0;
         p = dtb_next_prop(fdt, node, p, &name, &data, &len)) {
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
                fw_printf("%s0x%x", i ? " " : "", dtb_be32(d + i));
            fw_puts(len > 64 ? " ...>" : ">");
        } else if (len) {
            fw_puts(" = [");
            for (int i = 0; i < len && i < 32; i++)
                fw_printf("%s%02x", i ? " " : "", d[i]);
            fw_puts(len > 32 ? " ...]" : "]");
        }
        fw_puts(";\n");
    }
    for (int c = dtb_first_child(fdt, node); c >= 0; c = dtb_next_sibling(fdt, c))
        print_node(fdt, c, depth + 1);
    fw_printf("%*s};\n", depth * 2, "");
}

void dtb_print(const void *fdt, int node)
{
    print_node(fdt, node, 0);
}
