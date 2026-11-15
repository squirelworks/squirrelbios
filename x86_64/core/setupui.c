/* SquirrelBIOS x86_64-uefi: Setup UI / boot menu shell (Phase G).
 * Original code. CONTINUANCE asks for a graphical setup and boot menu on
 * GOP in the Warp look. This draws a title and a navigable list of boot /
 * setup stubs on the frame buffer when GOP is up, mirrors the same menu
 * on the serial console, and returns when the user Continues (or presses
 * Esc). Entered when QEMU provides
 * -fw_cfg name=opt/org.squirrelworks/setupui,... or when F2 is pressed
 * during a short post-GOP window (serial ANSI ESC O Q / CSI 12~). Later
 * steps: real BootOrder edits, Secure Boot / TPM / NVRAM notebooks. */
#include "efi.h"
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

struct menu_item {
    const char *label;
    const char *serial_tag;             /* short id in "setup: selected …" */
    int is_continue;                    /* leave the menu */
    int stub;                           /* print "(stub)" and stay */
};

static const struct menu_item items[] = {
    { "Continue boot",           "Continue boot", 1, 0 },
    { "Boot Manager (stub)",     "Boot Manager",  0, 1 },
    { "Secure Boot keys (stub)", "Secure Boot",   0, 1 },
    { "TPM clear (stub)",        "TPM clear",     0, 1 },
    { "Date and time (stub)",    "Date and time", 0, 1 },
    { "NVRAM reset (stub)",      "NVRAM reset",   0, 1 },
};
#define NITEMS ((int)(sizeof items / sizeof items[0]))

typedef EFI_STATUS (EFIAPI *locate_protocol_fn)(EFI_GUID *, void *, void **);
typedef EFI_STATUS (EFIAPI *wait_for_event_fn)(UINTN, EFI_EVENT *, UINTN *);
typedef EFI_STATUS (EFIAPI *check_event_fn)(EFI_EVENT);

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static int have_gop;
static int selected;

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

static void serial_menu(void)
{
    printf("setup: SquirrelBIOS Setup\n");
    printf("setup: --- Boot Manager ---\n");
    for (int i = 0; i < NITEMS; i++) {
        printf("setup: %c %s\n", i == selected ? '>' : ' ', items[i].label);
    }
    printf("setup: Up/Down Enter; Esc continues\n");
}

static void draw_menu(void)
{
    if (!have_gop)
        return;
    u32 W = gop->mode->info->horizontal_resolution;
    u32 H = gop->mode->info->vertical_resolution;
    fill_rect(0, 0, W, H, COL_DESKTOP);

    /* Title bar across the top (Warp-ish grey strip). */
    u32 bar_h = FONT_H * SCALE + 16;
    fill_rect(0, 0, W, bar_h, COL_TITLE_BG);
    put_str(24, 8, "SquirrelBIOS Setup", COL_TITLE_FG, COL_TITLE_BG);

    /* Central panel. */
    u32 panel_x = 80, panel_y = bar_h + 40;
    u32 panel_w = W > 160 ? W - 160 : W;
    u32 panel_h = FONT_H * SCALE * (NITEMS + 3) + 40;
    if (panel_y + panel_h > H)
        panel_h = H - panel_y;
    fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + 12, "Boot Manager", COL_PANEL_FG, COL_PANEL_BG);

    u32 list_y = panel_y + 12 + FONT_H * SCALE + 12;
    for (int i = 0; i < NITEMS; i++) {
        u32 fg = i == selected ? COL_SEL_FG : COL_PANEL_FG;
        u32 bg = i == selected ? COL_SEL_BG : COL_PANEL_BG;
        u32 row_y = list_y + (u32)i * (FONT_H * SCALE + 4);
        fill_rect(panel_x + 8, row_y, panel_w - 16, FONT_H * SCALE + 2, bg);
        put_str(panel_x + 24, row_y + 1, items[i].label, fg, bg);
    }

    put_str(panel_x + 16, panel_y + panel_h - FONT_H * SCALE - 12,
            "Up/Down  Enter  Esc=continue", COL_HINT_FG, COL_PANEL_BG);
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
    serial_menu();
    draw_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key))
            continue;
        if (key.scan_code == 0x01) {            /* up */
            if (selected > 0)
                selected--;
            else
                selected = NITEMS - 1;
            serial_menu();
            draw_menu();
        } else if (key.scan_code == 0x02) {     /* down */
            selected = (selected + 1) % NITEMS;
            serial_menu();
            draw_menu();
        } else if (key.scan_code == 0x17 ||     /* Esc */
                   key.unicode_char == 0x1B) {
            printf("setup: Escape — Continue boot\n");
            break;
        } else if (key.unicode_char == '\r') {
            const struct menu_item *it = &items[selected];
            if (it->is_continue) {
                printf("setup: selected %s\n", it->serial_tag);
                break;
            }
            if (it->stub)
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
