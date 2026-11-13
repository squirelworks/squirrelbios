/* SquirrelBIOS UEFI core (common/efi): firmware main, shared by the
 * device-tree machines (aarch64-uefi, riscv64-uefi). Original code.
 *
 * The architecture's start code sets up the CPU (stack, .bss, MMU where
 * there is one) and calls efi_main with the device tree QEMU gave it.
 * From there: the console, the memory map from the device tree, the EFI
 * tables and services, the device tree as a configuration table, then
 * something to boot:
 *   1. a -kernel image QEMU put in fw_cfg (with -initrd and -append), or
 *      on RISC-V one it put in RAM (arch_kernel; then the command line and
 *      initrd come from /chosen), the way a Linux EFI stub expects it:
 *      the command line as LoadOptions, the initrd through LoadFile2 on
 *      the Linux initrd media device path;
 *   2. an EFI image given as -fw_cfg name=opt/org.squirrelworks/boot.efi.
 * Otherwise it halts. */
#include "efi.h"

extern char __fw_start[], __fw_end[];

#define FWCFG_BOOT "opt/org.squirrelworks/boot.efi"
#define FW_CFG_KERNEL_SIZE  0x08
#define FW_CFG_INITRD_SIZE  0x0b
#define FW_CFG_KERNEL_DATA  0x11
#define FW_CFG_INITRD_DATA  0x12
#define FW_CFG_CMDLINE_SIZE 0x14
#define FW_CFG_CMDLINE_DATA 0x15

static const EFI_GUID dtb_guid = EFI_DTB_TABLE_GUID;
static const EFI_GUID load_file2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
static const EFI_GUID initrd_media_guid = LINUX_EFI_INITRD_MEDIA_GUID;

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

/* ------------------------------------------------- the -initrd file */

typedef struct EFI_LOAD_FILE2_PROTOCOL {
    EFI_STATUS (EFIAPI *load_file)(struct EFI_LOAD_FILE2_PROTOCOL *, EFI_DEVICE_PATH_PROTOCOL *,
                                   BOOLEAN, UINTN *, void *);
} EFI_LOAD_FILE2_PROTOCOL;

static void *initrd;
static UINTN initrd_size;

static EFI_STATUS EFIAPI initrd_load(EFI_LOAD_FILE2_PROTOCOL *self, EFI_DEVICE_PATH_PROTOCOL *path,
                                     BOOLEAN boot_policy, UINTN *size, void *buf)
{
    (void)self;
    (void)path;
    if (boot_policy)
        return EFI_UNSUPPORTED;
    if (!size)
        return EFI_INVALID_PARAMETER;
    if (!buf || *size < initrd_size) {
        *size = initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(buf, initrd, initrd_size);
    *size = initrd_size;
    return EFI_SUCCESS;
}

static EFI_LOAD_FILE2_PROTOCOL initrd_lf2 = { initrd_load };

static struct {
    EFI_DEVICE_PATH_PROTOCOL hdr;
    EFI_GUID guid;
    EFI_DEVICE_PATH_PROTOCOL end;
} __attribute__((packed)) initrd_path = {
    { MEDIA_DEVICE_PATH, 3, { 20, 0 } },            /* vendor-defined media */
    LINUX_EFI_INITRD_MEDIA_GUID,
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { 4, 0 } },
};

static void publish_initrd(void)
{
    EFI_HANDLE h = NULL;
    efi_install_protocol_interface(&h, (EFI_GUID *)&efi_device_path_guid, 0, &initrd_path);
    efi_install_protocol_interface(&h, (EFI_GUID *)&load_file2_guid, 0, &initrd_lf2);
    (void)initrd_media_guid;
}

static u32 fwcfg_u32(u16 sel)
{
    u8 b[4];
    fwcfg_read(sel, b, 4);
    return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

/* --------------------------------------------------------- booting */

static void run_image(void *buf, u32 size, CHAR16 *options, u32 options_size)
{
    u64 free_before = efi_free_pages_count();
    EFI_HANDLE h;
    EFI_STATUS s = efi_load_image(0, efi_firmware_handle, NULL, buf, size, &h);
    if (s != EFI_SUCCESS) {
        printf("efi: LoadImage failed: 0x%lx, halting\n", s);
        return;
    }
    EFI_LOADED_IMAGE_PROTOCOL *li;
    efi_handle_protocol(h, (EFI_GUID *)&efi_loaded_image_guid, (void **)&li);
    li->load_options = options;
    li->load_options_size = options_size;
    printf("efi: image of %u bytes loaded at %p\n", size, li->image_base);
    UINTN xsize = 0;
    CHAR16 *xdata = NULL;
    s = efi_start_image(h, &xsize, &xdata);
    printf("efi: image returned 0x%lx\n", s);
    if (efi_free_pages_count() == free_before)
        printf("efi: image unloaded, its memory given back\n");
    else
        printf("efi: %ld pages not given back\n", (s64)(free_before - efi_free_pages_count()));
}

/* A command line (ASCII) as UCS-2 LoadOptions */
static CHAR16 *options_from(const char *c, u32 len, u32 *osize)
{
    CHAR16 *opts;
    if (!len || efi_allocate_pool(EfiLoaderData, (len + 1) * 2, (void **)&opts) != EFI_SUCCESS)
        return NULL;
    u32 n = 0;
    for (; n < len && c[n]; n++)
        opts[n] = (u8)c[n];
    opts[n] = 0;
    *osize = (n + 1) * 2;
    return opts;
}

static u64 dt_cell(const u8 *p, u32 len)
{
    return len == 8 ? be64(p) : len == 4 ? be32(p) : 0;
}

/* -kernel, -initrd, -append: through fw_cfg, or (RISC-V) in RAM with
 * /chosen describing the rest */
static int boot_kernel(void)
{
    void *kbuf = NULL;
    u32 ksize = 0, osize = 0;
    CHAR16 *opts = NULL;
    int from_fwcfg = 0;

    if (fwcfg_present() && (ksize = fwcfg_u32(FW_CFG_KERNEL_SIZE)) != 0) {
        if (efi_allocate_pool(EfiBootServicesData, ksize, &kbuf) != EFI_SUCCESS) {
            printf("fw: no memory for a %u-byte kernel\n", ksize);
            return 1;
        }
        fwcfg_read(FW_CFG_KERNEL_DATA, kbuf, ksize);
        from_fwcfg = 1;
        printf("fw: -kernel image, %u bytes\n", ksize);

        u32 isize = fwcfg_u32(FW_CFG_INITRD_SIZE);
        if (isize && efi_allocate_pool(EfiLoaderData, isize, &initrd) == EFI_SUCCESS) {
            fwcfg_read(FW_CFG_INITRD_DATA, initrd, isize);
            initrd_size = isize;
        }
        u32 csize = fwcfg_u32(FW_CFG_CMDLINE_SIZE);
        char *c;
        if (csize > 1 && efi_allocate_pool(EfiBootServicesData, csize, (void **)&c) == EFI_SUCCESS) {
            fwcfg_read(FW_CFG_CMDLINE_DATA, c, csize);
            c[csize - 1] = 0;
            opts = options_from(c, csize, &osize);
            printf("fw: -append \"%s\"\n", c);
            efi_free_pool(c);
        }
    } else if (arch_kernel(&kbuf, &ksize)) {
        int chosen = fdt_path("/chosen");
        u32 len, elen;
        const char *args = fdt_prop(chosen, "bootargs", &len);
        if (args && len > 1) {
            opts = options_from(args, len, &osize);
            printf("fw: -append \"%s\" (/chosen bootargs)\n", args);
        }
        const u8 *s = fdt_prop(chosen, "linux,initrd-start", &len);
        const u8 *e = fdt_prop(chosen, "linux,initrd-end", &elen);
        if (s && e && dt_cell(e, elen) > dt_cell(s, len)) {
            initrd = (void *)(uptr)dt_cell(s, len);
            initrd_size = dt_cell(e, elen) - dt_cell(s, len);
        }
    } else
        return 0;

    if (initrd_size) {
        publish_initrd();
        printf("fw: -initrd, %lu bytes, for LoadFile2 on the Linux initrd media path\n", (u64)initrd_size);
    }
    run_image(kbuf, ksize, opts, osize);
    if (from_fwcfg)
        efi_free_pool(kbuf);
    return 1;
}

static int boot_fwcfg_file(void)
{
    u16 sel;
    u32 size;
    if (!fwcfg_find(FWCFG_BOOT, &sel, &size))
        return 0;
    void *buf;
    if (efi_allocate_pool(EfiBootServicesData, size, &buf) != EFI_SUCCESS) {
        printf("fw: no memory for a %u-byte image\n", size);
        return 1;
    }
    fwcfg_read(sel, buf, size);
    run_image(buf, size, NULL, 0);
    efi_free_pool(buf);
    return 1;
}

void efi_main(const void *dtb)
{
    arch_con_init();
    printf("\nSquirrelBIOS %s (%s)\n", SQ_VERSION, ARCH_NAME);
    if (!fdt_init(dtb)) {
        printf("fdt: no device tree at %p, halting\n", dtb);
        halt();
    }
    printf("fdt: device tree at %p, %u bytes\n", dtb, fdt_size());
    arch_init();
    printf("fw: running at %p-%p\n", __fw_start, __fw_end);

    memmap_init();
    u64 usable = 0;
    for (int i = 0; i < mem_count; i++) {
        printf("mem: %016lx-%016lx %s\n", mem_map[i].base, mem_map[i].base + mem_map[i].length - 1,
               mem_map[i].type == 1 ? "usable" : "reserved");
        if (mem_map[i].type == 1)
            usable += mem_map[i].length;
    }
    printf("mem: %lu MB usable (from the device tree)\n", usable >> 20);
    if (fwcfg_init())
        printf("fw_cfg: present\n");

    efi_memory_init();
    efi_tables_init();
    efi_console_init();
    printf("efi: system table at %p, UEFI %u.%u, boot services at %p\n", efi_st,
           efi_st->hdr.revision >> 16, efi_st->hdr.revision & 0xFFFF, efi_st->boot_services);
    efi_dump_memory_map();
    printf("efi: memory services %s\n", efi_memory_selftest() ? "ok" : "FAILED");
    printf("efi: counter at %lu Hz, console on the serial port\n", efi_counter_hz());
    efi_install_configuration_table((EFI_GUID *)&dtb_guid, (void *)dtb);
    printf("efi: device tree installed as a configuration table\n");
    arch_efi_init();

    if (boot_kernel() || boot_fwcfg_file()) {
        printf("fw: halting\n");
        halt();
    }
    printf("fw: nothing to boot (no -kernel, no %s), halting\n", FWCFG_BOOT);
    halt();
}
