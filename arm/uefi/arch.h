/* SquirrelBIOS aarch64-uefi: what the shared UEFI core (common/efi)
 * needs to know about AArch64 on QEMU virt. Original code. */
#ifndef ARCH_H
#define ARCH_H

#ifndef CONFIG_UART_BASE
#define CONFIG_UART_BASE 0x09000000
#endif
#ifndef CONFIG_FWCFG_BASE
#define CONFIG_FWCFG_BASE 0x09020000
#endif

#define ARCH_NAME            "aarch64-uefi"
#define ARCH_PE_MACHINE      0xAA64
#define ARCH_ALLOC_TOP       0x8000000000ULL    /* 512 GB: the first level-0 entry */
#define ARCH_TEST_ADDR       0x41000000ULL      /* RAM base + 16 MB */
/* The whole image (code, data, stack) stays for the runtime services */
#define ARCH_FW_MEMORY_TYPE  EfiRuntimeServicesCode
#define ARCH_FW_MEMORY_ATTR  EFI_MEMORY_RUNTIME
#define ARCH_BOOT_FILE       u"\\EFI\\BOOT\\BOOTAA64.EFI"

/* QEMU virt puts the device tree at the start of RAM for -bios */
#define ARCH_DTB_ADDR        0x40000000UL

/* acpi.c: the ACPI tables, from the device tree */
void arm_acpi_init(void);

#endif
