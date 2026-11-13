/* Smoke-test kernel for SquirrelBIOS sparc64-sun4u, linked at 0x404000.
 * Original code. Entered with %o4 = the client interface handler, the
 * MMU on and the firmware's trap table; sets its own stack and runs C. */
#define STACK_BIAS 2047
        .register %g2, #scratch
        .section .text
        .globl _start
_start:
        mov     %o4, %l0
        mov     %o0, %l1
        mov     %o1, %l2
        set     kstack_top, %g1
        sub     %g1, STACK_BIAS + 192, %sp
        mov     %l0, %o0
        mov     %l1, %o1
        call    kmain
         mov    %l2, %o2
        ba      .
         nop

        .globl get_pstate, get_tl, get_tba, get_lsu
get_pstate:
        retl
         rdpr   %pstate, %o0
get_tl:
        retl
         rdpr   %tl, %o0
get_tba:
        retl
         rdpr   %tba, %o0
get_lsu:
        retl
         ldxa   [%g0] 0x45, %o0

        .section .bss
        .balign 16
        .space  32768
kstack_top:
