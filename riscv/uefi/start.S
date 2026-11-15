/* SquirrelBIOS riscv64-uefi: S-mode entry of the UEFI core, its trap
 * vector, and the EFI image call thunk. Original code.
 *
 * The SBI (riscv/squirrel-bios.S built with UEFI=1) enters here in
 * S-mode with a0 = hart ID, a1 = device tree, a2 = the -kernel image
 * QEMU loaded or 0, translation off and interrupts off. */

        .option norelax
        .section .text.entry, "ax"
        .globl _start
_start:
        csrw sie, zero
        la t0, __bss_start
        la t1, __bss_end
1:      bgeu t0, t1, 2f
        sd zero, 0(t0)
        addi t0, t0, 8
        j 1b
2:      la sp, __stack_top
        la t0, s_trap
        csrw stvec, t0
        li t0, 0x2000                       /* sstatus.FS = initial: FP for images */
        csrs sstatus, t0
        call riscv_main                     /* (hart, dtb, kernel), does not return */
3:      wfi
        j 3b

/* image_call(entry, handle, system table, &saved_sp) */
        .text
        .globl image_call, image_exit
image_call:
        addi sp, sp, -128
        sd ra, 0(sp)
        sd s0, 8(sp)
        sd s1, 16(sp)
        sd s2, 24(sp)
        sd s3, 32(sp)
        sd s4, 40(sp)
        sd s5, 48(sp)
        sd s6, 56(sp)
        sd s7, 64(sp)
        sd s8, 72(sp)
        sd s9, 80(sp)
        sd s10, 88(sp)
        sd s11, 96(sp)
        sd gp, 104(sp)
        sd tp, 112(sp)
        sd sp, 0(a3)
        mv t0, a0
        mv a0, a1
        mv a1, a2
        jalr t0
image_call_out:
        ld ra, 0(sp)
        ld s0, 8(sp)
        ld s1, 16(sp)
        ld s2, 24(sp)
        ld s3, 32(sp)
        ld s4, 40(sp)
        ld s5, 48(sp)
        ld s6, 56(sp)
        ld s7, 64(sp)
        ld s8, 72(sp)
        ld s9, 80(sp)
        ld s10, 88(sp)
        ld s11, 96(sp)
        ld gp, 104(sp)
        ld tp, 112(sp)
        addi sp, sp, 128
        ret
/* image_exit(saved_sp, status) */
image_exit:
        mv sp, a0
        mv a0, a1
        j image_call_out

/* A trap the firmware did not expect: report it and halt */
        .align 2
s_trap:
        csrr a0, scause
        csrr a1, sepc
        csrr a2, stval
        la sp, __stack_top
        call arch_exception
4:      wfi
        j 4b

        .section .bss.stack, "aw", @nobits
        .balign 16
        .space  0x10000
__stack_top:
