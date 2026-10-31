/* SquirrelBIOS — RISC-V firmware for QEMU virt
 * Prints banner on UART, then enters the -kernel image QEMU loaded in the
 * mode QEMU asked for (S-mode for -kernel, through mret), with the hart ID
 * in a0 and the device tree in a1. An ecall from S-mode is an SBI call
 * (sbi_call below). Any other trap that reaches M-mode prints mcause and
 * halts.
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
        .equ FWDYN_OFF_VERSION, 8
        .equ FWDYN_OFF_NEXT,    16
        .equ FWDYN_OFF_MODE,    24          /* version 2 and later */
        .equ MODE_S,            1
        .equ MODE_M,            3

        .equ MSTATUS_MPP,       0x1800
        .equ MSTATUS_MPP_S,     0x0800
        .equ MSTATUS_MPIE,      0x80
        .equ PMP_RWX_NAPOT,     0x1f        /* R | W | X | A = NAPOT */

        /* Exceptions S-mode handles itself: misaligned, access faults,
         * illegal instruction, breakpoint, ecall from U-mode, page faults.
         * Not ecall from S-mode (9): that is the SBI, kept in M-mode. */
        .equ MEDELEG_S, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 12) | (1 << 13) | (1 << 15)
        /* Supervisor software, timer and external interrupts */
        .equ MIDELEG_S, (1 << 1) | (1 << 5) | (1 << 9)

        .equ CAUSE_ECALL_S,     9
        .equ TRAP_FRAME,        32          /* t0-t3, below trap_stack_top */
        .equ CAUSE_M_TIMER,     (1 << 63) | 7

        .equ MIP_SSIP,          (1 << 1)
        .equ MIP_STIP,          (1 << 5)
        .equ MIE_MTIE,          (1 << 7)
        .equ MCOUNTEREN_ALL,    0x7         /* CY, TM, IR readable below M */

        /* CLINT: one mtime, one mtimecmp per hart */
        .equ CLINT_MTIMECMP,    0x02004000

        /* SBI: a7 = extension, a6 = function, a0-a5 = arguments.
         * Returns a0 = error, a1 = value. */
        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_TIME,      0x54494d45
        .equ SBI_EXT_IPI,       0x735049
        .equ SBI_EXT_RFENCE,    0x52464e43
        .equ SBI_SPEC_VERSION,  0x01000000  /* 1.0 */
        .equ SBI_IMPL_ID,       0x5351      /* "SQ"; not BBL (0) or OpenSBI (1) */
        .equ SBI_IMPL_VERSION,  1
        .equ SBI_ERR_NOT_SUPPORTED, -2
        .equ SBI_ERR_INVALID_PARAM, -3

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
        la t0, trap_stack_top
        csrw mscratch, t0                   /* M-mode stack for traps */

        call uart_init
        la a0, banner
        call puts

        beqz s2, no_kernel
        ld t0, FWDYN_OFF_MAGIC(s2)
        li t1, FWDYN_MAGIC
        bne t0, t1, no_kernel
        ld s3, FWDYN_OFF_NEXT(s2)
        beqz s3, no_kernel

        /* next_mode: only version 2 and later carry it; older is M-mode */
        li s4, MODE_M
        ld t0, FWDYN_OFF_VERSION(s2)
        li t1, 2
        bltu t0, t1, 1f
        ld s4, FWDYN_OFF_MODE(s2)
1:      li t0, MODE_S
        beq s4, t0, 2f
        li t0, MODE_M
        bne s4, t0, bad_mode
2:
        la a0, msg_jump
        call puts
        mv a0, s3
        call puthex
        la a0, msg_mode
        call puts
        addi a0, s4, '0'
        call putc
        li a0, '\n'
        call putc

        mv a0, s0
        mv a1, s1
        li a2, 0
        fence.i
        li t0, MODE_M
        beq s4, t0, enter_m

        /* S-mode: one PMP entry granting R/W/X over all of physical memory
         * (without one, every S-mode access faults), supervisor faults and
         * interrupts delegated to S-mode, then mret to the image with
         * MPP = S and interrupts still off. */
        li t0, -1
        csrw pmpaddr0, t0
        li t0, PMP_RWX_NAPOT
        csrw pmpcfg0, t0
        sfence.vma
        li t0, MEDELEG_S
        csrw medeleg, t0
        li t0, MIDELEG_S
        csrw mideleg, t0
        li t0, MCOUNTEREN_ALL               /* rdtime from S-mode */
        csrw mcounteren, t0
        li t0, MSTATUS_MPP | MSTATUS_MPIE
        csrc mstatus, t0
        li t0, MSTATUS_MPP_S
        csrs mstatus, t0
        csrw mepc, s3
        mret

enter_m:
        jr s3

bad_mode:
        la a0, msg_bad_mode
        call puts
        mv a0, s4
        call puthex
        li a0, '\n'
        call putc
        la a0, msg_halted
        call puts
        j halt

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

/* Every trap lands here. mscratch holds the trap stack while the OS runs,
 * so the OS's sp is swapped out first and every register but a0 and a1
 * comes back unchanged. An ecall from S-mode is an SBI call, and the
 * M-mode timer interrupt is passed on to S-mode. Anything else is not
 * resumed: print the cause and halt. */
        .align 2
trap:
        csrrw sp, mscratch, sp
        addi sp, sp, -TRAP_FRAME
        sd t0, 0(sp)
        sd t1, 8(sp)
        sd t2, 16(sp)
        sd t3, 24(sp)
        csrr t0, mcause
        li t1, CAUSE_M_TIMER
        beq t0, t1, m_timer
        li t1, CAUSE_ECALL_S
        bne t0, t1, trap_fatal

/* SBI dispatcher.  t0-t3 are saved. A function sets a1 and goes to
 * sbi_ok, or sets a0 to an error and goes to sbi_ret. */
sbi_call:
        li t0, SBI_EXT_BASE
        beq a7, t0, sbi_base
        li t0, SBI_EXT_TIME
        beq a7, t0, sbi_time
        li t0, SBI_EXT_IPI
        beq a7, t0, sbi_ipi
        li t0, SBI_EXT_RFENCE
        beq a7, t0, sbi_rfence
sbi_not_supported:
        li a0, SBI_ERR_NOT_SUPPORTED
        j sbi_ret
sbi_invalid_param:
        li a0, SBI_ERR_INVALID_PARAM
        j sbi_ret
sbi_ok:
        li a0, 0
sbi_ret:
        csrr t0, mepc
        addi t0, t0, 4                      /* past the ecall */
        csrw mepc, t0
trap_ret:
        ld t0, 0(sp)
        ld t1, 8(sp)
        ld t2, 16(sp)
        ld t3, 24(sp)
        addi sp, sp, TRAP_FRAME
        csrrw sp, mscratch, sp
        mret

/* The S-mode timer is due: raise the supervisor timer interrupt, and push
 * mtimecmp out of reach so the M-mode one stops firing. STIP then stays
 * pending, once, until the next sbi_set_timer clears it. */
m_timer:
        li t0, MIP_STIP
        csrs mip, t0
        li t0, MIE_MTIE
        csrc mie, t0
        csrr t0, mhartid
        slli t0, t0, 3
        li t1, CLINT_MTIMECMP
        add t0, t0, t1
        li t1, -1
        sd t1, 0(t0)
        j trap_ret

/* TIME extension: set_timer (0), a0 = absolute time in mtime ticks */
sbi_time:
        bnez a6, sbi_not_supported
        csrr t0, mhartid
        slli t0, t0, 3
        li t1, CLINT_MTIMECMP
        add t0, t0, t1
        sd a0, 0(t0)
        li t0, MIP_STIP
        csrc mip, t0
        li t0, MIE_MTIE
        csrs mie, t0
        j sbi_ok

/* Base extension: a6 = function */
sbi_base:
        beqz a6, sbi_get_spec_version
        li t0, 1
        beq a6, t0, sbi_get_impl_id
        li t0, 2
        beq a6, t0, sbi_get_impl_version
        li t0, 3
        beq a6, t0, sbi_probe_extension
        li t0, 4
        beq a6, t0, sbi_get_mvendorid
        li t0, 5
        beq a6, t0, sbi_get_marchid
        li t0, 6
        beq a6, t0, sbi_get_mimpid
        j sbi_not_supported

sbi_get_spec_version:
        li a1, SBI_SPEC_VERSION
        j sbi_ok
sbi_get_impl_id:
        li a1, SBI_IMPL_ID
        j sbi_ok
sbi_get_impl_version:
        li a1, SBI_IMPL_VERSION
        j sbi_ok
sbi_get_mvendorid:
        csrr a1, mvendorid
        j sbi_ok
sbi_get_marchid:
        csrr a1, marchid
        j sbi_ok
sbi_get_mimpid:
        csrr a1, mimpid
        j sbi_ok

/* IPI extension: send_ipi (0), a0 = hart mask, a1 = hart mask base.
 * Only the calling hart is running, so that is the only one it can
 * reach: it gets a supervisor software interrupt. */
sbi_ipi:
        bnez a6, sbi_not_supported
        jal t2, hart_mask
        beqz t0, sbi_ok
        li t0, MIP_SSIP
        csrs mip, t0
        j sbi_ok

/* RFENCE extension, a0 = hart mask, a1 = hart mask base:
 *   remote_fence_i (0), remote_sfence_vma (1, a2 = start, a3 = size),
 *   remote_sfence_vma_asid (2, a4 = ASID).
 * Only the calling hart runs, so the fence is done here, on it. The
 * sfence.vma flushes everything, which covers any start, size and ASID.
 * The hypervisor fences (3-6) are not supported. */
sbi_rfence:
        li t0, 2
        bgtu a6, t0, sbi_not_supported
        jal t2, hart_mask
        beqz t0, sbi_ok
        bnez a6, 1f
        fence.i
        j sbi_ok
1:      sfence.vma
        j sbi_ok

/* Check an SBI hart mask: bit N of a0 selects hart a1 + N, and a1 = -1
 * selects every running hart. Only the calling hart runs, so a mask that
 * names any other hart is an invalid parameter (jumps to
 * sbi_invalid_param). Otherwise t0 = 1 if the calling hart is selected,
 * 0 if the mask is empty. Uses t1 and t3, returns through t2. */
hart_mask:
        li t0, 1
        li t1, -1
        beq a1, t1, 2f
        csrr t1, mhartid
        sub t1, t1, a1                      /* bit of the calling hart */
        li t0, 0
        li t3, 64
        bgeu t1, t3, 1f                     /* below the base or past bit 63 */
        li t0, 1
        sll t0, t0, t1
1:      not t3, t0
        and t3, t3, a0
        bnez t3, sbi_invalid_param          /* a hart that is not running */
        and t0, t0, a0
        snez t0, t0
2:      jr t2

/* a0 = extension ID.  a1 = 1 if implemented, 0 if not. */
sbi_probe_extension:
        li a1, 1
        li t0, SBI_EXT_BASE
        beq a0, t0, sbi_ok
        li t0, SBI_EXT_TIME
        beq a0, t0, sbi_ok
        li t0, SBI_EXT_IPI
        beq a0, t0, sbi_ok
        li t0, SBI_EXT_RFENCE
        beq a0, t0, sbi_ok
        li a1, 0
        j sbi_ok

trap_fatal:
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
msg_mode:       .asciz ", next_mode="
msg_bad_mode:   .asciz "boot: unsupported next_mode "
msg_no_kernel:  .asciz "boot: no kernel image (pass -kernel to QEMU)\n"
msg_trap:       .asciz "trap mcause="
msg_halted:     .asciz "boot halted\n"

.align 12
    .skip 4096
stack_top:
    /* Trap stack: mscratch points at its top, and the trap handler's frame
     * (TRAP_FRAME bytes) grows down from there. Keep it inside the 256. */
    .skip 256
trap_stack_top:
