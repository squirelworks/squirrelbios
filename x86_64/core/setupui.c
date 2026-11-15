/* SquirrelBIOS x86_64-uefi: Setup UI / boot menu shell (Phase G).
 * Original code. CONTINUANCE asks for a graphical setup and boot menu on
 * GOP in the Warp look. This draws a title and a navigable list of boot /
 * setup items on the frame buffer when GOP is up, mirrors the same menu
 * on the serial console, and returns when the user Continues (or presses
 * Esc). Boot Manager edits the UEFI BootOrder variable (and seeds demo
 * Boot#### load options when none exist). Entered when QEMU provides
 * -fw_cfg name=opt/org.squirrelworks/setupui,... or when F2 is pressed
 * during a short post-GOP window (serial ANSI ESC O Q / CSI 12~). Later
 * steps: Secure Boot / TPM / NVRAM notebooks. */
#include "efi.h"
#include "vars.h"
#include "font8x16.h"

#define FWCFG_SETUPUI "opt/org.squirrelworks/setupui"
#define SCAN_F2       0x0Cu
/* Hotkey window: short so ordinary smokes stay fast; SERIAL_IN delivers
 * F2 as soon as the guest reads COM1, and keys typed during earlier
 * firmware messages sit in the UART FIFO. */
#define HOTKEY_MS     250
#define HOTKEY_STEP_US 10000

/* Warp-ish palette (BGRX as Bochs DISPI / our GOP use). Teal desktop,
 * light grey title strip, dark ink, blue selection like a Warp menu. */
#define COL_DESKTOP   0x008080u         /* Warp teal */
#define COL_TITLE_BG  0xC0C0C0u
#define COL_TITLE_FG  0x000080u         /* dark blue */
#define COL_PANEL_BG  0xD4D0C8u         /* light warm grey */
#define COL_PANEL_FG  0x000000u
#define COL_SEL_BG    0x000080u
#define COL_SEL_FG    0xFFFFFFu
#define COL_HINT_FG   0x404040u

#define FONT_W 8
#define FONT_H 16
#define SCALE  2                        /* 16x32 cells on the 800x600 mode */

#define ATTR_NV_BS_RT 0x07u             /* NV | BS | RT */
#define LOAD_OPTION_ACTIVE 0x00000001u
#define BOOT_MAX 8
#define LABEL_MAX 40

/* EFI_GLOBAL_VARIABLE (UEFI Spec): BootOrder and Boot#### live here. */
static const EFI_GUID efi_global = {
    0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

enum {
    ACT_CONTINUE = 1,
    ACT_BOOTMGR,
    ACT_STUB,
};

struct menu_item {
    const char *label;
    const char *serial_tag;             /* short id in "setup: selected …" */
    int action;
};

static const struct menu_item items[] = {
    { "Continue boot",     "Continue boot", ACT_CONTINUE },
    { "Boot Manager",      "Boot Manager",  ACT_BOOTMGR },
    { "Secure Boot keys",  "Secure Boot",   ACT_STUB },
    { "TPM clear",         "TPM clear",     ACT_STUB },
    { "Date and time",     "Date and time", ACT_STUB },
    { "NVRAM reset",       "NVRAM reset",   ACT_STUB },
};
#define NITEMS ((int)(sizeof items / sizeof items[0]))

typedef EFI_STATUS (EFIAPI *locate_protocol_fn)(EFI_GUID *, void *, void **);
typedef EFI_STATUS (EFIAPI *wait_for_event_fn)(UINTN, EFI_EVENT *, UINTN *);
typedef EFI_STATUS (EFIAPI *check_event_fn)(EFI_EVENT);

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static int have_gop;
static int selected;

/* ---- BootOrder workspace ------------------------------------------- */

static u16 boot_order[BOOT_MAX];
static int boot_n;
static char boot_label[BOOT_MAX][LABEL_MAX];
static int boot_sel;
static int boot_dirty;

static void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 colour)
{
    if (!have_gop || !w || !h)
        return;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL p = {
        .blue = (u8)colour,
        .green = (u8)(colour >> 8),
        .red = (u8)(colour >> 16),
    };
    gop->blt(gop, &p, EfiBltVideoFill, 0, 0, x, y, w, h, 0);
}

