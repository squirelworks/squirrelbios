/* SquirrelBIOS riscv64-uefi: what the shared UEFI core (common/efi)
 * needs to know about RISC-V on QEMU virt. Original code. */
#ifndef ARCH_H
#define ARCH_H

#ifndef CONFIG_UART_BASE
#define CONFIG_UART_BASE 0x10000000
#endif
#ifndef CONFIG_FWCFG_BASE
#define CONFIG_FWCFG_BASE 0x10100000
#endif

#define ARCH_NAME            "riscv64-uefi"
#define ARCH_PE_MACHINE      0x5064
#define ARCH_ALLOC_TOP       0x100000000000000ULL   /* no paging: any RAM */
#define ARCH_TEST_ADDR       arch_test_addr()
#define ARCH_FW_MEMORY_TYPE  EfiRuntimeServicesCode
#define ARCH_FW_MEMORY_ATTR  EFI_MEMORY_RUNTIME
#define ARCH_BOOT_FILE       u"\\EFI\\BOOT\\BOOTRISCV64.EFI"

unsigned long long arch_test_addr(void);

#endif
