/* SquirrelBIOS sparc64-sun4u: reset, copy to RAM, MMU on, trap table,
 * register-window traps, and the jump into C.
 * Original code, written from the SPARC V9 architecture manual and the
 * UltraSPARC-IIi user's manual (MMU and ASI registers).
 *
 * QEMU's sun4u starts the CPU in RED_state at PROM + 0x20 with the MMUs
 * off. We read the RAM size from fw_cfg, copy the image to the top 4 MB
 * of RAM, lock one 4 MB TLB entry mapping it at FW_VBASE (0xf0000000,
 * where IEEE 1275 SPARC firmware lives), and run there with the MMUs on.
 * I/O and other physical memory are reached with the bypass ASIs.
 */

#define STACK_BIAS      2047
#define PSTATE_PRIV     0x04
#define PSTATE_PEF      0x10

#define ASI_PHYS_USE_EC         0x14
#define ASI_PHYS_BYPASS_EC_E    0x15
#define ASI_PHYS_BYPASS_EC_E_L  0x1d
#define ASI_LSU_CONTROL         0x45
#define ASI_IMMU                0x50
#define ASI_ITLB_DATA_IN        0x54
#define ASI_DMMU                0x58
#define ASI_DTLB_DATA_IN        0x5c
#define MMU_TAG_ACCESS          0x30
#define MMU_PRIMARY_CONTEXT     0x08

#define FWCFG_PORT      0x1fe02000510   /* selector; data at +1 */
#define FWCFG_RAM_SIZE  3

/* TTE: valid, 4 MB, locked, cacheable, privileged, global */
#define TTE_4M_HI       0xe0000000      /* V | size 4 MB, bits 63..61 */
#define TTE_LO_RAM      0x77            /* L CP CV P W G */
#define TTE_LO_ROM      0x45            /* L P G */
#define FW_VBASE        0xf0000000
#define FOUR_MB         0x400000

        .register %g2, #scratch
        .register %g3, #scratch
        .register %g6, #ignore
        .register %g7, #ignore

        .section .text.head, "ax"
        .globl _start
_start:
        ba      reset                   /* PROM + 0x00 */
         nop
        .skip   0x20 - (. - _start)
reset:                                  /* PROM + 0x20: power-on reset */
        wrpr    %g0, PSTATE_PRIV | PSTATE_PEF, %pstate
        wrpr    %g0, 0, %tl
        wr      %g0, 0, %fprs

        /* g4 = RAM size: fw_cfg key 3, a little-endian u64 */
        setx    FWCFG_PORT, %g2, %g1
        mov     FWCFG_RAM_SIZE, %g3
        stha    %g3, [%g1] ASI_PHYS_BYPASS_EC_E_L
        add     %g1, 1, %g1
        clr     %g4
        clr     %g5
1:      lduba   [%g1] ASI_PHYS_BYPASS_EC_E, %g3
        sllx    %g3, %g5, %g3
        or      %g4, %g3, %g4
        add     %g5, 8, %g5
        cmp     %g5, 64
        bne     %xcc, 1b
         nop

        /* g6 = physical base of the firmware: the top 4 MB of RAM */
        setx    FOUR_MB, %g2, %g3
        sub     %g4, %g3, %g6
        sub     %g3, 1, %g2
        andn    %g6, %g2, %g6

        /* g1 = where _start is now (the PROM), g2 = bytes to copy */
2:      rd      %pc, %g1
        sub     %g1, (2b - _start), %g1
        set     __image_end, %g2
        set     _start, %g3
        sub     %g2, %g3, %g2
        add     %g2, 7, %g2
        andn    %g2, 7, %g2
        mov     %g6, %g5
