; Stand-in OS loader for the runtime services after ExitBootServices.
; Original code. Does what an OS may do once boot services are gone:
; switches to page tables and a stack of its own, then overwrites every
; boot services code and data page in the memory map with 0xCC. Only
; the runtime pages are left, so the runtime services must still work:
;   APP rt pages=ok time=ok var=ok next=ok
; pages  some boot services pages were overwritten
; time   GetTime returns a plausible year
; var    SetVariable then GetVariable round-trips (runtime access)
; next   GetNextVariableName finds the variable
; then ResetSystem(EfiResetShutdown) powers off.

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

ST_RT           equ 88
ST_BS           equ 96
BS_ALLOC_PAGES  equ 40
BS_GET_MAP      equ 56
BS_ALLOC_POOL   equ 64
BS_EXIT_BS      equ 232
RT_GET_TIME     equ 24
RT_GET_VAR      equ 72
RT_GET_NEXT     equ 80
RT_SET_VAR      equ 88
RT_RESET        equ 104

PAGES           equ 8               ; PML4, PDPT, 4 PDs, 2 stack pages
MAP_SIZE        equ 16384

%include "x86_64-pe.inc"

entry:
        sub rsp, 88
        mov r12, rcx                    ; ImageHandle
        mov r13, rdx                    ; SystemTable
        mov rbx, [r13 + ST_BS]
        mov r14, [r13 + ST_RT]
        lea rsi, [rel s_app]
        call puts

        ; Pages for our own page tables and stack, a pool for the map
        xor ecx, ecx                    ; AllocateAnyPages
        mov edx, 2                      ; EfiLoaderData
        mov r8d, PAGES
        lea r9, [rsp + 64]
        call [rbx + BS_ALLOC_PAGES]
        test rax, rax
        jnz fail
        mov r15, [rsp + 64]
        mov ecx, 2
        mov edx, MAP_SIZE
        lea r8, [rsp + 64]
        call [rbx + BS_ALLOC_POOL]
        test rax, rax
        jnz fail
        mov rdi, [rsp + 64]             ; map buffer

        ; 4 GB identity map with 2 MB pages
        push rdi
        mov rdi, r15
        mov ecx, 6 * 512
        xor eax, eax
        rep stosq
        pop rdi
        lea rax, [r15 + 0x1003]
        mov [r15], rax                  ; PML4[0] -> PDPT
        xor ecx, ecx
.pdpt:  lea rax, [r15 + 0x2003]
        mov rdx, rcx
        shl rdx, 12
        add rax, rdx
        mov [r15 + 0x1000 + rcx * 8], rax
        inc ecx
        cmp ecx, 4
        jb .pdpt
        xor ecx, ecx
.pd:    mov rax, rcx
        shl rax, 21
        or rax, 0x83                    ; present, writable, 2 MB
        mov [r15 + 0x2000 + rcx * 8], rax
        inc ecx
        cmp ecx, 2048
        jb .pd

        ; GetMemoryMap, ExitBootServices
        mov qword [rsp + 40], MAP_SIZE
        lea rcx, [rsp + 40]
        mov rdx, rdi
        lea r8, [rsp + 48]              ; key
        lea r9, [rsp + 56]              ; descriptor size
        lea rax, [rsp + 64]             ; version
        mov [rsp + 32], rax
        call [rbx + BS_GET_MAP]
        test rax, rax
        jnz fail
        lea rsi, [rel s_nl]             ; ExitBootServices prints on COM1
        call puts
        mov rcx, r12
        mov rdx, [rsp + 48]
        call [rbx + BS_EXIT_BS]
        test rax, rax
        jnz fail
        mov rbx, [rsp + 40]             ; map size
        mov rbp, [rsp + 56]             ; descriptor size

        ; From here on nothing of the boot services may be used
        cli
        lea rax, [r15]
        mov cr3, rax
        lea rsp, [r15 + PAGES * 0x1000]
        sub rsp, 64

        ; Overwrite every boot services code (3) and data (4) page
        xor r8d, r8d                    ; pages overwritten
        mov r9, rdi
        lea r10, [rdi + rbx]
.desc:  cmp r9, r10
        jae .desc_done
        mov eax, [r9]
        cmp eax, 3
        je .scrub
        cmp eax, 4
        jne .desc_next
.scrub: push rdi
        mov rdi, [r9 + 8]
        mov rcx, [r9 + 24]
        add r8, rcx
        shl rcx, 12
        mov al, 0xCC
        rep stosb
        pop rdi
.desc_next:
        add r9, rbp
        jmp .desc
.desc_done:
        lea rsi, [rel s_app]
        call puts
        lea rsi, [rel s_pages]
        test r8, r8
        setz al
        test al, al
        call result

        ; GetTime
        lea rcx, [rsp + 32]
        xor edx, edx
        call [r14 + RT_GET_TIME]
        test rax, rax
        jnz .time_bad
        movzx eax, word [rsp + 32]
        cmp eax, 1980
        jb .time_bad
        cmp eax, 2100
        jae .time_bad
        xor eax, eax
        jmp .time_done
.time_bad:
        or eax, 1
.time_done:
        lea rsi, [rel s_time]
        call result

        ; SetVariable (non-volatile, boot and runtime access), GetVariable
        lea rcx, [rel var_name]
        lea rdx, [rel var_guid]
        mov r8d, 7
        mov r9d, 4
        lea rax, [rel var_data]
        mov [rsp + 32], rax
        call [r14 + RT_SET_VAR]
        test rax, rax
        jnz .var_bad
        mov qword [rsp + 40], 4
        lea rcx, [rel var_name]
        lea rdx, [rel var_guid]
        xor r8d, r8d
        lea r9, [rsp + 40]
        lea rax, [rsp + 48]
        mov [rsp + 32], rax
        call [r14 + RT_GET_VAR]
        test rax, rax
        jnz .var_bad
        cmp dword [rsp + 48], 0x0C0FFEE5
.var_bad:
        lea rsi, [rel s_var]
        call result

        ; GetNextVariableName until it is found
        mov word [rel name_buf], 0
.next:  mov qword [rsp + 40], 64
        lea rcx, [rsp + 40]
        lea rdx, [rel name_buf]
        lea r8, [rel guid_buf]
        call [r14 + RT_GET_NEXT]
        test rax, rax
        jnz .next_bad
        mov rax, [rel guid_buf]
        cmp rax, [rel var_guid]
        jne .next
        mov rax, [rel guid_buf + 8]
        cmp rax, [rel var_guid + 8]
        jne .next
        mov rax, [rel name_buf]
        cmp rax, [rel var_name]
        jne .next
        mov eax, [rel name_buf + 8]
        cmp eax, [rel var_name + 8]
        jne .next
        mov ax, [rel name_buf + 12]
        cmp ax, [rel var_name + 12]
        jmp .next_done
.next_bad:
        or eax, 1
.next_done:
        lea rsi, [rel s_next]
        call result
        lea rsi, [rel s_nl]
        call puts

        mov ecx, 2                      ; EfiResetShutdown
        xor edx, edx
        xor r8d, r8d
        xor r9d, r9d
        call [r14 + RT_RESET]
.hang:  hlt
        jmp .hang

fail:
        lea rsi, [rel s_fail]
        call puts
.h:     hlt
        jmp .h

; result: RSI = label, ZF = ok
result:
        pushf
        call puts
        popf
        lea rsi, [rel s_ok]
        je puts
        lea rsi, [rel s_bad]
puts:
        push rax
        push rdx
.n:     lodsb
        test al, al
        jz .d
        mov ah, al
        mov dx, COM1 + 5
.w:     in al, dx
        test al, 0x20
        jz .w
        mov al, ah
        mov dx, COM1
        out dx, al
        jmp .n
.d:     pop rdx
        pop rax
        ret

        align 8
var_data:       dd 0x0C0FFEE5
var_name:       dw 'S','q','R','t','O','k',0,0
var_guid:
        dd 0x5c1a77e1
        dw 0x4b2e, 0x4d0f
        db 0x9a,0x31,0x7e,0x52,0x0d,0x84,0x6b,0x19
guid_buf:       times 16 db 0
name_buf:       times 64 db 0
s_app:  db 'APP rt', 0
s_pages: db ' pages=', 0
s_time: db ' time=', 0
s_var:  db ' var=', 0
s_next: db ' next=', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_fail: db ' =bad', 13, 10, 0
s_nl:   db 13, 10, 0
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
