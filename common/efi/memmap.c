/* SquirrelBIOS UEFI core (common/efi): the machine's memory, from the
 * device tree. Original code. Every node with device_type "memory"
 * gives usable ranges (all its reg entries); the children of
 * /reserved-memory are marked reserved on top of them. */
#include "sq.h"

struct mem_range mem_map[MEM_MAX];
int mem_count;

static void add(u64 base, u64 len, u32 type)
{
    if (!len || mem_count >= MEM_MAX)
        return;
    int i = mem_count++;
    while (i > 0 && mem_map[i - 1].base > base) {
        mem_map[i] = mem_map[i - 1];
        i--;
    }
    mem_map[i].base = base;
    mem_map[i].length = len;
    mem_map[i].type = type;
}

void memmap_init(void)
{
    mem_count = 0;
    for (int n = fdt_next_node(-1, NULL); n >= 0; n = fdt_next_node(n, NULL)) {
        u32 len;
        const char *t = fdt_prop(n, "device_type", &len);
        if (!t || strcmp(t, "memory"))
            continue;
        u64 b, s;
        for (int i = 0; fdt_reg(n, i, &b, &s); i++)
            add(b, s, 1);
    }
    int rm = fdt_path("/reserved-memory");
    if (rm >= 0) {
        int depth = 0;
        for (int n = fdt_next_node(rm, &depth); n >= 0 && depth > 0; n = fdt_next_node(n, &depth)) {
            u64 b, s;
            if (depth == 1 && fdt_reg(n, 0, &b, &s))
                add(b, s, 2);
        }
    }
}
