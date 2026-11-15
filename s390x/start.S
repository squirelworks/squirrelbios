/* SquirrelBIOS s390x: entry, program-check reports, waiting for the
 * service signal and the jump to the OS. Original code, written from the
 * z/Architecture Principles of Operation.
 *
 * QEMU loads the -bios ELF (linked at 0, position independent) near
 * the top of the first 2 GB and starts it at _start in 64-bit addressing
 * mode, DAT off, interrupts off, with the lowcore at absolute 0. The
 * firmware applies its own R_390_RELATIVE relocations first (they set
 * each word to load address + addend, so doing them again is harmless).
 */
#define LC_EXT_OLD_PSW   0x130
#define LC_PGM_OLD_PSW   0x150
#define LC_EXT_NEW_PSW   0x1b0
#define LC_PGM_NEW_PSW   0x1d0
#define LC_PGM_CODE      0x8e

        .section .text.head, "ax"
        .globl _start
_start:
        larl    %r10, _start
        larl    %r2, __rela_start
        larl    %r3, __rela_end
0:      clgr    %r2, %r3
        jhe     1f
        llgf    %r5, 12(%r2)                    /* r_info: type in the low word */
        cghi    %r5, 12                         /* R_390_RELATIVE */
        jne     5f
        lg      %r6, 0(%r2)
        lg      %r7, 16(%r2)
        agr     %r7, %r10
        stg     %r7, 0(%r6,%r10)
5:      aghi    %r2, 24
        j       0b
1:      larl    %r15, stack_top
        /* .bss (the stack is in it) */
        larl    %r2, __bss_start
        larl    %r3, __bss_end
6:      clgr    %r2, %r3
        jhe     2f
        mvi     0(%r2), 0
        aghi    %r2, 1
        j       6b
2:      aghi    %r15, -160
        xc      0(8,%r15), 0(%r15)
        /* Program checks come to pgm_entry, 64-bit, interrupts off */
        larl    %r1, psw_64
        mvc     LC_PGM_NEW_PSW(8), 0(%r1)
        larl    %r2, pgm_entry
        stg     %r2, LC_PGM_NEW_PSW + 8
        brasl   %r14, fw_main
3:      larl    %r1, psw_stop
        lpswe   0(%r1)

pgm_entry:
        larl    %r15, trap_stack_top
        aghi    %r15, -160
        llgh    %r2, LC_PGM_CODE
        lg      %r3, LC_PGM_OLD_PSW + 8
        lg      %r4, LC_PGM_OLD_PSW
        brasl   %r14, fw_trap
        j       3b

/* void sclp_wait(void): enable the service-signal external interrupt
 * and wait for it (an SCLP request has finished) */
        .globl sclp_wait
sclp_wait:
        stctg   %c0, %c0, 0(%r15)
        oi      6(%r15), 0x02                   /* CR0 bit 54: service signal */
        lctlg   %c0, %c0, 0(%r15)
        larl    %r1, psw_64
        mvc     LC_EXT_NEW_PSW(8), 0(%r1)
        larl    %r1, 4f
        stg     %r1, LC_EXT_NEW_PSW + 8
        larl    %r1, psw_wait_ext
        lpswe   0(%r1)
4:      stctg   %c0, %c0, 0(%r15)
        ni      6(%r15), 0xfd
        lctlg   %c0, %c0, 0(%r15)
        br      %r14

/* void fw_halt(uint64_t addr): disabled wait at 'addr' (QEMU treats a
 * disabled wait at 0xfff as a shutdown) */
        .globl fw_halt
fw_halt:
        larl    %r1, halt_psw
        stg     %r2, 8(%r1)
        lpswe   0(%r1)

/* void fw_enter(uint64_t entry, uint64_t r2, uint64_t r3): 64-bit, DAT
 * off, interrupts off */
        .globl fw_enter
fw_enter:
        larl    %r1, enter_psw
        stg     %r2, 8(%r1)
        lgr     %r2, %r3
        lgr     %r3, %r4
        lghi    %r15, 0
        lpswe   0(%r1)

        .section .rodata
        .balign 8
psw_64:
        .quad   0x0000000180000000, 0
psw_wait_ext:
        .quad   0x0102000180000000, 0           /* EXT enabled, wait */
psw_stop:
        .quad   0x0002000180000000, 0           /* disabled wait */

        .section .data
        .balign 8
enter_psw:
        .quad   0x0000000180000000, 0
halt_psw:
        .quad   0x0002000180000000, 0

        .section .bss
        .balign 4096
        .space  16384
stack_top:
        .space  8192
trap_stack_top:
