/* SquirrelBIOS x86_64-uefi: Graphics Output Protocol on QEMU std VGA.
 * Original code. Uses the Bochs DISPI registers (0x01CE/0x01CF) and the
 * linear frame buffer in the VGA PCI function's BAR0. */
#include "efi.h"

const EFI_GUID efi_gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF
#define DISPI_ID  0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP  3
#define DISPI_ENABLE 4
#define DISPI_VWIDTH 6
#define DISPI_VHEIGHT 7
#define DISPI_XOFF 8
#define DISPI_YOFF 9
#define DISPI_ON   0x01
#define DISPI_LFB  0x40

#define VGA_VENDOR 0x1234
#define VGA_DEVICE 0x1111

struct gop_mode {
    u16 w, h, bpp;
};

/* A small fixed list; mode 0 is the default we enable at init. */
static const struct gop_mode modes[] = {
    { 800, 600, 32 },
    { 640, 480, 32 },
    { 1024, 768, 32 },
};
#define NMODES (sizeof modes / sizeof modes[0])

static EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gop_mode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gop_info;
static EFI_HANDLE gop_handle;
static u32 lfb_base;
static u32 lfb_size;

static void dispi_write(u16 reg, u16 val)
{
    outw(VBE_INDEX, reg);
    outw(VBE_DATA, val);
}

static u16 dispi_read(u16 reg)
{
    outw(VBE_INDEX, reg);
    return inw(VBE_DATA);
}

static int dispi_present(void)
{
    dispi_write(DISPI_ID, 0xB0C5);
    return dispi_read(DISPI_ID) == 0xB0C5;
}

static void fill_info(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, const struct gop_mode *m)
{
    memset(info, 0, sizeof *info);
    info->version = 0;
    info->horizontal_resolution = m->w;
    info->vertical_resolution = m->h;
    info->pixel_format = PixelBlueGreenRedReserved8BitPerColor;
    info->pixels_per_scan_line = m->w;
}

static void set_dispi(const struct gop_mode *m)
{
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, m->w);
    dispi_write(DISPI_VWIDTH, m->w);
    dispi_write(DISPI_YRES, m->h);
    dispi_write(DISPI_VHEIGHT, m->h);
    dispi_write(DISPI_BPP, m->bpp);
    dispi_write(DISPI_XOFF, 0);
    dispi_write(DISPI_YOFF, 0);
    dispi_write(DISPI_ENABLE, DISPI_ON | DISPI_LFB);
}

