/* Test shim for the riscv-boot-hart case, loaded at 0x81000000. QEMU's own
 * fw_dynamic_info always names hart 0, so every hart starts here instead
 * (QEMU's generic loader sets their PC) and does what QEMU's reset code
 * does: a0 = hart ID, a1 = the device tree, a2 = fw_dynamic_info. The
 * info is QEMU's, copied to RAM with boot_hart set to BOOT_HART. Then it
 * jumps to the firmware at 0x80000000. Every hart writes the same copy.
 * Original code.
 */

        .option norelax

        .equ QEMU_RESET_FDT,  0x1020        /* the device tree address QEMU's reset code loads */
        .equ QEMU_FWDYN,      0x1028        /* QEMU's fw_dynamic_info */
        .equ FWDYN_DWORDS,    6
        .equ FWDYN_OFF_BOOT,  40
        .equ BOOT_HART,       2
        .equ FIRMWARE,        0x80000000

.global _start
_start:
        csrr a0, mhartid
        li t0, QEMU_RESET_FDT
        ld a1, 0(t0)
        li t0, QEMU_FWDYN
        la a2, info
        li t1, FWDYN_DWORDS
1:      ld t2, 0(t0)
        sd t2, 0(a2)
        addi t0, t0, 8
        addi a2, a2, 8
        addi t1, t1, -1
        bnez t1, 1b
        la a2, info
        li t0, BOOT_HART
        sd t0, FWDYN_OFF_BOOT(a2)
        fence rw, rw
        li t0, FIRMWARE
        jr t0

        .data
        .align 3
info:   .skip 8 * FWDYN_DWORDS
