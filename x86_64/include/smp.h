/* SquirrelBIOS x86_64-uefi: application processors (roadmap C4).
 * Original code. */
#ifndef SMP_H
#define SMP_H

#include "efi.h"

#define SMP_MAX_CPUS 1024

/* MP services protocol (UEFI PI specification, volume 2) */
#define EFI_MP_SERVICES_PROTOCOL_GUID \
    { 0x3fdda605, 0xa76e, 0x4f46, { 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08 } }

#define PROCESSOR_AS_BSP_BIT        0x1
#define PROCESSOR_ENABLED_BIT       0x2
#define PROCESSOR_HEALTH_STATUS_BIT 0x4
#define END_OF_CPU_LIST             0xFFFFFFFFu

#define EFI_NOT_STARTED EFI_ERR(19)
#define EFI_TIMEOUT     EFI_ERR(18)

typedef struct {
    u32 package;
    u32 core;
    u32 thread;
} EFI_CPU_PHYSICAL_LOCATION;

typedef struct {
    u64 processor_id;
    u32 status_flag;
    EFI_CPU_PHYSICAL_LOCATION location;
} EFI_PROCESSOR_INFORMATION;

typedef void (EFIAPI *EFI_AP_PROCEDURE)(void *);

typedef struct EFI_MP_SERVICES_PROTOCOL {
    EFI_STATUS (EFIAPI *get_number_of_processors)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN *, UINTN *);
    EFI_STATUS (EFIAPI *get_processor_info)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN,
                                            EFI_PROCESSOR_INFORMATION *);
    EFI_STATUS (EFIAPI *startup_all_aps)(struct EFI_MP_SERVICES_PROTOCOL *, EFI_AP_PROCEDURE,
                                         BOOLEAN, EFI_EVENT, UINTN, void *, UINTN **);
    EFI_STATUS (EFIAPI *startup_this_ap)(struct EFI_MP_SERVICES_PROTOCOL *, EFI_AP_PROCEDURE,
                                         UINTN, EFI_EVENT, UINTN, void *, BOOLEAN *);
    EFI_STATUS (EFIAPI *switch_bsp)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN, BOOLEAN);
    EFI_STATUS (EFIAPI *enable_disable_ap)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN, BOOLEAN, u32 *);
    EFI_STATUS (EFIAPI *who_am_i)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN *);
} EFI_MP_SERVICES_PROTOCOL;

extern const EFI_GUID efi_mp_services_guid;

/* What the platform tables need to know about each processor, in
 * processor-number order (the BSP first, then by APIC ID) */
struct smp_cpu_info {
    u32 apic_id;
    u32 package, core, thread;
    int enabled;
};

void smp_init(void);
void smp_park_aps(void);
int smp_cpu_count(void);
int smp_x2apic(void);
const struct smp_cpu_info *smp_cpu(int n);
void smp_run_all(EFI_AP_PROCEDURE fn, void *arg);   /* the BSP, then every AP */

/* apic.c: the local APIC, in xAPIC (MMIO) or x2APIC (MSR) mode */
static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (u64)hi << 32 | lo;
}

static inline void wrmsr(u32 msr, u64 v)
{
    __asm__ volatile("wrmsr" :: "a"((u32)v), "d"((u32)(v >> 32)), "c"(msr));
}

static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }

int apic_has_x2apic(void);
void apic_enable(int x2apic);
int apic_is_x2apic(void);
u32 apic_id(void);
void apic_send_ipi(u32 dest, u32 low, int all_but_self);

/* mptable.c: the MultiProcessor Specification 1.4 table */
#define MPS_TABLE_GUID \
    { 0xeb9d2d2f, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
void mptable_install(void);

#endif
