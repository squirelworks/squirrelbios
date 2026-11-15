; Stand-in EFI application for the MP services protocol and the MP table
; (roadmap C4). Original code. Reports on COM1:
;   APP mp n=<total> en=<enabled> who=0 info=<APIC ID:package.core.thread,...>
;   bsp=ok all=ok single=ok this=ok self=ok timeout=ok disable=ok mps=ok
; (with one CPU: bsp=ok aps=none notstarted=ok mps=ok)
; then returns 0.

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

ST_BS           equ 96
ST_NTABLES      equ 104
ST_TABLES       equ 112
BS_LOCATE_PROT  equ 320

MP_COUNT        equ 0
MP_INFO         equ 8
MP_ALL          equ 16
MP_THIS         equ 24
MP_ENABLE       equ 40
MP_WHO          equ 48

EFI_INVALID     equ 0x8000000000000002
EFI_TIMEOUT     equ 0x8000000000000012
EFI_NOT_STARTED equ 0x8000000000000013

%include "x86_64-pe.inc"

; call MP service SLOT with up to four register arguments after `this`
%macro mpcall 1
        mov rcx, [rel mp]
        call [rcx + %1]
%endmacro

entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        push r14
        sub rsp, 88
        mov r13, rdx                    ; SystemTable
        mov rbx, [r13 + ST_BS]
        lea rsi, [rel s_app]
        call puts

        ; LocateProtocol(MP services)
        lea rcx, [rel mp_guid]
        xor edx, edx
        lea r8, [rel mp]
        call [rbx + BS_LOCATE_PROT]
        test rax, rax
        jnz fail

        ; GetNumberOfProcessors
        lea rdx, [rel total]
        lea r8, [rel enabled]
        mpcall MP_COUNT
        test rax, rax
        jnz fail
        lea rsi, [rel s_n]
        call puts
        mov rax, [rel total]
        call putd
        lea rsi, [rel s_en]
        call puts
        mov rax, [rel enabled]
        call putd

        ; WhoAmI on the BSP
        lea rdx, [rsp + 64]
        mpcall MP_WHO
        test rax, rax
        jnz fail
        lea rsi, [rel s_who]
        call puts
        mov rax, [rsp + 64]
        call putd

        ; GetProcessorInfo for each: APIC ID:package.core.thread; the BSP
        ; flag on 0 only, every one enabled and healthy
        lea rsi, [rel s_info]
        call puts
        xor r12d, r12d
        mov byte [rel flags_ok], 1
.info:
        cmp r12, [rel total]
        jae .info_done
        test r12, r12
        jz .first
        mov al, ','
        call putc
.first:
        mov rdx, r12
        lea r8, [rel pinfo]
        mpcall MP_INFO
        test rax, rax
        jnz fail
        mov eax, [rel pinfo + 8]
        mov ecx, 6                      ; enabled, healthy
        test r12, r12
        jnz .ap_flags
        or ecx, 1                       ; BSP
.ap_flags:
        cmp eax, ecx
        je .flag_ok
        mov byte [rel flags_ok], 0
.flag_ok:
        mov rax, [rel pinfo]
        call putd
        mov al, ':'
        call putc
        mov eax, [rel pinfo + 12]
        call putd
        mov al, '.'
        call putc
        mov eax, [rel pinfo + 16]
        call putd
        mov al, '.'
        call putc
        mov eax, [rel pinfo + 20]
        call putd
        inc r12
        jmp .info
.info_done:
        lea rsi, [rel s_flags]
        cmp byte [rel flags_ok], 1
        call result

        ; StartupThisAP on the BSP is refused
        lea rdx, [rel ap_proc]
        xor r8d, r8d
        call this_ap
        mov rcx, EFI_INVALID
        cmp rax, rcx
        lea rsi, [rel s_bsp]
        call result

        ; One CPU: StartupAllAPs answers EFI_NOT_STARTED, nothing else to run
        cmp qword [rel total], 1
        jne .have_aps
        lea rdx, [rel ap_proc]
        xor r8d, r8d
        xor r9d, r9d
        mov qword [rsp + 32], 0
        mov qword [rsp + 40], 0
        mov qword [rsp + 48], 0
        mpcall MP_ALL
        mov rcx, EFI_NOT_STARTED
        cmp rax, rcx
        lea rsi, [rel s_none]
        call result
        jmp .mps
.have_aps:
        ; StartupAllAPs, all at once: each AP counts itself and marks its
        ; own processor number (from WhoAmI)
        xor r8d, r8d
        call all_aps
        lea rsi, [rel s_all]
        call result

        ; ... and one after another
        mov r8d, 1
        call all_aps
        lea rsi, [rel s_single]
        call result

        ; StartupThisAP on the last processor
        mov dword [rel counter], 0
        mov rdx, [rel total]
        dec rdx
        lea r8, [rel ap_proc]
        xchg rdx, r8
        call this_ap
        test rax, rax
        jnz .this_bad
        cmp dword [rel counter], 1
        jne .this_bad
        mov rax, [rel total]
        dec rax
        lea rcx, [rel marks]
        cmp byte [rcx + rax], 1
.this_bad:
        lea rsi, [rel s_this]
        call result

        ; An AP asked who it is answers its own number, not the BSP's
        mov qword [rel who_ap], 0
        lea rdx, [rel who_proc]
        mov r8d, 1
        call this_ap
        test rax, rax
        jnz .self_bad
        cmp qword [rel who_ap], 1
.self_bad:
        lea rsi, [rel s_self]
        call result

        ; A procedure that never returns times out; the AP is restarted
        ; and runs the next one
        lea rdx, [rel spin_proc]
        mov r8d, 1
        mov qword [rsp + 32], 2000      ; 2 ms
        call this_ap_t
        mov rcx, EFI_TIMEOUT
        cmp rax, rcx
        jne .to_bad
        mov dword [rel counter], 0
        lea rdx, [rel ap_proc]
        mov r8d, 1
        call this_ap
        test rax, rax
        jnz .to_bad
        cmp dword [rel counter], 1
.to_bad:
        lea rsi, [rel s_timeout]
        call result

        ; EnableDisableAP(1, off): one fewer enabled and one fewer runs
        mov edx, 1
        xor r8d, r8d
        xor r9d, r9d
        mpcall MP_ENABLE
        test rax, rax
        jnz .dis_bad
        lea rdx, [rsp + 64]
        lea r8, [rsp + 72]
        mpcall MP_COUNT
        mov rax, [rel total]
        dec rax
        cmp [rsp + 72], rax
        jne .dis_bad
        mov dword [rel counter], 0
        lea rdx, [rel ap_proc]
        xor r8d, r8d
        xor r9d, r9d
        lea rax, [rel counter]
        mov qword [rsp + 32], 0
        mov [rsp + 40], rax
        mov qword [rsp + 48], 0
        mpcall MP_ALL
        mov rcx, [rel total]
        sub rcx, 2
        cmp [rel counter], ecx
        jne .dis_bad
        mov edx, 1
        mov r8d, 1
        xor r9d, r9d
        mpcall MP_ENABLE
        test rax, rax
.dis_bad:
        lea rsi, [rel s_disable]
        call result

        ; The MPS configuration table: floating pointer and PCMP header
        ; with good checksums, one processor entry per CPU
.mps:
        mov rcx, [r13 + ST_NTABLES]
        mov rdi, [r13 + ST_TABLES]
.find:
        test rcx, rcx
        jz .mps_bad
        mov rax, [rdi]
        cmp rax, [rel mps_guid]
        jne .find_next
        mov rax, [rdi + 8]
        cmp rax, [rel mps_guid + 8]
        je .found
.find_next:
        add rdi, 24
        dec rcx
        jmp .find
.found:
        mov rdi, [rdi + 16]             ; floating pointer
        cmp dword [rdi], '_MP_'
        jne .mps_bad
        mov ecx, 16
        call sum
        jnz .mps_bad
        mov edi, [rdi + 4]              ; PCMP header
        cmp dword [rdi], 'PCMP'
        jne .mps_bad
        movzx ecx, word [rdi + 4]
        call sum
        jnz .mps_bad
        movzx ecx, word [rdi + 34]      ; entries
        lea rsi, [rdi + 44]
        xor edx, edx                    ; processors seen
.entry:
        test ecx, ecx
        jz .entries_done
        cmp byte [rsi], 0
        jne .not_cpu
        inc edx
        add rsi, 20
        dec ecx
        jmp .entry
.not_cpu:
        add rsi, 8
        dec ecx
        jmp .entry
.entries_done:
        cmp rdx, [rel total]
        jmp .mps_done
.mps_bad:
        or eax, 1
.mps_done:
        lea rsi, [rel s_mps]
        call result

        lea rsi, [rel s_nl]
        call puts
        xor eax, eax
        jmp done
fail:
        lea rsi, [rel s_fail]
        call puts
        mov eax, 1
