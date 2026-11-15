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
    u8 regb = cmos(0x0B);
    u8 sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    u8 day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    u8 century = cmos(0x32);
    /* Register B bit 1: 1 = 24-hour, 0 = 12-hour (hour bit 7 = PM). */
    int use_24h = (regb & 0x02) != 0;
    int pm = 0;
    if (!use_24h) {
        pm = (hour & 0x80) != 0;
        hour = (u8)(hour & 0x7F);
    }
    if (!(regb & 0x04)) {                       /* BCD mode */
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
        century = bcd(century);
    }
    if (!use_24h) {
        /* 1..12 + PM → 0..23 civil hour. */
        if (hour == 12)
            hour = 0;
        if (pm)
            hour = (u8)(hour + 12);
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

static u8 to_bcd(u8 v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

static void cmos_write(u8 idx, u8 v)
{
    outb(0x70, idx);
    outb(0x71, v);
}

/* Write civil time to the MC146818 CMOS (QEMU and typical PC RTC).
 * SET bit in register B holds updates while the fields are written.
 * Register B bit 1 selects 12/24-hour encoding of the hour byte. */
int arch_rtc_set_civil(u16 year, u8 month, u8 day, u8 hour, u8 minute, u8 second)
{
    if (year < 1980 || year > 2099 ||
        month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59)
        return 0;
    for (int i = 0; i < 10000; i++)
        if (!(cmos(0x0A) & 0x80))
            break;
    u8 regb = cmos(0x0B);
    int use_bcd = !(regb & 0x04);
    int use_24h = (regb & 0x02) != 0;
    u8 sec = second, min = minute, hr = hour;
    u8 d = day, mon = month;
    u8 yr = (u8)(year % 100);
    u8 century = (u8)(year / 100);
    int pm = 0;
    if (!use_24h) {
        /* Civil 0..23 → 1..12 + PM (hour bit 7). */
        if (hour == 0) {
            hr = 12;
            pm = 0;
        } else if (hour < 12) {
            hr = hour;
            pm = 0;
        } else if (hour == 12) {
            hr = 12;
            pm = 1;
        } else {
            hr = (u8)(hour - 12);
            pm = 1;
        }
    }
    if (use_bcd) {
        sec = to_bcd(sec); min = to_bcd(min); hr = to_bcd(hr);
        d = to_bcd(d); mon = to_bcd(mon);
        yr = to_bcd(yr); century = to_bcd(century);
    }
    if (!use_24h && pm)
        hr = (u8)(hr | 0x80);
    cmos_write(0x0B, (u8)(regb | 0x80));       /* SET: stop updates */
    cmos_write(0x00, sec);
    cmos_write(0x02, min);
    cmos_write(0x04, hr);
    cmos_write(0x07, d);
    cmos_write(0x08, mon);
    cmos_write(0x09, yr);
    cmos_write(0x32, century);
    cmos_write(0x0B, regb);                     /* clear SET */
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
