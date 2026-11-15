/* Smoke-test kernel for SquirrelBIOS m68k-virt, linked at 0x1000 like an
 * m68k Linux. Original code. Sets its own stack and runs C; the bootinfo
 * records are right after _end. */
        .text
        .globl _start
_start:
        lea     kstack_top, %sp
        jsr     kmain
1:      bra.s   1b

        .globl get_sr, get_vbr, get_tc
get_sr:
        moveq   #0, %d0
        move.w  %sr, %d0
        rts
get_vbr:
        movec   %vbr, %d0
        rts
get_tc:
        movec   %tc, %d0
        rts

        .bss
        .balign 4
        .space  8192
kstack_top:
