; Stand-in EFI application for the x86_64-uefi firmware (roadmap C1).
; Original code: a PE32+ image written out by hand, file layout equal to
; memory layout (both aligned to 4 KB). Its preferred base, 256 MB, is not
; free on a 128 MB machine, so the firmware must relocate it.
;
; It reports on COM1, since the firmware has no console protocol yet:
;   APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok
; then returns 1234h, or with -DEXIT calls Exit(5678h) instead.

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

; system table / boot services offsets (UEFI 2.x)
ST_BS           equ 96
BS_GET_MAP      equ 56
BS_ALLOC_POOL   equ 64
BS_FREE_POOL    equ 72
BS_HANDLE_PROT  equ 152
BS_EXIT         equ 216
LI_PARENT       equ 8
LI_ST           equ 16
LI_BASE         equ 64
LI_SIZE         equ 72

        org 0
mz:     db 'MZ'
        times 0x3C - ($ - $$) db 0
        dd pe
pe:     db 'PE', 0, 0
        dw 0x8664                       ; x64
        dw 2                            ; sections
        dd 0, 0, 0
        dw opt_end - opt
        dw 0x0022                       ; executable, large address aware
opt:    dw 0x20B                        ; PE32+
        db 0, 0
        dd 0x1000, 0, 0                 ; code, data, bss sizes
        dd entry                        ; entry point (RVA)
        dd 0x1000                       ; base of code
        dq IMAGE_BASE
        dd 0x1000, 0x1000               ; section and file alignment
        dw 0, 0, 0, 0, 0, 0
        dd 0
        dd 0x3000                       ; size of image
        dd 0x1000                       ; size of headers
        dd 0
        dw 10                           ; EFI application
        dw 0
        dq 0x10000, 0x1000, 0x10000, 0x1000
        dd 0
        dd 16                           ; data directories
        times 5 dq 0
        dd 0x2000, reloc_end - reloc    ; [5] base relocations
        times 10 dq 0
opt_end:
        db '.text', 0, 0, 0
        dd 0x1000, 0x1000, 0x1000, 0x1000, 0, 0
        dw 0, 0
        dd 0xE0000020
        db '.reloc', 0, 0
        dd reloc_end - reloc, 0x2000, 0x1000, 0x2000, 0, 0
        dw 0, 0
        dd 0x42000040
        times 0x1000 - ($ - $$) db 0

; ------------------------------------------------------------------ .text
; entry(ImageHandle = RCX, SystemTable = RDX), Microsoft x64 convention
entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        sub rsp, 48                     ; shadow space + two locals; RSP 16-aligned
        mov r12, rcx
        mov r13, rdx
        lea rsi, [rel s_app]
        call puts

        ; system table and boot services signatures
        mov rax, 0x5453595320494249     ; "IBI SYST"
        cmp [r13], rax
        lea rsi, [rel s_st]
        call result
        mov rbx, [r13 + ST_BS]
        mov rax, 0x56524553544f4f42     ; "BOOTSERV"
        cmp [rbx], rax
        lea rsi, [rel s_bs]
        call result

        ; the loaded image protocol on our handle
        mov rcx, r12
        lea rdx, [rel loaded_image_guid]
        lea r8, [rsp + 32]
        call [rbx + BS_HANDLE_PROT]
        test rax, rax
        jnz .li_bad
        mov rdi, [rsp + 32]
        cmp [rdi + LI_ST], r13
        jne .li_bad
        cmp qword [rdi + LI_PARENT], 0
        je .li_bad
        cmp qword [rdi + LI_SIZE], 0x3000
.li_bad:
        lea rsi, [rel s_li]
        call result
        lea rax, [rel mz]               ; where we really are
        cmp [rdi + LI_BASE], rax
        lea rsi, [rel s_base]
        call result

        ; an absolute pointer the firmware had to relocate
        mov rax, [rel abs_msg]
        lea rdx, [rel s_app]
        cmp rax, rdx
        lea rsi, [rel s_reloc]
        call result

        ; AllocatePool / FreePool
        mov ecx, 2                      ; EfiLoaderData
        mov edx, 200
        lea r8, [rsp + 40]
        call [rbx + BS_ALLOC_POOL]
        test rax, rax
        jnz .pool_bad
        mov rcx, [rsp + 40]
        mov byte [rcx + 199], 0x5A
        call [rbx + BS_FREE_POOL]
        test rax, rax
.pool_bad:
        lea rsi, [rel s_pool]
        call result

        ; GetMemoryMap size query: EFI_BUFFER_TOO_SMALL and a size
        mov qword [rsp + 40], 0
        lea rcx, [rsp + 40]
        xor edx, edx
        lea r8, [rsp + 32]
        lea r9, [rsp + 32]
        sub rsp, 16
        lea rax, [rsp + 48 + 16 + 8]    ; descriptor version (scratch)
        mov [rsp + 32], rax
        call [rbx + BS_GET_MAP]
        add rsp, 16
        mov rdx, 0x8000000000000005
        cmp rax, rdx
        jne .map_bad
        cmp qword [rsp + 40], 0
        je .map_bad
        cmp eax, eax
.map_bad:
        lea rsi, [rel s_map]
        call result

        lea rsi, [rel s_nl]
        call puts

%ifdef EXIT
        mov rcx, r12
        mov edx, 0x5678
        xor r8d, r8d
        xor r9d, r9d
        call [rbx + BS_EXIT]
        lea rsi, [rel s_exit_returned]
        call puts
%endif
        mov eax, 0x1234
        add rsp, 48
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; result: RSI = " name=", ZF = ok
result:
        pushf
        call puts
        popf
        lea rsi, [rel s_ok]
        je puts
        lea rsi, [rel s_bad]
; puts: RSI = NUL-terminated string to COM1
puts:
        push rax
        push rdx
.next:
        lodsb
        test al, al
        jz .done
        mov ah, al
        mov dx, COM1 + 5
.wait:
        in al, dx
        test al, 0x20
        jz .wait
        mov al, ah
        mov dx, COM1
        out dx, al
        jmp .next
.done:
        pop rdx
        pop rax
        ret

        align 8
abs_msg:        dq IMAGE_BASE + s_app
loaded_image_guid:
        dd 0x5B1B31A1
        dw 0x9562, 0x11d2
        db 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
s_app:  db 'APP x86_64', 0
s_st:   db ' st=', 0
s_bs:   db ' bs=', 0
s_li:   db ' li=', 0
s_base: db ' base=', 0
s_reloc: db ' reloc=', 0
s_pool: db ' pool=', 0
s_map:  db ' map=', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_nl:   db 13, 10, 0
s_exit_returned: db 'exit=returned', 13, 10, 0
        times 0x2000 - ($ - $$) db 0

; ------------------------------------------------------------------ .reloc
reloc:  dd 0x1000                       ; page
        dd 12                           ; block size
        dw 0xA000 | (abs_msg - $$ - 0x1000)    ; DIR64
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
