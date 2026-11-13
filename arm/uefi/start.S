/* SquirrelBIOS aarch64-uefi: reset entry, exception vectors and the EFI
 * image call thunk. Original code, written from the Arm Architecture
 * Reference Manual (A-profile).
 *
 * QEMU virt starts the CPU at address 0 (the flash holding this image)
 * with the MMU and caches off, at EL1, or EL2 with virtualization=on.
 * _start copies the image to RAM at FW_BASE (where it is linked), jumps
 * there, clears .bss, sets the stack and the vector base for the current
 * EL, lets FP/SIMD run, then calls arch_mmu_init (identity map, caches
 * on) and efi_main with the device tree QEMU placed at the start of RAM.
 *
 * Started at EL3 (virt with secure=on), every CPU runs from 0: the boot
 * CPU (affinity 0) copies the image, sets up the EL3 monitor (el3.c) and
 * drops to non-secure EL2 or EL1, the others wait in el3_pen until PSCI
 * CPU_ON gives them an entry point. */

        .section .text.entry, "ax"
        .globl _start
_start:
        msr     daifset, #0xf
        mrs     x0, CurrentEL
        cmp     x0, #(3 << 2)
        b.ne    .Lcopy
        mrs     x0, mpidr_el1
        ubfx    x1, x0, #16, #8         // Aff2
        ubfx    x2, x0, #32, #8         // Aff3
        orr     x1, x1, x2
        cbnz    x1, el3_dead
        ubfx    x1, x0, #0, #8          // Aff0
        ubfx    x2, x0, #8, #8          // Aff1
        orr     x3, x1, x2
        cbz     x3, .Lcopy              // the boot CPU
        cmp     x1, #16
        b.hs    el3_dead
        add     x19, x1, x2, lsl #4     // el3_cpu_index: Aff1 * 16 + Aff0
        cmp     x19, #64
        b.hs    el3_dead
        b       el3_pen_index
.Lcopy:
        adr     x9, _start              // where we run now (flash)
        ldr     x10, =__fw_start        // where we are linked (RAM)
        ldr     x11, =__image_end
        cmp     x9, x10
        b.eq    1f
        sub     x12, x11, x10
0:      ldp     x13, x14, [x9], #16
        stp     x13, x14, [x10], #16
        subs    x12, x12, #16
        b.gt    0b
        dsb     sy
        ic      iallu
        dsb     sy
        isb
        ldr     x10, =1f
        br      x10
1:      ldr     x0, =__bss_start
        ldr     x1, =__bss_end
2:      cmp     x0, x1
        b.hs    3f
        str     xzr, [x0], #8
        b       2b
3:      ldr     x0, =__stack_top
        mov     sp, x0
        mrs     x0, CurrentEL
        cmp     x0, #(3 << 2)
        b.ne    8f
        ldr     x1, =el3_vectors
        msr     vbar_el3, x1
        isb
        mov     x0, #0x40000000         // ARCH_DTB_ADDR
        bl      el3_init                // returns the SPSR for the drop
        msr     spsr_el3, x0
        ldr     x0, =el3_stacks + 2048  // CPU 0's monitor stack
        mov     sp, x0
        ldr     x1, =8f
        msr     elr_el3, x1
        eret
8:      ldr     x0, =__stack_top
        mov     sp, x0
        ldr     x1, =vectors
        mrs     x0, CurrentEL
        cmp     x0, #(2 << 2)
        b.eq    4f
        b.hi    5f
        msr     vbar_el1, x1
        mov     x0, #(3 << 20)          // CPACR_EL1.FPEN: FP/SIMD at EL0 and EL1
        msr     cpacr_el1, x0
        b       6f
4:      msr     vbar_el2, x1
        mov     x0, #0x33ff             // CPTR_EL2: RES1 bits, TFP clear
        msr     cptr_el2, x0
        mov     x0, #(3 << 20)
        msr     cpacr_el1, x0
        b       6f
5:      msr     vbar_el3, x1            // EL3: arch_mmu_init reports it
6:      isb
        bl      arch_mmu_init
        mov     x0, #0x40000000         // ARCH_DTB_ADDR
        bl      efi_main
7:      wfi
        b       7b
        .ltorg

/* The holding pen: x19 is the CPU's index into el3_cpus (32 bytes each),
 * whose entry turns non-zero on PSCI CPU_ON. Runs from flash before the
 * first CPU_ON and from RAM after a CPU_OFF; the MMU is off either way. */
el3_pen_index:
        ldr     x20, =el3_cpus
        add     x20, x20, x19, lsl #5
1:      ldr     x0, [x20]
        cbnz    x0, 2f
        wfe
        b       1b
2:      ldr     x1, =el3_secondary
        br      x1
el3_dead:
        wfi
        b       el3_dead
        .ltorg

        .text
/* A CPU leaving the pen: its monitor stack and vectors, its secure set-up
 * (el3_cpu_start), then eret to the entry point with x0 = context ID */
