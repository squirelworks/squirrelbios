; Stand-in EFI application for the ACPI and SMBIOS tables (roadmap C2):
; an acpidump-style self-check. Original code. Reports on COM1:
;   APP acpi rsdp=ok xsdt=ok sums=ok facp=ok dsdt=ok facs=ok madt=ok cpus=N
;   APP acpi hpet=ok mcfg=ok|none
;   APP smbios sm3=ok walk=ok types=ok
; then returns 0.
;
; rsdp  ACPI 2.0 configuration table: "RSD PTR ", both checksums, revision 2
; xsdt  signature and checksum
; sums  every table the XSDT lists has a good checksum
; facp  FADT present, at least 244 bytes; dsdt  X_DSDT signature, checksum,
;       AML starts with a Scope; facs  FIRMWARE_CTRL points at a 64-byte FACS
; madt  MADT present; cpus  enabled local APIC entries
; hpet  present; mcfg  present ("none" when the machine has no ECAM)
; sm3   SMBIOS 3 configuration table: "_SM3_", checksum, length 18h
; walk  the structure table walks to a type 127 inside its stated size
; types types 0, 1, 4, 16, 17, 19 and 32 are present

        bits 64
IMAGE_BASE      equ 0x10000000
COM1            equ 0x3F8

ST_COUNT        equ 104
ST_CONFIG       equ 112

%include "x86_64-pe.inc"

entry:
        push rbx
        push rsi
        push rdi
        push r12
        push r13
        push r14
        push r15
        mov r13, rdx                    ; SystemTable
        lea rsi, [rel s_app]
        call puts

        ; RSDP
        lea rdi, [rel acpi20_guid]
        call find_cfg
        mov r12, rax                    ; RSDP or 0
        test rax, rax
        jz .rsdp_bad
        mov rsi, r12
        lea rdi, [rel sig_rsdp]
        mov ecx, 8
        repe cmpsb
        jne .rsdp_bad
        mov rsi, r12
        mov ecx, 20
        call sum8
        jnz .rsdp_bad
        cmp byte [r12 + 15], 2
        jb .rsdp_bad
        mov rsi, r12
        mov ecx, 36
        call sum8
        jmp .rsdp_done
.rsdp_bad:
        xor r12d, r12d
        or eax, 1                       ; ZF=0
.rsdp_done:
        lea rsi, [rel s_rsdp]
        call result

        ; XSDT
        xor r14d, r14d
        test r12, r12
        jz .xsdt_bad
        mov r14, [r12 + 24]
        cmp dword [r14], 'XSDT'
        jne .xsdt_bad
        mov rsi, r14
        mov ecx, [r14 + 4]
        call sum8
        jz .xsdt_done
.xsdt_bad:
        xor r14d, r14d
        or eax, 1
.xsdt_done:
        lea rsi, [rel s_xsdt]
        call result

        ; Walk the XSDT: every checksum, and remember the tables we know
        xor ebx, ebx                    ; bad checksums
        test r14, r14
        jz .walk_done
        mov ecx, [r14 + 4]
        sub ecx, 36
        shr ecx, 3
        lea r15, [r14 + 36]
.walk:
        test ecx, ecx
        jz .walk_done
        push rcx
        mov rdx, [r15]
        mov rsi, rdx
        mov ecx, [rdx + 4]
        call sum8
        jz .sum_ok
        inc ebx
.sum_ok:
        mov eax, [rdx]
        cmp eax, 'FACP'
        jne .n1
        mov [rel t_facp], rdx
.n1:    cmp eax, 'APIC'
        jne .n2
        mov [rel t_apic], rdx
.n2:    cmp eax, 'HPET'
        jne .n3
        mov [rel t_hpet], rdx
.n3:    cmp eax, 'MCFG'
        jne .n4
        mov [rel t_mcfg], rdx
.n4:    pop rcx
        add r15, 8
        dec ecx
        jmp .walk
.walk_done:
        test r14, r14
        jz .sums_bad
        test ebx, ebx
        jmp .sums_done
.sums_bad:
        or eax, 1
.sums_done:
        lea rsi, [rel s_sums]
        call result

        ; FADT, DSDT, FACS
        mov rdx, [rel t_facp]
        test rdx, rdx
        jz .facp_bad
        cmp dword [rdx + 4], 244
        jb .facp_bad
        xor eax, eax
        jmp .facp_done
.facp_bad:
        xor edx, edx
        or eax, 1
.facp_done:
        mov r15, rdx
        lea rsi, [rel s_facp]
        call result

        test r15, r15
        jz .dsdt_bad
        mov rdx, [r15 + 140]            ; X_DSDT
        test rdx, rdx
        jnz .have_dsdt
        mov edx, [r15 + 40]
.have_dsdt:
        cmp dword [rdx], 'DSDT'
        jne .dsdt_bad
        cmp byte [rdx + 36], 0x10       ; ScopeOp first
        jne .dsdt_bad
        mov rsi, rdx
        mov ecx, [rdx + 4]
        call sum8
        jmp .dsdt_done
.dsdt_bad:
        or eax, 1
.dsdt_done:
        lea rsi, [rel s_dsdt]
        call result

        test r15, r15
        jz .facs_bad
        mov edx, [r15 + 36]             ; FIRMWARE_CTRL
        test edx, edx
        jnz .have_facs
        mov rdx, [r15 + 132]            ; X_FIRMWARE_CTRL
.have_facs:
        test rdx, rdx
        jz .facs_bad
        test dl, 63
        jnz .facs_bad
        cmp dword [rdx], 'FACS'
        jne .facs_bad
        cmp dword [rdx + 4], 64
        jmp .facs_done
.facs_bad:
        or eax, 1
.facs_done:
        lea rsi, [rel s_facs]
        call result

        ; MADT: count enabled local APICs
        xor ebx, ebx
        mov rdx, [rel t_apic]
        test rdx, rdx
        jz .madt_bad
        mov ecx, [rdx + 4]
        lea r15, [rdx + rcx]            ; end
        lea rsi, [rdx + 44]
.madt_walk:
        cmp rsi, r15
        jae .madt_end
        movzx ecx, byte [rsi + 1]
        test ecx, ecx
        jz .madt_bad
        cmp byte [rsi], 0
        jne .madt_next
        test byte [rsi + 4], 1
        jz .madt_next
        inc ebx
.madt_next:
        add rsi, rcx
        jmp .madt_walk
.madt_end:
        test ebx, ebx
        jz .madt_bad
        xor eax, eax
        jmp .madt_done
.madt_bad:
        or eax, 1
.madt_done:
        lea rsi, [rel s_madt]
        call result
        lea rsi, [rel s_cpus]
        call puts
        mov eax, ebx
        call putdec
        lea rsi, [rel s_nl]
        call puts

        ; HPET, MCFG
        lea rsi, [rel s_app2]
        call puts
        cmp qword [rel t_hpet], 0
        je .hpet_bad
        xor eax, eax
        jmp .hpet_done
.hpet_bad:
        or eax, 1
.hpet_done:
        lea rsi, [rel s_hpet]
        call result
        lea rsi, [rel s_mcfg]
        call puts
        mov rdx, [rel t_mcfg]
        lea rsi, [rel s_none]
        test rdx, rdx
        jz .mcfg_print
        lea rsi, [rel s_ok]
        cmp dword [rdx + 4], 60
        jae .mcfg_print
        lea rsi, [rel s_bad]
.mcfg_print:
        call puts
        lea rsi, [rel s_nl]
        call puts

        ; SMBIOS 3
        lea rsi, [rel s_app3]
        call puts
        lea rdi, [rel smbios3_guid]
        call find_cfg
        mov r12, rax
        test rax, rax
        jz .sm3_bad
        cmp dword [r12], '_SM3'
        jne .sm3_bad
        cmp byte [r12 + 4], '_'
        jne .sm3_bad
        cmp byte [r12 + 6], 0x18
        jne .sm3_bad
        mov rsi, r12
        mov ecx, 0x18
        call sum8
        jz .sm3_done
.sm3_bad:
        xor r12d, r12d
        or eax, 1
.sm3_done:
        lea rsi, [rel s_sm3]
        call result

        ; walk the structures, collecting a bitmap of types below 64
        xor ebx, ebx                    ; seen types (bits)
        test r12, r12
        jz .smw_bad
        mov rsi, [r12 + 16]
        mov ecx, [r12 + 12]
        lea r15, [rsi + rcx]            ; end
