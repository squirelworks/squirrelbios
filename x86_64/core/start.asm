; SquirrelBIOS x86_64-uefi: entry of the firmware body at FW_BASE.
; Original code.
;
; Reached from reset/reset.asm in 64-bit mode with RSP = FW_BASE and the
; first 4 GB identity-mapped. Clears .bss and calls fw_main.

        bits 64
        section .text.entry
        global _start
        extern fw_main, __bss_start, __bss_end

_start:
        cld
        mov rdi, __bss_start
        mov rcx, __bss_end
        sub rcx, rdi
        xor eax, eax
        rep stosb
        and rsp, -16
        call fw_main
.halt:
        cli
        hlt
        jmp .halt

        section .note.GNU-stack noalloc noexec nowrite progbits