el3_secondary:
        ldr     x0, =el3_stacks + 2048
        add     x0, x0, x19, lsl #11
        mov     sp, x0
        ldr     x1, =el3_vectors
        msr     vbar_el3, x1
        isb
        mov     x0, x19
        bl      el3_cpu_start
        msr     spsr_el3, x0
        ldr     x1, [x20]
        msr     elr_el3, x1
        ldr     x0, [x20, #8]
        mov     x1, xzr
        mov     x2, xzr
        mov     x3, xzr
        eret

/* el3_park(index): PSCI CPU_OFF, back to the pen with an empty stack */
        .globl  el3_park
el3_park:
        mov     x19, x0
        ldr     x1, =el3_stacks + 2048
        add     x1, x1, x19, lsl #11
        mov     sp, x1
        b       el3_pen_index

/* EL3 vectors: smc from a lower EL goes to el3_smc with x0-x30 saved on
 * the CPU's monitor stack; anything else is reported */
        .macro  el3_vec n
        .balign 0x80
        .if     \n == 8
        b       el3_sync_lower
        .else
        mov     x0, #\n
        b       el3_bad
        .endif
        .endm

        .balign 0x800
el3_vectors:
        el3_vec 0
        el3_vec 1
        el3_vec 2
        el3_vec 3
        el3_vec 4
        el3_vec 5
        el3_vec 6
        el3_vec 7
        el3_vec 8
        el3_vec 9
        el3_vec 10
        el3_vec 11
        el3_vec 12
        el3_vec 13
        el3_vec 14
        el3_vec 15

el3_sync_lower:
        sub     sp, sp, #256
        stp     x0, x1, [sp]
        stp     x2, x3, [sp, #16]
        stp     x4, x5, [sp, #32]
        stp     x6, x7, [sp, #48]
        stp     x8, x9, [sp, #64]
        stp     x10, x11, [sp, #80]
        stp     x12, x13, [sp, #96]
        stp     x14, x15, [sp, #112]
        stp     x16, x17, [sp, #128]
        stp     x18, x19, [sp, #144]
        stp     x20, x21, [sp, #160]
        stp     x22, x23, [sp, #176]
        stp     x24, x25, [sp, #192]
        stp     x26, x27, [sp, #208]
        stp     x28, x29, [sp, #224]
        str     x30, [sp, #240]
        mrs     x0, esr_el3
        lsr     x0, x0, #26
        cmp     x0, #0x17               // smc from AArch64
        b.eq    1f
        cmp     x0, #0x13               // smc from AArch32
        b.eq    1f
        mov     x0, #8
        b       el3_bad
1:      mov     x0, sp
        bl      el3_smc
        ldp     x0, x1, [sp]
        ldp     x2, x3, [sp, #16]
        ldp     x4, x5, [sp, #32]
        ldp     x6, x7, [sp, #48]
        ldp     x8, x9, [sp, #64]
        ldp     x10, x11, [sp, #80]
        ldp     x12, x13, [sp, #96]
        ldp     x14, x15, [sp, #112]
        ldp     x16, x17, [sp, #128]
        ldp     x18, x19, [sp, #144]
        ldp     x20, x21, [sp, #160]
        ldp     x22, x23, [sp, #176]
        ldp     x24, x25, [sp, #192]
        ldp     x26, x27, [sp, #208]
        ldp     x28, x29, [sp, #224]
        ldr     x30, [sp, #240]
        add     sp, sp, #256
        eret
el3_bad:
        bl      el3_unexpected
        b       el3_bad

/* image_call(entry, handle, system table, &saved_sp) */
        .text
        .globl image_call, image_exit
image_call:
        stp     x29, x30, [sp, #-96]!
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]
        stp     x23, x24, [sp, #48]
        stp     x25, x26, [sp, #64]
        stp     x27, x28, [sp, #80]
        mov     x29, sp
        mov     x9, sp
        str     x9, [x3]
        mov     x9, x0
        mov     x0, x1
        mov     x1, x2
        blr     x9
image_call_out:
        ldp     x19, x20, [sp, #16]
        ldp     x21, x22, [sp, #32]
        ldp     x23, x24, [sp, #48]
        ldp     x25, x26, [sp, #64]
        ldp     x27, x28, [sp, #80]
        ldp     x29, x30, [sp], #96
        ret
/* image_exit(saved_sp, status) */
image_exit:
        mov     sp, x0
        mov     x0, x1
        b       image_call_out

/* Exception vectors: every entry saves x0-x3 and lr and calls
 * arch_exception(entry number), which reports and halts. */
        .macro  vec n
        .balign 0x80
        sub     sp, sp, #48
        stp     x0, x1, [sp]
        stp     x2, x3, [sp, #16]
        str     x30, [sp, #32]
        mov     x0, #\n
        bl      arch_exception
        .endm

        .balign 0x800
vectors:
        vec 0
        vec 1
        vec 2
        vec 3
        vec 4
        vec 5
        vec 6
        vec 7
        vec 8
        vec 9
        vec 10
        vec 11
        vec 12
        vec 13
        vec 14
        vec 15

        .section .bss.stack, "aw", %nobits
        .balign 16
        .space  0x10000
__stack_top:
