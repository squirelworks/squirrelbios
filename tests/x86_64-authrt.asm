; Stand-in OS for an authenticated variable write at run time (C5).
; Original code. Exits the boot services and overwrites their pages as
; x86_64-rt.asm does, then appends to db with SetVariable the way an OS
; tool would: auth-db-append.auth (tests/mksigned.py: a SHA-256 entry,
; signed by the KEK) is built in. Reports on COM1:
;   APP authrt pages=ok bad=ok append=ok grown=ok
; pages   some boot services pages were overwritten
; bad     a copy with one byte of the data changed: EFI_SECURITY_VIOLATION
; append  the copy as signed: EFI_SUCCESS
; grown   db is larger afterwards
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
RT_GET_VAR      equ 72
RT_SET_VAR      equ 88
RT_RESET        equ 104

PAGES           equ 8               ; PML4, PDPT, 4 PDs, 2 stack pages
MAP_SIZE        equ 16384
VIOLATION       equ 0x800000000000001A

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
        call db_size
        mov r13, rax

        ; One byte of the data changed: the signature no longer covers it
        xor byte [rel auth_end - 1], 0x01
        call append
        xor byte [rel auth_end - 1], 0x01
        mov rcx, VIOLATION
        cmp rax, rcx
        lea rsi, [rel s_bad_]
        call result

        call append
        test rax, rax
        lea rsi, [rel s_append]
        call result

        call db_size
        cmp rax, r13
        seta al
        cmp al, 1
        lea rsi, [rel s_grown]
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

; append: SetVariable(db, append) with the built-in authenticated data
append:
        sub rsp, 56
        lea rcx, [rel n_db]
        lea rdx, [rel image_security]
        mov r8d, 0x67                   ; NV, BS, RT, time-auth, append
        mov r9d, auth_end - auth
        lea rax, [rel auth]
        mov [rsp + 32], rax
        call [r14 + RT_SET_VAR]
        add rsp, 56
        ret

; db_size: RAX = the size of db (0 when absent)
db_size:
        sub rsp, 56
        mov qword [rsp + 40], 0
        lea rcx, [rel n_db]
        lea rdx, [rel image_security]
        xor r8d, r8d
        lea r9, [rsp + 40]
        mov qword [rsp + 32], 0
        call [r14 + RT_GET_VAR]
        mov rax, [rsp + 40]
        add rsp, 56
        ret

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
image_security:
        dd 0xd719b2cb
        dw 0x3d3a, 0x4596
        db 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f
n_db:   dw 'd','b',0
s_app:  db 'APP authrt', 0
s_pages: db ' pages=', 0
s_bad_: db ' bad=', 0
s_append: db ' append=', 0
s_grown: db ' grown=', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_fail: db ' =bad', 13, 10, 0
s_nl:   db 13, 10, 0
        align 8
auth:   incbin "auth-db-append.auth"
auth_end:
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