static void put_glyph(u32 x, u32 y, unsigned char ch, u32 fg, u32 bg)
{
    if (!have_gop)
        return;
    const u8 *rows = font8x16[ch];
    u32 cw = FONT_W * SCALE, chh = FONT_H * SCALE;
    /* One glyph into a small scratch, then BltBufferToVideo. */
    static EFI_GRAPHICS_OUTPUT_BLT_PIXEL buf[FONT_W * SCALE * FONT_H * SCALE];
    for (u32 row = 0; row < FONT_H; row++) {
        u8 bits = rows[row];
        for (u32 col = 0; col < FONT_W; col++) {
            u32 c = (bits & (0x80u >> col)) ? fg : bg;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL px = {
                .blue = (u8)c, .green = (u8)(c >> 8), .red = (u8)(c >> 16)
            };
            for (u32 sy = 0; sy < SCALE; sy++)
                for (u32 sx = 0; sx < SCALE; sx++)
                    buf[(row * SCALE + sy) * cw + col * SCALE + sx] = px;
        }
    }
    gop->blt(gop, buf, EfiBltBufferToVideo, 0, 0, x, y, cw, chh, cw * 4);
}

static void put_str(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    u32 cx = x;
    for (; *s; s++) {
        put_glyph(cx, y, (unsigned char)*s, fg, bg);
        cx += FONT_W * SCALE;
    }
}

static void boot_name(u16 id, CHAR16 *out)
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'B';
    out[1] = 'o';
    out[2] = 'o';
    out[3] = 't';
    out[4] = hex[(id >> 12) & 0xF];
    out[5] = hex[(id >> 8) & 0xF];
    out[6] = hex[(id >> 4) & 0xF];
    out[7] = hex[id & 0xF];
    out[8] = 0;
}

static void ascii_from_ucs2(char *dst, int dst_n, const CHAR16 *src, int max_chars)
{
    int i = 0;
    if (dst_n < 2)
        return;
    for (; i < dst_n - 1 && i < max_chars && src[i]; i++) {
        CHAR16 c = src[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[i] = 0;
}

static void format_order_line(char *dst, int dst_n)
{
    int n = 0;
    if (dst_n < 8) {
        if (dst_n)
            dst[0] = 0;
        return;
    }
    if (boot_n == 0) {
        /* short enough for any reasonable LABEL_MAX-style buffer */
        dst[0] = '('; dst[1] = 'e'; dst[2] = 'm'; dst[3] = 'p';
        dst[4] = 't'; dst[5] = 'y'; dst[6] = ')'; dst[7] = 0;
        return;
    }
    for (int i = 0; i < boot_n && n + 5 < dst_n; i++) {
        if (i) {
            dst[n++] = ',';
        }
        static const char hex[] = "0123456789ABCDEF";
        u16 id = boot_order[i];
        dst[n++] = hex[(id >> 12) & 0xF];
        dst[n++] = hex[(id >> 8) & 0xF];
        dst[n++] = hex[(id >> 4) & 0xF];
        dst[n++] = hex[id & 0xF];
    }
    dst[n] = 0;
}

static void print_boot_order(const char *tag)
{
    char line[80];
    format_order_line(line, (int)sizeof line);
    printf("setup: BootOrder %s %s\n", tag, line);
}

static EFI_STATUS write_load_option(u16 id, const char *desc)
{
    u8 buf[128];
    int di = 0;
    while (desc[di])
        di++;
    UINTN size = 4 + 2 + (di + 1) * 2 + 4;
    if (size > sizeof buf)
        return EFI_OUT_OF_RESOURCES;
    memset(buf, 0, size);
    buf[0] = (u8)LOAD_OPTION_ACTIVE;
    buf[1] = (u8)(LOAD_OPTION_ACTIVE >> 8);
    buf[2] = (u8)(LOAD_OPTION_ACTIVE >> 16);
    buf[3] = (u8)(LOAD_OPTION_ACTIVE >> 24);
    buf[4] = 4;                         /* FilePathListLength: End node */
    buf[5] = 0;
    CHAR16 *d = (CHAR16 *)(buf + 6);
    for (int i = 0; i < di; i++)
        d[i] = (CHAR16)(u8)desc[i];
    d[di] = 0;
    u8 *dp = buf + 6 + (di + 1) * 2;
    dp[0] = END_DEVICE_PATH_TYPE;
    dp[1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    dp[2] = 4;
    dp[3] = 0;
    CHAR16 name[9];
    boot_name(id, name);
    return vars_set(name, (EFI_GUID *)&efi_global, ATTR_NV_BS_RT, size, buf);
}

static void load_boot_label(int idx)
{
    CHAR16 name[9];
    u8 data[256];
    UINTN size = sizeof data;
    boot_name(boot_order[idx], name);
    EFI_STATUS st = vars_get(name, (EFI_GUID *)&efi_global, NULL, &size, data);
    char *out = boot_label[idx];
    int n = 0;
    /* "Boot0001 Description" for the menu row */
    out[n++] = 'B'; out[n++] = 'o'; out[n++] = 'o'; out[n++] = 't';
    static const char hex[] = "0123456789ABCDEF";
    u16 id = boot_order[idx];
    out[n++] = hex[(id >> 12) & 0xF];
    out[n++] = hex[(id >> 8) & 0xF];
    out[n++] = hex[(id >> 4) & 0xF];
    out[n++] = hex[id & 0xF];
    out[n++] = ' ';
    out[n] = 0;
    if (st != EFI_SUCCESS || size < 8) {
        /* keep the Boot#### id alone */
        return;
    }
    const CHAR16 *desc = (const CHAR16 *)(data + 6);
    char tmp[LABEL_MAX];
    ascii_from_ucs2(tmp, LABEL_MAX, desc, LABEL_MAX - 1 - n);
    for (int i = 0; tmp[i] && n < LABEL_MAX - 1; i++)
        out[n++] = tmp[i];
    out[n] = 0;
}

static int seed_demo_boot_options(void)
{
    /* Three original demo load options so Boot Manager has something to
     * reorder under QEMU without an OS installer. Descriptions only —
     * FilePathList is an End node (not a real boot path). */
    static const struct {
        u16 id;
        const char *desc;
    } demo[] = {
        { 0x0001, "Disk" },
        { 0x0002, "Network" },
        { 0x0003, "USB" },
    };
    for (unsigned i = 0; i < sizeof demo / sizeof demo[0]; i++) {
        if (write_load_option(demo[i].id, demo[i].desc) != EFI_SUCCESS)
            return 0;
        boot_order[i] = demo[i].id;
    }
    boot_n = 3;
    if (vars_set(u"BootOrder", (EFI_GUID *)&efi_global, ATTR_NV_BS_RT,
                 (UINTN)(boot_n * 2), boot_order) != EFI_SUCCESS)
        return 0;
    printf("setup: seeded Boot0001/0002/0003 and BootOrder\n");
    return 1;
}

static int load_boot_order(void)
{
    u16 buf[BOOT_MAX];
    UINTN size = sizeof buf;
    boot_n = 0;
    boot_dirty = 0;
    boot_sel = 0;
    EFI_STATUS st = vars_get(u"BootOrder", (EFI_GUID *)&efi_global, NULL, &size, buf);
    if (st != EFI_SUCCESS || size < 2 || (size & 1)) {
        if (!seed_demo_boot_options())
            return 0;
    } else {
        boot_n = (int)(size / 2);
        if (boot_n > BOOT_MAX)
            boot_n = BOOT_MAX;
        for (int i = 0; i < boot_n; i++)
            boot_order[i] = buf[i];
    }
    for (int i = 0; i < boot_n; i++)
        load_boot_label(i);
    print_boot_order("was");
    return 1;
}

static int save_boot_order(void)
{
    EFI_STATUS st = vars_set(u"BootOrder", (EFI_GUID *)&efi_global, ATTR_NV_BS_RT,
                             (UINTN)(boot_n * 2), boot_order);
    if (st != EFI_SUCCESS) {
        printf("setup: BootOrder save failed (0x%lx)\n", st);
        return 0;
    }
    boot_dirty = 0;
    /* Read back so the smoke can see the variable, not only the workspace. */
    u16 check[BOOT_MAX];
    UINTN size = sizeof check;
    if (vars_get(u"BootOrder", (EFI_GUID *)&efi_global, NULL, &size, check) != EFI_SUCCESS ||
        size != (UINTN)(boot_n * 2) || memcmp(check, boot_order, size) != 0) {
        printf("setup: BootOrder read-back mismatch\n");
        return 0;
    }
    print_boot_order("now");
    printf("setup: BootOrder saved\n");
    return 1;
}

static void move_boot_entry(int delta)
{
    int to = boot_sel + delta;
    if (boot_n < 2 || to < 0 || to >= boot_n)
        return;
    u16 t = boot_order[boot_sel];
    boot_order[boot_sel] = boot_order[to];
    boot_order[to] = t;
    char tmp[LABEL_MAX];
    memcpy(tmp, boot_label[boot_sel], LABEL_MAX);
    memcpy(boot_label[boot_sel], boot_label[to], LABEL_MAX);
    memcpy(boot_label[to], tmp, LABEL_MAX);
    boot_sel = to;
    boot_dirty = 1;
    printf("setup: BootOrder moved\n");
    print_boot_order("edit");
}

/* ---- Drawing / serial ---------------------------------------------- */

static void serial_main_menu(void)
{
    printf("setup: SquirrelBIOS Setup\n");
    printf("setup: --- Boot Manager ---\n");
    for (int i = 0; i < NITEMS; i++) {
        printf("setup: %c %s\n", i == selected ? '>' : ' ', items[i].label);
    }
    printf("setup: Up/Down Enter; Esc continues\n");
}

static void serial_boot_menu(void)
{
    printf("setup: --- Boot Order ---\n");
    print_boot_order("edit");
    for (int i = 0; i < boot_n; i++) {
        printf("setup: %c %s\n", i == boot_sel ? '>' : ' ', boot_label[i]);
    }
    printf("setup: Up/Down Left/Right reorder; Enter saves; Esc cancels\n");
}

static void draw_panel_list(const char *title, const char *hint,
                            int n, int sel,
                            const char *(*label_at)(int))
{
    if (!have_gop)
        return;
    u32 W = gop->mode->info->horizontal_resolution;
    u32 H = gop->mode->info->vertical_resolution;
    fill_rect(0, 0, W, H, COL_DESKTOP);

    u32 bar_h = FONT_H * SCALE + 16;
    fill_rect(0, 0, W, bar_h, COL_TITLE_BG);
    put_str(24, 8, "SquirrelBIOS Setup", COL_TITLE_FG, COL_TITLE_BG);

    u32 panel_x = 80, panel_y = bar_h + 40;
    u32 panel_w = W > 160 ? W - 160 : W;
    u32 panel_h = FONT_H * SCALE * (n + 3) + 40;
    if (panel_y + panel_h > H)
        panel_h = H - panel_y;
    fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + 12, title, COL_PANEL_FG, COL_PANEL_BG);

    u32 list_y = panel_y + 12 + FONT_H * SCALE + 12;
    for (int i = 0; i < n; i++) {
        u32 fg = i == sel ? COL_SEL_FG : COL_PANEL_FG;
        u32 bg = i == sel ? COL_SEL_BG : COL_PANEL_BG;
        u32 row_y = list_y + (u32)i * (FONT_H * SCALE + 4);
        fill_rect(panel_x + 8, row_y, panel_w - 16, FONT_H * SCALE + 2, bg);
        put_str(panel_x + 24, row_y + 1, label_at(i), fg, bg);
    }

    put_str(panel_x + 16, panel_y + panel_h - FONT_H * SCALE - 12,
            hint, COL_HINT_FG, COL_PANEL_BG);
}

static const char *main_label_at(int i) { return items[i].label; }
static const char *boot_label_at(int i) { return boot_label[i]; }

static void draw_main_menu(void)
{
    draw_panel_list("Boot Manager", "Up/Down  Enter  Esc=continue",
                    NITEMS, selected, main_label_at);
}

static void draw_boot_menu(void)
{
    draw_panel_list("Boot Order", "Up/Down  L/R reorder  Enter=save  Esc",
                    boot_n, boot_sel, boot_label_at);
}

static int read_key(EFI_INPUT_KEY *key)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = efi_st->con_in;
    if (!in || !key)
        return 0;
    wait_for_event_fn wait = (wait_for_event_fn)efi_st->boot_services->wait_for_event;
    UINTN idx = 0;
    if (wait(1, &in->wait_for_key, &idx) != EFI_SUCCESS)
        return 0;
    return in->read_key_stroke(in, key) == EFI_SUCCESS;
}

static void handle_stub(const struct menu_item *it)
{
    printf("setup: selected %s (stub)\n", it->serial_tag);
    if (have_gop) {
        u32 W = gop->mode->info->horizontal_resolution;
        u32 H = gop->mode->info->vertical_resolution;
        u32 y = H - FONT_H * SCALE - 8;
        fill_rect(0, y, W, FONT_H * SCALE + 8, COL_DESKTOP);
        put_str(24, y, "Not implemented yet — pick Continue boot", COL_TITLE_BG, COL_DESKTOP);
    }
}

/* Boot Manager notebook: reorder BootOrder, SetVariable on Enter. */
static void boot_manager_run(void)
{
    printf("setup: selected Boot Manager\n");
    if (!load_boot_order()) {
        printf("setup: BootOrder unavailable\n");
        return;
    }
    serial_boot_menu();
    draw_boot_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key))
            continue;
        if (key.scan_code == 0x01) {            /* up */
            if (boot_sel > 0)
                boot_sel--;
            else if (boot_n)
                boot_sel = boot_n - 1;
            serial_boot_menu();
            draw_boot_menu();
        } else if (key.scan_code == 0x02) {     /* down */
            if (boot_n)
                boot_sel = (boot_sel + 1) % boot_n;
            serial_boot_menu();
            draw_boot_menu();
        } else if (key.scan_code == 0x04 ||     /* left: earlier */
                   key.unicode_char == '-') {
            move_boot_entry(-1);
            serial_boot_menu();
            draw_boot_menu();
        } else if (key.scan_code == 0x03 ||     /* right: later */
                   key.unicode_char == '+') {
            move_boot_entry(+1);
            serial_boot_menu();
            draw_boot_menu();
        } else if (key.scan_code == 0x17 ||     /* Esc: cancel */
                   key.unicode_char == 0x1B) {
            printf("setup: BootOrder cancelled\n");
            break;
        } else if (key.unicode_char == '\r') {
            if (boot_dirty || boot_n > 0)
                save_boot_order();
            break;
        }
    }
}

void setup_ui_run(void)
{
    locate_protocol_fn locate =
        (locate_protocol_fn)efi_st->boot_services->locate_protocol;
    gop = NULL;
    have_gop = 0;
    if (locate((EFI_GUID *)&efi_gop_guid, NULL, (void **)&gop) == EFI_SUCCESS &&
        gop && gop->mode && gop->mode->frame_buffer_base)
        have_gop = 1;

    selected = 0;
    printf("setup: UI open%s\n", have_gop ? " (serial+GOP)" : " (serial only)");
    serial_main_menu();
    draw_main_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key))
            continue;
        if (key.scan_code == 0x01) {            /* up */
            if (selected > 0)
                selected--;
            else
                selected = NITEMS - 1;
            serial_main_menu();
            draw_main_menu();
        } else if (key.scan_code == 0x02) {     /* down */
            selected = (selected + 1) % NITEMS;
            serial_main_menu();
            draw_main_menu();
        } else if (key.scan_code == 0x17 ||     /* Esc */
                   key.unicode_char == 0x1B) {
            printf("setup: Escape — Continue boot\n");
            break;
        } else if (key.unicode_char == '\r') {
            const struct menu_item *it = &items[selected];
            if (it->action == ACT_CONTINUE) {
                printf("setup: selected %s\n", it->serial_tag);
                break;
            }
            if (it->action == ACT_BOOTMGR) {
                boot_manager_run();
                serial_main_menu();
                draw_main_menu();
                continue;
            }
            if (it->action == ACT_STUB)
                handle_stub(it);
        }
    }
    printf("setup: UI closed\n");
}

int setup_ui_wanted(void)
{
    u16 sel;
    u32 size;
    return fwcfg_find(FWCFG_SETUPUI, &sel, &size) != 0;
}

/* Non-blocking ConIn read: CheckEvent on WaitForKey runs the serial
 * poll notify, then ReadKeyStroke if a stroke is queued. */
static int try_read_key(EFI_INPUT_KEY *key)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = efi_st->con_in;
    if (!in || !key)
        return 0;
    check_event_fn check = (check_event_fn)efi_st->boot_services->check_event;
    if (check(in->wait_for_key) != EFI_SUCCESS)
        return 0;
    return in->read_key_stroke(in, key) == EFI_SUCCESS;
}

/* Brief post-GOP window for F2 without fw_cfg. Returns 1 if F2 was seen.
 * Any other stroke is pushed back and ends the wait so ConIn apps (and
 * SERIAL_IN fixtures like x86_64-con) still see their keys. */
int setup_ui_hotkey(void)
{
    printf("setup: press F2 for Setup UI\n");
    int steps = HOTKEY_MS * 1000 / HOTKEY_STEP_US;
    for (int i = 0; i < steps; i++) {
        EFI_INPUT_KEY key;
        if (try_read_key(&key)) {
            if (key.scan_code == SCAN_F2) {
                printf("setup: F2 — opening Setup UI\n");
                return 1;
            }
            efi_con_unread_key(key);
            return 0;
        }
        efi_stall(HOTKEY_STEP_US);
    }
    return 0;
}
