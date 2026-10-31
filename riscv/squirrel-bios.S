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
        .equ PMP_NONE_NAPOT,    0x18        /* A = NAPOT, no access */
/* The firmware region: everything below QEMU's rv64 kernel load address
 * (0x80200000). PMP entry 0 denies it to S-mode; NAPOT needs a
 * power-of-two size aligned to that size. */
        .equ FW_BASE,           0x80000000
        .equ FW_SIZE,           0x200000    /* 2 MB */
        .equ FW_PMPADDR,        (FW_BASE >> 2) | ((FW_SIZE / 2 - 1) >> 2)

/* Flattened device tree (big-endian) */
        .equ FDT_MAGIC,         0xd00dfeed
        .equ FDT_TOTALSIZE,     4           /* header field offsets */
        .equ FDT_OFF_STRUCT,    8
        .equ FDT_OFF_STRINGS,   12
        .equ FDT_LAST_COMP,     24
        .equ FDT_SIZE_STRINGS,  32
        .equ FDT_SIZE_STRUCT,   36
        .equ FDT_BEGIN_NODE,    1
        .equ FDT_END_NODE,      2
        .equ FDT_PROP,          3
        .equ FDT_NOP,           4
        .equ FDT_END,           9
/* QEMU puts the tree at a 2 MB boundary, ending at or below the end of
 * RAM (or 3 GB), with free RAM after it. RAM sizes are whole MB, so RAM
 * goes on at least to the next 1 MB boundary after the tree. */
        .equ FDT_ROOM,          0x100000
/* dt_node and dt_strings (end of file): sizes and the offsets of the name
 * fields in dt_node, checked against the data there */
        .equ DT_NODE_LEN,       140
        .equ DT_STRINGS_LEN,    45
        .equ DT_GROW,           DT_NODE_LEN + DT_STRINGS_LEN
        .equ DT_NAME_ADDR_CELLS, 28
        .equ DT_NAME_SIZE_CELLS, 44
        .equ DT_NAME_RANGES,    60
        .equ DT_NAME_REG,       100
        .equ DT_NAME_NO_MAP,    128

        /* Exceptions S-mode handles itself: misaligned, access faults,
         * illegal instruction, breakpoint, ecall from U-mode, page faults.
         * Not ecall from S-mode (9): that is the SBI, kept in M-mode. */
        .equ MEDELEG_S, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 12) | (1 << 13) | (1 << 15)
        /* Supervisor software, timer and external interrupts */
        .equ MIDELEG_S, (1 << 1) | (1 << 5) | (1 << 9)

        .equ CAUSE_ECALL_S,     9
        .equ MIE_MSIE,          (1 << 3)
        .equ TRAP_FRAME,        32          /* t0-t3, below trap_stack_top */
        .equ CAUSE_M_TIMER,     (1 << 63) | 7

        .equ MIP_SSIP,          (1 << 1)
        .equ MIP_STIP,          (1 << 5)
        .equ MSTATUS_SIE,       (1 << 1)
        .equ MIE_MTIE,          (1 << 7)
        .equ MCOUNTEREN_ALL,    0x7         /* CY, TM, IR readable below M */
/* menvcfg (priv 1.12): what S-mode may use, for the extensions the hart has.
 * Bits for an extension the hart lacks stay 0 (WARL). */
        .equ CSR_MENVCFG,       0x30a
        .equ CSR_STIMECMP,      0x14d
        .equ MENVCFG_CBIE_FLUSH, (1 << 4)   /* Zicbom: cbo.inval acts as a flush */
        .equ MENVCFG_CBCFE,     (1 << 6)    /* Zicbom: cbo.clean, cbo.flush */
        .equ MENVCFG_CBZE,      (1 << 7)    /* Zicboz: cbo.zero */
        .equ MENVCFG_STCE,      (1 << 63)   /* Sstc: stimecmp */
        .equ MENVCFG_S,         MENVCFG_CBIE_FLUSH | MENVCFG_CBCFE | MENVCFG_CBZE | MENVCFG_STCE

        /* CLINT: one msip and one mtimecmp per hart, one mtime */
        .equ CLINT_MSIP,        0x02000000
        .equ CLINT_MTIMECMP,    0x02004000
        .equ CLINT_MTIME,       0x0200bff8

        /* Harts the firmware can start. Higher hart IDs stay parked. */
        .equ MAX_HARTS,         8
        .equ TRAP_STACK,        256         /* per hart */

        /* hart_table entry: state, start address, opaque (8 bytes each) */
        .equ HART_ENTRY_SHIFT,  5           /* 32 bytes */
        .equ HART_STATE,        0
        .equ HART_ADDR,         8
        .equ HART_OPAQUE,       16
        .equ HSM_STARTED,       0           /* SBI hart states */
        .equ HSM_STOPPED,       1
        .equ HSM_START_PENDING, 2
        .equ HSM_ABSENT,        -1          /* no such hart has reached park */
        .equ HSM_CHECKIN_WAIT,  1000000     /* mtime ticks: 100 ms at 10 MHz */

/* QEMU virt "sifive_test" finisher: a 32-bit write powers off or resets */
        .equ TEST_FINISHER,     0x100000
        .equ FINISHER_FAIL,     0x3333      /* exit code in bits 31:16 */
        .equ FINISHER_PASS,     0x5555      /* power off */
        .equ FINISHER_RESET,    0x7777
        .equ SRST_WAIT,         10000000    /* mtime ticks: 1 s at 10 MHz */

        /* SBI: a7 = extension, a6 = function, a0-a5 = arguments.
         * Returns a0 = error, a1 = value. */
        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_TIME,      0x54494d45
        .equ SBI_EXT_IPI,       0x735049
        .equ SBI_EXT_RFENCE,    0x52464e43
        .equ SBI_EXT_HSM,       0x48534d
        .equ SBI_EXT_SRST,      0x53525354
        .equ SBI_SPEC_VERSION,  0x01000000  /* 1.0 */
        .equ SBI_IMPL_ID,       0x5351      /* "SQ"; not BBL (0) or OpenSBI (1) */
        .equ SBI_IMPL_VERSION,  1
        .equ SBI_ERR_FAILED,    -1
        .equ SBI_ERR_NOT_SUPPORTED, -2
        .equ SBI_ERR_INVALID_PARAM, -3
        .equ SBI_ERR_ALREADY_AVAILABLE, -6

.global _start
_start:
        csrw mie, zero
        csrci mstatus, 0x8                  /* MIE */
        la t0, trap
        csrw mtvec, t0                      /* direct mode */
        li t0, MAX_HARTS
        bgeu a0, t0, park_forever
        addi t0, a0, 1                      /* M-mode trap stack of this hart */
        li t1, TRAP_STACK
        mul t0, t0, t1
        la t1, trap_stacks
        add t0, t0, t1
        csrw mscratch, t0
        bnez a0, park                       /* only hart 0 boots */

        mv s0, a0                           /* hart ID */
        mv s1, a1                           /* device tree */
        mv s2, a2                           /* fw_dynamic_info */
        la sp, stack_top

        la t0, hart_table                   /* hart 0 is running */
        sd zero, HART_STATE(t0)

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

        li t0, MODE_S                       /* tell an S-mode OS to keep out */
        bne s4, t0, 3f
        mv a0, s1
        call dt_reserve
3:
        mv a0, s0
        mv a1, s1
        li a2, 0
        fence.i
        li t0, MODE_M
        beq s4, t0, enter_m

/* Enter S-mode at s3 with a0 and a1 as they are: PMP entry 0 denying the
 * firmware region to S-mode (no read, write or execute; unlocked, so it
 * does not apply to M-mode), entry 1 granting R/W/X over the rest of
 * physical memory (without it, every S-mode access faults), translation
 * off (satp = 0, which a restart after hart_stop relies on), menvcfg
 * letting S-mode use cbo.* and stimecmp where the hart has them (sstc_on
 * records which timer set_timer uses), supervisor faults and interrupts
 * delegated to S-mode, then
 * mret with MPP = S, interrupts still off and sp = 0, so an image that
 * uses sp before setting it faults instead of writing firmware memory.
 * Used by every hart. */
