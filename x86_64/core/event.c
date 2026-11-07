/* SquirrelBIOS x86_64-uefi: events, timers and time.
 * Original code.
 *
 * The firmware runs with interrupts off, so timers are polled: every
 * CheckEvent, WaitForEvent and Stall looks at the TSC, which is measured
 * against the PIT once at start. Notification functions run at the
 * moment the event is signalled (NOTIFY_SIGNAL) or checked (NOTIFY_WAIT);
 * there is no queue by TPL. */
#include "efi.h"

#define EVENT_MAGIC 0x746e657645517153ULL       /* "SqQEvent" */

struct event {
    u64 magic;
    struct event *next;
    u32 type;
    EFI_TPL tpl;
    EFI_EVENT_NOTIFY notify;
    void *context;
    int signaled;
    u64 trigger;                /* TSC value, 0 = no timer */
    u64 period;                 /* TSC ticks, 0 = one shot */
    EFI_GUID group;
    int has_group;
};

static struct event *events;
static u64 tsc_hz;
static u64 monotonic;

u64 efi_tsc_hz(void) { return tsc_hz; }

/* PIT channel 2, gated through port 61h: count down 10 ms, see how far
 * the TSC moved. */
static void calibrate(void)
{
    u8 gate = inb(0x61);
    outb(0x61, (u8)((gate & ~0x02) | 0x01));    /* gate on, speaker off */
    outb(0x43, 0xB0);                           /* ch2, lo/hi, mode 0 */
    outb(0x42, 11932 & 0xFF);                   /* 1193182 Hz / 100 */
    outb(0x42, 11932 >> 8);
    u64 t0 = rdtsc();
    u64 n = 0;
    while (!(inb(0x61) & 0x20) && ++n < 100000000)
        ;
    u64 t1 = rdtsc();
    outb(0x61, gate);
    tsc_hz = (n < 100000000 && t1 > t0) ? (t1 - t0) * 100 : 1000000000ULL;
}

static struct event *valid(EFI_EVENT e)
{
    for (struct event *p = events; p; p = p->next)
        if (p == e && p->magic == EVENT_MAGIC)
            return p;
    return NULL;
}

static void fire(struct event *e)
{
    if (e->type & EVT_NOTIFY_SIGNAL) {
        if (e->notify)
            e->notify(e, e->context);
    } else
        e->signaled = 1;
}

void efi_timers_poll(void)
{
    u64 now = rdtsc();
    for (struct event *e = events; e; e = e->next) {
        if (!e->trigger || now < e->trigger)
            continue;
        if (e->period)
            e->trigger += e->period * ((now - e->trigger) / e->period + 1);
        else
            e->trigger = 0;
        fire(e);
    }
}

static EFI_STATUS create(u32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY fn, void *ctx,
                         const EFI_GUID *group, EFI_EVENT *out)
{
    if (!out)
        return EFI_INVALID_PARAMETER;
    if ((type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL)) == (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL))
        return EFI_INVALID_PARAMETER;
    if ((type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL)) &&
        (!fn || tpl < TPL_APPLICATION || tpl > TPL_HIGH_LEVEL))
        return EFI_INVALID_PARAMETER;
    void *mem;
    if (efi_allocate_pool(EfiBootServicesData, sizeof(struct event), &mem) != EFI_SUCCESS)
        return EFI_OUT_OF_RESOURCES;
    struct event *e = mem;
    memset(e, 0, sizeof *e);
    e->magic = EVENT_MAGIC;
    e->type = type;
    e->tpl = tpl;
    e->notify = fn;
    e->context = ctx;
    if (group) {
        e->group = *group;
        e->has_group = 1;
    }
    e->next = events;
    events = e;
    *out = e;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_create_event(u32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY fn, void *ctx, EFI_EVENT *out)
{
    return create(type, tpl, fn, ctx, NULL, out);
}

static EFI_STATUS EFIAPI create_event_ex(u32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY fn, void *ctx,
                                         const EFI_GUID *group, EFI_EVENT *out)
{
    return create(type, tpl, fn, ctx, group, out);
}

static EFI_STATUS EFIAPI set_timer(EFI_EVENT ev, EFI_TIMER_DELAY how, u64 time100ns)
{
    struct event *e = valid(ev);
    if (!e || !(e->type & EVT_TIMER) || how > TimerRelative)
        return EFI_INVALID_PARAMETER;
    if (how == TimerCancel) {
        e->trigger = 0;
        return EFI_SUCCESS;
    }
    u64 ticks = time100ns * (tsc_hz / 10000000);
    if (ticks == 0)
        ticks = 1;
    e->period = how == TimerPeriodic ? ticks : 0;
    e->trigger = rdtsc() + ticks;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_signal_event(EFI_EVENT ev)
{
    struct event *e = valid(ev);
    if (!e)
        return EFI_INVALID_PARAMETER;
    if (e->has_group) {                 /* every event of the group */
        for (struct event *p = events; p; p = p->next)
            if (p->has_group && guid_eq(&p->group, &e->group))
                fire(p);
    } else
        fire(e);
    return EFI_SUCCESS;
}

static EFI_STATUS check(struct event *e)
{
    if (e->type & EVT_NOTIFY_SIGNAL)
        return EFI_INVALID_PARAMETER;
    efi_timers_poll();
    if (!e->signaled && (e->type & EVT_NOTIFY_WAIT))
        e->notify(e, e->context);
    if (e->signaled) {
        e->signaled = 0;
        return EFI_SUCCESS;
    }
    return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI check_event(EFI_EVENT ev)
{
    struct event *e = valid(ev);
    return e ? check(e) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI wait_for_event(UINTN n, EFI_EVENT *list, UINTN *index)
{
    if (!n || !list || !index)
        return EFI_INVALID_PARAMETER;
    for (;;) {
        for (UINTN i = 0; i < n; i++) {
            struct event *e = valid(list[i]);
            if (!e) {
                *index = i;
                return EFI_INVALID_PARAMETER;
            }
            EFI_STATUS s = check(e);
            if (s != EFI_NOT_READY) {
                *index = i;
                return s;
            }
        }
        __asm__ volatile("pause");
    }
}

static EFI_STATUS EFIAPI close_event(EFI_EVENT ev)
{
    struct event *e = valid(ev);
    if (!e)
        return EFI_INVALID_PARAMETER;
    for (struct event **pp = &events; *pp; pp = &(*pp)->next)
        if (*pp == e) {
            *pp = e->next;
            break;
        }
    e->magic = 0;
    efi_free_pool(e);
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_stall(UINTN us)
{
    u64 end = rdtsc() + us * (tsc_hz / 1000000);
    while (rdtsc() < end) {
        efi_timers_poll();
        __asm__ volatile("pause");
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_next_monotonic_count(u64 *count)
{
    if (!count)
        return EFI_INVALID_PARAMETER;
    *count = ++monotonic;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI set_watchdog_timer(UINTN timeout, u64 code, UINTN size, CHAR16 *data)
{
    (void)timeout, (void)code, (void)size, (void)data;
    return EFI_SUCCESS;                 /* no watchdog: nothing to arm */
}

void efi_event_init(EFI_BOOT_SERVICES *bs)
{
    calibrate();
    bs->create_event = (void *)efi_create_event;
    bs->create_event_ex = (void *)create_event_ex;
    bs->set_timer = (void *)set_timer;
    bs->wait_for_event = (void *)wait_for_event;
    bs->signal_event = (void *)efi_signal_event;
    bs->close_event = (void *)close_event;
    bs->check_event = (void *)check_event;
    bs->stall = efi_stall;
    bs->get_next_monotonic_count = (void *)get_next_monotonic_count;
    bs->set_watchdog_timer = (void *)set_watchdog_timer;
}
