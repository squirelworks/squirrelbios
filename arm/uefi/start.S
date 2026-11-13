/* SquirrelBIOS aarch64-uefi: reset entry, exception vectors and the EFI
 * image call thunk. Original code, written from the Arm Architecture
 * Reference Manual (A-profile).
 *
 * QEMU virt starts the CPU at address 0 (the flash holding this image)
 * with the MMU and caches off, at EL1, or EL2 with virtualization=on.
 * _start copies the image to RAM at FW_BASE (where it is linked), jumps
 * there, clears .bss, sets the stack and the vector base for the current
 * EL, lets FP/SIMD run, then calls arch_mmu_init (identity map, caches
 * on) and efi_main with the device tree QEMU placed at the start of RAM. */

        .section .text.entry, "ax"
        .globl _start
_start:
        msr     daifset, #0xf
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
