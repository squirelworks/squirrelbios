/* SquirrelBIOS x86_64-uefi: chipset set-up for QEMU's two PC machines.
 * Original code, written from the Intel 82371AB (PIIX4), 82371SB (PIIX3)
 * and ICH9 / 3 Series (MCH) datasheets and QEMU's documented defaults.
 *
 * We are the platform firmware, so nothing else opens the ACPI PM I/O
 * block, the PCI Express configuration window (q35) or the PCI interrupt
 * router (i440fx). This file does all three, once, and remembers what it
 * chose so the ACPI tables (acpi.c) describe the same hardware. */
#include "sq.h"
#include "smm.h"

#define PM_BASE         0x600
#define MCFG_BASE_Q35   0xB0000000u     /* 256 MB window: buses 0-255 */

static struct chipset cs;
static int mcfg_enabled_here;

const struct chipset *chipset(void)
{
    return &cs;
}

/* i440fx: PIRQA-D steered to ISA IRQs 10, 10, 11, 11; the same IRQs are
 * made level-triggered in the ELCR so the 8259s agree. */
static const u8 piix_pirq[4] = { 10, 10, 11, 11 };

/* Which PIRQ (0-3 = A-D, 4-7 = E-H) a bus 0 slot/pin is wired to, as
 * QEMU wires them: on i440fx (slot + pin - 1) mod 4; on q35 the ICH9
 * functions at 19h-1Dh and 1Fh use A-D by pin, everything else E-H by
 * (slot + pin) mod 4. 8 stands for the SCI (i440fx slot 1 pin A, the
 * PIIX4 PM function). */
u8 chipset_pirq(u8 slot, u8 pin)
{
    if (cs.q35) {
        if ((slot >= 0x19 && slot <= 0x1D) || slot == 0x1F)
            return pin & 3;
        return (u8)(4 + ((slot + pin) & 3));
    }
    if (slot == 1 && pin == 0)
        return 8;                       /* PIIX4 PM function: the SCI */
    return (u8)((slot + pin - 1) & 3);
}

/* The IRQ (i440fx: ISA IRQ through the router) or GSI (q35: IOAPIC pins
 * 16-23, which QEMU's ICH9 always drives) of a PIRQ. */
u8 chipset_pirq_irq(u8 pirq)
{
    if (pirq == 8)
        return 9;
    return cs.q35 ? (u8)(16 + pirq) : piix_pirq[pirq & 3];
}

static void pm_enable(u8 dev, u8 fn, u8 en_reg, u8 en_bit)
{
    u32 pm = pci_read32(0, dev, fn, 0x40);
    if (!(pm & 1) || !(pm & ~1u)) {
        pci_write32(0, dev, fn, 0x40, PM_BASE | 1);
        pm = PM_BASE | 1;
    }
    pci_write8(0, dev, fn, en_reg, (u8)(pci_read8(0, dev, fn, en_reg) | en_bit));
    cs.pm_base = (u16)(pm & 0xFFC0);
}

/* Write the interrupt line register of every bus 0 function with a pin,
 * for software that reads it instead of the ACPI routing table. */
static void pci_lines(void)
{
    for (u8 dev = 0; dev < 32; dev++) {
        for (u8 fn = 0; fn < 8; fn++) {
            u16 vend = pci_read16(0, dev, fn, 0x00);
            if (vend == 0xFFFF) {
                if (fn == 0)
                    break;
                continue;
            }
            u8 pin = pci_read8(0, dev, fn, 0x3D);
            if (pin >= 1 && pin <= 4) {
                u8 irq = chipset_pirq_irq(chipset_pirq(dev, (u8)(pin - 1)));
                pci_write8(0, dev, fn, 0x3C, irq);
            }
            if (fn == 0 && !(pci_read8(0, dev, 0, 0x0E) & 0x80))
                break;
        }
    }
}

void chipset_init(void)
{
    u16 host = pci_read16(0, 0, 0, 0x02);
    cs.q35 = pci_read16(0, 0, 0, 0x00) == 0x8086 && host == 0x29C0;

    if (cs.q35) {
        /* ICH9 LPC at 00:1f.0: PMBASE at 40h, ACPI_EN is bit 7 of 44h.
         * GPE0 is the 16-byte block at PMBASE+20h. */
        pm_enable(0x1F, 0, 0x44, 0x80);
        cs.gpe0 = (u16)(cs.pm_base + 0x20);
        cs.gpe0_len = 16;
        /* MCH PCIEXBAR (00:00.0, 60h, 64-bit): base, length 256 MB
         * (bits 2:1 = 0), enable (bit 0). */
        u32 lo = pci_read32(0, 0, 0, 0x60);
        if (!(lo & 1)) {
            pci_write32(0, 0, 0, 0x64, 0);
            pci_write32(0, 0, 0, 0x60, MCFG_BASE_Q35 | 1);
            lo = MCFG_BASE_Q35 | 1;
            mcfg_enabled_here = 1;
        }
        cs.mcfg_base = lo & 0xF0000000u;
        cs.mcfg_buses = 256;
    } else {
        /* PIIX4 PM at 00:01.3: PMBA at 40h, PMIOSE is bit 0 of 80h.
         * QEMU puts the GPE0 block at 0xAFE0, 4 bytes. */
        pm_enable(1, 3, 0x80, 0x01);
        cs.gpe0 = 0xAFE0;
        cs.gpe0_len = 4;
        /* PIIX3 PIRQ route control, 00:01.0 60h-63h */
        u16 elcr = 0;
        for (int i = 0; i < 4; i++) {
            pci_write8(0, 1, 0, (u8)(0x60 + i), piix_pirq[i]);
            elcr |= (u16)(1u << piix_pirq[i]);
        }
        elcr |= 1u << 9;                /* SCI */
        outb(0x4D0, (u8)(inb(0x4D0) | (elcr & 0xFF)));
        outb(0x4D1, (u8)(inb(0x4D1) | (elcr >> 8)));
    }
    /* ACPI mode from the start: SCI_EN in PM1a_CNT. The FADT then has no
     * SMI command port, which tells the OS there is nothing to switch. */
    u16 cnt = (u16)(cs.pm_base + 4);
    outw(cnt, (u16)(inw(cnt) | 1));
    pci_lines();
}

/* Before the CSM hands the machine to the legacy BIOS: give back what
 * only the UEFI side wanted. The legacy BIOS switches to ACPI mode itself
 * when its OS asks, and does not expect an ECAM window on q35. */
void chipset_legacy_handoff(void)
{
    smm_legacy_handoff();
    if (cs.pm_base) {
        u16 cnt = (u16)(cs.pm_base + 4);
        outw(cnt, (u16)(inw(cnt) & ~1u));
    }
    if (mcfg_enabled_here) {
        pci_write32(0, 0, 0, 0x60, 0);
        mcfg_enabled_here = 0;
    }
}
