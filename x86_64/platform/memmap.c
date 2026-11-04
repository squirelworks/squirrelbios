/* SquirrelBIOS x86_64-uefi: the machine's memory map.
 * Original code.
 *
 * QEMU's "etc/e820" fw_cfg file gives the RAM ranges as 20-byte records
 * (base, length, type). Without it, the CMOS RAM-size registers give
 * the RAM below 4 GB. */
#include "sq.h"

struct mem_range mem_map[MEM_MAX];
int mem_count;

static void add(u64 base, u64 length, u32 type)
{
    if (mem_count < MEM_MAX && length) {
        mem_map[mem_count].base = base;
        mem_map[mem_count].length = length;
        mem_map[mem_count].type = type;
        mem_count++;
    }
}

static u8 cmos(u8 reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

void memmap_init(void)
{
    u16 sel;
    u32 size;
    mem_count = 0;
    if (fwcfg_find("etc/e820", &sel, &size)) {
        u8 rec[20];
        outw(0x510, sel);
        for (u32 off = 0; off + 20 <= size; off += 20) {
            for (int i = 0; i < 20; i++)
                rec[i] = inb(0x511);
            u64 base, length;
            u32 type;
            memcpy(&base, rec, 8);
            memcpy(&length, rec + 8, 8);
            memcpy(&type, rec + 16, 4);
            add(base, length, type);
        }
        if (mem_count)
            return;
    }
    /* CMOS 34h/35h: 64 KB blocks above 16 MB; 30h/31h: KB above 1 MB */
    u64 above16 = ((u64)cmos(0x35) << 8 | cmos(0x34)) << 16;
    u64 above1 = ((u64)cmos(0x31) << 8 | cmos(0x30)) << 10;
    add(0, 0xA0000, 1);
    if (above16)
        add(0x100000, 0xF00000 + above16, 1);
    else
        add(0x100000, above1, 1);
}
