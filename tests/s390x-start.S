/* SquirrelBIOS make test: s390x test kernel entry. Keeps the hand-off
 * registers and the PSW mask for tests/s390x-kernel.c. Original code. */
        .text
        .globl _start
_start:
        larl    %r1, entry_regs
        stg     %r2, 0(%r1)
        stg     %r3, 8(%r1)
        stg     %r15, 16(%r1)
        epsw    %r4, %r5
        st      %r4, 24(%r1)
        st      %r5, 28(%r1)
        larl    %r15, kstack_top
        aghi    %r15, -160
        brasl   %r14, kernel_main
        j       .

        .globl entry_regs
        .data
        .balign 8
entry_regs:
        .quad   0, 0, 0, 0

        .bss
        .balign 4096
        .space  16384
kstack_top:
