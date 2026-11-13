/* SquirrelBIOS x86_64-uefi: UEFI variables, kept in flash (roadmap C5).
 * Original code, written from the UEFI specification (8.2 Variable
 * Services) and the CFI / Intel command set as QEMU's pflash_cfi01
 * implements it.
 *
 * Built into the runtime pages (Makefile X64_RT_C): GetVariable,
 * GetNextVariableName, SetVariable and QueryVariableInfo run after
 * ExitBootServices and may only call this file and lib.c. The boot-time
 * part (vars_init) is marked.
 *
 * Variables live in RAM in a packed arena. Non-volatile ones are also
 * written to a second flash device when there is one: QEMU started with
 *   -drive if=pflash,unit=0,readonly=on,file=squirrel-bios.fd
 *   -drive if=pflash,unit=1,file=vars.fd
 * maps the store right below the firmware. It is found by a CFI query.
 *
 * The store is two banks of half the device each. A bank starts with a
 * 32-byte header (magic, generation, then a valid mark written last);
 * records follow, each a 48-byte header, the name and the data, 8-byte
 * aligned. Flash bits only go from 1 to 0 without an erase, so a
 * record's state byte steps down: FF free, 7F being written, 3F valid,
 * 1F deleted. A change appends the new record, marks it valid, then
 * marks the old one deleted; a torn write leaves either the old or the
 * new record valid (the later one wins at boot). When a bank is full the
 * live variables are copied into the other bank, erased first, under the
 * next generation; its valid mark goes on last and the old bank is then
 * erased. At boot the valid bank with the highest generation is read. */
#include "efi.h"

#define ARENA       (96 * 1024)
#define VAR_MAX_SIZE (32 * 1024)            /* one variable's data */
#define REC_HDR     48
#define BANK_HDR    32
#define ST_FREE     0xFF
#define ST_WRITING  0x7F
#define ST_VALID    0x3F
#define ST_DELETED  0x1F
#define BANK_MARK   0x5A

#define EFI_VARIABLE_NON_VOLATILE       0x01
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x02
#define EFI_VARIABLE_RUNTIME_ACCESS     0x04
#define EFI_VARIABLE_HARDWARE_ERROR     0x08
#define EFI_VARIABLE_AUTH_WRITE         0x10
#define EFI_VARIABLE_TIME_AUTH_WRITE    0x20
#define EFI_VARIABLE_APPEND_WRITE       0x40

static const u8 bank_magic[8] = { 'S', 'Q', 'R', 'L', 'N', 'V', 'S', '1' };

/* A variable in the arena: total length (0 ends the arena), attributes,
 * sizes, vendor, the time of an authenticated write, where it is in
 * flash (0: not there), then the name and the data */
struct var {
    u32 total;
    u32 attr;
    u32 name_size;
    u32 data_size;
    EFI_GUID vendor;
    EFI_TIME time;
    u64 flash;
};
#define VAR_HDR ((u32)sizeof(struct var))

static u8 arena[ARENA] __attribute__((aligned(8)));
static u32 arena_used;

/* The flash store: its base, size, the bank in use and where its free
 * space begins */
static volatile u8 *fl;
static u32 fl_size, fl_block, fl_wbuf, bank_size;
static u32 bank, bank_free, generation;
static int fl_broken;

#define exited_vars efi_boot_services_exited()

/* A pointer to MMIO at a fixed address (kept opaque to the compiler) */
static volatile u8 *mmio(u64 a)
{
    volatile u8 *p = (volatile u8 *)(uptr)a;
    __asm__("" : "+r"(p));
    return p;
}

static u32 align8(u32 v) { return (v + 7) & ~7u; }
static CHAR16 *var_name(struct var *v) { return (CHAR16 *)(v + 1); }
static u8 *var_data(struct var *v) { return (u8 *)(v + 1) + v->name_size; }

static void move(u8 *d, const u8 *s, u32 n)
{
    if (d < s)
        for (u32 i = 0; i < n; i++)
            d[i] = s[i];
    else
        for (u32 i = n; i--;)
            d[i] = s[i];
}

static u32 name_size(const CHAR16 *n)
{
    u32 i = 0;
    while (n[i])
        i++;
    return (i + 1) * 2;
}

static struct var *find(const CHAR16 *name, const EFI_GUID *vendor)
{
    u32 ns = name_size(name);
    for (u32 at = 0; at < arena_used;) {
        struct var *v = (struct var *)(arena + at);
        if (v->name_size == ns && !memcmp(var_name(v), name, ns) && !memcmp(&v->vendor, vendor, 16))
            return v;
        at += v->total;
    }
    return NULL;
}

static void drop(struct var *v)
{
    u32 at = (u32)((u8 *)v - arena), len = v->total;
    move(arena + at, arena + at + len, arena_used - at - len);
    arena_used -= len;
}

static struct var *add(u32 attr, const EFI_GUID *vendor, const CHAR16 *name, u32 ns,
                       const void *data, u32 ds, const EFI_TIME *time)
{
    u32 total = align8(VAR_HDR + ns + ds);
    if (arena_used + total > ARENA)
        return NULL;
    struct var *v = (struct var *)(arena + arena_used);
    memset(v, 0, VAR_HDR);
    v->total = total;
    v->attr = attr;
    v->name_size = ns;
    v->data_size = ds;
    v->vendor = *vendor;
    if (time)
        v->time = *time;
    memcpy(var_name(v), name, ns);
    memcpy(var_data(v), data, ds);
    arena_used += total;
    return v;
}

/* ---- CFI flash (Intel command set, byte wide) ---- */

/* After a program or erase command the device reads back its status;
 * bit 7 is ready, bits 5, 4, 3, 1 errors */
static int fl_wait(volatile u8 *p)
{
    for (u32 i = 0; i < 10000000; i++) {
        u8 s = *p;
        if (s & 0x80) {
            if (s & 0x3A) {
                *p = 0x50;              /* clear status */
                *p = 0xFF;
                return 0;
            }
            return 1;
        }
    }
    *p = 0xFF;
    return 0;
}

/* Program n bytes, then check them in read-array mode. With a write
 * buffer (CFI 0x2A), a run within one aligned buffer is one command:
 * E8, the count less one, the bytes, D0. */
static int fl_program(u32 off, const void *src, u32 n)
{
    const u8 *s = src;
    volatile u8 *p = fl + off;
    for (u32 i = 0; i < n;) {
        u32 at = off + i;
        if (fl_wbuf > 1 && n - i > 1) {
            u32 run = fl_wbuf - (at & (fl_wbuf - 1));
            if (run > n - i)
                run = n - i;
            volatile u8 *b = fl + at;
            *b = 0xE8;
            if (!fl_wait(b))
                return 0;
            *b = (u8)(run - 1);
            for (u32 k = 0; k < run; k++)
                b[k] = s[i + k];
            *b = 0xD0;
            if (!fl_wait(b))
                return 0;
            i += run;
        } else {
            p[i] = 0x40;
            p[i] = s[i];
            if (!fl_wait(p + i))
                return 0;
            i++;
        }
    }
    if (n)
        p[0] = 0xFF;                    /* read array */
    for (u32 i = 0; i < n; i++)
        if (p[i] != s[i])
            return 0;
    return 1;
}

static int fl_erase(u32 off, u32 n)
{
    for (u32 b = off; b < off + n; b += fl_block) {
        volatile u8 *p = fl + b;
        *p = 0x20;
        *p = 0xD0;
        int ok = fl_wait(p);
        *p = 0xFF;
        if (!ok)
            return 0;
    }
    return 1;
}

/* ---- the log ---- */

static int put_record(u32 base, u32 *off, const struct var *v)
{
    u32 len = align8(REC_HDR + v->name_size + v->data_size);
    if (*off + len > bank_size)
        return 0;
    u32 at = base + *off;
    u8 st = ST_WRITING, h[REC_HDR];
    memset(h, 0xFF, sizeof h);
    h[0] = ST_WRITING;
    memcpy(h + 4, &v->attr, 4);
    memcpy(h + 8, &v->name_size, 4);
    memcpy(h + 12, &v->data_size, 4);
    memcpy(h + 16, &v->vendor, 16);
    memcpy(h + 32, &v->time, 16);
    if (!fl_program(at, &st, 1) || !fl_program(at + 1, h + 1, REC_HDR - 1) ||
        !fl_program(at + REC_HDR, (const u8 *)(v + 1), v->name_size + v->data_size))
        return 0;
    st = ST_VALID;
    if (!fl_program(at, &st, 1))
        return 0;
    *off += len;
    return 1;
}

/* Copy every non-volatile variable into the other bank */
static int compact(void)
{
    u32 nb = bank ^ 1, base = nb * bank_size, off = BANK_HDR;
    if (!fl_erase(base, bank_size))
        return 0;
    u32 gen = generation + 1;
    if (!fl_program(base, bank_magic, 8) || !fl_program(base + 8, &gen, 4))
        return 0;
    for (u32 at = 0; at < arena_used;) {
        struct var *v = (struct var *)(arena + at);
        if (v->attr & EFI_VARIABLE_NON_VOLATILE) {
            v->flash = base + off;
            if (!put_record(base, &off, v))
                return 0;
        }
        at += v->total;
    }
    u8 mark = BANK_MARK;
    if (!fl_program(base + 12, &mark, 1))
        return 0;
    fl_erase(bank * bank_size, bank_size);
    bank = nb;
    bank_free = off;
    generation = gen;
    return 1;
}

/* Write v to flash (and drop the record of what it replaces) */
static int persist(struct var *v, u64 old)
{
    if (!fl || fl_broken)
        return 1;
    u32 base = bank * bank_size, off = bank_free;
    v->flash = base + off;
    if (!put_record(base, &off, v)) {
        if (!compact())                 /* writes v too, and drops old */
            return fl_broken = 1, 0;
        return 1;
    }
    bank_free = off;
    if (old) {
        u8 st = ST_DELETED;
        fl_program((u32)old, &st, 1);
    }
    return 1;
}

static void unpersist(struct var *v)
{
    if (fl && !fl_broken && v->flash) {
        u8 st = ST_DELETED;
        fl_program((u32)v->flash, &st, 1);
    }
}

/* ---- the services ---- */

EFI_STATUS EFIAPI vars_get(CHAR16 *name, EFI_GUID *vendor, u32 *attr, UINTN *size, void *data)
{
    if (!name || !vendor || !size)
        return EFI_INVALID_PARAMETER;
    struct var *v = find(name, vendor);
    if (!v || (exited_vars && !(v->attr & EFI_VARIABLE_RUNTIME_ACCESS)))
        return EFI_NOT_FOUND;
    if (attr)
        *attr = v->attr;
    if (*size < v->data_size || !data) {
        *size = v->data_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(data, var_data(v), v->data_size);
    *size = v->data_size;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI vars_next(UINTN *size, CHAR16 *name, EFI_GUID *vendor)
{
    if (!size || !name || !vendor)
        return EFI_INVALID_PARAMETER;
    u32 at = 0;
    if (name[0]) {
        struct var *cur = find(name, vendor);
        if (!cur)
            return EFI_INVALID_PARAMETER;
        at = (u32)((u8 *)cur - arena) + cur->total;
    }
    for (; at < arena_used;) {
        struct var *v = (struct var *)(arena + at);
        at += v->total;
        if (exited_vars && !(v->attr & EFI_VARIABLE_RUNTIME_ACCESS))
            continue;
        if (*size < v->name_size) {
            *size = v->name_size;
            return EFI_BUFFER_TOO_SMALL;
        }
        memcpy(name, var_name(v), v->name_size);
        *size = v->name_size;
        *vendor = v->vendor;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

/* The authenticated-variable check, when Secure Boot's variable code is
 * linked in (authvar.c): EFI_SUCCESS to go on with the plain write */
vars_auth_fn vars_auth_hook;

EFI_STATUS EFIAPI vars_set(CHAR16 *name, EFI_GUID *vendor, u32 attr, UINTN size, void *data)
{
    if (!name || !vendor || !name[0] || (size && !data))
        return EFI_INVALID_PARAMETER;
    if (attr & (EFI_VARIABLE_HARDWARE_ERROR | EFI_VARIABLE_AUTH_WRITE))
        return EFI_UNSUPPORTED;
    if (attr && !(attr & EFI_VARIABLE_BOOTSERVICE_ACCESS))
        return EFI_INVALID_PARAMETER;
    if ((attr & EFI_VARIABLE_RUNTIME_ACCESS) == 0 && attr && exited_vars)
        return EFI_INVALID_PARAMETER;
    struct var *v = find(name, vendor);
    if (v && exited_vars && !(v->attr & EFI_VARIABLE_RUNTIME_ACCESS))
        return EFI_WRITE_PROTECTED;
    if (v && attr && (attr & ~EFI_VARIABLE_APPEND_WRITE) != v->attr)
        return EFI_INVALID_PARAMETER;
    EFI_TIME time;
    memset(&time, 0, sizeof time);
    if (v)
        time = v->time;
    const u8 *payload = data;
    u32 psize = (u32)size;
    if (vars_auth_hook) {
        EFI_STATUS st = vars_auth_hook(name, vendor, attr, &payload, &psize, &time,
                                       v ? &v->time : NULL, exited_vars);
        if (st != EFI_SUCCESS)
            return st;
    } else if (attr & EFI_VARIABLE_TIME_AUTH_WRITE) {
        return EFI_UNSUPPORTED;
    }
    int append = (attr & EFI_VARIABLE_APPEND_WRITE) != 0;
    attr &= ~EFI_VARIABLE_APPEND_WRITE;
    if ((!append && !psize) || !attr) {             /* delete */
        if (!v)
            return EFI_NOT_FOUND;
        unpersist(v);
        drop(v);
        return EFI_SUCCESS;
    }
    if (append && !psize && v)
        return EFI_SUCCESS;
    u32 old_size = v && append ? v->data_size : 0;
    if (old_size + psize > VAR_MAX_SIZE)
        return EFI_OUT_OF_RESOURCES;
    u32 ns = name_size(name);
    u32 need = align8(VAR_HDR + ns + old_size + psize);
    if (arena_used + need > ARENA)          /* old and new copies, for a moment */
        return EFI_OUT_OF_RESOURCES;
    /* The new variable goes at the end of the arena: its old data first
     * when appending, then the new; the old copy is dropped after */
    u64 old_flash = v ? v->flash : 0;
    struct var *n = add(attr, vendor, name, ns, v && append ? var_data(v) : payload,
                        v && append ? old_size : psize, &time);
    if (!n)
        return EFI_OUT_OF_RESOURCES;
    if (v && append) {
        memcpy(var_data(n) + old_size, payload, psize);
        n->data_size = old_size + psize;
        n->total = align8(VAR_HDR + ns + n->data_size);
        arena_used = (u32)((u8 *)n - arena) + n->total;
    }
    if (v) {
        u32 shift = v->total;
        drop(v);
        n = (struct var *)((u8 *)n - shift);
    }
    if ((attr & EFI_VARIABLE_NON_VOLATILE) && !persist(n, old_flash))
        return EFI_DEVICE_ERROR;
    if (!(attr & EFI_VARIABLE_NON_VOLATILE) && old_flash) {
        u8 st = ST_DELETED;
        if (fl && !fl_broken)
            fl_program((u32)old_flash, &st, 1);
    }
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI vars_query(u32 attr, u64 *max, u64 *rem, u64 *maxsz)
{
    if (!max || !rem || !maxsz)
        return EFI_INVALID_PARAMETER;
    if (attr & EFI_VARIABLE_NON_VOLATILE) {
        u64 store = fl ? bank_size - BANK_HDR : 0;
        u64 nv = 0;
        for (u32 at = 0; at < arena_used;) {
            struct var *v = (struct var *)(arena + at);
            if (v->attr & EFI_VARIABLE_NON_VOLATILE)
                nv += align8(REC_HDR + v->name_size + v->data_size);
            at += v->total;
        }
        *max = store;
        *rem = store > nv ? store - nv : 0;
    } else {
        *max = ARENA;
        *rem = ARENA - arena_used;
    }
    if (*rem > ARENA - arena_used)
        *rem = ARENA - arena_used;
    *maxsz = VAR_MAX_SIZE;
    return EFI_SUCCESS;
}

/* ---- boot time ---- */

/* CFI query anywhere in the device right below the firmware: the
 * firmware is the 1 MB device at the top of the first 4 GB */
static u32 probe(u64 top)
{
    volatile u8 *p = mmio(top - 0x100);
    p[0x55] = 0x98;
    u32 size = 0;
    if (p[0x10] == 'Q' && p[0x11] == 'R' && p[0x12] == 'Y') {
        u8 n = p[0x27];
        if (n >= 12 && n <= 26)
            size = 1u << n;
        u32 blk = (u32)(p[0x2F] | p[0x30] << 8) * 256;
        fl_block = blk >= 512 && blk <= 0x40000 ? blk : 4096;
        u8 w = p[0x2A];                 /* write buffer: 2^w bytes */
        fl_wbuf = w >= 1 && w <= 8 ? 1u << w : 0;
    }
    p[0] = 0xFF;
    return size;
}

static int load_bank(u32 b)
{
    u32 base = b * bank_size, off = BANK_HDR;
    while (off + REC_HDR <= bank_size) {
        volatile u8 *h = fl + base + off;
        u8 st = h[0];
        if (st == ST_FREE)
            break;
        u32 attr, ns, ds;
        memcpy(&attr, (const void *)(h + 4), 4);
        memcpy(&ns, (const void *)(h + 8), 4);
        memcpy(&ds, (const void *)(h + 12), 4);
        u32 len = align8(REC_HDR + ns + ds);
        if (ns < 4 || ns > 1024 || (ns & 1) || ds > VAR_MAX_SIZE || off + len > bank_size) {
            printf("vars: torn record at 0x%x, the store will be rewritten\n", off);
            off = bank_size;            /* full: the next write compacts */
            break;
        }
        if (st == ST_VALID) {
            EFI_GUID g;
            EFI_TIME t;
            memcpy(&g, (const void *)(h + 16), 16);
            memcpy(&t, (const void *)(h + 32), 16);
            const CHAR16 *name = (const CHAR16 *)(h + REC_HDR);
            if (name[ns / 2 - 1] == 0) {
                struct var *old = find(name, &g);
                if (old)
                    drop(old);          /* a torn update: the later one wins */
                struct var *v = add(attr, &g, name, ns, (const u8 *)(h + REC_HDR + ns), ds, &t);
                if (v)
                    v->flash = base + off;
            }
        }
        off += len;
    }
    bank_free = off;
    return 1;
}

void vars_init(void)                    /* boot time */
{
    u64 top = 0x100000000ull - 0x100000;
    fl_size = probe(top);
    if (!fl_size || fl_size < 2 * fl_block) {
        printf("vars: no flash store, variables are kept until reset\n");
        return;
    }
    fl = mmio(top - fl_size);
    bank_size = (fl_size / 2) & ~(fl_block - 1);
    int best = -1;
    u32 gen[2] = { 0, 0 };
    for (u32 b = 0; b < 2; b++) {
        volatile u8 *h = fl + b * bank_size;
        if (memcmp((const void *)h, bank_magic, 8) || h[12] != BANK_MARK)
            continue;
        memcpy(&gen[b], (const void *)(h + 8), 4);
        if (best < 0 || gen[b] > gen[best])
            best = (int)b;
    }
    if (best < 0) {
        /* A new store: bank 0, generation 1 */
        u32 g = 1;
        u8 mark = BANK_MARK;
        if (!fl_erase(0, fl_size) || !fl_program(0, bank_magic, 8) || !fl_program(8, &g, 4) ||
            !fl_program(12, &mark, 1)) {
            printf("vars: flash at %p does not take writes, variables are kept until reset\n", fl);
            fl = NULL;
            return;
        }
        bank = 0;
        generation = 1;
        bank_free = BANK_HDR;
        printf("vars: new store in %u KB of flash at %p\n", fl_size >> 10, fl);
    } else {
        bank = (u32)best;
        generation = gen[best];
        load_bank(bank);
        u32 n = 0;
        for (u32 at = 0; at < arena_used; at += ((struct var *)(arena + at))->total)
            n++;
        printf("vars: %u variable%s from %u KB of flash at %p (bank %u, generation %u)\n", n,
               n == 1 ? "" : "s", fl_size >> 10, fl, bank, generation);
    }
    efi_memory_mmio((u64)(uptr)fl, fl_size / 4096);
}

/* Boot time: the variable services in the runtime services table */
void vars_install(EFI_RUNTIME_SERVICES *rt)
{
    rt->get_variable = (void *)vars_get;
    rt->get_next_variable_name = (void *)vars_next;
    rt->set_variable = (void *)vars_set;
    rt->query_variable_info = (void *)vars_query;
    efi_reseal(&rt->hdr);
}
