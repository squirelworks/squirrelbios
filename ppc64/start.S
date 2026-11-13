/* SquirrelBIOS ppc64le-powernv: reset entry, relocation, CPU parking,
 * the OPAL-style call entry and the jump to the OS.
 * Original code.
 *
 * QEMU's powernv machine copies the -bios image to physical 0 and starts
 * every hardware thread at 0x10, big-endian, in hypervisor real mode,
 * with r3 = its device tree. The image is linked at FW_BASE: the first
 * thread to arrive copies it there and enters C; the others wait for
 * the copy and then park in it (see ppc64/README.md). The low copy keeps
 * the exception vectors until the OS replaces them.
 *
 * The firmware itself runs big-endian (ELFv2 ABI), like the OPAL
 * interface it provides.
 */

#define SPR_PIR    1023
#define SPR_SRR0   26
#define SPR_SRR1   27
#define SPR_HSRR0  314
#define SPR_HSRR1  315

#define RELOC_MAGIC 0x53514657      /* "SQFW" */

/* Load a 32-bit absolute address (everything here is below 2 GB) */
#define LOADADDR(r, sym)  lis r, sym@ha; addi r, r, sym@l

        .section .text.head, "ax"
        .globl _start
_start:
        b       reloc_entry             /* 0x00 */
        .long   0, 0, 0
        b       reloc_entry             /* 0x10: QEMU's reset entry */

        /* Exception vectors 0x100-0x17e0: one stub every 0x20 bytes. Each
         * passes its own offset to fw_trap in the relocated image. */
        .org    0x100
        .rept   (0x1800 - 0x100) / 0x20
1:      mtsprg  2, %r3                  /* keep r3 for the report */
        li      %r3, (1b - _start)
        LOADADDR(%r4, trap_common)
        mtctr   %r4
        bctr
        .balign 0x20
        .endr

        .org    0x1800
reloc_entry:
        mr      %r31, %r3               /* device tree from QEMU */
        mfspr   %r30, SPR_PIR
        bl      1f
1:      mflr    %r29                    /* r29 = where 1b runs now */
        addi    %r28, %r29, (boot_claim - 1b)
2:      lwarx   %r4, 0, %r28
        cmpwi   %r4, 0
        bne     wait_reloc
        li      %r4, 1
        stwcx.  %r4, 0, %r28
        bne-    2b
        isync

        /* Boot thread: copy [_start, __image_end) from where it runs to
         * FW_BASE, a doubleword at a time */
        addi    %r5, %r29, (_start - 1b)        /* source */
        LOADADDR(%r6, _start)                   /* destination */
        LOADADDR(%r7, __image_end)
        subf    %r7, %r6, %r7
        addi    %r7, %r7, 7
        srdi    %r7, %r7, 3
        mtctr   %r7
        cmpld   %r5, %r6
        beq     4f                              /* already there */
3:      ld      %r8, 0(%r5)
        std     %r8, 0(%r6)
        addi    %r5, %r5, 8
        addi    %r6, %r6, 8
        bdnz    3b
4:      /* Make the new copy visible to instruction fetch */
        LOADADDR(%r5, _start)
        LOADADDR(%r7, __image_end)
5:      dcbst   0, %r5
        addi    %r5, %r5, 128
        cmpld   %r5, %r7
        blt     5b
        sync
        LOADADDR(%r5, _start)
6:      icbi    0, %r5
        addi    %r5, %r5, 128
        cmpld   %r5, %r7
        blt     6b
        sync
        isync
        /* Tell the waiting threads */
        LOADADDR(%r5, reloc_done)
        lis     %r6, RELOC_MAGIC@h
        ori     %r6, %r6, RELOC_MAGIC@l
        stw     %r6, 0(%r5)
        sync
        LOADADDR(%r5, boot_high)
        mtctr   %r5
        bctr

wait_reloc:
        /* Other threads: wait for the relocated image, then park in it */
        LOADADDR(%r5, reloc_done)
        lis     %r6, RELOC_MAGIC@h
        ori     %r6, %r6, RELOC_MAGIC@l
7:      lwz     %r4, 0(%r5)
        cmpw    %r4, %r6
        bne     7b
        isync
        LOADADDR(%r5, park_high)
        mtctr   %r5
        bctr

        .balign 8
boot_claim:
        .long   0
reloc_done:
        .long   0

        .text

boot_high:
        LOADADDR(%r1, boot_stack_top)
        li      %r0, 0
        stdu    %r0, -64(%r1)
        LOADADDR(%r2, .TOC.)
        /* Clear .bss */
        LOADADDR(%r5, __bss_start)
        LOADADDR(%r6, __bss_end)
1:      cmpld   %r5, %r6
        bge     2f
        std     %r0, 0(%r5)
        addi    %r5, %r5, 8
        b       1b
2:      mr      %r3, %r31
        mr      %r4, %r30
        bl      fw_main
        b       .

park_high:
        /* Wait for the boot thread to set up the slot table (it sets
         * cpus_ready once .bss is clear and the slots are numbered) */
        LOADADDR(%r5, cpus_ready)
1:      lwz     %r4, 0(%r5)
        cmpwi   %r4, 0
        beq     1b
        isync
        LOADADDR(%r2, .TOC.)
        mr      %r3, %r30
        bl      cpu_stack_for           /* r3 = stack top for this PIR, or 0 */
        cmpdi   %r3, 0
        beq     .                       /* no slot: stay here */
        mr      %r1, %r3
        li      %r0, 0
        stdu    %r0, -64(%r1)
        mr      %r3, %r30
        bl      fw_secondary            /* does not return */
        b       .

/* void fw_park(uint64_t stack_top, uint32_t pir): back to the waiting
 * loop on a fresh stack (OPAL_RETURN_CPU) */
        .globl fw_park
fw_park:
        mr      %r1, %r3
        li      %r0, 0
        stdu    %r0, -64(%r1)
        mr      %r3, %r4
        bl      fw_secondary
        b       .

/* void fw_enter(uint64_t entry, uint64_t r3, uint64_t r4, uint64_t r5,
 *               uint64_t r8, uint64_t r9): jump to the OS, MSR unchanged
 * (hypervisor real mode, big-endian, external interrupts off) */
        .globl fw_enter
fw_enter:
        mtctr   %r3
        mr      %r3, %r4
        mr      %r4, %r5
        mr      %r5, %r6
        mr      %r9, %r8
        mr      %r8, %r7
        li      %r6, 0
        li      %r7, 0
        li      %r10, 0
        li      %r0, 0
        sync
        isync
        bctr

/* OPAL-style entry. Called with the token in r0, arguments in r3-r10,
 * the return address in LR, big-endian real mode. r1 and the C
 * non-volatile registers come back unchanged; r3 holds the result. Each
 * thread runs on its own stack, found from its PIR. */
        .globl opal_entry
opal_entry:
        mfspr   %r11, SPR_PIR
        clrldi  %r11, %r11, 52          /* PIR & 0xfff */
        sldi    %r11, %r11, 3
        LOADADDR(%r12, opal_stack_by_pir)
        ldx     %r12, %r12, %r11
        cmpdi   %r12, 0
        beq     opal_no_stack
        addi    %r12, %r12, -144
        std     %r1, 0(%r12)            /* back chain = caller's r1 */
        mr      %r1, %r12
        std     %r0, 32(%r1)
        std     %r3, 40(%r1)
        std     %r4, 48(%r1)
        std     %r5, 56(%r1)
        std     %r6, 64(%r1)
        std     %r7, 72(%r1)
        std     %r8, 80(%r1)
        std     %r9, 88(%r1)
        std     %r10, 96(%r1)
        mflr    %r0
        std     %r0, 112(%r1)
        std     %r2, 120(%r1)
        LOADADDR(%r2, .TOC.)
        addi    %r3, %r1, 32
        bl      opal_dispatch
        ld      %r0, 112(%r1)
        mtlr    %r0
        ld      %r2, 120(%r1)
        ld      %r1, 0(%r1)
        blr
opal_no_stack:
        li      %r3, -11                /* OPAL_INTERNAL_ERROR */
        blr

/* Exceptions before the OS has its own vectors: r3 = vector offset,
 * SPRG2 = the interrupted r3 */
trap_common:
        LOADADDR(%r1, trap_stack_top)
        li      %r0, 0
        stdu    %r0, -64(%r1)
        LOADADDR(%r2, .TOC.)
        mfspr   %r4, SPR_SRR0
        mfspr   %r5, SPR_SRR1
        mfspr   %r6, SPR_HSRR0
        mfspr   %r7, SPR_HSRR1
        bl      fw_trap
        b       .

        .section .bss
        .balign 16
        .space  16384
boot_stack_top:
        .space  8192
trap_stack_top:
