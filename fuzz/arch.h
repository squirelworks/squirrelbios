/* Host-side arch.h for fuzzing common/efi parsers without QEMU.
 * Original code. Satisfies the ARCH_* constants sq.h expects; the
 * firmware's arch_* functions are not linked into the harness. */
#ifndef ARCH_H
#define ARCH_H

#define ARCH_NAME            "host-fuzz"
#define ARCH_PE_MACHINE      0x8664
#define ARCH_ALLOC_TOP       0x8000000000ULL
#define ARCH_TEST_ADDR       0x1000000ULL
#define ARCH_FW_MEMORY_TYPE  EfiRuntimeServicesCode
#define ARCH_FW_MEMORY_ATTR  EFI_MEMORY_RUNTIME
#define ARCH_BOOT_FILE       u"\\EFI\\BOOT\\BOOTX64.EFI"

#endif
