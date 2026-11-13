/* SquirrelBIOS x86_64-uefi: CSM, the legacy boot path (roadmap C3).
 * Original code.
 *
 * When nothing boots the UEFI way, the firmware starts the 16-bit x86-pc
 * SquirrelBIOS that is built into the flash image: it opens the chipset's
 * shadow RAM at E0000-FFFFF (PAM registers on i440FX and Q35), copies the
 * legacy ROM to F0000, write-protects it like a ROM, puts the display
 * back in VGA text mode and leaves long mode for the legacy reset vector
 * (csm.asm). The legacy BIOS then runs its own POST and boots a floppy,
 * an MBR disk, a CD or the network with INT 13h and friends, exactly as
 * on qemu-system-i386 -bios squirrel-bios.rom. */
#include "efi.h"

extern const u8 csm_tramp[], csm_tramp_end[];
extern const u8 csm_legacy_rom[], csm_legacy_rom_end[];

#define CSM_TRAMP   0x8000
#define LEGACY_BASE 0xF0000
#define LEGACY_SIZE 0x10000

/* PAM: one nibble per segment, bits 0-1 of a nibble = read DRAM, write DRAM */
#define PAM_OFF   0
#define PAM_RO    1
#define PAM_RW    3

static u8 pam_base;    /* config offset of PAM0 on host bridge 0:0.0 */

static int pam_find(void)
{
    u32 id = pci_read32(0, 0, 0, 0);
    switch (id) {
    case 0x12378086: pam_base = 0x59; return 1;   /* i440FX (QEMU pc) */
    case 0x29C08086: pam_base = 0x90; return 1;   /* Q35 MCH (QEMU q35) */
    default: return 0;
    }
}

/* PAM0 bits 5:4 hold F0000-FFFFF; PAM5 and PAM6 hold E0000-EFFFF in four
 * 16 KB nibbles */
static void pam_set_f(u8 mode)
{
    u8 v = pci_read8(0, 0, 0, pam_base);
    pci_write8(0, 0, 0, pam_base, (u8)((v & 0x0F) | mode << 4));
}

static void pam_set_e(u8 mode)
{
    for (int i = 5; i <= 6; i++)
        pci_write8(0, 0, 0, (u8)(pam_base + i), (u8)(mode | mode << 4));
}

/* Bochs DISPI off, so the legacy VGA code finds plain VGA again */
static void dispi_off(void)
{
    outw(0x01CE, 4);
    outw(0x01CF, 0);
}

int csm_boot(void)
{
    usize rom = (usize)(csm_legacy_rom_end - csm_legacy_rom);
    if (rom != LEGACY_SIZE || csm_legacy_rom[rom - 16] != 0xEA) {
        printf("csm: no legacy BIOS in this image\n");
        return 0;
    }
    if (!pam_find()) {
        printf("csm: unknown host bridge %08x, no shadow RAM\n", pci_read32(0, 0, 0, 0));
        return 0;
    }
    chipset_legacy_handoff();
    pam_set_e(PAM_RW);
    memset((void *)0xE0000, 0, 0x10000);
    pam_set_f(PAM_RW);
    memcpy((void *)LEGACY_BASE, csm_legacy_rom, LEGACY_SIZE);
    pam_set_f(PAM_RO);
    if (memcmp((void *)LEGACY_BASE, csm_legacy_rom, LEGACY_SIZE)) {
        printf("csm: shadow RAM at F0000 did not take the legacy BIOS\n");
        return 0;
    }
    /* Read-only now: a write must not stick */
    volatile u8 *probe = (volatile u8 *)(LEGACY_BASE + LEGACY_SIZE - 1);
    u8 was = *probe;
    *probe = (u8)~was;
    if (*probe != was) {
        printf("csm: shadow RAM at F0000 is not write-protected\n");
        return 0;
    }
    printf("csm: legacy BIOS (%lu KB) in shadow RAM at F0000, PAM at 0x%02x\n",
           (u64)(LEGACY_SIZE >> 10), pam_base);
    dispi_off();
    printf("csm: leaving long mode for F000:FFF0\n");
    memcpy((void *)CSM_TRAMP, csm_tramp, (usize)(csm_tramp_end - csm_tramp));
    ((void (*)(void))(uptr)CSM_TRAMP)();
    return 0;   /* not reached */
}
