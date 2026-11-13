; Stand-in EFI application for measured boot (roadmap C5). Original code.
; Uses EFI_TCG2_PROTOCOL and reports on COM1:
;   APP tpm cap=ok log=ok extend=ok pcr8=ok
; cap     GetCapability: a TPM is present and the SHA-256 bank active
; log     GetEventLog (TCG2 format) starts with the Spec ID Event03 header
; extend  HashLogExtendEvent of "squirrel" into PCR 8
; pcr8    SubmitCommand(TPM2_PCR_Read) gives SHA-256(0^32 | SHA-256("squirrel"))
; then, for tests/tcglog.py to replay:
;   TPMLOG <offset of the last event> <the log in hex, up to it and past>
;   TPMPCR <the TPM2_PCR_Read response for SHA-256 PCRs 0-7 in hex>
; then returns 0.

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

ST_BS           equ 96
BS_LOCATE_PROT  equ 320

TCG2_CAP        equ 0
TCG2_LOG        equ 8
TCG2_EXTEND     equ 16
TCG2_SUBMIT     equ 24

%include "x86_64-pe.inc"

entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        sub rsp, 80
        mov rbx, [rdx + ST_BS]
        lea rsi, [rel s_app]
        call puts

        lea rcx, [rel tcg2_guid]
        xor edx, edx
        lea r8, [rel tcg2]
        call [rbx + BS_LOCATE_PROT]
        test rax, rax
        jnz fail
        mov r12, [rel tcg2]

        ; GetCapability
        mov byte [rel cap], 30
        mov rcx, r12
        lea rdx, [rel cap]
        call [r12 + TCG2_CAP]
        test rax, rax
        jnz .cap_bad
        cmp byte [rel cap + 13], 1
        jne .cap_bad
        test byte [rel cap + 26], 2
        jz .cap_no
        cmp al, al
        jmp .cap_bad
.cap_no:
        or eax, 1                       ; ZF clear: bad
.cap_bad:
        lea rsi, [rel s_cap]
        call result

        ; GetEventLog(EFI_TCG2_EVENT_LOG_FORMAT_TCG_2)
        call getlog
        test rax, rax
        jnz .log_bad
        mov rsi, [rel where]
        test rsi, rsi
        jnz .log_have
        or eax, 1
        jmp .log_bad
.log_have:
        lea rdi, [rel spec_id]
        add rsi, 32
        mov ecx, 16
        repe cmpsb
.log_bad:
        lea rsi, [rel s_log]
        call result

        ; HashLogExtendEvent(PCR 8, EV_IPL, "squirrel")
        mov rcx, r12
        xor edx, edx
        lea r8, [rel squirrel]
        mov r9d, 8
        lea rax, [rel event]
        mov [rsp + 32], rax
        call [r12 + TCG2_EXTEND]
        test rax, rax
        lea rsi, [rel s_extend]
        call result

        ; SubmitCommand(TPM2_PCR_Read, SHA-256 PCR 8)
        lea rdx, [rel read8]
        call submit
        jnz .pcr_bad
        lea rsi, [rel resp + 30]
        lea rdi, [rel expect8]
        mov ecx, 32
        repe cmpsb
.pcr_bad:
        lea rsi, [rel s_pcr8]
        call result
        lea rsi, [rel s_nl]
        call puts

        ; The log and PCRs 0-7 for tcglog.py
        call getlog
        lea rsi, [rel s_tpmlog]
        call puts
        mov rsi, [rel where]
        mov rax, [rel last]
        sub rax, rsi
        mov r13, rax
        call putd
        mov al, ' '
        call putc
        add r13, 1024                   ; the last event fits in this
.dump:  lodsb
        call puthex
        dec r13
        jnz .dump
        lea rsi, [rel s_nl]
        call puts

        lea rdx, [rel read07]
        call submit
        lea rsi, [rel s_tpmpcr]
        call puts
        lea rsi, [rel resp]
        mov r13d, 10 + 4 + 10 + 4 + 8 * 34
.dump2: lodsb
        call puthex
        dec r13d
        jnz .dump2
        lea rsi, [rel s_nl]
        call puts

        xor eax, eax
        jmp out
fail:
        lea rsi, [rel s_fail]
        call puts
        mov eax, 1
out:    add rsp, 80
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; getlog: GetEventLog into [where], [last], [trunc]; RAX = status
getlog:
        sub rsp, 56
        mov rcx, r12
        mov edx, 2
        lea r8, [rel where]
        lea r9, [rel last]
        lea rax, [rel trunc]
        mov [rsp + 32], rax
        call [r12 + TCG2_LOG]
        add rsp, 56
        ret

; submit: RDX = a 20-byte command; the response in resp; ZF = success
submit:
        sub rsp, 56
        mov r8, rdx
        mov rcx, r12
        mov edx, 20
        mov r9d, 1024
        lea rax, [rel resp]
        mov [rsp + 32], rax
        call [r12 + TCG2_SUBMIT]
        add rsp, 56
        test rax, rax
        jnz .r
        cmp dword [rel resp + 6], 0     ; TPM_RC_SUCCESS
.r:     ret

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
.n:     lodsb
        test al, al
        jz .d
        call putc
        jmp .n
.d:     pop rax
        ret

putc:
        push rdx
        push rax
        mov dx, COM1 + 5
.w:     in al, dx
        test al, 0x20
        jz .w
        pop rax
        mov dx, COM1
        out dx, al
        pop rdx
        ret

; puthex: AL as two hex digits
puthex:
        push rax
        shr al, 4
        call .digit
        pop rax
        push rax
        and al, 15
        call .digit
        pop rax
        ret
.digit: add al, '0'
        cmp al, '9'
        jbe putc
        add al, 'a' - '9' - 1
        jmp putc

; putd: RAX in decimal
putd:
        push rbx
        push rcx
        push rdx
        mov ebx, 10
        xor ecx, ecx
.div:   xor edx, edx
        div rbx
        push rdx
        inc ecx
        test rax, rax
        jnz .div
.out:   pop rax
        add al, '0'
        call putc
        loop .out
        pop rdx
        pop rcx
        pop rbx
        ret

        align 8
tcg2_guid:
        dd 0x607f766c
        dw 0x7455, 0x42be
        db 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f
tcg2:   dq 0
where:  dq 0
last:   dq 0
trunc:  dq 0
cap:    times 32 db 0
; EFI_TCG2_EVENT: Size, HeaderSize, HeaderVersion, PCRIndex, EventType, data
event:  dd 24
        dd 14
        dw 1
        dd 8
        dd 0x0D                         ; EV_IPL
        db 'SqTest'
squirrel: db 'squirrel'
spec_id: db 'Spec ID Event03', 0
; TPM2_PCR_Read, one TPMS_PCR_SELECTION: SHA-256, 3 bytes of PCR bits
read8:  db 0x80, 0x01, 0, 0, 0, 20, 0, 0, 0x01, 0x7E, 0, 0, 0, 1, 0, 0x0B, 3, 0x00, 0x01, 0x00
read07: db 0x80, 0x01, 0, 0, 0, 20, 0, 0, 0x01, 0x7E, 0, 0, 0, 1, 0, 0x0B, 3, 0xFF, 0x00, 0x00
expect8:
        db 0xd3, 0x62, 0xf3, 0x77, 0x0f, 0x2a, 0xcd, 0xc1, 0x4f, 0x5a, 0x36, 0x5b, 0x3c, 0xe0, 0xab, 0x80
        db 0x77, 0x6b, 0x9a, 0x66, 0x0b, 0xa3, 0x5f, 0x6d, 0xde, 0xc6, 0xbf, 0x01, 0x6f, 0x83, 0x53, 0x84
s_app:  db 'APP tpm', 0
s_cap:  db ' cap=', 0
s_log:  db ' log=', 0
s_extend: db ' extend=', 0
s_pcr8: db ' pcr8=', 0
s_tpmlog: db 'TPMLOG ', 0
s_tpmpcr: db 'TPMPCR ', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_fail: db ' =bad', 13, 10, 0
s_nl:   db 13, 10, 0
resp:   times 1024 db 0
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