enter_s:
        csrw mie, zero
        csrw satp, zero
        la t0, skip_insn                    /* a hart without menvcfg skips this */
        csrrw t2, mtvec, t0
        li t0, MENVCFG_S
        li t1, 0
        csrs CSR_MENVCFG, t0
        csrr t1, CSR_MENVCFG
        csrw mtvec, t2
        srli t1, t1, 63                     /* Sstc on: set_timer uses stimecmp */
        la t0, sstc_on
        sd t1, 0(t0)
        li t0, FW_PMPADDR
        csrw pmpaddr0, t0
        li t0, -1
        csrw pmpaddr1, t0
        li t0, PMP_NONE_NAPOT | (PMP_RWX_NAPOT << 8)
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
        li sp, 0
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

/* Secondary harts wait here for sbi_hart_start, and so does a hart that
 * called sbi_hart_stop (a0 = hart ID either way). Interrupts stay off at
 * the CPU (mstatus.MIE = 0), but the machine software interrupt is enabled
 * in mie so that the hart_start CLINT msip write ends the wfi. */
park:
        slli t0, a0, HART_ENTRY_SHIFT
        la s5, hart_table
        add s5, s5, t0                      /* s5 = this hart's entry */
        li t0, CLINT_MSIP
        slli t1, a0, 2
        add s6, t0, t1                      /* s6 = this hart's msip */
        mv s0, a0
        li t0, HSM_STOPPED
        sd t0, HART_STATE(s5)
        li t0, MIE_MSIE
        csrw mie, t0
1:      wfi
        sw zero, 0(s6)                      /* consume the wake-up */
        fence
        ld t0, HART_STATE(s5)
        li t1, HSM_START_PENDING
        bne t0, t1, 1b
        fence r, r
        ld s3, HART_ADDR(s5)
        ld a1, HART_OPAQUE(s5)
        sd zero, HART_STATE(s5)             /* HSM_STARTED */
        mv a0, s0
        fence.i
        j enter_s

/* Hart IDs above MAX_HARTS never run. */
park_forever:
        wfi
        j park_forever

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
        li t0, SBI_EXT_HSM
        beq a7, t0, sbi_hsm
        li t0, SBI_EXT_SRST
        beq a7, t0, sbi_srst
sbi_not_supported:
        li a0, SBI_ERR_NOT_SUPPORTED
        j sbi_ret
sbi_invalid_param:
        li a0, SBI_ERR_INVALID_PARAM
        j sbi_ret
sbi_already_available:
        li a0, SBI_ERR_ALREADY_AVAILABLE
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
        la t0, sstc_on
        ld t0, 0(t0)
        beqz t0, 1f
        csrw CSR_STIMECMP, a0               /* STIP follows stimecmp */
        j sbi_ok
1:
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

/* HSM extension: hart_start (0, a0 = hart, a1 = start address,
 * a2 = opaque), hart_stop (1, the calling hart), hart_get_status (2,
 * a0 = hart). */
sbi_hsm:
        li t0, 1
        beq a6, t0, sbi_hart_stop
        li t0, MAX_HARTS
        bgeu a0, t0, sbi_invalid_param
        slli t0, a0, HART_ENTRY_SHIFT
        la t1, hart_table
        add t0, t0, t1                      /* t0 = the hart's entry */
        ld t1, HART_STATE(t0)
        li t2, HSM_ABSENT
        bne t1, t2, 2f

        /* Secondary harts reach park on their own schedule, which on QEMU
         * can be after the OS is already running. Give a hart that has not
         * checked in HSM_CHECKIN_WAIT to do so; after that it does not
         * exist. */
        li t2, CLINT_MTIME
        ld t3, 0(t2)
        li t2, HSM_CHECKIN_WAIT
        add t3, t3, t2                      /* t3 = deadline */
1:      ld t1, HART_STATE(t0)
        li t2, HSM_ABSENT
        bne t1, t2, 2f
        li t2, CLINT_MTIME
        ld t2, 0(t2)
        bltu t2, t3, 1b
        j sbi_invalid_param
2:      fence r, r
        li t2, 2
        beq a6, t2, sbi_hart_get_status
        bnez a6, sbi_not_supported

/* hart_start: the target must be stopped. Publish the address and the
 * opaque value, then the state, then wake it through its CLINT msip. */
        li t2, HSM_STOPPED
        bne t1, t2, sbi_already_available
        sd a1, HART_ADDR(t0)
        sd a2, HART_OPAQUE(t0)
        fence w, w
        li t1, HSM_START_PENDING
        sd t1, HART_STATE(t0)
        fence
        li t1, CLINT_MSIP
        slli t2, a0, 2
        add t1, t1, t2
        li t2, 1
        sw t2, 0(t1)
        j sbi_ok

sbi_hart_get_status:
        mv a1, t1
        j sbi_ok

/* hart_stop: the calling hart goes back to park and does not return to
 * S-mode. Put the trap stack back in mscratch, drop the S-mode interrupts
 * it may have pending, and park. Its registers are not kept. */
sbi_hart_stop:
        addi sp, sp, TRAP_FRAME
        csrw mscratch, sp
        csrw mie, zero
        li t0, MIP_SSIP | MIP_STIP
        csrc mip, t0
        li t0, MSTATUS_SIE
        csrc mstatus, t0
        csrr a0, mhartid
        j park

/* Check an SBI hart mask: bit N of a0 selects hart a1 + N, and a1 = -1
 * selects the calling hart. IPIs and fences reach only the calling hart,
 * even when hart_start has started others, so a mask that names any other
 * hart is an invalid parameter (jumps to sbi_invalid_param). Otherwise t0 = 1 if the calling hart is selected,
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

/* SRST extension: system_reset (0, a0 = type, a1 = reason).  Types:
 * shutdown (0), cold reboot (1), warm reboot (2), all through the QEMU
 * test finisher.  Reasons: none (0), system failure (1); a shutdown for
 * a system failure makes QEMU exit with status 1.  On success the call
 * does not return; if the machine is still running SRST_WAIT later, it
 * returns SBI_ERR_FAILED. */
sbi_srst:
        bnez a6, sbi_not_supported
        li t0, 1
        bgtu a1, t0, sbi_invalid_param
        li t0, FINISHER_RESET
        bnez a0, 1f
        li t0, FINISHER_PASS
        beqz a1, 2f
        li t0, (1 << 16) | FINISHER_FAIL
        j 2f
1:      li t1, 2
        bgtu a0, t1, sbi_invalid_param
2:      li t1, TEST_FINISHER
        sw t0, 0(t1)
        li t1, CLINT_MTIME
        ld t2, 0(t1)
        li t0, SRST_WAIT
        add t2, t2, t0
3:      ld t0, 0(t1)                        /* QEMU acts on the write a little later */
        bltu t0, t2, 3b
        li a0, SBI_ERR_FAILED
        j sbi_ret

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
        li t0, SBI_EXT_HSM
        beq a0, t0, sbi_ok
        li t0, SBI_EXT_SRST
        beq a0, t0, sbi_ok
        li a1, 0
        j sbi_ok

/* Trap handler while enter_s probes menvcfg: skip the instruction */
        .align 2
skip_insn:
        csrr t0, mepc
        addi t0, t0, 4
        csrw mepc, t0
        mret

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
/* Device tree                                                         */
/* ------------------------------------------------------------------ */

/* rd = big-endian 32-bit word at off(base), any alignment */
.macro ldbe rd, off, base, tmp
        lbu \rd, \off(\base)
        slli \rd, \rd, 8
        lbu \tmp, \off+1(\base)
        or \rd, \rd, \tmp
        slli \rd, \rd, 8
        lbu \tmp, \off+2(\base)
        or \rd, \rd, \tmp
        slli \rd, \rd, 8
        lbu \tmp, \off+3(\base)
        or \rd, \rd, \tmp
.endm

/* off(base) = rs as a big-endian 32-bit word */
.macro stbe rs, off, base, tmp
        srli \tmp, \rs, 24
        sb \tmp, \off(\base)
        srli \tmp, \rs, 16
        sb \tmp, \off+1(\base)
        srli \tmp, \rs, 8
        sb \tmp, \off+2(\base)
        sb \rs, \off+3(\base)
.endm

/* a0 = device tree.  Adds /reserved-memory with a no-map child over the
 * firmware region, so an OS that reads the tree does not use memory it
 * cannot reach (PMP entry 0).  The node goes at the end of the root node,
 * and its property names at the end of the strings block, which grows the
 * tree in place by DT_GROW bytes.  Prints what it did.  Leaves the tree as
 * it is, and says so, when it is not laid out the way QEMU writes it
 * (strings block last, right after the structure block), already has a
 * /reserved-memory node, or has no room to grow. */
dt_reserve:
        addi sp, sp, -16
        sd ra, 0(sp)
        la t6, msg_dt_odd
        beqz a0, 9f
        andi t0, a0, 3
        bnez t0, 9f
        ldbe t0, 0, a0, t1
        li t1, FDT_MAGIC
        bne t0, t1, 9f
        ldbe t0, FDT_LAST_COMP, a0, t1
        li t1, 17
        bgtu t0, t1, 9f
        ldbe a1, FDT_TOTALSIZE, a0, t1
        ldbe a2, FDT_OFF_STRUCT, a0, t1
        ldbe a3, FDT_OFF_STRINGS, a0, t1
        ldbe a4, FDT_SIZE_STRINGS, a0, t1
        ldbe a5, FDT_SIZE_STRUCT, a0, t1
        add t0, a2, a5
        bne t0, a3, 9f                      /* strings right after struct */
        add t0, a3, a4
        bne t0, a1, 9f                      /* and last */
        add a6, a0, a3                      /* a6 = end of the structure block */
        ldbe t0, -4, a6, t1
        li t1, FDT_END
        bne t0, t1, 9f
        ldbe t0, -8, a6, t1
        li t1, FDT_END_NODE                 /* the root node's end */
        bne t0, t1, 9f

        /* Walk the tree: no /reserved-memory yet */
        add t0, a0, a2                      /* t0 = token */
        li t5, 0                            /* depth: 1 in the root node */
1:      bgeu t0, a6, 9f
        ldbe t1, 0, t0, t2
        addi t0, t0, 4
        li t2, FDT_BEGIN_NODE
        beq t1, t2, 2f
        li t2, FDT_END_NODE
        beq t1, t2, 5f
        li t2, FDT_NOP
        beq t1, t2, 1b
        li t2, FDT_END
        beq t1, t2, 6f
        li t2, FDT_PROP
        bne t1, t2, 9f
        ldbe t1, 0, t0, t2                  /* value length */
        addi t1, t1, 8 + 3                  /* length and name offset words */
        andi t1, t1, -4
        add t0, t0, t1
        j 1b
2:      addi t5, t5, 1                      /* node: compare a child of the root */
        li t2, 2
        bne t5, t2, 4f
        la t1, str_reserved_memory
        mv t2, t0
3:      lbu t3, 0(t1)
        beqz t3, 7f
        lbu t4, 0(t2)
        bne t3, t4, 4f
        addi t1, t1, 1
        addi t2, t2, 1
        j 3b
7:      lbu t4, 0(t2)                       /* "reserved-memory" or "reserved-memory@..." */
        la t6, msg_dt_exists
        beqz t4, 9f
        li t3, '@'
        beq t4, t3, 9f
        la t6, msg_dt_odd
4:      lbu t1, 0(t0)                       /* skip the name */
        addi t0, t0, 1
        bnez t1, 4b
        addi t0, t0, 3
        andi t0, t0, -4
        j 1b
5:      addi t5, t5, -1
        j 1b
6:
        /* Room: up to the next FDT_ROOM boundary after the tree */
        la t6, msg_dt_room
        add t0, a0, a1
        li t1, FDT_ROOM - 1
        add t2, t0, t1
        not t1, t1
        and t2, t2, t1
        li t1, DT_GROW
        add t0, t0, t1
        bgtu t0, t2, 9f

        /* Move the root's end, END and the strings up by DT_NODE_LEN */
        addi a7, a6, -8                     /* a7 = where the node goes */
        add t0, a0, a1                      /* copy down from the old end */
1:      addi t0, t0, -1
        lbu t1, 0(t0)
        sb t1, DT_NODE_LEN(t0)
        bne t0, a7, 1b
        la t0, dt_node                      /* the node */
        mv t2, a7
        li t3, DT_NODE_LEN
1:      lbu t1, 0(t0)
        sb t1, 0(t2)
        addi t0, t0, 1
        addi t2, t2, 1
        addi t3, t3, -1
        bnez t3, 1b
        .irp off, DT_NAME_ADDR_CELLS, DT_NAME_SIZE_CELLS, DT_NAME_RANGES, DT_NAME_REG, DT_NAME_NO_MAP
        ldbe t1, \off, a7, t2              /* name offsets: after the old strings */
        add t1, t1, a4
        stbe t1, \off, a7, t2
        .endr
        la t0, dt_strings                   /* the names, at the new end */
        add t2, a0, a1
        addi t2, t2, DT_NODE_LEN
        li t3, DT_STRINGS_LEN
1:      lbu t1, 0(t0)
        sb t1, 0(t2)
        addi t0, t0, 1
        addi t2, t2, 1
        addi t3, t3, -1
        bnez t3, 1b

        addi t1, a3, DT_NODE_LEN
        stbe t1, FDT_OFF_STRINGS, a0, t2
        addi t1, a5, DT_NODE_LEN
        stbe t1, FDT_SIZE_STRUCT, a0, t2
        addi t1, a4, DT_STRINGS_LEN
        stbe t1, FDT_SIZE_STRINGS, a0, t2
        addi t1, a1, DT_GROW
        stbe t1, FDT_TOTALSIZE, a0, t2
        la t6, msg_dt_ok
9:      mv a0, t6
        call puts
        ld ra, 0(sp)
        addi sp, sp, 16
        ret

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
msg_dt_ok:      .asciz "dt: firmware region 0x80000000-0x801fffff reserved (no-map)\n"
msg_dt_odd:     .asciz "dt: firmware region not reserved: unexpected device tree layout\n"
msg_dt_exists:  .asciz "dt: firmware region not reserved: /reserved-memory already present\n"
msg_dt_room:    .asciz "dt: firmware region not reserved: no room to grow the device tree\n"
str_reserved_memory: .asciz "reserved-memory"

.align 12
    .skip 4096
stack_top:
    /* Trap stacks, TRAP_STACK bytes per hart: each hart's mscratch points
     * at the top of its own, and the trap handler's frame (TRAP_FRAME
     * bytes) grows down from there. Keep it inside TRAP_STACK. */
trap_stacks:
    .skip TRAP_STACK * MAX_HARTS

    .align 3
sstc_on:                                    /* 1 once enter_s found Sstc */
    .dword 0
hart_table:
    .rept MAX_HARTS
    .dword HSM_ABSENT, 0, 0, 0
    .endr

/* The node dt_reserve adds to the device tree, big-endian. Name offsets
 * are into dt_strings; dt_reserve adds the old strings size to each. */
.macro be32 v
        .byte ((\v) >> 24) & 0xff, ((\v) >> 16) & 0xff, ((\v) >> 8) & 0xff, (\v) & 0xff
.endm

        .section .rodata
        .align 2
dt_node:
        be32 FDT_BEGIN_NODE
        .asciz "reserved-memory"                /* 16 bytes */
        be32 FDT_PROP
        be32 4
n_addr: be32 s_addr - dt_strings
        be32 2
        be32 FDT_PROP
        be32 4
n_size: be32 s_size - dt_strings
        be32 2
        be32 FDT_PROP
        be32 0
n_rng:  be32 s_rng - dt_strings
        be32 FDT_BEGIN_NODE
        .asciz "squirrelbios@80000000"          /* 22 bytes */
        .byte 0, 0                              /* pad to 24 */
        be32 FDT_PROP
        be32 16
n_reg:  be32 s_reg - dt_strings
        be32 0
        be32 FW_BASE
        be32 0
        be32 FW_SIZE
        be32 FDT_PROP
        be32 0
n_nomap: be32 s_nomap - dt_strings
        be32 FDT_END_NODE
        be32 FDT_END_NODE
dt_node_end:
dt_strings:
s_addr: .asciz "#address-cells"
s_size: .asciz "#size-cells"
s_rng:  .asciz "ranges"
s_reg:  .asciz "reg"
s_nomap: .asciz "no-map"
dt_strings_end:

        .if (dt_node_end - dt_node != DT_NODE_LEN) || (dt_strings_end - dt_strings != DT_STRINGS_LEN)
        .error "dt_node or dt_strings changed size: update DT_NODE_LEN and DT_STRINGS_LEN"
        .endif
        .if (n_addr - dt_node != DT_NAME_ADDR_CELLS) || (n_size - dt_node != DT_NAME_SIZE_CELLS) || (n_rng - dt_node != DT_NAME_RANGES) || (n_reg - dt_node != DT_NAME_REG) || (n_nomap - dt_node != DT_NAME_NO_MAP)
        .error "a name offset in dt_node moved: update the DT_NAME_ constants"
        .endif
