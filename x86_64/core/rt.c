/* SquirrelBIOS x86_64-uefi: the architecture's runtime services for the
 * shared core (common/efi/runtime.c): the CMOS clock and reset. Original
 * code. Built into the runtime pages with runtime.c and lib.c (Makefile,
 * link.ld): nothing here may call outside those after ExitBootServices. */
#include "efi.h"
#include "acpi.h"

/* chipset()->pm_base, kept where the OS keeps it (x86_rt_init) */
static u16 rt_pm_base;

void x86_rt_init(u16 pm_base)
{
    rt_pm_base = pm_base;
}

static u8 cmos(u8 idx)
{
    outb(0x70, idx);
    return inb(0x71);
}

static u8 bcd(u8 v)
{
    return (u8)((v >> 4) * 10 + (v & 0x0F));
}

/* Days from 1970-01-01 to a civil date (proleptic Gregorian) */
static u64 days(u32 y, u32 m, u32 d)
{
    y -= m <= 2;
    u32 era = y / 400, yoe = y - era * 400;
    u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (u64)era * 146097 + doe - 719468;
}

int arch_rtc_seconds(u64 *secs)
{
    /* Wait for update-in-progress to clear. */
    for (int i = 0; i < 10000; i++)
        if (!(cmos(0x0A) & 0x80))
            break;
    u8 sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    u8 day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    u8 century = cmos(0x32);
    if (!(cmos(0x0B) & 0x04)) {                 /* BCD mode */
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
        century = bcd(century);
    }
    u32 y = century * 100u + year;
    if (y < 1980 || y > 2099)
        y = 2000 + year % 100;
    if (!day || day > 31)
        day = 1;
    if (!mon || mon > 12)
        mon = 1;
    *secs = days(y, mon, day) * 86400 + hour * 3600u + min * 60u + sec;
    return 1;
}

void arch_reset(int type, u64 status)
{
    (void)status;
    if (type == EfiResetShutdown) {
        /* isa-debug-exit if the smoke test attached one at 0xF4. */
        outb(0xF4, 0x00);
        /* SLP_EN with the SLP_TYP the DSDT gives \_S5 (0: QEMU's soft
         * off on both PIIX4 and ICH9), on the PM block chipset.c opened. */
        if (rt_pm_base)
            outw((u16)(rt_pm_base + 4), 0x2000 | (ACPI_S5_SLP_TYP << 10));
        else
            outw(0x604, 0x2000 | (ACPI_S5_SLP_TYP << 10));  /* QEMU's default PM block */
        outw(0xB004, 0x2000);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    /* Warm/cold: pulse the keyboard-controller reset line, then 0xCF9. */
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* The services use port I/O only: nothing moves */
void arch_virtual_map(void)
{
}