3:      ldx     [%g1], %g3
        stxa    %g3, [%g5] ASI_PHYS_USE_EC
        add     %g1, 8, %g1
        subcc   %g2, 8, %g2
        bne     %xcc, 3b
         add    %g5, 8, %g5

        /* Lock FW_VBASE -> g6 in both TLBs */
        mov     MMU_TAG_ACCESS, %g7
        set     FW_VBASE, %g1
        sethi   %hi(TTE_4M_HI), %g2
        sllx    %g2, 32, %g2
        or      %g2, TTE_LO_RAM, %g2
        or      %g2, %g6, %g2
        stxa    %g1, [%g7] ASI_DMMU
        stxa    %g2, [%g0] ASI_DTLB_DATA_IN
        membar  #Sync
        stxa    %g1, [%g7] ASI_IMMU
        stxa    %g2, [%g0] ASI_ITLB_DATA_IN
        membar  #Sync
        /* And the PROM where it is, so fetching carries on when the
         * MMUs come on (dropped again before an OS starts) */
        rd      %pc, %g1
        srlx    %g1, 22, %g1
        sllx    %g1, 22, %g1
        sethi   %hi(TTE_4M_HI), %g2
        sllx    %g2, 32, %g2
        or      %g2, TTE_LO_ROM, %g2
        or      %g2, %g1, %g2
        stxa    %g1, [%g7] ASI_IMMU
        stxa    %g2, [%g0] ASI_ITLB_DATA_IN
        membar  #Sync
        mov     MMU_PRIMARY_CONTEXT, %g1
        stxa    %g0, [%g1] ASI_DMMU
        membar  #Sync
        mov     0xf, %g1                /* IC DC IM DM */
        stxa    %g1, [%g0] ASI_LSU_CONTROL
        membar  #Sync
        set     ram_entry, %g1
        jmpl    %g1, %g0
         nop

        .text
/* Fresh register windows and the boot stack; g1 = stack top */
#define WINDOWS_AND_STACK(top)                  \
        rdpr    %ver, %g1;                      \
        and     %g1, 0x1f, %g1;                 \
        wrpr    %g0, 0, %cwp;                   \
        wrpr    %g0, 0, %canrestore;            \
        sub     %g1, 1, %g2;                    \
        wrpr    %g2, %cansave;                  \
        wrpr    %g0, 0, %otherwin;              \
        wrpr    %g1, %cleanwin;                 \
        wrpr    %g0, 0, %wstate;                \
        set     top, %g1;                       \
        sub     %g1, STACK_BIAS + 192, %sp;     \
        clr     %fp

ram_entry:
        set     trap_table, %g1
        wrpr    %g1, %tba
        WINDOWS_AND_STACK(boot_stack_top)
        /* Clear .bss */
        set     __bss_start, %g1
        set     __bss_end, %g2
1:      cmp     %g1, %g2
        bgeu    %xcc, 2f
         nop
        stx     %g0, [%g1]
        ba      1b
         add    %g1, 8, %g1
2:      mov     %g6, %o0
        call    fw_main
         mov    %g4, %o1
        ba      .
         nop

/* Unexpected trap: save its state, reset windows and TL, report in C */
bad_trap:
        set     trap_save, %g1
        rdpr    %tt, %g2
        stx     %g2, [%g1 + 0]
        rdpr    %tpc, %g2
        stx     %g2, [%g1 + 8]
        rdpr    %tnpc, %g2
        stx     %g2, [%g1 + 16]
        rdpr    %tstate, %g2
        stx     %g2, [%g1 + 24]
        rdpr    %tl, %g2
        stx     %g2, [%g1 + 32]
        wrpr    %g0, 0, %tl
        wrpr    %g0, PSTATE_PRIV | PSTATE_PEF, %pstate
        WINDOWS_AND_STACK(trap_stack_top)
        set     trap_save, %o0
        call    fw_trap
         nop
        ba      .
         nop

/* void fw_enter(uint64_t entry, uint64_t cif, uint64_t o0, uint64_t o1):
 * the OS starts with %o4 = client interface handler, a fresh window
 * state and its own stack to set up */
        .globl fw_enter
fw_enter:
        mov     %o0, %g3
        mov     %o1, %g4
        mov     %o2, %g5
        mov     %o3, %g7
        WINDOWS_AND_STACK(os_stack_top)
        mov     %g4, %o4
        mov     %g5, %o0
        mov     %g7, %o1
        clr     %o2
        clr     %o3
        jmpl    %g3, %g0
         clr    %o5

/* void fw_set_tba(uint64_t) */
        .globl fw_set_tba
