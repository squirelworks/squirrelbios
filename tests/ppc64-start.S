/* Smoke-test kernel for SquirrelBIOS ppc64le-powernv: entry points and
 * the OPAL call path of a little-endian OS. Original code.
 *
 * The firmware enters big-endian (as OPAL firmware does), so each entry
 * starts with a word that is a no-op trap in little-endian and a branch
 * in big-endian, into a big-endian sequence that turns MSR[LE] on.
 * Calls into the firmware switch back to big-endian real mode, and the
 * return path switches to little-endian again.
 */

/* One instruction word in big-endian byte order, whatever we assemble for */
.macro BE w
        .byte   (\w >> 24) & 0xff, (\w >> 16) & 0xff, (\w >> 8) & 0xff, \w & 0xff
.endm

/* Entered big-endian or little-endian: carry on little-endian at 1: */
.macro TO_LITTLE_ENDIAN
        tdi     0, 0, 0x48              /* LE: no-op. BE reads 48 00 00 08: b .+8 */
        b       1f                      /* LE: skip the BE words */
        BE      0x7d6000a6              /* mfmsr  r11 */
        BE      0x616b0001              /* ori    r11,r11,1 (MSR[LE]) */
        BE      0x48000005              /* bl     .+4 */
        BE      0x7d4802a6              /* mflr   r10 */
        BE      0x394a0014              /* addi   r10,r10,20 (to 1:) */
        BE      0x7d5a03a6              /* mtsrr0 r10 */
        BE      0x7d7b03a6              /* mtsrr1 r11 */
        BE      0x4c000024              /* rfid */
1:
.endm

#define LOADADDR(r, sym)  lis r, sym@ha; addi r, r, sym@l

        .section .text.entry, "ax"
        .globl _start
_start:
        TO_LITTLE_ENDIAN                /* r3-r9 kept; r11 = entry MSR | LE */
        mr      %r20, %r3
        mr      %r21, %r4
        mr      %r22, %r5
        mr      %r23, %r8
        mr      %r24, %r9
        mr      %r25, %r11
        LOADADDR(%r1, stack0_top)
        li      %r0, 0
        stdu    %r0, -64(%r1)
        LOADADDR(%r2, .TOC.)
        mr      %r3, %r20
        mr      %r4, %r21
        mr      %r5, %r22
        mr      %r6, %r23
        mr      %r7, %r24
        mr      %r8, %r25
        bl      kmain
        b       .

/* Secondary threads started with OPAL_START_CPU: r3 = PIR */
        .globl secondary_entry
secondary_entry:
        TO_LITTLE_ENDIAN
        mr      %r20, %r3
        /* stack = sec_stacks + (n + 1) * 4 KB, n = this thread's index */
        LOADADDR(%r2, .TOC.)
        LOADADDR(%r5, sec_slot)
2:      lwarx   %r6, 0, %r5
        addi    %r7, %r6, 1
        stwcx.  %r7, 0, %r5
        bne-    2b
        andi.   %r6, %r6, 7
        addi    %r6, %r6, 1
        sldi    %r6, %r6, 12
        LOADADDR(%r1, sec_stacks)
        add     %r1, %r1, %r6
        li      %r0, 0
        stdu    %r0, -64(%r1)
        mr      %r3, %r20
        bl      ksecondary
        b       .

/* int64_t opal_call(uint64_t token, a0, a1, a2, a3): r3 = token */
        .text
        .globl opal_call
opal_call:
        mflr    %r0
        std     %r0, 16(%r1)
        std     %r2, 24(%r1)
        stdu    %r1, -64(%r1)
        mr      %r0, %r3
        mr      %r3, %r4
        mr      %r4, %r5
        mr      %r5, %r6
        mr      %r6, %r7
        mr      %r7, %r8
        LOADADDR(%r12, opal_entry_addr)
        ld      %r12, 0(%r12)
        mtsrr0  %r12
        mfmsr   %r11
        li      %r12, 0
        ori     %r12, %r12, 0x8001      /* MSR[EE] | MSR[LE] */
        andc    %r11, %r11, %r12
        mtsrr1  %r11
        bl      1f
1:      mflr    %r12
        addi    %r12, %r12, (opal_return - 1b)
        mtlr    %r12
        LOADADDR(%r12, opal_base_addr)
        ld      %r2, 0(%r12)
        rfid
opal_return:                            /* the firmware returns here big-endian */
        BE      0x7d6000a6              /* mfmsr  r11 */
        BE      0x616b0001              /* ori    r11,r11,1 */
        BE      0x48000005              /* bl     .+4 */
        BE      0x7d8802a6              /* mflr   r12 */
        BE      0x398c0014              /* addi   r12,r12,20 */
        BE      0x7d9a03a6              /* mtsrr0 r12 */
        BE      0x7d7b03a6              /* mtsrr1 r11 */
        BE      0x4c000024              /* rfid */
        addi    %r1, %r1, 64
        ld      %r2, 24(%r1)
        ld      %r0, 16(%r1)
        mtlr    %r0
        blr

        .globl get_msr
get_msr:
        mfmsr   %r3
        blr

        .globl get_pir
get_pir:
        mfspr   %r3, 1023
        blr

        .data
        .balign 8
        .globl opal_entry_addr, opal_base_addr
opal_entry_addr: .quad 0
opal_base_addr:  .quad 0
sec_slot:        .long 0

        .bss
        .balign 16
        .space  16384
stack0_top:
sec_stacks:
        .space  9 * 4096
