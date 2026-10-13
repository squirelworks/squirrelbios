/* SquirrelBIOS — AArch64 firmware for QEMU virt
 * Prints banner on PL011 then halts.
 * Original code.
 *
 * Build (from repo root):
 *   make arm
 * Run:
 *   qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin
 */

.global _start
_start:
    ldr x1, =stack_top
    mov sp, x1

    ldr x2, =0x09000000
    adr x3, banner
1:
    ldrb w4, [x3], #1
    cbz w4, 2f
    str w4, [x2]
    b 1b
2:
    wfi
    b 2b

.align 4
banner:
    .asciz "SquirrelBIOS ARM\n"

.align 12
    .skip 4096
stack_top:
