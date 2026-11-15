/* SquirrelBIOS x86_64-uefi: Setup UI / boot menu shell (Phase G).
 * Original code. CONTINUANCE asks for a graphical setup and boot menu on
 * GOP in the Warp look. This draws a title and a navigable list of boot /
 * setup items on the frame buffer when GOP is up, mirrors the same menu
 * on the serial console, and returns when the user Continues (or presses
 * Esc). Boot Manager edits the UEFI BootOrder variable (and seeds demo
 * Boot#### load options when none exist under the setupui fw_cfg test
 * gate). Secure Boot keys shows PK/KEK/db/dbx status and can clear them
 * (firmware delete before auth hooks); TPM clear is a confirmation that
 * skips when no TPM is present and is a safe stub when one is (no
 * TPM2_Clear mid-boot). Date and time views/edits the CMOS RTC via
 * GetTime/SetTime when available. NVRAM reset confirms and deletes
 * non-volatile user variables (Boot#### / BootOrder / demo; not Secure
 * Boot PK/KEK/db/dbx). Entered when QEMU provides
 * -fw_cfg name=opt/org.squirrelworks/setupui,... or when F2 is pressed
 * during a short post-GOP window (serial ANSI ESC O Q / CSI 12~).
 *
 * Strings that reach the 8x16 GOP font or COM1 use ASCII only (plain '-'
 * not UTF-8 em dashes): put_glyph indexes one byte per cell. */
#include "efi.h"
#include "vars.h"
#include "tpm.h"
#include "font8x16.h"

#define FWCFG_SETUPUI "opt/org.squirrelworks/setupui"
#define SCAN_F2       0x0Cu
/* Hotkey window: short so ordinary smokes stay fast; SERIAL_IN delivers
 * F2 as soon as the guest reads COM1, and keys typed during earlier
 * firmware messages sit in the UART FIFO. Skipped when ConIn is absent. */
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
#define ATTR_BS_RT    0x06u             /* BS | RT (volatile, not NV) */
#define LOAD_OPTION_ACTIVE 0x00000001u
#define BOOT_MAX 8
#define LABEL_MAX 40

/* EFI_GLOBAL_VARIABLE (UEFI Spec): BootOrder, Boot####, PK, KEK. */
static const EFI_GUID efi_global = {
    0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};
/* EFI_IMAGE_SECURITY_DATABASE_GUID: db and dbx. */
static const EFI_GUID efi_image_security = {
    0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f }
};

enum {
    ACT_CONTINUE = 1,
    ACT_BOOTMGR,
    ACT_SECBOOT,
    ACT_TPMCLEAR,
    ACT_DATETIME,
    ACT_NVRAM,
};

struct menu_item {
    const char *label;
    const char *serial_tag;             /* short id in "setup: selected ..." */
    int action;
};

static const struct menu_item items[] = {
    { "Continue boot",     "Continue boot", ACT_CONTINUE },
    { "Boot Manager",      "Boot Manager",  ACT_BOOTMGR },
    { "Secure Boot keys",  "Secure Boot",   ACT_SECBOOT },
    { "TPM clear",         "TPM clear",     ACT_TPMCLEAR },
    { "Date and time",     "Date and time", ACT_DATETIME },
    { "NVRAM reset",       "NVRAM reset",   ACT_NVRAM },
};

/* Original vendor GUID for Setup UI demo user NVRAM (not a Microsoft GUID). */
static const EFI_GUID squirrel_user_nv = {
    0xa7c3e91b, 0x2d54, 0x4f18, { 0x9b, 0x6e, 0x51, 0x8c, 0xd0, 0x3e, 0x7a, 0x22 }
};

typedef EFI_STATUS (EFIAPI *get_time_fn)(EFI_TIME *, EFI_TIME_CAPABILITIES *);
typedef EFI_STATUS (EFIAPI *set_time_fn)(EFI_TIME *);
#define NITEMS ((int)(sizeof items / sizeof items[0]))

/* Defined with the Secure Boot notebook; used by NVRAM seed too. */
static int var_size(const CHAR16 *name, EFI_GUID *vendor, UINTN *out);

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
     * reorder under QEMU without an OS installer. Descriptions only -
     * FilePathList is an End node (not a real boot path). Only called
     * when the setupui fw_cfg test gate is set, so fake Boot#### do not
     * land in NVRAM on real hardware via F2 alone. */
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
        /* Demo seed only under the test fw_cfg gate - not on F2 alone. */
        u16 sel;
        u32 fsz;
        if (!fwcfg_find(FWCFG_SETUPUI, &sel, &fsz) || !seed_demo_boot_options())
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
    u32 fill_w = panel_w > 16 ? panel_w - 16 : panel_w;
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
        fill_rect(panel_x + 8, row_y, fill_w, FONT_H * SCALE + 2, bg);
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

/* 1 = key ready, 0 = ConIn missing or WaitForEvent/ReadKeyStroke failed.
 * Callers break out of their menu loop on 0 so boot can continue. */
static int read_key(EFI_INPUT_KEY *key)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = efi_st->con_in;
    if (!in || !key || !in->wait_for_key)
        return 0;
    wait_for_event_fn wait = (wait_for_event_fn)efi_st->boot_services->wait_for_event;
    UINTN idx = 0;
    if (wait(1, &in->wait_for_key, &idx) != EFI_SUCCESS)
        return 0;
    return in->read_key_stroke(in, key) == EFI_SUCCESS;
}

/* ---- Date and time notebook ---------------------------------------- */

enum { DT_YEAR = 0, DT_MONTH, DT_DAY, DT_HOUR, DT_MIN, DT_SEC, DT_NFIELDS };
static const char *dt_field_name[] = {
    "Year", "Month", "Day", "Hour", "Minute", "Second"
};

static EFI_TIME dt_time;
static int dt_have_rtc;
static int dt_field;
static char dt_line[LABEL_MAX];
static char dt_field_line[LABEL_MAX];

static int days_in_month(u16 year, u8 month)
{
    static const u8 mdays[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 31;
    if (month == 2) {
        int leap = ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0));
        return leap ? 29 : 28;
    }
    return mdays[month];
}

static void clamp_dt_day(void)
{
    int dim = days_in_month(dt_time.year, dt_time.month);
    if (dt_time.day > dim)
        dt_time.day = (u8)dim;
    if (dt_time.day < 1)
        dt_time.day = 1;
}

static void format_dt_line(void)
{
    /* "RTC YYYY-MM-DD HH:MM:SS" fits LABEL_MAX. */
    if (!dt_have_rtc) {
        const char *s = "RTC unavailable";
        int i = 0;
        for (; s[i] && i < LABEL_MAX - 1; i++)
            dt_line[i] = s[i];
        dt_line[i] = 0;
        dt_field_line[0] = 0;
        return;
    }
    printf("setup: RTC %04d-%02d-%02d %02d:%02d:%02d\n",
           dt_time.year, dt_time.month, dt_time.day,
           dt_time.hour, dt_time.minute, dt_time.second);
    /* Rebuild a short GOP/status line without relying on snprintf. */
    {
        char tmp[40];
        int n = 0;
        const char *p = "RTC ";
        for (; *p && n < (int)sizeof tmp - 1; p++)
            tmp[n++] = *p;
        /* year */
        u16 y = dt_time.year;
        tmp[n++] = (char)('0' + (y / 1000) % 10);
        tmp[n++] = (char)('0' + (y / 100) % 10);
        tmp[n++] = (char)('0' + (y / 10) % 10);
        tmp[n++] = (char)('0' + y % 10);
        tmp[n++] = '-';
        tmp[n++] = (char)('0' + dt_time.month / 10);
        tmp[n++] = (char)('0' + dt_time.month % 10);
        tmp[n++] = '-';
        tmp[n++] = (char)('0' + dt_time.day / 10);
        tmp[n++] = (char)('0' + dt_time.day % 10);
        tmp[n++] = ' ';
        tmp[n++] = (char)('0' + dt_time.hour / 10);
        tmp[n++] = (char)('0' + dt_time.hour % 10);
        tmp[n++] = ':';
        tmp[n++] = (char)('0' + dt_time.minute / 10);
        tmp[n++] = (char)('0' + dt_time.minute % 10);
        tmp[n++] = ':';
        tmp[n++] = (char)('0' + dt_time.second / 10);
        tmp[n++] = (char)('0' + dt_time.second % 10);
        tmp[n] = 0;
        int i = 0;
        for (; tmp[i] && i < LABEL_MAX - 1; i++)
            dt_line[i] = tmp[i];
        dt_line[i] = 0;
    }
    {
        const char *f = "editing ";
        int n = 0;
        for (; f[n] && n < LABEL_MAX - 1; n++)
            dt_field_line[n] = f[n];
        const char *name = dt_field_name[dt_field];
        for (int i = 0; name[i] && n < LABEL_MAX - 1; i++)
            dt_field_line[n++] = name[i];
        dt_field_line[n] = 0;
    }
}

static int load_datetime(void)
{
    get_time_fn get =
        (get_time_fn)efi_st->runtime_services->get_time;
    memset(&dt_time, 0, sizeof dt_time);
    dt_have_rtc = 0;
    dt_field = DT_YEAR;
    if (!get || get(&dt_time, NULL) != EFI_SUCCESS) {
        format_dt_line();
        return 0;
    }
    if (dt_time.year < 1980 || dt_time.year > 2099)
        dt_time.year = 2000;
    clamp_dt_day();
    dt_have_rtc = 1;
    format_dt_line();
    return 1;
}