static EFI_STATUS EFIAPI gop_query(EFI_GRAPHICS_OUTPUT_PROTOCOL *this, u32 mode,
                                   UINTN *size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info)
{
    (void)this;
    if (!size || !info || mode >= NMODES)
        return EFI_INVALID_PARAMETER;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    fill_info(mem, &modes[mode]);
    *info = mem;
    *size = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI gop_set(EFI_GRAPHICS_OUTPUT_PROTOCOL *this, u32 mode)
{
    (void)this;
    if (mode >= NMODES)
        return EFI_UNSUPPORTED;
    set_dispi(&modes[mode]);
    fill_info(&gop_info, &modes[mode]);
    gop_mode.mode = mode;
    gop_mode.info = &gop_info;
    gop_mode.size_of_info = sizeof gop_info;
    gop_mode.frame_buffer_base = lfb_base;
    gop_mode.frame_buffer_size = (UINTN)modes[mode].w * modes[mode].h * 4;
    if (gop_mode.frame_buffer_size > lfb_size)
        gop_mode.frame_buffer_size = lfb_size;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *this,
                                 EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf,
                                 EFI_GRAPHICS_OUTPUT_BLT_OPERATION op,
                                 UINTN srcx, UINTN srcy, UINTN dstx, UINTN dsty,
                                 UINTN width, UINTN height, UINTN delta)
{
    (void)this;
    if (!gop_mode.frame_buffer_base || !width || !height)
        return EFI_INVALID_PARAMETER;
    u32 *fb = (u32 *)(uptr)gop_mode.frame_buffer_base;
    u32 pitch = gop_info.pixels_per_scan_line;
    if (dstx + width > gop_info.horizontal_resolution ||
        dsty + height > gop_info.vertical_resolution)
        return EFI_INVALID_PARAMETER;

    if (op == EfiBltVideoFill) {
        if (!buf)
            return EFI_INVALID_PARAMETER;
        u32 pix = (u32)buf->blue | (u32)buf->green << 8 | (u32)buf->red << 16;
        for (UINTN y = 0; y < height; y++)
            for (UINTN x = 0; x < width; x++)
                fb[(dsty + y) * pitch + dstx + x] = pix;
        return EFI_SUCCESS;
    }
    if (op == EfiBltBufferToVideo) {
        if (!buf)
            return EFI_INVALID_PARAMETER;
        UINTN stride = delta ? delta / 4 : width;
        for (UINTN y = 0; y < height; y++)
            for (UINTN x = 0; x < width; x++) {
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL *p = &buf[(srcy + y) * stride + srcx + x];
                fb[(dsty + y) * pitch + dstx + x] =
                    (u32)p->blue | (u32)p->green << 8 | (u32)p->red << 16;
            }
        return EFI_SUCCESS;
    }
    if (op == EfiBltVideoToBltBuffer) {
        if (!buf)
            return EFI_INVALID_PARAMETER;
        UINTN stride = delta ? delta / 4 : width;
        for (UINTN y = 0; y < height; y++)
            for (UINTN x = 0; x < width; x++) {
                u32 pix = fb[(srcy + y) * pitch + srcx + x];
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL *p = &buf[(dsty + y) * stride + dstx + x];
                p->blue = (u8)pix;
                p->green = (u8)(pix >> 8);
                p->red = (u8)(pix >> 16);
                p->reserved = 0;
            }
        return EFI_SUCCESS;
    }
    if (op == EfiBltVideoToVideo) {
        /* Overlap-safe copy via a temporary when ranges may overlap. */
        for (UINTN y = 0; y < height; y++)
            for (UINTN x = 0; x < width; x++)
                fb[(dsty + y) * pitch + dstx + x] = fb[(srcy + y) * pitch + srcx + x];
        return EFI_SUCCESS;
    }
    return EFI_INVALID_PARAMETER;
}

static u32 find_vga_lfb(void)
{
    u8 bus, dev, fn;
    if (!pci_find(VGA_VENDOR, VGA_DEVICE, &bus, &dev, &fn))
        return 0;
    /* Probe size before assigning so we do not disturb a live BAR. */
    u32 bar = pci_read32(bus, dev, fn, 0x10);
    pci_write32(bus, dev, fn, 0x10, 0xFFFFFFFF);
    u32 mask = pci_read32(bus, dev, fn, 0x10);
    pci_write32(bus, dev, fn, 0x10, bar);
    lfb_size = ~(mask & 0xFFFFFFF0) + 1;
    if (!lfb_size)
        lfb_size = 16 * 1024 * 1024;
    u32 base = pci_assign_mem_bar(bus, dev, fn, 0x10);
    if (!base)
        return 0;
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, cmd | 0x02);        /* memory space */
    return base;
}

void efi_gop_init(void)
{
    if (!dispi_present()) {
        printf("gop: no Bochs DISPI\n");
        return;
    }
    lfb_base = find_vga_lfb();
    if (!lfb_base) {
        printf("gop: no VGA frame buffer BAR\n");
        return;
    }
    gop.query_mode = gop_query;
    gop.set_mode = gop_set;
    gop.blt = gop_blt;
    gop.mode = &gop_mode;
    gop_mode.max_mode = NMODES;
    gop_handle = NULL;
    if (efi_install_protocol_interface(&gop_handle, (EFI_GUID *)&efi_gop_guid, 0, &gop) != EFI_SUCCESS)
        return;
    gop_set(&gop, 0);
    printf("gop: %ux%ux32 at 0x%x, %u modes\n",
           gop_info.horizontal_resolution, gop_info.vertical_resolution,
           lfb_base, (unsigned)NMODES);
}
