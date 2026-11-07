/* SquirrelBIOS x86_64-uefi: firmware main.
 * Original code. */
#include "efi.h"

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

static const char *efi_type_name(u32 t)
{
    static const char *const names[] = {
        "reserved", "loader code", "loader data", "BS code", "BS data",
        "RT code", "RT data", "free", "unusable", "ACPI reclaim", "ACPI NVS",
        "MMIO", "MMIO port", "PAL code", "persistent",
    };
    return t < sizeof names / sizeof names[0] ? names[t] : "other";
}

/* The UEFI memory map, read through the boot services table */
static void efi_dump_memory_map(void)
{
    static u8 buf[256 * 48];
    UINTN size = sizeof buf, key, dsize;
    u32 dver;
    if (efi_st->boot_services->get_memory_map(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsize, &dver)) {
        printf("efi: GetMemoryMap failed\n");
        return;
    }
    u64 free_pages = 0;
    for (UINTN off = 0; off < size; off += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(buf + off);
        printf("efi: %016lx-%016lx %s\n", d->physical_start,
               d->physical_start + d->number_of_pages * 4096 - 1, efi_type_name(d->type));
        if (d->type == EfiConventionalMemory)
            free_pages += d->number_of_pages;
    }
    printf("efi: %lu descriptors of %lu bytes, %lu KB free\n", size / dsize, dsize, free_pages * 4);
}

/* An EFI image given to QEMU as -fw_cfg name=opt/org.squirrelworks/boot.efi,file=...:
 * load and start it. Real boot devices come with the file system step. */
#define FWCFG_BOOT "opt/org.squirrelworks/boot.efi"

static void boot_fwcfg_image(void)
{
    u16 sel;
    u32 size;
    if (!fwcfg_find(FWCFG_BOOT, &sel, &size)) {
        printf("fw: nothing to boot (no %s), halting\n", FWCFG_BOOT);
        return;
    }
    u64 free_before = efi_free_pages_count();
    void *buf;
    if (efi_allocate_pool(EfiBootServicesData, size, &buf) != EFI_SUCCESS) {
        printf("fw: no memory for a %u-byte image, halting\n", size);
        return;
    }
    fwcfg_read(sel, buf, size);
    EFI_HANDLE h;
    EFI_STATUS s = efi_load_image(0, efi_firmware_handle, NULL, buf, size, &h);
    efi_free_pool(buf);
    if (s != EFI_SUCCESS) {
        printf("efi: LoadImage failed: 0x%lx, halting\n", s);
        return;
    }
    EFI_LOADED_IMAGE_PROTOCOL *li;
    efi_handle_protocol(h, (EFI_GUID *)&efi_loaded_image_guid, (void **)&li);
    printf("efi: image of %u bytes loaded at %p\n", size, li->image_base);
    UINTN xsize = 0;
    CHAR16 *xdata = NULL;
    s = efi_start_image(h, &xsize, &xdata);
    printf("efi: image returned 0x%lx\n", s);
    if (efi_free_pages_count() == free_before)
        printf("efi: image unloaded, its memory given back\n");
    else
        printf("efi: %ld pages not given back\n", (s64)(free_before - efi_free_pages_count()));
    printf("fw: halting\n");
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

    efi_memory_init();
    efi_tables_init();
    efi_console_init();
    printf("efi: system table at %p, UEFI %u.%u, boot services at %p\n", efi_st,
           efi_st->hdr.revision >> 16, efi_st->hdr.revision & 0xFFFF, efi_st->boot_services);
    efi_dump_memory_map();
    printf("efi: memory services %s\n", efi_memory_selftest() ? "ok" : "FAILED");
    printf("efi: TSC at %lu MHz, console on COM1\n", efi_tsc_hz() / 1000000);

    efi_disk_init();
    if (efi_boot_esp()) {
        printf("fw: halting\n");
        halt();
    }
    boot_fwcfg_image();
    halt();
}