done:
        add rsp, 88
        pop r14
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; all_aps: R8 = SingleThread. Runs ap_proc on every AP with the marks
; cleared; ZF set when the call succeeded, every AP ran once and marked
; its own number, and the BSP did not run it.
all_aps:
        sub rsp, 56
        mov dword [rel counter], 0
        lea rdi, [rel marks]
        mov ecx, 64
        xor eax, eax
        rep stosb
        lea rdx, [rel ap_proc]
        xor r9d, r9d
        lea rax, [rel counter]
        mov qword [rsp + 32], 0
        mov [rsp + 40], rax
        lea rax, [rel failed]
        mov [rsp + 48], rax
        mpcall MP_ALL
        add rsp, 56
        test rax, rax
        jnz .bad
        mov rcx, [rel total]
        dec rcx
        cmp [rel counter], ecx
        jne .bad
        lea rdi, [rel marks]
        cmp byte [rdi], 0               ; not the BSP
        jne .bad
        mov ecx, 1
.mark:
        cmp rcx, [rel total]
        jae .ok
        cmp byte [rdi + rcx], 1
        jne .bad
        inc ecx
        jmp .mark
.ok:
        xor eax, eax
        ret
.bad:
        or eax, 1
        ret

; this_ap: RDX = procedure, R8 = processor number, no timeout;
; this_ap_t: the same with the timeout at [rsp + 32] of the caller's frame
this_ap:
        mov qword [rsp + 8 + 32], 0
this_ap_t:
        mov rax, [rsp + 8 + 32]
        sub rsp, 56
        mov [rsp + 32], rax
        lea rax, [rel counter]
        mov [rsp + 40], rax
        mov qword [rsp + 48], 0
        xor r9d, r9d
        mpcall MP_THIS
        add rsp, 56
        ret

; The AP procedures (Microsoft ABI, RCX = argument)
ap_proc:
        sub rsp, 40
        lock inc dword [rel counter]
        lea rdx, [rsp + 32]
        mpcall MP_WHO
        mov rax, [rsp + 32]
        lea rdx, [rel marks]
        and eax, 63
        mov byte [rdx + rax], 1
        add rsp, 40
        ret

who_proc:
        sub rsp, 40
        lea rdx, [rel who_ap]
        mpcall MP_WHO
        add rsp, 40
        ret

spin_proc:
        pause
        jmp spin_proc

; sum: RDI = bytes, ECX = count; ZF set when they add up to 0
sum:
        push rcx
        push rdi
        xor eax, eax
.add:   add al, [rdi]
        inc rdi
        dec ecx
        jnz .add
        pop rdi
        pop rcx
        test al, al
        ret

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
.next:
        lodsb
        test al, al
        jz .done
        call putc
        jmp .next
.done:
        pop rax
        ret

putc:
        push rdx
        push rax
        mov dx, COM1 + 5
.wait:
        in al, dx
        test al, 0x20
        jz .wait
        pop rax
        mov dx, COM1
        out dx, al
        pop rdx
        ret

; putd: RAX in decimal
putd:
        push rbx
        push rcx
        push rdx
        mov rbx, 10
        xor ecx, ecx
.div:
        xor edx, edx
        div rbx
        push rdx
        inc ecx
        test rax, rax
        jnz .div
.out:
        pop rax
        add al, '0'
        call putc
        loop .out
        pop rdx
        pop rcx
        pop rbx
        ret

        align 8
mp:             dq 0
total:          dq 0
enabled:        dq 0
failed:         dq 0
who_ap:         dq 0
counter:        dd 0
flags_ok:       db 0
        align 8
pinfo:          times 32 db 0
marks:          times 64 db 0
mp_guid:
        dd 0x3fdda605
        dw 0xa76e, 0x4f46
        db 0xad,0x29,0x12,0xf4,0x53,0x1b,0x3d,0x08
mps_guid:
        dd 0xeb9d2d2f
        dw 0x2d88, 0x11d3
        db 0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d
s_app:     db 'APP mp', 0
s_n:       db ' n=', 0
s_en:      db ' en=', 0
s_who:     db ' who=', 0
s_info:    db ' info=', 0
s_flags:   db ' flags=', 0
s_bsp:     db ' bsp=', 0
s_all:     db ' all=', 0
s_single:  db ' single=', 0
s_this:    db ' this=', 0
s_self:    db ' self=', 0
s_timeout: db ' timeout=', 0
s_disable: db ' disable=', 0
s_mps:     db ' mps=', 0
s_none:    db ' aps=none notstarted=', 0
s_fail:    db ' =bad', 13, 10, 0
s_ok:      db 'ok', 0
s_bad:     db 'bad', 0
s_nl:      db 13, 10, 0
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
