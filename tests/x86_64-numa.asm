; Stand-in EFI application for the NUMA and error tables (roadmap F1/F2).
; Original code. Walks the XSDT from the ACPI 2.0 configuration table and
; reports on COM1:
;   APP numa sums=ok madt=L/X srat=C cpu=D,D,... mem=D:B+S,... slit=N:dd,dd,...
;   APP numa bert=ok hest=ok
; L/X   local APIC / local x2APIC entries in the MADT
; C     processor affinity entries in the SRAT; cpu= their domains in order
; mem   memory affinity entries: domain, base and size in MB
; slit  locality count, then the matrix row by row
; (srat=none, slit=none when the tables are absent) then returns 0.

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

        ; RSDP -> XSDT
        mov rcx, [r13 + ST_COUNT]
        mov rdi, [r13 + ST_CONFIG]
.find:
        test rcx, rcx
        jz fail
        mov rax, [rdi]
        cmp rax, [rel acpi20_guid]
        jne .next
        mov rax, [rdi + 8]
        cmp rax, [rel acpi20_guid + 8]
        je .found
.next:
        add rdi, 24
        dec rcx
        jmp .find
.found:
        mov rax, [rdi + 16]
        mov r12, [rax + 24]             ; XSDT
        mov r14d, [r12 + 4]
        sub r14d, 36
        shr r14d, 3                     ; entries

        ; every table's checksum
        xor ebx, ebx
        xor r15d, r15d                  ; bad count
.sums:
        cmp ebx, r14d
        jae .sums_done
        mov rsi, [r12 + 36 + rbx * 8]
        mov ecx, [rsi + 4]
        call sum8
        jz .sum_ok
        inc r15d
.sum_ok:
        inc ebx
        jmp .sums
.sums_done:
        lea rsi, [rel s_sums]
        test r15d, r15d
        call result

        ; MADT: count type 0 and type 9
        mov eax, 'APIC'
        call table
        test rax, rax
        jz fail
        mov rsi, rax
        mov ecx, [rsi + 4]
        lea rdx, [rsi + rcx]            ; end
        add rsi, 44
        xor r8d, r8d
        xor r9d, r9d
.madt:
        cmp rsi, rdx
        jae .madt_done
        cmp byte [rsi], 0
        jne .not_l
        inc r8d
.not_l:
        cmp byte [rsi], 9
        jne .not_x
        inc r9d
.not_x:
        movzx eax, byte [rsi + 1]
        test eax, eax
        jz fail
        add rsi, rax
        jmp .madt
.madt_done:
        lea rsi, [rel s_madt]
        call puts
        mov eax, r8d
        call putd
        mov al, '/'
        call putc
        mov eax, r9d
        call putd

        ; SRAT
        lea rsi, [rel s_srat]
        call puts
        mov eax, 'SRAT'
        call table
        test rax, rax
        jnz .srat
        lea rsi, [rel s_none]
        call puts
        jmp .slit
.srat:
        mov rbx, rax
        mov ecx, [rbx + 4]
        lea r15, [rbx + rcx]            ; end
        ; first pass: processor entries count, then their domains
        lea rsi, [rbx + 48]
        xor r8d, r8d
.cnt:
        cmp rsi, r15
        jae .cnt_done
        mov al, [rsi]
        cmp al, 0
        je .cnt_one
        cmp al, 2
        jne .cnt_next
.cnt_one:
        inc r8d
.cnt_next:
        movzx eax, byte [rsi + 1]
        test eax, eax
        jz fail
        add rsi, rax
        jmp .cnt
.cnt_done:
        mov eax, r8d
        call putd
        lea rsi, [rel s_cpu]
        call puts
        lea rsi, [rbx + 48]
        xor r9d, r9d                    ; printed any
.dom:
        cmp rsi, r15
        jae .dom_done
        cmp byte [rsi], 0
        jne .dom_x2
        movzx eax, byte [rsi + 2]       ; domain bits 7:0
        mov edx, [rsi + 8]
        shr edx, 8                      ; bits 31:8
        shl edx, 8
        or eax, edx
        jmp .dom_put
.dom_x2:
        cmp byte [rsi], 2
        jne .dom_next
        mov eax, [rsi + 4]
.dom_put:
        test r9d, r9d
        jz .dom_first
        push rax
        mov al, ','
        call putc
        pop rax
