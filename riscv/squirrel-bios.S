/* SquirrelBIOS — RISC-V firmware for QEMU virt
 * Prints banner on UART, then jumps in M-mode to the -kernel image QEMU
 * loaded, with the hart ID in a0 and the device tree in a1. Any trap
 * that reaches M-mode prints mcause and halts.
 * Original code.
 *
 * Build (from repo root):
 *   make riscv
 * Run:
 *   qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
 */

        .equ UART_BASE,     0x10000000      /* NS16550A */
        .equ UART_THR,      0
        .equ UART_IER,      1
        .equ UART_FCR,      2
        .equ UART_LCR,      3
        .equ UART_LSR,      5
        .equ UART_LSR_THRE, 0x20
        .equ UART_TIMEOUT,  0x100000        /* LSR polls per character */

        /* QEMU's reset code passes a2 = struct fw_dynamic_info:
         * magic, version, next_addr, next_mode, options, boot_hart (XLEN each) */
        .equ FWDYN_MAGIC,       0x4942534f  /* "OSBI" */
        .equ FWDYN_OFF_MAGIC,   0
        .equ FWDYN_OFF_NEXT,    16

.global _start
_start:
        csrw mie, zero
        csrci mstatus, 0x8                  /* MIE */
        bnez a0, park                       /* only hart 0 boots */

        mv s0, a0                           /* hart ID */
        mv s1, a1                           /* device tree */
        mv s2, a2                           /* fw_dynamic_info */
        la sp, stack_top

        la t0, trap
        csrw mtvec, t0                      /* direct mode */

        call uart_init
        la a0, banner
        call puts

        beqz s2, no_kernel
        ld t0, FWDYN_OFF_MAGIC(s2)
        li t1, FWDYN_MAGIC
        bne t0, t1, no_kernel
        ld s3, FWDYN_OFF_NEXT(s2)
        beqz s3, no_kernel

        la a0, msg_jump
        call puts
        mv a0, s3
        call puthex
        li a0, '\n'
        call putc

        mv a0, s0
        mv a1, s1
        li a2, 0
        fence.i
        jr s3

no_kernel:
        la a0, msg_no_kernel
        call puts
        la a0, msg_halted
        call puts
halt:
        wfi
        j halt

/* Secondary harts stay here with interrupts off. */
park:
        wfi
        j park

/* Every trap lands here. Nothing is resumed: print the cause and halt,
 * on the firmware stack, which the OS was free to reuse. */
        .align 2
trap:
        la sp, stack_top
        la a0, msg_trap
        call puts
        csrr a0, mcause
        call puthex
        li a0, '\n'
        call putc
        la a0, msg_halted
        call puts
        j halt

/* ------------------------------------------------------------------ */
/* NS16550A                                                            */
/* ------------------------------------------------------------------ */

uart_init:
        li t0, UART_BASE
        sb zero, UART_IER(t0)
        li t1, 0x03                         /* 8N1 */
        sb t1, UART_LCR(t0)
        li t1, 0x07                         /* enable and clear FIFOs */
        sb t1, UART_FCR(t0)
        ret

/* a0 = character.  Gives up after UART_TIMEOUT polls so a stuck UART
 * cannot hang the firmware. */
putc:
        li t0, UART_BASE
        li t2, UART_TIMEOUT
1:      lbu t1, UART_LSR(t0)
        andi t1, t1, UART_LSR_THRE
        bnez t1, 2f
        addi t2, t2, -1
        bnez t2, 1b
2:      sb a0, UART_THR(t0)
        ret

/* a0 = zero-terminated string */
puts:
        addi sp, sp, -16
        sd ra, 0(sp)
        sd s4, 8(sp)
        mv s4, a0
1:      lbu a0, 0(s4)
        beqz a0, 2f
        call putc
        addi s4, s4, 1
        j 1b
2:      ld ra, 0(sp)
        ld s4, 8(sp)
        addi sp, sp, 16
        ret

/* a0 = value, printed as 16 lowercase hex digits */
puthex:
        addi sp, sp, -32
        sd ra, 0(sp)
        sd s4, 8(sp)
        sd s5, 16(sp)
        mv s4, a0
        li s5, 60
1:      srl a0, s4, s5
        andi a0, a0, 0xf
        li t0, 10
        blt a0, t0, 2f
        addi a0, a0, 'a' - '0' - 10
2:      addi a0, a0, '0'
        call putc
        addi s5, s5, -4
        bgez s5, 1b
        ld ra, 0(sp)
        ld s4, 8(sp)
        ld s5, 16(sp)
        addi sp, sp, 32
        ret

banner:         .asciz "SquirrelBIOS RISC-V\n"
msg_jump:       .asciz "jumping to kernel at 0x"
msg_no_kernel:  .asciz "boot: no kernel image (pass -kernel to QEMU)\n"
msg_trap:       .asciz "trap mcause="
msg_halted:     .asciz "boot halted\n"

.align 12
    .skip 4096
stack_top:
