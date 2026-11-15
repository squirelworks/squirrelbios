/* Stand-in EFI application for the firmware's own EL3 monitor (QEMU
 * virt with secure=on, arm/uefi/el3.c). Original code, built like
 * tests/efi-app.c (static ELF, tests/mkpe.py) for AArch64. It makes
 * PSCI calls through smc, the way an OS does, and reports through ConOut:
 *
 *   PSCI version=ok features=ok migrate=ok self=ok bad=ok on=ok cpus=3
 *       off=ok again=ok
 *
 * "on" starts every other CPU (MPIDR 1, 2, 3) at a small entry point
 * that writes its MPIDR to the context ID's address and calls CPU_OFF;
 * "off" waits for AFFINITY_INFO to say each is off again, "again"
 * starts CPU 1 a second time. Then it prints "PSCI all ok" (or "PSCI
 * failed") and calls ResetSystem(EfiResetShutdown): the firmware makes
 * that PSCI SYSTEM_OFF, which the monitor turns into the GPIO power-off,
 * so QEMU exits with status 0. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned long uintn;
typedef u64 status;
typedef void *handle;
typedef u16 char16;
typedef status (*fn)();

typedef struct { u64 signature; u32 revision, header_size, crc32, reserved; } table_header;
typedef struct text_out { fn reset, output_string; } text_out;
typedef struct {
    table_header hdr;
    fn get_time, set_time, get_wakeup_time, set_wakeup_time, set_virtual_address_map,
       convert_pointer, get_variable, get_next_variable_name, set_variable,
       get_next_high_monotonic_count, reset_system;
} runtime_services;
typedef struct {
    table_header hdr;
    char16 *firmware_vendor;
    u32 firmware_revision;
    handle con_in_handle;
    void *con_in;
    handle con_out_handle;
    text_out *con_out;
    handle stderr_handle;
    void *std_err;
    runtime_services *rt;
    void *bs;
} system_table;

static system_table *st;

static void puts(const char *s)
{
    char16 buf[128];
    uintn n = 0;
    while (*s && n < 120) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (u8)*s++;
    }
    buf[n] = 0;
    st->con_out->output_string(st->con_out, buf);
}

static int all = 1;

static void result(const char *name, int ok)
{
    puts(" ");
    puts(name);
    puts(ok ? "=ok" : "=bad");
    all &= ok;
}

static s64 psci(u64 f, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = f;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (s64)x0;
}

#define VERSION      0x84000000
#define CPU_OFF      0x84000002
#define CPU_ON       0xC4000003
#define AFFINITY     0xC4000004
#define MIGRATE_INFO 0x84000006
#define SYSTEM_OFF   0x84000008
#define FEATURES     0x8400000A
#define RESET2       0xC4000012

/* A started CPU arrives here with its MMU off and x0 = context ID: it
 * stores MPIDR | 1 << 32 there and calls CPU_OFF */
void secondary(void);
__asm__(".text\n.globl secondary\nsecondary:\n"
        "  mrs x1, mpidr_el1\n"
        "  and x1, x1, #0xffffff\n"
        "  orr x1, x1, #0x100000000\n"
        "  str x1, [x0]\n"
        "  dsb sy\n"
        "  movz x0, #0x0002\n"
        "  movk x0, #0x8400, lsl #16\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n");

static volatile u64 seen[4];

static u64 counter(void)
{
    u64 v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static u64 hz(void)
{
    u64 v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v & 0xFFFFFFFF;
}

/* until cond holds or a second passes */
#define WAIT(cond) ({ u64 _end = counter() + hz(); while (!(cond) && counter() < _end) ; (cond); })

static void invalidate(volatile void *p)
{
    __asm__ volatile("dc civac, %0; dsb sy" :: "r"(p) : "memory");
}

status efi_main(handle image, system_table *systab)
{
    (void)image;
    st = systab;
    puts("PSCI");
    result("version", psci(VERSION, 0, 0, 0) == 0x10001);
    result("features", psci(FEATURES, CPU_ON, 0, 0) == 0 && psci(FEATURES, CPU_OFF, 0, 0) == 0 &&
                       psci(FEATURES, RESET2, 0, 0) == -1 && psci(0x8400FFFF, 0, 0, 0) == -1);
    result("migrate", psci(MIGRATE_INFO, 0, 0, 0) == 2);
    u64 me;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(me));
    result("self", psci(CPU_ON, me & 0xFFFFFF, (u64)secondary, 0) == -4 &&
                   psci(AFFINITY, me & 0xFFFFFF, 0, 0) == 0);
    result("bad", psci(CPU_ON, 0x55, (u64)secondary, 0) == -2 && psci(AFFINITY, 0x55, 0, 0) == -2);

    int ok = 1, cpus = 0;
    for (u64 c = 1; c < 4; c++)
        ok &= psci(CPU_ON, c, (u64)secondary, (u64)&seen[c]) == 0;
    for (u64 c = 1; c < 4; c++) {
        invalidate(&seen[c]);
        if (WAIT((invalidate(&seen[c]), seen[c] == (c | 1ULL << 32))))
            cpus++;
    }
    result("on", ok && cpus == 3);
    puts(cpus == 3 ? " cpus=3" : " cpus=?");
    ok = 1;
    for (u64 c = 1; c < 4; c++)
        ok &= WAIT(psci(AFFINITY, c, 0, 0) == 1);
    result("off", ok);
    seen[1] = 0;
    invalidate(&seen[1]);
    ok = psci(CPU_ON, 1, (u64)secondary, (u64)&seen[1]) == 0 &&
         WAIT((invalidate(&seen[1]), seen[1] == (1 | 1ULL << 32))) && WAIT(psci(AFFINITY, 1, 0, 0) == 1);
    result("again", ok);
    puts(all ? "\nPSCI all ok\n" : "\nPSCI failed\n");
    st->rt->reset_system(2, 0, 0, (void *)0);       /* EfiResetShutdown */
    puts("PSCI still running after ResetSystem\n");
    return 0x1234;
}