.smw:
        lea rax, [rsi + 4]
        cmp rax, r15
        ja .smw_bad
        movzx eax, byte [rsi]
        movzx ecx, byte [rsi + 1]
        cmp ecx, 4
        jb .smw_bad
        cmp eax, 64
        jae .smw_skip_bit
        bts rbx, rax
.smw_skip_bit:
        mov edx, eax                    ; type
        add rsi, rcx
.smw_str:                               ; strings end at two NULs
        cmp rsi, r15
        jae .smw_bad
        cmp word [rsi], 0
        je .smw_str_end
        inc rsi
        jmp .smw_str
.smw_str_end:
        add rsi, 2
        cmp edx, 127
        jne .smw
        cmp rsi, r15
        ja .smw_bad
        xor eax, eax
        jmp .smw_done
.smw_bad:
        or eax, 1
.smw_done:
        lea rsi, [rel s_walk]
        call result
        mov rax, (1 << 0) | (1 << 1) | (1 << 4) | (1 << 16) | (1 << 17) | (1 << 19) | (1 << 32)
        and rbx, rax
        cmp rbx, rax
        lea rsi, [rel s_types]
        call result
        lea rsi, [rel s_nl]
        call puts

        xor eax, eax
        pop r15
        pop r14
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; rdi = GUID: rax = its configuration table pointer, or 0
find_cfg:
        mov rcx, [r13 + ST_COUNT]
        mov rdx, [r13 + ST_CONFIG]
.next:
        test rcx, rcx
        jz .none
        mov rax, [rdx]
        cmp rax, [rdi]
        jne .skip
        mov rax, [rdx + 8]
        cmp rax, [rdi + 8]
        jne .skip
        mov rax, [rdx + 16]
        ret
.skip:
        add rdx, 24
        dec rcx
        jmp .next
.none:
        xor eax, eax
        ret

; rsi = bytes, ecx = length: al = their sum, ZF set when it is 0
sum8:
        push rsi
        xor eax, eax
.add:
        test ecx, ecx
        jz .end
        add al, [rsi]
        inc rsi
        dec ecx
        jmp .add
.end:
        pop rsi
        test al, al
        ret

; eax = number to print in decimal
putdec:
        push rbx
        push rcx
        push rdx
        push rsi
        lea rsi, [rel numbuf + 11]
        mov byte [rsi], 0
        mov ebx, 10
.div:
        xor edx, edx
        div ebx
        add dl, '0'
        dec rsi
        mov [rsi], dl
        test eax, eax
        jnz .div
        call puts
        pop rsi
        pop rdx
        pop rcx
        pop rbx
        ret

; ZF set: print rsi then "ok", else rsi then "bad"
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
t_facp: dq 0
t_apic: dq 0
t_hpet: dq 0
t_mcfg: dq 0
numbuf: times 12 db 0
acpi20_guid:
        dd 0x8868e871
        dw 0xe4f1, 0x11d3
        db 0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81
smbios3_guid:
        dd 0xf2fd1544
        dw 0x9794, 0x4a2c
        db 0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94
sig_rsdp: db 'RSD PTR '
s_app:  db 'APP acpi', 0
s_app2: db 'APP acpi', 0
s_app3: db 'APP smbios', 0
s_rsdp: db ' rsdp=', 0
s_xsdt: db ' xsdt=', 0
s_sums: db ' sums=', 0
s_facp: db ' facp=', 0
s_dsdt: db ' dsdt=', 0
s_facs: db ' facs=', 0
s_madt: db ' madt=', 0
s_cpus: db ' cpus=', 0
s_hpet: db ' hpet=', 0
s_mcfg: db ' mcfg=', 0
s_sm3:  db ' sm3=', 0
s_walk: db ' walk=', 0
s_types: db ' types=', 0
s_ok:   db 'ok', 0
s_bad:  db 'bad', 0
s_none: db 'none', 0
s_nl:   db 13, 10, 0
        times 0x2000 - ($ - $$) db 0

reloc:  dd 0x1000
        dd 12
        dw 0
        dw 0
reloc_end:
        times 0x3000 - ($ - $$) db 0