fw_set_tba:
        retl
         wrpr   %o0, %tba

/* --- trap table -------------------------------------------------------- */

.macro BAD
        ba      bad_trap
         nop
        .skip   24
.endm

/* clean_window (0x24-0x27) */
.macro CLEANWIN
        clr     %o0
        clr     %o1
        clr     %o2
        clr     %o3
        clr     %o4
        clr     %o5
        clr     %o6
        clr     %o7
        clr     %l1
        clr     %l2
        clr     %l3
        clr     %l4
        clr     %l5
        clr     %l6
        clr     %l7
        rdpr    %cleanwin, %l0
        inc     %l0
        wrpr    %l0, %cleanwin
        clr     %l0
        retry
        .balign 128
.endm

/* spill_0_normal / fill_0_normal: 64-bit frames with the stack bias */
.macro SPILL
        stx     %l0, [%sp + STACK_BIAS + 0x00]
        stx     %l1, [%sp + STACK_BIAS + 0x08]
        stx     %l2, [%sp + STACK_BIAS + 0x10]
        stx     %l3, [%sp + STACK_BIAS + 0x18]
        stx     %l4, [%sp + STACK_BIAS + 0x20]
        stx     %l5, [%sp + STACK_BIAS + 0x28]
        stx     %l6, [%sp + STACK_BIAS + 0x30]
        stx     %l7, [%sp + STACK_BIAS + 0x38]
        stx     %i0, [%sp + STACK_BIAS + 0x40]
        stx     %i1, [%sp + STACK_BIAS + 0x48]
        stx     %i2, [%sp + STACK_BIAS + 0x50]
        stx     %i3, [%sp + STACK_BIAS + 0x58]
        stx     %i4, [%sp + STACK_BIAS + 0x60]
        stx     %i5, [%sp + STACK_BIAS + 0x68]
        stx     %i6, [%sp + STACK_BIAS + 0x70]
        stx     %i7, [%sp + STACK_BIAS + 0x78]
        saved
        retry
        .balign 128
.endm

.macro FILL
        ldx     [%sp + STACK_BIAS + 0x00], %l0
        ldx     [%sp + STACK_BIAS + 0x08], %l1
        ldx     [%sp + STACK_BIAS + 0x10], %l2
        ldx     [%sp + STACK_BIAS + 0x18], %l3
        ldx     [%sp + STACK_BIAS + 0x20], %l4
        ldx     [%sp + STACK_BIAS + 0x28], %l5
        ldx     [%sp + STACK_BIAS + 0x30], %l6
        ldx     [%sp + STACK_BIAS + 0x38], %l7
        ldx     [%sp + STACK_BIAS + 0x40], %i0
        ldx     [%sp + STACK_BIAS + 0x48], %i1
        ldx     [%sp + STACK_BIAS + 0x50], %i2
        ldx     [%sp + STACK_BIAS + 0x58], %i3
        ldx     [%sp + STACK_BIAS + 0x60], %i4
        ldx     [%sp + STACK_BIAS + 0x68], %i5
        ldx     [%sp + STACK_BIAS + 0x70], %i6
        ldx     [%sp + STACK_BIAS + 0x78], %i7
        restored
        retry
        .balign 128
.endm

.macro TRAP_LEVEL
        .rept   0x24
        BAD
        .endr
        CLEANWIN                        /* 0x24-0x27 */
        .rept   0x80 - 0x28
        BAD
        .endr
        SPILL                           /* 0x80-0x83 */
        .rept   0xc0 - 0x84
        BAD
        .endr
        FILL                            /* 0xc0-0xc3 */
        .rept   0x200 - 0xc4
        BAD
        .endr
.endm

        .section .text.traps, "ax"
        .balign 32768
        .globl trap_table
trap_table:
        TRAP_LEVEL                      /* TL = 0 */
        TRAP_LEVEL                      /* TL > 0 */

        .section .bss
        .balign 16
        .globl trap_save
trap_save:
        .space  64
        .space  32768
boot_stack_top:
        .space  16384
trap_stack_top:
        .space  16384
os_stack_top:
