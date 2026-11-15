; Stand-in EFI application for the x86_64-uefi console (roadmap C1).
; Original code. Everything it says goes through ConOut:
;   APP con: hello, <U+263A>
;   modes=ok attr=ok cursor=ok timer=ok
;   key=0000/0061 ... until it reads 'q'
; and SetAttribute(1Eh) and SetCursorPosition(5, 3) leave their ANSI
; sequences in the serial log. Keys come from the serial line.

        bits 64
IMAGE_BASE      equ 0x10000000

ST_CONIN        equ 48
ST_CONOUT       equ 64
ST_BS           equ 96
BS_CREATE_EVENT equ 80
BS_SET_TIMER    equ 88
BS_WAIT         equ 96
BS_CLOSE_EVENT  equ 112
BS_STALL        equ 248
IN_READ_KEY     equ 8
IN_WAIT_KEY     equ 16
OUT_STRING      equ 8
OUT_QUERY_MODE  equ 24
OUT_SET_ATTR    equ 40
OUT_SET_CURSOR  equ 56
OUT_MODE        equ 72
MODE_COL        equ 12
MODE_ROW        equ 16

%include "x86_64-pe.inc"

; ------------------------------------------------------------------ .text
entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        sub rsp, 64                     ; shadow space + locals at 32..63
        mov r12, rcx
        mov r13, rdx
        mov rbx, [r13 + ST_BS]
        mov rdi, [r13 + ST_CONOUT]

        lea rdx, [rel s_hello]
        call say

        ; QueryMode: 0 is 80x25, 1 does not exist
        mov rcx, rdi
        xor edx, edx
        lea r8, [rsp + 32]
        lea r9, [rsp + 40]
        call [rdi + OUT_QUERY_MODE]
        test rax, rax
        jnz .modes_bad
        cmp qword [rsp + 32], 80
        jne .modes_bad
        cmp qword [rsp + 40], 25
        jne .modes_bad
        mov rcx, rdi
        mov edx, 1
        lea r8, [rsp + 32]
        lea r9, [rsp + 40]
        call [rdi + OUT_QUERY_MODE]
        mov rdx, 0x8000000000000003     ; EFI_UNSUPPORTED
        cmp rax, rdx
.modes_bad:
        lea rdx, [rel s_modes]
        call result

        ; SetAttribute: yellow on blue, then back to light grey on black
        mov rcx, rdi
        mov edx, 0x1E
        call [rdi + OUT_SET_ATTR]
        mov rsi, rax
        mov rcx, rdi
        mov edx, 0x07
        call [rdi + OUT_SET_ATTR]
        or rax, rsi
        lea rdx, [rel s_attr]
        call result

        ; SetCursorPosition(5, 3); the mode follows it
        mov rcx, rdi
        mov edx, 5
        mov r8d, 3
        call [rdi + OUT_SET_CURSOR]
        test rax, rax
        jnz .cursor_bad
        mov rax, [rdi + OUT_MODE]
        cmp dword [rax + MODE_COL], 5
        jne .cursor_bad
        cmp dword [rax + MODE_ROW], 3
.cursor_bad:
        lea rdx, [rel s_cursor]
        call result

        ; A 10 ms timer event, waited for, then a 1 ms Stall
        mov ecx, 0x80000000             ; EVT_TIMER
        xor edx, edx
        xor r8d, r8d
        xor r9d, r9d
        lea rax, [rsp + 48]
        sub rsp, 16
        mov [rsp + 32], rax
        call [rbx + BS_CREATE_EVENT]
        add rsp, 16
        test rax, rax
        jnz .timer_bad
        mov rcx, [rsp + 48]
        mov edx, 2                      ; TimerRelative
        mov r8d, 100000                 ; 10 ms in 100 ns units
        call [rbx + BS_SET_TIMER]
        test rax, rax
        jnz .timer_bad
        mov ecx, 1
        lea rdx, [rsp + 48]
        lea r8, [rsp + 56]
        call [rbx + BS_WAIT]
        test rax, rax
        jnz .timer_bad
        cmp qword [rsp + 56], 0
        jne .timer_bad
        mov rcx, [rsp + 48]
        call [rbx + BS_CLOSE_EVENT]
        test rax, rax
        jnz .timer_bad
        mov ecx, 1000
        call [rbx + BS_STALL]
        test rax, rax
.timer_bad:
        lea rdx, [rel s_timer]
        call result
        lea rdx, [rel s_crlf]
        call say

        ; Keys until 'q': wait on ConIn's WaitForKey, then read
.key:
        mov rsi, [r13 + ST_CONIN]
        mov ecx, 1
        lea rdx, [rsi + IN_WAIT_KEY]
        lea r8, [rsp + 56]
        call [rbx + BS_WAIT]
        mov rcx, rsi
        lea rdx, [rsp + 48]
        call [rsi + IN_READ_KEY]
        test rax, rax
        jnz .key
        lea rdx, [rel s_key]
        call say
        movzx eax, word [rsp + 48]      ; scan code
        call hex4
        lea rdx, [rel s_slash]
        call say
        movzx eax, word [rsp + 50]      ; character
        call hex4
        cmp word [rsp + 50], 'q'
        jne .key
        lea rdx, [rel s_crlf]
        call say

        xor eax, eax
        add rsp, 64
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; say: RDX = CHAR16 string, through ConOut (RDI)
say:
        sub rsp, 40
        mov rcx, rdi
        call [rdi + OUT_STRING]
        add rsp, 40
        ret

; result: RDX = " name=", ZF = ok
result:
        pushf
        call say
        popf
        lea rdx, [rel s_ok]
        je say
        lea rdx, [rel s_bad]
        jmp say

; hex4: AX as four hex digits
hex4:
        push rbx
        lea rbx, [rel hexbuf]
        mov ecx, 4
.digit:
        rol ax, 4
        mov edx, eax
        and edx, 0x0F
        add edx, '0'
        cmp edx, '9'
        jbe .store
        add edx, 'a' - '9' - 1
.store:
        mov [rbx], dx
        add rbx, 2
        loop .digit
        pop rbx
        lea rdx, [rel hexbuf]
        jmp say

        align 2
s_hello:  dw __utf16__('APP con: hello, '), 0x263A, 13, 10, 0
s_modes:  dw __utf16__('modes='), 0
s_attr:   dw __utf16__(' attr='), 0
s_cursor: dw __utf16__(' cursor='), 0
s_timer:  dw __utf16__(' timer='), 0
s_ok:     dw __utf16__('ok'), 0
s_bad:    dw __utf16__('bad'), 0
s_key:    dw __utf16__(' key='), 0
s_slash:  dw __utf16__('/'), 0
s_crlf:   dw 13, 10, 0
hexbuf:   dw 0, 0, 0, 0, 0
        times 0x2000 - ($ - $$) db 0

; ------------------------------------------------------------------ .reloc
reloc:  dd 0x1000, 8                    ; one empty block: nothing to fix up
reloc_end:
        times 0x3000 - ($ - $$) db 0
