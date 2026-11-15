/* SquirrelBIOS m68k-virt: entry, exception vectors and the jump to the OS.
 * Original code.
 *
 * QEMU loads this ELF (-kernel) and starts it at _start: 68040,
 * supervisor mode, MMU off. Its bootinfo records sit right after _end,
 * which is where .bss ends: they are copied out before anything else
 * runs past _end (the stack is inside .bss).
 */
        .section .text.head, "ax"
        .globl _start
_start:
        move.w  #0x2700, %sr                    /* supervisor, interrupts masked */
        lea     stack_top, %sp
        /* Exceptions come to our table */
        lea     vectors, %a0
        movec   %a0, %vbr
        /* Save the bootinfo (after _end) before .bss is touched: it may
         * not be far past _end, but .bss is below it, so clearing .bss
         * is safe; the copy goes into .bss afterwards, from C */
        lea     __bss_start, %a0
        lea     __bss_end, %a1
1:      cmp.l   %a1, %a0
        bcc.s   2f
        clr.l   (%a0)+
        bra.s   1b
2:      jsr     fw_main
3:      stop    #0x2700
        bra.s   3b

/* void fw_enter(uint32_t entry, uint32_t sp): MMU off, supervisor,
 * interrupts masked, VBR back to 0 */
        .globl fw_enter
fw_enter:
        move.l  4(%sp), %a0
        move.l  8(%sp), %d0
        move.w  #0x2700, %sr
        moveq   #0, %d1
        movec   %d1, %vbr
        move.l  %d0, %sp
        moveq   #0, %d0
        jmp     (%a0)

/* Any exception: the frame's format/vector word gives the vector */
exc_common:
        move.w  #0x2700, %sr
        movem.l %d0-%d7/%a0-%a6, -(%sp)
        move.l  %sp, -(%sp)                     /* struct frame * */
        jsr     fw_exception
4:      stop    #0x2700
        bra.s   4b

        .section .rodata
        .balign 4
vectors:
        .long   0, 0                            /* reset SSP / PC: not used */
        .rept   254
        .long   exc_common
        .endr

        .section .bss
        .balign 4
        .space  16384
stack_top:
