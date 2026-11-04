/* SquirrelBIOS x86_64-uefi: firmware main.
 * Original code. */
#include "sq.h"

extern char __fw_start[], __fw_end[];

static const char *mem_type(u32 type)
{
    switch (type) {
    case 1: return "usable";
    case 2: return "reserved";
    case 3: return "ACPI";
    case 4: return "ACPI NVS";
    default: return "other";
    }
}

static void cpu_report(void)
{
    u32 a, b, c, d, max_ext;
    char vendor[13];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    cpuid(0x80000000, &max_ext, &b, &c, &d);
    u32 phys = 36;
    if (max_ext >= 0x80000008) {
        cpuid(0x80000008, &a, &b, &c, &d);
        phys = a & 0xFF;
    }
    printf("cpu: %s, long mode, %u-bit physical addresses\n", vendor, phys);
}

void fw_main(void)
{
    con_init();
    printf("\nSquirrelBIOS %s (x86_64-uefi)\n", SQ_VERSION);
    cpu_report();
    printf("fw: running at %p-%p, first 4 GB mapped 1:1\n", __fw_start, __fw_end);

    memmap_init();
    u64 usable = 0;
    for (int i = 0; i < mem_count; i++) {
        printf("mem: %016lx-%016lx %s\n", mem_map[i].base,
               mem_map[i].base + mem_map[i].length - 1, mem_type(mem_map[i].type));
        if (mem_map[i].type == 1)
            usable += mem_map[i].length;
    }
    printf("mem: %lu MB usable%s\n", usable >> 20, fwcfg_present() ? " (from fw_cfg)" : " (from CMOS)");

    printf("fw: no boot services yet, halting\n");
    halt();
}