static void adjust_dt_field(int delta)
{
    if (!dt_have_rtc)
        return;
    switch (dt_field) {
    case DT_YEAR: {
        int y = (int)dt_time.year + delta;
        if (y < 1980)
            y = 2099;
        if (y > 2099)
            y = 1980;
        dt_time.year = (u16)y;
        clamp_dt_day();
        break;
    }
    case DT_MONTH: {
        int m = (int)dt_time.month + delta;
        if (m < 1)
            m = 12;
        if (m > 12)
            m = 1;
        dt_time.month = (u8)m;
        clamp_dt_day();
        break;
    }
    case DT_DAY: {
        int dim = days_in_month(dt_time.year, dt_time.month);
        int d = (int)dt_time.day + delta;
        if (d < 1)
            d = dim;
        if (d > dim)
            d = 1;
        dt_time.day = (u8)d;
        break;
    }
    case DT_HOUR: {
        int h = (int)dt_time.hour + delta;
        if (h < 0)
            h = 23;
        if (h > 23)
            h = 0;
        dt_time.hour = (u8)h;
        break;
    }
    case DT_MIN: {
        int m = (int)dt_time.minute + delta;
        if (m < 0)
            m = 59;
        if (m > 59)
            m = 0;
        dt_time.minute = (u8)m;
        break;
    }
    case DT_SEC: {
        int s = (int)dt_time.second + delta;
        if (s < 0)
            s = 59;
        if (s > 59)
            s = 0;
        dt_time.second = (u8)s;
        break;
    }
    }
    format_dt_line();
}

static int apply_datetime(void)
{
    set_time_fn set =
        (set_time_fn)efi_st->runtime_services->set_time;
    if (!dt_have_rtc || !set) {
        printf("setup: date-time apply skipped (no RTC)\n");
        return 0;
    }
    clamp_dt_day();
    if (set(&dt_time) != EFI_SUCCESS) {
        printf("setup: date-time apply failed\n");
        return 0;
    }
    /* Read back so the smoke sees CMOS, not only the workspace. Second
     * may tick between SetTime and GetTime; require Y/M/D/H/M. */
    get_time_fn get =
        (get_time_fn)efi_st->runtime_services->get_time;
    EFI_TIME check;
    memset(&check, 0, sizeof check);
    if (!get || get(&check, NULL) != EFI_SUCCESS ||
        check.year != dt_time.year || check.month != dt_time.month ||
        check.day != dt_time.day || check.hour != dt_time.hour ||
        check.minute != dt_time.minute) {
        printf("setup: date-time read-back mismatch\n");
        return 0;
    }
    printf("setup: date-time applied %04d-%02d-%02d %02d:%02d:%02d\n",
           check.year, check.month, check.day,
           check.hour, check.minute, check.second);
    return 1;
}

static void serial_datetime_menu(void)
{
    printf("setup: --- Date and Time ---\n");
    if (!dt_have_rtc) {
        printf("setup: RTC unavailable\n");
        printf("setup: Esc backs out\n");
        return;
    }
    /* format_dt_line already printed the RTC line when refreshing. */
    printf("setup: %s\n", dt_field_line);
    printf("setup: Up/Down adjust; Left/Right field; Enter applies; Esc cancels\n");
}

static void draw_datetime_menu(void)
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
    u32 panel_h = FONT_H * SCALE * 6 + 48;
    if (panel_y + panel_h > H)
        panel_h = H - panel_y;
    fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + 12, "Date and Time", COL_PANEL_FG, COL_PANEL_BG);

    u32 y = panel_y + 12 + FONT_H * SCALE + 8;
    put_str(panel_x + 24, y, dt_line, COL_PANEL_FG, COL_PANEL_BG);
    y += FONT_H * SCALE + 8;
    if (dt_have_rtc)
        put_str(panel_x + 24, y, dt_field_line, COL_HINT_FG, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + panel_h - FONT_H * SCALE - 12,
            dt_have_rtc ? "Up/Down  L/R field  Enter=apply  Esc"
                        : "Esc=back",
            COL_HINT_FG, COL_PANEL_BG);
}

static void datetime_run(void)
{
    printf("setup: selected Date and time\n");
    load_datetime();
    /* First serial paint: print RTC once (format_dt_line already did when
     * RTC is present); avoid a duplicate by only calling the menu shell. */
    printf("setup: --- Date and Time ---\n");
    if (!dt_have_rtc) {
        printf("setup: RTC unavailable\n");
        printf("setup: Esc backs out\n");
    } else {
        printf("setup: %s\n", dt_field_line);
        printf("setup: Up/Down adjust; Left/Right field; Enter applies; Esc cancels\n");
    }
    draw_datetime_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key)) {
            printf("setup: ConIn wait failed, leaving Date and time\n");
            break;
        }
        if (!dt_have_rtc) {
            if (key.scan_code == 0x17 || key.unicode_char == 0x1B ||
                key.unicode_char == '\r') {
                printf("setup: Date and time closed\n");
                break;
            }
            continue;
        }
        if (key.scan_code == 0x01) {            /* up: +1 */
            adjust_dt_field(+1);
            serial_datetime_menu();
            draw_datetime_menu();
        } else if (key.scan_code == 0x02) {     /* down: -1 */
            adjust_dt_field(-1);
            serial_datetime_menu();
            draw_datetime_menu();
        } else if (key.scan_code == 0x04 ||     /* left: previous field */
                   key.unicode_char == '-') {
            dt_field = dt_field ? dt_field - 1 : DT_NFIELDS - 1;
            format_dt_line();
            serial_datetime_menu();
            draw_datetime_menu();
        } else if (key.scan_code == 0x03 ||     /* right: next field */
                   key.unicode_char == '+') {
            dt_field = (dt_field + 1) % DT_NFIELDS;
            format_dt_line();
            serial_datetime_menu();
            draw_datetime_menu();
        } else if (key.scan_code == 0x17 ||
                   key.unicode_char == 0x1B) {
            printf("setup: date-time cancelled\n");
            break;
        } else if (key.unicode_char == '\r') {
            apply_datetime();
            break;
        }
    }
}

/* ---- NVRAM reset notebook ------------------------------------------ */

static int nv_sel;
static int nv_count;
static char nv_status[LABEL_MAX];

static int name_is(const CHAR16 *a, const char *b)
{
    for (; *b; a++, b++) {
        if (!*a || *a != (CHAR16)(u8)*b)
            return 0;
    }
    return *a == 0;
}

/* Secure Boot keys stay under their own notebook; NVRAM reset leaves them. */
static int is_secboot_var(const CHAR16 *name, const EFI_GUID *vendor)
{
    if (guid_eq(vendor, &efi_global) &&
        (name_is(name, "PK") || name_is(name, "KEK")))
        return 1;
    if (guid_eq(vendor, &efi_image_security) &&
        (name_is(name, "db") || name_is(name, "dbx")))
        return 1;
    return 0;
}

#define NV_CLEAR_MAX 32
struct nv_entry {
    CHAR16 name[32];
    EFI_GUID vendor;
};
static struct nv_entry nv_list[NV_CLEAR_MAX];

static int collect_user_nv(void)
{
    CHAR16 name[32];
    EFI_GUID vendor;
    UINTN size;
    int n = 0;

    name[0] = 0;
    size = sizeof name;
    for (;;) {
        EFI_STATUS st = vars_next(&size, name, &vendor);
        if (st == EFI_NOT_FOUND)
            break;
        if (st == EFI_BUFFER_TOO_SMALL) {
            /* Name longer than our buffer: skip by not enumerating further
             * safely - reset and stop so we do not loop forever. */
            break;
        }
        if (st != EFI_SUCCESS)
            break;
        u32 attr = 0;
        UINTN dsz = 0;
        EFI_STATUS gst = vars_get(name, &vendor, &attr, &dsz, NULL);
        if (gst == EFI_BUFFER_TOO_SMALL || gst == EFI_SUCCESS) {
            /* NV bit: user non-volatile settings (BootOrder, Boot####, ...). */
            if ((attr & 0x01u) && !is_secboot_var(name, &vendor) && n < NV_CLEAR_MAX) {
                int i = 0;
                for (; i < 31 && name[i]; i++)
                    nv_list[n].name[i] = name[i];
                nv_list[n].name[i] = 0;
                nv_list[n].vendor = vendor;
                n++;
            }
        }
        size = sizeof name;
    }
    return n;
}

static void format_nv_status(void)
{
    const char *p = "user NV vars: ";
    int n = 0;
    for (; p[n] && n < LABEL_MAX - 1; n++)
        nv_status[n] = p[n];
    int c = nv_count;
    if (c > 99)
        c = 99;
    if (c >= 10)
        nv_status[n++] = (char)('0' + c / 10);
    nv_status[n++] = (char)('0' + c % 10);
    nv_status[n] = 0;
}

static void seed_demo_user_nv(void)
{
    u16 sel;
    u32 fsz;
    UINTN z = 0;
    if (!fwcfg_find(FWCFG_SETUPUI, &sel, &fsz))
        return;
    if (var_size(u"SquirrelDemo", (EFI_GUID *)&squirrel_user_nv, &z))
        return;
    u8 stub = 0xA5;
    if (vars_set(u"SquirrelDemo", (EFI_GUID *)&squirrel_user_nv,
                 ATTR_NV_BS_RT, 1, &stub) != EFI_SUCCESS) {
        printf("setup: demo user NV seed failed\n");
        return;
    }
    printf("setup: seeded demo user NV var\n");
}

static int clear_user_nv(void)
{
    int removed = 0;
    for (int i = 0; i < nv_count; i++) {
        if (vars_set(nv_list[i].name, &nv_list[i].vendor, 0, 0, NULL) == EFI_SUCCESS)
            removed++;
    }
    if (removed == 0)
        printf("setup: NVRAM reset: nothing to clear\n");
    else
        printf("setup: NVRAM reset cleared %d user var%s\n",
               removed, removed == 1 ? "" : "s");
    return removed;
}

static const char *nv_act_label(int i)
{
    static const char *acts[] = { "Confirm reset", "Cancel" };
    return acts[i];
}

static void serial_nvram_menu(void)
{
    printf("setup: --- NVRAM Reset ---\n");
    printf("setup: %s\n", nv_status);
    printf("setup: %c Confirm reset\n", nv_sel == 0 ? '>' : ' ');
    printf("setup: %c Cancel\n", nv_sel == 1 ? '>' : ' ');
    printf("setup: Up/Down Enter; Esc cancels\n");
}

static void draw_nvram_menu(void)
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
    u32 fill_w = panel_w > 16 ? panel_w - 16 : panel_w;
    u32 panel_h = FONT_H * SCALE * 6 + 48;
    if (panel_y + panel_h > H)
        panel_h = H - panel_y;
    fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + 12, "NVRAM Reset", COL_PANEL_FG, COL_PANEL_BG);

    u32 y = panel_y + 12 + FONT_H * SCALE + 8;
    put_str(panel_x + 24, y, nv_status, COL_HINT_FG, COL_PANEL_BG);
    y += FONT_H * SCALE + 10;
    for (int i = 0; i < 2; i++) {
        u32 fg = i == nv_sel ? COL_SEL_FG : COL_PANEL_FG;
        u32 bg = i == nv_sel ? COL_SEL_BG : COL_PANEL_BG;
        fill_rect(panel_x + 8, y, fill_w, FONT_H * SCALE + 2, bg);
        put_str(panel_x + 24, y + 1, nv_act_label(i), fg, bg);
        y += FONT_H * SCALE + 4;
    }
    put_str(panel_x + 16, panel_y + panel_h - FONT_H * SCALE - 12,
            "Up/Down  Enter  Esc=cancel", COL_HINT_FG, COL_PANEL_BG);
}

static void nvram_reset_run(void)
{
    printf("setup: selected NVRAM reset\n");
    seed_demo_user_nv();
    nv_count = collect_user_nv();
    format_nv_status();
    nv_sel = 0;
    serial_nvram_menu();
    draw_nvram_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key)) {
            printf("setup: ConIn wait failed, leaving NVRAM reset\n");
            break;
        }
        if (key.scan_code == 0x01) {
            nv_sel = nv_sel ? nv_sel - 1 : 1;
            serial_nvram_menu();
            draw_nvram_menu();
        } else if (key.scan_code == 0x02) {
            nv_sel = (nv_sel + 1) % 2;
            serial_nvram_menu();
            draw_nvram_menu();
        } else if (key.scan_code == 0x17 ||
                   key.unicode_char == 0x1B) {
            printf("setup: NVRAM reset cancelled\n");
            break;
        } else if (key.unicode_char == '\r') {
            if (nv_sel == 0)
                clear_user_nv();
            else
                printf("setup: NVRAM reset cancelled\n");
            break;
        }
    }
}

/* ---- Secure Boot keys notebook ------------------------------------- */

static char sb_line[4][LABEL_MAX];
static char sb_mode[LABEL_MAX];
static int sb_sel;
static int sb_confirm;                  /* 0 = main, 1 = clear confirm */
static int sb_demo_seeded;              /* in-RAM stubs for empty QEMU */

/* Probe without reading the payload: absent vs present (N bytes). */
static int var_size(const CHAR16 *name, EFI_GUID *vendor, UINTN *out)
{
    UINTN size = 0;
    EFI_STATUS st = vars_get((CHAR16 *)name, vendor, NULL, &size, NULL);
    if (st == EFI_NOT_FOUND || size == 0)
        return 0;
    if (st == EFI_SUCCESS || st == EFI_BUFFER_TOO_SMALL) {
        *out = size;
        return 1;
    }
    return 0;
}

static void format_key_line(char *dst, int dst_n, const char *tag,
                            const CHAR16 *name, EFI_GUID *vendor)
{
    UINTN size = 0;
    int n = 0;
    for (; tag[n] && n < dst_n - 1; n++)
        dst[n] = tag[n];
    if (n < dst_n - 1)
        dst[n++] = ' ';
    if (!var_size(name, vendor, &size)) {
        const char *a = "absent";
        for (int i = 0; a[i] && n < dst_n - 1; i++)
            dst[n++] = a[i];
        dst[n] = 0;
        return;
    }
    /* "present (N bytes)" - N capped for the label width. */
    const char *p = "present (";
    for (int i = 0; p[i] && n < dst_n - 1; i++)
        dst[n++] = p[i];
    if (size > 9999)
        size = 9999;
    char num[5];
    int nd = 0;
    UINTN v = size;
    do {
        num[nd++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && nd < 4);
    while (nd-- && n < dst_n - 1)
        dst[n++] = num[nd];
    const char *s = " bytes)";
    for (int i = 0; s[i] && n < dst_n - 1; i++)
        dst[n++] = s[i];
    dst[n] = 0;
}

static void load_secboot_status(void)
{
    UINTN pk_size = 0;
    int have_pk = var_size(u"PK", (EFI_GUID *)&efi_global, &pk_size);
    const char *mode = have_pk ? "mode user (PK enrolled)" : "mode setup (no PK)";
    int i = 0;
    for (; mode[i] && i < LABEL_MAX - 1; i++)
        sb_mode[i] = mode[i];
    sb_mode[i] = 0;

    format_key_line(sb_line[0], LABEL_MAX, "PK", u"PK", (EFI_GUID *)&efi_global);
    format_key_line(sb_line[1], LABEL_MAX, "KEK", u"KEK", (EFI_GUID *)&efi_global);
    format_key_line(sb_line[2], LABEL_MAX, "db", u"db", (EFI_GUID *)&efi_image_security);
    format_key_line(sb_line[3], LABEL_MAX, "dbx", u"dbx", (EFI_GUID *)&efi_image_security);
}

static int delete_sb_var(const CHAR16 *name, EFI_GUID *vendor)
{
    EFI_STATUS st = vars_set((CHAR16 *)name, vendor, 0, 0, NULL);
    return st == EFI_SUCCESS;
}

/* When NVRAM has no Secure Boot keys (typical QEMU -bios), plant four
 * 1-byte volatile (BS|RT) stubs so the view/clear notebook has something
 * to show under the setupui fw_cfg test gate. Not valid
 * EFI_SIGNATURE_LISTs - scrubbed on clear, on partial seed failure, or
 * when leaving so secure_init (later this boot) still sees setup mode. */
static void scrub_demo_sb_keys(void)
{
    if (!sb_demo_seeded)
        return;
    delete_sb_var(u"PK", (EFI_GUID *)&efi_global);
    delete_sb_var(u"KEK", (EFI_GUID *)&efi_global);
    delete_sb_var(u"db", (EFI_GUID *)&efi_image_security);
    delete_sb_var(u"dbx", (EFI_GUID *)&efi_image_security);
    sb_demo_seeded = 0;
}

static void seed_demo_sb_keys(void)
{
    u16 sel;
    u32 fsz;
    UINTN z = 0;
    /* Test fw_cfg only: do not plant stubs on F2 / real hardware. */
    if (!fwcfg_find(FWCFG_SETUPUI, &sel, &fsz))
        return;
    if (var_size(u"PK", (EFI_GUID *)&efi_global, &z) ||
        var_size(u"KEK", (EFI_GUID *)&efi_global, &z) ||
        var_size(u"db", (EFI_GUID *)&efi_image_security, &z) ||
        var_size(u"dbx", (EFI_GUID *)&efi_image_security, &z))
        return;
    u8 stub = 0x5A;
    /* Mark seeded before the writes so a partial failure still scrubs. */
    sb_demo_seeded = 1;
    if (vars_set(u"PK", (EFI_GUID *)&efi_global, ATTR_BS_RT, 1, &stub) != EFI_SUCCESS ||
        vars_set(u"KEK", (EFI_GUID *)&efi_global, ATTR_BS_RT, 1, &stub) != EFI_SUCCESS ||
        vars_set(u"db", (EFI_GUID *)&efi_image_security, ATTR_BS_RT, 1, &stub) != EFI_SUCCESS ||
        vars_set(u"dbx", (EFI_GUID *)&efi_image_security, ATTR_BS_RT, 1, &stub) != EFI_SUCCESS) {
        scrub_demo_sb_keys();
        printf("setup: demo Secure Boot key seed failed (scrubbed)\n");
        return;
    }
    printf("setup: seeded demo Secure Boot key stubs\n");
}

static void clear_secboot_keys(void)
{
    int n = 0;
    n += delete_sb_var(u"PK", (EFI_GUID *)&efi_global);
    n += delete_sb_var(u"KEK", (EFI_GUID *)&efi_global);
    n += delete_sb_var(u"db", (EFI_GUID *)&efi_image_security);
    n += delete_sb_var(u"dbx", (EFI_GUID *)&efi_image_security);
    sb_demo_seeded = 0;
    if (n == 0)
        printf("setup: Secure Boot keys: nothing to clear\n");
    else
        /* Setup UI runs before secure_init, so clears apply this boot. */
        printf("setup: Secure Boot keys cleared (%d removed)\n", n);
    load_secboot_status();
}

static const char *sb_act_label(int i)
{
    static const char *acts[] = { "Clear all keys", "Back" };
    static const char *conf[] = { "Yes, clear keys", "No, cancel" };
    return sb_confirm ? conf[i] : acts[i];
}

static void serial_secboot_menu(void)
{
    if (sb_confirm) {
        printf("setup: Clear all Secure Boot keys?\n");
        printf("setup: %c Yes, clear keys\n", sb_sel == 0 ? '>' : ' ');
        printf("setup: %c No, cancel\n", sb_sel == 1 ? '>' : ' ');
        printf("setup: Up/Down Enter; Esc cancels\n");
        return;
    }
    printf("setup: --- Secure Boot Keys ---\n");
    printf("setup: %s\n", sb_mode);
    for (int i = 0; i < 4; i++)
        printf("setup: %s\n", sb_line[i]);
    printf("setup: %c Clear all keys\n", sb_sel == 0 ? '>' : ' ');
    printf("setup: %c Back\n", sb_sel == 1 ? '>' : ' ');
    printf("setup: Up/Down Enter; Esc backs out\n");
}

static void draw_secboot_menu(void)
{
    if (!have_gop)
        return;
    if (sb_confirm) {
        draw_panel_list("Clear Secure Boot keys?", "Up/Down  Enter  Esc=cancel",
                        2, sb_sel, sb_act_label);
        return;
    }
    u32 W = gop->mode->info->horizontal_resolution;
    u32 H = gop->mode->info->vertical_resolution;
    fill_rect(0, 0, W, H, COL_DESKTOP);
    u32 bar_h = FONT_H * SCALE + 16;
    fill_rect(0, 0, W, bar_h, COL_TITLE_BG);
    put_str(24, 8, "SquirrelBIOS Setup", COL_TITLE_FG, COL_TITLE_BG);

    u32 panel_x = 80, panel_y = bar_h + 40;
    u32 panel_w = W > 160 ? W - 160 : W;
    u32 fill_w = panel_w > 16 ? panel_w - 16 : panel_w;
    u32 rows = 4 + 2 + 2;               /* status + actions + title/hint */
    u32 panel_h = FONT_H * SCALE * rows + 48;
    if (panel_y + panel_h > H)
        panel_h = H - panel_y;
    fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + 12, "Secure Boot Keys", COL_PANEL_FG, COL_PANEL_BG);

    u32 y = panel_y + 12 + FONT_H * SCALE + 8;
    put_str(panel_x + 24, y, sb_mode, COL_HINT_FG, COL_PANEL_BG);
    y += FONT_H * SCALE + 4;
    for (int i = 0; i < 4; i++) {
        put_str(panel_x + 24, y, sb_line[i], COL_PANEL_FG, COL_PANEL_BG);
        y += FONT_H * SCALE + 2;
    }
    y += 6;
    for (int i = 0; i < 2; i++) {
        u32 fg = i == sb_sel ? COL_SEL_FG : COL_PANEL_FG;
        u32 bg = i == sb_sel ? COL_SEL_BG : COL_PANEL_BG;
        fill_rect(panel_x + 8, y, fill_w, FONT_H * SCALE + 2, bg);
        put_str(panel_x + 24, y + 1, sb_act_label(i), fg, bg);
        y += FONT_H * SCALE + 4;
    }
    put_str(panel_x + 16, panel_y + panel_h - FONT_H * SCALE - 12,
            "Up/Down  Enter  Esc=back", COL_HINT_FG, COL_PANEL_BG);
}

static void secboot_keys_run(void)
{
    printf("setup: selected Secure Boot\n");
    sb_demo_seeded = 0;
    seed_demo_sb_keys();
    load_secboot_status();
    sb_sel = 0;
    sb_confirm = 0;
    serial_secboot_menu();
    draw_secboot_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key)) {
            printf("setup: ConIn wait failed, leaving Secure Boot\n");
            break;
        }
        if (key.scan_code == 0x01) {            /* up */
            sb_sel = sb_sel ? sb_sel - 1 : 1;
            serial_secboot_menu();
            draw_secboot_menu();
        } else if (key.scan_code == 0x02) {     /* down */
            sb_sel = (sb_sel + 1) % 2;
            serial_secboot_menu();
            draw_secboot_menu();
        } else if (key.scan_code == 0x17 ||     /* Esc */
                   key.unicode_char == 0x1B) {
            if (sb_confirm) {
                printf("setup: Secure Boot clear cancelled\n");
                sb_confirm = 0;
                sb_sel = 0;
                serial_secboot_menu();
                draw_secboot_menu();
            } else {
                printf("setup: Secure Boot keys closed\n");
                break;
            }
        } else if (key.unicode_char == '\r') {
            if (sb_confirm) {
                if (sb_sel == 0) {
                    clear_secboot_keys();
                    break;              /* like BootOrder save: leave notebook */
                }
                printf("setup: Secure Boot clear cancelled\n");
                sb_confirm = 0;
                sb_sel = 0;
                serial_secboot_menu();
                draw_secboot_menu();
            } else if (sb_sel == 0) {
                sb_confirm = 1;
                sb_sel = 0;
                serial_secboot_menu();
                draw_secboot_menu();
            } else {
                printf("setup: Secure Boot keys closed\n");
                break;
            }
        }
    }
    scrub_demo_sb_keys();
}

/* ---- TPM clear confirmation ---------------------------------------- */

static int tpm_sel;
static char tpm_status[LABEL_MAX];

static const char *tpm_act_label(int i)
{
    static const char *acts[] = { "Confirm clear", "Cancel" };
    return acts[i];
}

static void load_tpm_status(void)
{
    const char *s = tpm_present() ? "TPM present (clear is a stub)" : "TPM none";
    int i = 0;
    for (; s[i] && i < LABEL_MAX - 1; i++)
        tpm_status[i] = s[i];
    tpm_status[i] = 0;
}

static void serial_tpm_menu(void)
{
    printf("setup: --- TPM Clear ---\n");
    printf("setup: %s\n", tpm_status);
    printf("setup: %c Confirm clear\n", tpm_sel == 0 ? '>' : ' ');
    printf("setup: %c Cancel\n", tpm_sel == 1 ? '>' : ' ');
    printf("setup: Up/Down Enter; Esc cancels\n");
}

static void draw_tpm_menu(void)
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
    u32 fill_w = panel_w > 16 ? panel_w - 16 : panel_w;
    u32 panel_h = FONT_H * SCALE * 6 + 48;
    if (panel_y + panel_h > H)
        panel_h = H - panel_y;
    fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL_BG);
    put_str(panel_x + 16, panel_y + 12, "TPM Clear", COL_PANEL_FG, COL_PANEL_BG);

    u32 y = panel_y + 12 + FONT_H * SCALE + 8;
    put_str(panel_x + 24, y, tpm_status, COL_HINT_FG, COL_PANEL_BG);
    y += FONT_H * SCALE + 10;
    for (int i = 0; i < 2; i++) {
        u32 fg = i == tpm_sel ? COL_SEL_FG : COL_PANEL_FG;
        u32 bg = i == tpm_sel ? COL_SEL_BG : COL_PANEL_BG;
        fill_rect(panel_x + 8, y, fill_w, FONT_H * SCALE + 2, bg);
        put_str(panel_x + 24, y + 1, tpm_act_label(i), fg, bg);
        y += FONT_H * SCALE + 4;
    }
    put_str(panel_x + 16, panel_y + panel_h - FONT_H * SCALE - 12,
            "Up/Down  Enter  Esc=cancel", COL_HINT_FG, COL_PANEL_BG);
}

static void do_tpm_clear(void)
{
    if (!tpm_present()) {
        printf("setup: TPM clear skipped (no TPM)\n");
        return;
    }
    /* Mid-boot TPM2_Clear would disturb measured PCRs already extended
     * in tpm_init. The confirmation is real; the command stays a stub
     * so Setup UI smokes stay safe with or without swtpm. */
    printf("setup: TPM clear confirmed (stub - not sent)\n");
}

static void tpm_clear_run(void)
{
    printf("setup: selected TPM clear\n");
    load_tpm_status();
    tpm_sel = 0;
    serial_tpm_menu();
    draw_tpm_menu();

    for (;;) {
        EFI_INPUT_KEY key;
        if (!read_key(&key)) {
            printf("setup: ConIn wait failed, leaving TPM clear\n");
            break;
        }
        if (key.scan_code == 0x01) {
            tpm_sel = tpm_sel ? tpm_sel - 1 : 1;
            serial_tpm_menu();
            draw_tpm_menu();
        } else if (key.scan_code == 0x02) {
            tpm_sel = (tpm_sel + 1) % 2;
            serial_tpm_menu();
            draw_tpm_menu();
        } else if (key.scan_code == 0x17 ||
                   key.unicode_char == 0x1B) {
            printf("setup: TPM clear cancelled\n");
            break;
        } else if (key.unicode_char == '\r') {
            if (tpm_sel == 0)
                do_tpm_clear();
            else
                printf("setup: TPM clear cancelled\n");
            break;
        }
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
        if (!read_key(&key)) {
            printf("setup: ConIn wait failed, leaving Boot Manager\n");
            break;
        }
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
            /* Skip SetVariable when nothing moved (avoids flash wear). */
            if (boot_dirty)
                save_boot_order();
            else
                printf("setup: BootOrder unchanged\n");
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
        if (!read_key(&key)) {
            printf("setup: ConIn wait failed, continuing boot\n");
            break;
        }
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
            printf("setup: Escape - Continue boot\n");
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
            if (it->action == ACT_SECBOOT) {
                secboot_keys_run();
                serial_main_menu();
                draw_main_menu();
                continue;
            }
            if (it->action == ACT_TPMCLEAR) {
                tpm_clear_run();
                serial_main_menu();
                draw_main_menu();
                continue;
            }
            if (it->action == ACT_DATETIME) {
                datetime_run();
                serial_main_menu();
                draw_main_menu();
                continue;
            }
            if (it->action == ACT_NVRAM) {
                nvram_reset_run();
                serial_main_menu();
                draw_main_menu();
                continue;
            }
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
 * SERIAL_IN fixtures like x86_64-con) still see their keys. Skipped when
 * ConIn is missing so boots without a console do not stall 250 ms. */
int setup_ui_hotkey(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = efi_st->con_in;
    if (!in || !in->wait_for_key)
        return 0;
    printf("setup: press F2 for Setup UI\n");
    int steps = HOTKEY_MS * 1000 / HOTKEY_STEP_US;
    for (int i = 0; i < steps; i++) {
        EFI_INPUT_KEY key;
        if (try_read_key(&key)) {
            if (key.scan_code == SCAN_F2) {
                printf("setup: F2 - opening Setup UI\n");
                return 1;
            }
            efi_con_unread_key(key);
            return 0;
        }
        efi_stall(HOTKEY_STEP_US);
    }
    return 0;
}
