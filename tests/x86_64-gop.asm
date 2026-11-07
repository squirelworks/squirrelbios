; Stand-in EFI application for GOP, runtime and ExitBootServices (C1 step 6).
; Original code. Reports on COM1:
;   APP gop mode=ok fb=ok blt=ok time=ok var=ok ebs=ok
; then returns 0.

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

ST_RT           equ 88
ST_BS           equ 96
BS_GET_MAP      equ 56
BS_ALLOC_POOL   equ 64
BS_FREE_POOL    equ 72
BS_EXIT_BS      equ 232
BS_LOCATE_PROT  equ 320
RT_GET_TIME     equ 24
RT_GET_VAR      equ 72
RT_SET_VAR      equ 88
RT_RESET        equ 104
GOP_SET         equ 8
GOP_BLT         equ 16
GOP_MODE        equ 24
MODE_FB_BASE    equ 24
MODE_INFO       equ 8
INFO_W          equ 4
INFO_H          equ 8

%include "x86_64-pe.inc"

entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        push r14
        sub rsp, 80
        mov r12, rcx                    ; ImageHandle
        mov r13, rdx                    ; SystemTable
        mov rbx, [r13 + ST_BS]
        mov r14, [r13 + ST_RT]
        lea rsi, [rel s_app]
        call puts

        ; LocateProtocol(GOP)
        xor edi, edi
        lea rcx, [rel gop_guid]
        xor edx, edx
        lea r8, [rsp + 32]
        call [rbx + BS_LOCATE_PROT]
        test rax, rax
        jnz .mode_bad
        mov rdi, [rsp + 32]
        mov rcx, rdi
        xor edx, edx
        call [rdi + GOP_SET]
        test rax, rax
        jnz .mode_bad
        mov rax, [rdi + GOP_MODE]
        mov rcx, [rax + MODE_INFO]
        cmp dword [rcx + INFO_W], 800
        jne .mode_bad
        cmp dword [rcx + INFO_H], 600
.mode_bad:
        lea rsi, [rel s_mode]
        call result

        test rdi, rdi
        jz .fb_skip
        mov rax, [rdi + GOP_MODE]
        mov rax, [rax + MODE_FB_BASE]
        test rax, rax
        jnz .fb_ok
.fb_skip:
        or eax, 1
        jmp .fb_done
.fb_ok: xor eax, eax
.fb_done:
        test eax, eax
        lea rsi, [rel s_fb]
        call result

        ; Blt fill 50x50 red at (10,10)
        test rdi, rdi
        jz .blt_bad
        mov rcx, rdi
        lea rdx, [rel red_pix]
        xor r8d, r8d                    ; EfiBltVideoFill
        xor r9d, r9d                    ; srcx
        mov qword [rsp + 32], 0         ; srcy
        mov qword [rsp + 40], 10        ; dstx
        mov qword [rsp + 48], 10        ; dsty
        mov qword [rsp + 56], 50        ; width
        mov qword [rsp + 64], 50        ; height
        mov qword [rsp + 72], 0         ; delta
        call [rdi + GOP_BLT]
        test rax, rax
        jmp .blt_done
.blt_bad:
        or eax, 1
.blt_done:
        lea rsi, [rel s_blt]
        call result

        ; GetTime succeeds and returns a plausible year
        lea rcx, [rsp + 32]
        xor edx, edx
        call [r14 + RT_GET_TIME]
        test rax, rax
        jnz .time_bad
        movzx eax, word [rsp + 32]
        cmp eax, 1980
        jb .time_bad
        cmp eax, 2100
        jb .time_ok
.time_bad:
        or eax, 1
        jmp .time_done
.time_ok:
        xor eax, eax
.time_done:
        test eax, eax
        lea rsi, [rel s_time]
        call result

        ; SetVariable / GetVariable
        lea rcx, [rel var_name]
        lea rdx, [rel var_guid]
        mov r8d, 0x00000002
        mov r9, 4
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
        cmp dword [rsp + 48], 0x44553322
.var_bad:
        lea rsi, [rel s_var]
        call result

        ; Allocate a map buffer, GetMemoryMap, ExitBootServices
        mov ecx, 2                      ; EfiLoaderData
        mov edx, 8192
        lea r8, [rsp + 32]
        call [rbx + BS_ALLOC_POOL]
        test rax, rax
        jnz .ebs_bad
        mov rdi, [rsp + 32]             ; map buffer
        mov qword [rsp + 40], 8192
        lea rcx, [rsp + 40]
        mov rdx, rdi
        lea r8, [rsp + 48]              ; key
        lea r9, [rsp + 56]              ; desc size
        sub rsp, 16
        lea rax, [rsp + 64 + 16]        ; desc version
        mov [rsp + 32], rax
        call [rbx + BS_GET_MAP]
        add rsp, 16
        test rax, rax
        jnz .ebs_bad
        ; End the status line before ExitBootServices prints on COM1.
        lea rsi, [rel s_nl]
        call puts
        mov rcx, r12
        mov rdx, [rsp + 48]
        call [rbx + BS_EXIT_BS]
        test rax, rax
        jmp .ebs_done
.ebs_bad:
        or eax, 1                       ; force NZ (bad)
.ebs_done:
        lea rsi, [rel s_ebs]
        call result
        lea rsi, [rel s_nl]
        call puts
        ; Do not return into the firmware after ExitBootServices: shut down.
        and rsp, -16
        sub rsp, 32                     ; MS ABI shadow space
        mov ecx, 2                      ; EfiResetShutdown
        xor edx, edx
        xor r8d, r8d
        xor r9d, r9d
        call [r14 + RT_RESET]
.hang:  hlt
        jmp .hang

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
red_pix:        db 0, 0, 0xFF, 0
var_data:       dd 0x44553322
var_name:       dw 'S','q','T','e','s','t',0
var_guid:
        dd 0x12345678
        dw 0x9ABC, 0xDEF0
        db 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88
gop_guid:
        dd 0x9042a9de
        dw 0x23dc, 0x4a38
        db 0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a
s_app:  db 'APP gop', 0
s_mode: db ' mode=', 0
s_fb:   db ' fb=', 0
s_blt:  db ' blt=', 0
s_time: db ' time=', 0
s_var:  db ' var=', 0
s_ebs:  db 'APP ebs=', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_nl:   db 13, 10, 0
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