.dom_first:
        mov r9d, 1
        call putd
.dom_next:
        movzx eax, byte [rsi + 1]
        add rsi, rax
        jmp .dom
.dom_done:
        lea rsi, [rel s_mem]
        call puts
        lea rsi, [rbx + 48]
        xor r9d, r9d
.mem:
        cmp rsi, r15
        jae .slit
        cmp byte [rsi], 1
        jne .mem_next
        test r9d, r9d
        jz .mem_first
        mov al, ','
        call putc
.mem_first:
        mov r9d, 1
        mov eax, [rsi + 2]
        call putd
        mov al, ':'
        call putc
        mov rax, [rsi + 8]
        shr rax, 20
        call putd
        mov al, '+'
        call putc
        mov rax, [rsi + 16]
        shr rax, 20
        call putd
.mem_next:
        movzx eax, byte [rsi + 1]
        add rsi, rax
        jmp .mem

.slit:
        lea rsi, [rel s_slit]
        call puts
        mov eax, 'SLIT'
        call table
        test rax, rax
        jnz .slit_have
        lea rsi, [rel s_none]
        call puts
        jmp .ras
.slit_have:
        mov rbx, rax
        mov r8, [rbx + 36]              ; localities
        mov rax, r8
        call putd
        mov al, ':'
        call putc
        xor r9d, r9d                    ; row
.row:
        cmp r9, r8
        jae .ras
        test r9, r9
        jz .row_first
        mov al, ','
        call putc
.row_first:
        xor r10d, r10d
.col:
        cmp r10, r8
        jae .row_next
        mov rax, r9
        imul rax, r8
        add rax, r10
        movzx eax, byte [rbx + 44 + rax]
        call putd2
        inc r10
        jmp .col
.row_next:
        inc r9
        jmp .row

.ras:
        lea rsi, [rel s_nl]
        call puts
        lea rsi, [rel s_app]
        call puts
        ; BERT: 48 bytes, region whose block status is 0
        mov eax, 'BERT'
        call table
        test rax, rax
        jz .bert_bad
        cmp dword [rax + 4], 48
        jne .bert_bad
        cmp dword [rax + 36], 20
        jb .bert_bad
        mov rax, [rax + 40]
        test rax, rax
        jz .bert_bad
        cmp dword [rax], 0
        jmp .bert_done
.bert_bad:
        or eax, 1
.bert_done:
        lea rsi, [rel s_bert]
        call result
        mov eax, 'HEST'
        call table
        test rax, rax
        jz .hest_no
        cmp dword [rax + 4], 40
        jne .hest_no
        xor eax, eax
        jmp .hest_done
.hest_no:
        or eax, 1
.hest_done:
        lea rsi, [rel s_hest]
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
        pop r15
        pop r14
        pop r13
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret

; table: EAX = signature; RAX = the XSDT entry with it, or 0
table:
        push rcx
        xor ecx, ecx
.t:
        cmp ecx, r14d
        jae .none
        mov rdx, [r12 + 36 + rcx * 8]
        cmp [rdx], eax
        je .hit
        inc ecx
        jmp .t
.hit:
        mov rax, rdx
        pop rcx
        ret
.none:
        xor eax, eax
        pop rcx
        ret

; sum8: RSI = bytes, ECX = count; ZF when they add up to 0
sum8:
        push rcx
        push rsi
        xor eax, eax
.a:     add al, [rsi]
        inc rsi
        dec ecx
        jnz .a
        pop rsi
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

; putd2: EAX as two decimal digits
putd2:
        push rdx
        push rcx
        xor edx, edx
        mov ecx, 10
        div ecx
        add al, '0'
        call putc
        mov al, dl
        add al, '0'
        call putc
        pop rcx
        pop rdx
        ret

; putd: RAX in decimal
putd:
        push rbx
        push rcx
        push rdx
        mov rbx, 10
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
acpi20_guid:
        dd 0x8868e871
        dw 0xe4f1, 0x11d3
        db 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81
s_app:  db 'APP numa', 0
s_sums: db ' sums=', 0
s_madt: db ' madt=', 0
s_srat: db ' srat=', 0
s_cpu:  db ' cpu=', 0
s_mem:  db ' mem=', 0
s_slit: db ' slit=', 0
s_bert: db ' bert=', 0
s_hest: db ' hest=', 0
s_none: db 'none', 0
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
