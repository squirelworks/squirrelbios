; Stand-in EFI application for non-volatile variables in flash (C5).
; Original code. Reports on COM1:
;   APP nv before=N|none after=N big=ok|none|bad vol=none|bad churn=ok|bad
; before  the boot counter SqBoots (non-volatile) as found, then set to +1
; big     an 8000-byte variable as found (set when missing): its bytes are
;         i * 7 mod 256
; vol     a volatile variable must not survive a restart (set every run)
; churn   200 writes of a 1000-byte variable, enough to fill a bank of
;         the store and copy it into the other; the last value reads back
; then returns 0.

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

ST_RT           equ 88
ST_BS           equ 96
BS_ALLOC_POOL   equ 64
RT_GET_VAR      equ 72
RT_SET_VAR      equ 88
BIG             equ 8000
CHURN           equ 1000

%include "x86_64-pe.inc"

entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        push r14
        push r15
        sub rsp, 64
        mov r14, [rdx + ST_RT]
        mov rbx, [rdx + ST_BS]
        mov ecx, 4                      ; EfiBootServicesData: a buffer
        mov edx, BIG
        lea r8, [rsp + 40]
        call [rbx + BS_ALLOC_POOL]
        mov r15, [rsp + 40]
        lea rsi, [rel s_app]
        call puts

        ; SqBoots
        lea rsi, [rel s_before]
        call puts
        lea rcx, [rel n_boots]
        mov edx, 4
        lea r8, [rel counter]
        call getvar
        test rax, rax
        jz .have
        lea rsi, [rel s_none]
        call puts
        mov dword [rel counter], 0
        jmp .inc
.have:  mov eax, [rel counter]
        call putd
.inc:   inc dword [rel counter]
        lea rcx, [rel n_boots]
        mov edx, 4
        lea r8, [rel counter]
        mov r9d, 7
        call setvar
        lea rsi, [rel s_after]
        call puts
        test rax, rax
        jnz .after_bad
        mov eax, [rel counter]
        call putd
        jmp .big
.after_bad:
        lea rsi, [rel s_bad]
        call puts

        ; SqBig
.big:   lea rsi, [rel s_big]
        call puts
        lea rcx, [rel n_big]
        mov edx, BIG
        mov r8, r15
        call getvar
        test rax, rax
        jz .big_check
        lea rsi, [rel s_none]
        call puts
        mov rdi, r15              ; fill and set
        xor ecx, ecx
.fill:  imul eax, ecx, 7
        mov [rdi + rcx], al
        inc ecx
        cmp ecx, BIG
        jb .fill
        lea rcx, [rel n_big]
        mov edx, BIG
        mov r8, r15
        mov r9d, 7
        call setvar
        test rax, rax
        jnz .big_bad
        jmp .vol
.big_check:
        cmp qword [rel got], BIG
        jne .big_bad
        mov rdi, r15
        xor ecx, ecx
.chk:   imul eax, ecx, 7
        cmp [rdi + rcx], al
        jne .big_bad
        inc ecx
        cmp ecx, BIG
        jb .chk
        lea rsi, [rel s_ok]
        call puts
        jmp .vol
.big_bad:
        lea rsi, [rel s_bad]
        call puts

        ; SqVol: absent at start, set (volatile) every run
.vol:   lea rsi, [rel s_vol]
        call puts
        lea rcx, [rel n_vol]
        mov edx, 4
        lea r8, [rel scratch]
        call getvar
        lea rsi, [rel s_none]
        test rax, rax
        jnz .vol_put
        lea rsi, [rel s_bad]
.vol_put:
        call puts
        lea rcx, [rel n_vol]
        mov edx, 4
        lea r8, [rel counter]
        mov r9d, 6
        call setvar

        ; SqChurn: 200 writes, byte value = the write's number
        lea rsi, [rel s_churn]
        call puts
        xor r12d, r12d
.churn: mov rdi, r15
        mov eax, r12d
        mov ecx, CHURN
        rep stosb
        lea rcx, [rel n_churn]
        mov edx, CHURN
        mov r8, r15
        mov r9d, 7
        call setvar
        test rax, rax
        jnz .churn_bad
        inc r12d
        cmp r12d, 200
        jb .churn
        mov rdi, r15
        xor eax, eax
        mov ecx, CHURN
        rep stosb
        lea rcx, [rel n_churn]
        mov edx, CHURN
        mov r8, r15
        call getvar
        test rax, rax
        jnz .churn_bad
        cmp byte [r15], 199
        jne .churn_bad
        cmp byte [r15 + CHURN - 1], 199
        jne .churn_bad
        lea rsi, [rel s_ok]
        call puts
        jmp .end
.churn_bad:
        lea rsi, [rel s_bad]
        call puts
.end:   lea rsi, [rel s_nl]
        call puts
        xor eax, eax
        add rsp, 64
        pop r15
        pop r14
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; getvar: RCX = name, EDX = buffer size, R8 = buffer; RAX = status,
; the size read in [got]
getvar:
        push rbp
        mov rbp, rsp
        sub rsp, 48
        and rsp, -16
        mov [rel got], rdx
        mov [rsp + 32], r8
        lea rdx, [rel guid]
        xor r8d, r8d
        lea r9, [rel got]
        call [r14 + RT_GET_VAR]
        leave
        ret

; setvar: RCX = name, EDX = size, R8 = data, R9D = attributes; RAX = status
setvar:
        push rbp
        mov rbp, rsp
        sub rsp, 48
        and rsp, -16
        mov [rsp + 32], r8
        mov r8d, r9d
        mov r9d, edx
        lea rdx, [rel guid]
        call [r14 + RT_SET_VAR]
        leave
        ret

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

; putd: EAX in decimal
putd:
        push rax
        push rbx
        push rcx
        push rdx
        push rsi
        mov ebx, 10
        xor ecx, ecx
.div:   xor edx, edx
        div ebx
        push rdx
        inc ecx
        test eax, eax
        jnz .div
.out:   pop rax
        add al, '0'
        mov [rel digit], al
        lea rsi, [rel digit]
        call puts
        loop .out
        pop rsi
        pop rdx
        pop rcx
        pop rbx
        pop rax
        ret

        align 8
guid:
        dd 0x7d3e1a52
        dw 0x6c0b, 0x4f8e
        db 0x91, 0x2a, 0x55, 0x03, 0xc4, 0x7e, 0x18, 0xb6
counter: dd 0
scratch: dd 0
got:    dq 0
n_boots: dw 'S','q','B','o','o','t','s',0
n_big:  dw 'S','q','B','i','g',0
n_vol:  dw 'S','q','V','o','l',0
n_churn: dw 'S','q','C','h','u','r','n',0
s_app:  db 'APP nv', 0
s_before: db ' before=', 0
s_after: db ' after=', 0
s_big:  db ' big=', 0
s_vol:  db ' vol=', 0
s_churn: db ' churn=', 0
s_none: db 'none', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_nl:   db 13, 10, 0
digit:  db 0, 0
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
