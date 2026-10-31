/* Smoke-test images with a Linux arm64 Image header. Original code.
 *
 * Built three ways (see the Makefile):
 *   default          flags bit 3 clear: placement fixed at the 2 MB-aligned
 *                    RAM base + text_offset, so linked at 0x40400000
 *   --defsym RELOC=1 flags bit 3 set: linked at 0x40200000, and text_offset
 *                    is still 0x400000, so honouring it would be a bug
 *   --defsym SHORT=1 image_size smaller than the file: must be refused
 * The image prints "OS-ENTRY arm image" and " at=ok" when it runs at the
 * address it was linked at.
 */

        .equ UART_BASE,   0x09000000
        .equ TEXT_OFFSET, 0x00400000

        .ifdef RELOC
        .equ FLAGS, (1 << 3) | (1 << 1)     /* anywhere, 4 KB pages */
        .else
        .equ FLAGS, (1 << 1)                /* fixed, 4 KB pages */
        .endif

.global _start
_start:
        b entry                             /* code0 */
        .word 0                             /* code1 */
        .quad TEXT_OFFSET
        .ifdef SHORT
        .quad 16                            /* image_size: too small */
        .else
        .quad _end - _start + 0x1000        /* image_size, with a BSS */
        .endif
        .quad FLAGS
        .quad 0, 0, 0                       /* res2-res4 */
        .ascii "ARM\x64"                    /* magic at 0x38 */
        .word 0                             /* res5 */

entry:
        adr x0, msg_entry
        bl puts
        adr x1, _start                      /* where it runs */
        ldr x2, =_start                     /* where it was linked */
        adr x0, msg_at_bad
        cmp x1, x2
        b.ne 1f
        adr x0, msg_at_ok
1:      bl puts
2:      wfi
        b 2b

puts:
        ldr x1, =UART_BASE
1:      ldrb w2, [x0], #1
        cbz w2, 2f
        str w2, [x1]
        b 1b
2:      ret

        .ltorg

msg_entry:      .asciz "OS-ENTRY arm image"
msg_at_ok:      .asciz " at=ok\n"
msg_at_bad:     .asciz " at=bad\n"
        .balign 8
_end:
