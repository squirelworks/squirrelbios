/* Smoke-test kernel for SquirrelBIOS sparc64-sun4u, linked at 0x404000.
 * Original code. Entered with %o4 = the client interface handler, the
 * MMU on and the firmware's trap table; sets its own stack and runs C. */
#define STACK_BIAS 2047
        .register %g2, #scratch
        .register %g3, #scratch
        .register %g6, #scratch
        .register %g7, #scratch
        .section .text
        .globl _start
_start:
        ba      1f
         nop
        /* A Linux-style "HdrS" header (version 0x0301): the firmware
         * fills in the initrd fields as it does for Linux */
        .globl hdrs
hdrs:
        .ascii  "HdrS"
        .word   0
        .half   0x0301
        .half   0, 0, 0
        .word   0, 0                    /* +16 image, +20 size */
        .xword  0, 0
        .xword  0                       /* +40 image64 */
        .word   0
1:      mov     %o4, %l0
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

/* uint64_t cif_keeps_globals(void *cif, uint64_t *args): calls the
 * client interface with known values in %g1-%g7 and returns a bit for
 * each one that came back changed */
        .globl cif_keeps_globals
cif_keeps_globals:
        save    %sp, -192, %sp
        set     0x11111111, %g1
        set     0x22222222, %g2
        set     0x33333333, %g3
        set     0x44444444, %g4
        set     0x55555555, %g5
        set     0x66666666, %g6
        set     0x77777777, %g7
        call    %i0
         mov    %i1, %o0
        clr     %i0
        set     0x11111111, %l0
        cmp     %g1, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x02, %i0
1:      set     0x22222222, %l0
        cmp     %g2, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x04, %i0
1:      set     0x33333333, %l0
        cmp     %g3, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x08, %i0
1:      set     0x44444444, %l0
        cmp     %g4, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x10, %i0
1:      set     0x55555555, %l0
        cmp     %g5, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x20, %i0
1:      set     0x66666666, %l0
        cmp     %g6, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x40, %i0
1:      set     0x77777777, %l0
        cmp     %g7, %l0
        bne,a,pn %xcc, 1f
         or     %i0, 0x80, %i0
1:      ret
         restore

/* D-TLB entry 'i': data and tag */
        .globl dtlb_data, dtlb_tag
dtlb_data:
        sllx    %o0, 3, %o0
        retl
         ldxa   [%o0] 0x5d, %o0
dtlb_tag:
        sllx    %o0, 3, %o0
        retl
         ldxa   [%o0] 0x5e, %o0

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
