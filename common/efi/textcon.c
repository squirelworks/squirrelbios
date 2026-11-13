/* SquirrelBIOS UEFI core (common/efi): Simple Text Input and Output on the serial console.
 * Original code.
 *
 * Output: UCS-2 is sent as UTF-8; attributes, clearing and cursor moves
 * as ANSI escape sequences, for a terminal on the serial line. One mode,
 * 80x25. Input: bytes from the line, with the usual terminal escape
 * sequences turned into UEFI scan codes. */
#include "efi.h"

#define COLS 80
#define ROWS 25

static EFI_SIMPLE_TEXT_INPUT_PROTOCOL con_in;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con_out;
static SIMPLE_TEXT_OUTPUT_MODE out_mode;
static const EFI_GUID in_guid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
static const EFI_GUID out_guid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;

/* ------------------------------------------------------------ output */

static void emit(const char *s)
{
    while (*s)
        con_raw(*s++);
}

static void emit_num(unsigned v)
{
    char b[12];
    int n = 0;
    do
        b[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        con_raw(b[--n]);
}

static void put_utf8(CHAR16 c)
{
    if (c < 0x80)
        con_raw((char)c);
    else if (c < 0x800) {
        con_raw((char)(0xC0 | c >> 6));
        con_raw((char)(0x80 | (c & 0x3F)));
    } else {
        con_raw((char)(0xE0 | c >> 12));
        con_raw((char)(0x80 | ((c >> 6) & 0x3F)));
        con_raw((char)(0x80 | (c & 0x3F)));
    }
}

static EFI_STATUS EFIAPI out_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, BOOLEAN ext)
{
    (void)ext;
    this->set_attribute(this, 0x07);
    return this->clear_screen(this);
}

static EFI_STATUS EFIAPI output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, CHAR16 *s)
{
    (void)this;
    if (!s)
        return EFI_INVALID_PARAMETER;
    for (; *s; s++) {
        CHAR16 c = *s;
        switch (c) {
        case '\n':
            con_raw('\n');
            if (out_mode.cursor_row < ROWS - 1)
                out_mode.cursor_row++;
            break;
        case '\r':
            con_raw('\r');
            out_mode.cursor_column = 0;
            break;
        case '\b':
            if (out_mode.cursor_column > 0) {
                con_raw('\b');
                out_mode.cursor_column--;
            }
            break;
        default:
            if (c >= 0xD800 && c < 0xE000)
                c = 0xFFFD;             /* no surrogates in UCS-2 */
            put_utf8(c);
            if (++out_mode.cursor_column == COLS) {
                out_mode.cursor_column = 0;
                if (out_mode.cursor_row < ROWS - 1)
                    out_mode.cursor_row++;
            }
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, CHAR16 *s)
{
    (void)this;
    return s ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, UINTN mode, UINTN *cols, UINTN *rows)
{
    (void)this;
    if (!cols || !rows)
        return EFI_INVALID_PARAMETER;
    if (mode != 0)
        return EFI_UNSUPPORTED;
    *cols = COLS;
    *rows = ROWS;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, UINTN mode)
{
    if (mode != 0)
        return EFI_UNSUPPORTED;
    return this->clear_screen(this);
}

/* EFI colour number to ANSI: blue and red, cyan and brown swap places */
static const u8 ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static EFI_STATUS EFIAPI set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, UINTN attr)
{
    (void)this;
    if (attr > 0x7F)
        return EFI_UNSUPPORTED;
    unsigned fg = attr & 0x0F, bg = (attr >> 4) & 0x07;
    emit("\033[0;");
    if (fg & 8)
        emit("1;");
    emit_num(30 + ansi[fg & 7]);
    con_raw(';');
    emit_num(40 + ansi[bg]);
    con_raw('m');
    out_mode.attribute = (s32)attr;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this)
{
    (void)this;
    emit("\033[2J\033[H");
    out_mode.cursor_column = 0;
    out_mode.cursor_row = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI set_cursor_position(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, UINTN col, UINTN row)
{
    (void)this;
    if (col >= COLS || row >= ROWS)
        return EFI_UNSUPPORTED;
    con_raw('\033');
    con_raw('[');
    emit_num((unsigned)row + 1);
    con_raw(';');
    emit_num((unsigned)col + 1);
    con_raw('H');
    out_mode.cursor_column = (s32)col;
    out_mode.cursor_row = (s32)row;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, BOOLEAN on)
{
    (void)this;
    emit(on ? "\033[?25h" : "\033[?25l");
    out_mode.cursor_visible = on ? 1 : 0;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------- input */

#define KEYS 16
static EFI_INPUT_KEY keys[KEYS];
static int key_head, key_tail;

static void push_key(u16 scan, CHAR16 ch)
{
    int next = (key_tail + 1) % KEYS;
    if (next == key_head)
        return;                         /* full: drop */
    keys[key_tail].scan_code = scan;
    keys[key_tail].unicode_char = ch;
    key_tail = next;
}

/* A byte from the line, waiting up to about 20 ms for it */
static int getc_wait(void)
{
    u64 end = arch_counter() + efi_counter_hz() / 50;
    int c;
    while ((c = con_getc()) < 0 && arch_counter() < end)
        arch_relax();
    return c;
}

/* After ESC [ : a final letter, or digits then ~ */
static void csi(void)
{
    int c = getc_wait(), n = 0;
    while (c >= '0' && c <= '9') {
        n = n * 10 + (c - '0');
        c = getc_wait();
    }
    u16 scan = 0;
    switch (c) {
    case 'A': scan = 0x01; break;       /* up */
    case 'B': scan = 0x02; break;       /* down */
    case 'C': scan = 0x03; break;       /* right */
    case 'D': scan = 0x04; break;       /* left */
    case 'H': scan = 0x05; break;       /* home */
    case 'F': scan = 0x06; break;       /* end */
    case '~':
        switch (n) {
        case 1: scan = 0x05; break;
        case 2: scan = 0x07; break;     /* insert */
        case 3: scan = 0x08; break;     /* delete */
        case 4: scan = 0x06; break;
        case 5: scan = 0x09; break;     /* page up */
        case 6: scan = 0x0A; break;     /* page down */
        case 15: scan = 0x0F; break;    /* F5 */
        case 17: case 18: case 19: case 20: case 21:
            scan = (u16)(0x10 + n - 17); break;         /* F6-F10 */
        case 23: scan = 0x15; break;    /* F11 */
        case 24: scan = 0x16; break;    /* F12 */
        }
        break;
    }
    if (scan)
        push_key(scan, 0);
}

static void poll_line(void)
{
    int c;
    while ((c = con_getc()) >= 0) {
        if (c == 0x1B) {
            int d = getc_wait();
            if (d == '[')
                csi();
            else if (d == 'O') {        /* ESC O P..S: F1-F4 */
                int e = getc_wait();
                if (e >= 'P' && e <= 'S')
                    push_key((u16)(0x0B + e - 'P'), 0);
            } else {
                push_key(0x17, 0);      /* ESC by itself */
                if (d >= 0)
                    push_key(0, (CHAR16)d);
            }
        } else if (c == 0x7F)
            push_key(0, 0x08);          /* DEL key: backspace */
        else if (c == '\n')
            push_key(0, '\r');
        else
            push_key(0, (CHAR16)c);     /* ASCII and Latin-1 */
    }
}

static EFI_STATUS EFIAPI in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *this, BOOLEAN ext)
{
    (void)this;
    (void)ext;
    key_head = key_tail = 0;
    while (con_getc() >= 0)
        ;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI read_key_stroke(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *this, EFI_INPUT_KEY *key)
{
    (void)this;
    if (!key)
        return EFI_INVALID_PARAMETER;
    poll_line();
    if (key_head == key_tail)
        return EFI_NOT_READY;
    *key = keys[key_head];
    key_head = (key_head + 1) % KEYS;
    return EFI_SUCCESS;
}

static void EFIAPI wait_for_key_notify(EFI_EVENT ev, void *ctx)
{
    (void)ctx;
    poll_line();
    if (key_head != key_tail)
        efi_signal_event(ev);
}

void efi_console_init(void)
{
    con_out.reset = out_reset;
    con_out.output_string = output_string;
    con_out.test_string = test_string;
    con_out.query_mode = query_mode;
    con_out.set_mode = set_mode;
    con_out.set_attribute = set_attribute;
    con_out.clear_screen = clear_screen;
    con_out.set_cursor_position = set_cursor_position;
    con_out.enable_cursor = enable_cursor;
    con_out.mode = &out_mode;
    out_mode.max_mode = 1;
    out_mode.mode = 0;
    out_mode.attribute = 0x07;
    out_mode.cursor_visible = 1;

    con_in.reset = in_reset;
    con_in.read_key_stroke = read_key_stroke;
    efi_create_event(EVT_NOTIFY_WAIT, TPL_NOTIFY, wait_for_key_notify, NULL, &con_in.wait_for_key);

    EFI_HANDLE h = NULL;
    efi_install_protocol_interface(&h, (EFI_GUID *)&in_guid, 0, &con_in);
    efi_install_protocol_interface(&h, (EFI_GUID *)&out_guid, 0, &con_out);
    efi_st->console_in_handle = h;
    efi_st->con_in = &con_in;
    efi_st->console_out_handle = h;
    efi_st->con_out = &con_out;
    efi_st->standard_error_handle = h;
    efi_st->std_err = &con_out;
    efi_reseal(&efi_st->hdr);
}
