; SquirrelBIOS x86 — NVMe driver: namespace 1 of one controller as DL 80h
; Original code. Included by x86/squirrel-bios.asm when CONFIG_NVME.
;
; Written from the NVM Express Base Specification 1.4. At POST, when no
; IDE or SATA disk answered, nvme_detect looks on PCI bus 0 for a class
; 01:08:02 function, resets the controller, sets up the admin queues,
; identifies namespace 1 and creates one I/O queue pair. Namespace 1
; then becomes DL = 80h: ata_read, ata_write, ata_capacity and ata_reset
; hand over to the routines here while DISK_KIND says NVMe, so the boot
; path and INT 13h use it unchanged.
;
; Every queue has its own 4 KB page in the EBDA (the memory map reserves
; it), so they stay put after the hand-off. Commands are polled, one at
; a time, with interrupts off; a transfer of up to 128 sectors uses PRP1,
; PRP2 or a PRP list in the EBDA's first page. Registers are reached
; through the flat GS from flat_gs (core/post.inc), as for AHCI.

NVME_VARS       equ EBDA_BASE + 0x40
NVME_BAR        equ NVME_VARS + 0       ; dword: controller registers
NVME_DBS        equ NVME_VARS + 4       ; dword: doorbell stride in bytes
NVME_SECTORS    equ NVME_VARS + 8       ; qword: namespace size (NSZE)
NVME_BDF        equ NVME_VARS + 16      ; word: bus << 8 | dev << 3 | fn
NVME_ASQT       equ NVME_VARS + 18      ; byte: admin SQ tail
NVME_ACQH       equ NVME_VARS + 19      ; byte: admin CQ head
NVME_APHASE     equ NVME_VARS + 20      ; byte: admin CQ phase expected
NVME_ISQT       equ NVME_VARS + 21      ; byte: I/O SQ tail
NVME_ICQH       equ NVME_VARS + 22      ; byte: I/O CQ head
NVME_IPHASE     equ NVME_VARS + 23      ; byte: I/O CQ phase expected
NVME_CID        equ NVME_VARS + 24      ; word: next command identifier
NVME_PRPL       equ EBDA_BASE + 0xC00   ; PRP list, 16 entries
NVME_ASQ        equ EBDA_BASE + 0x1000  ; admin submission queue
NVME_ACQ        equ EBDA_BASE + 0x2000  ; admin completion queue
NVME_IOSQ       equ EBDA_BASE + 0x3000  ; I/O submission queue
NVME_IOCQ       equ EBDA_BASE + 0x4000  ; I/O completion queue
NVME_IDBUF      equ NVME_IOSQ           ; Identify data, before the I/O SQ exists
NVME_QSIZE      equ 8                   ; entries in each queue

%if EBDA_SIZE < 0x5000
%error "NVMe needs a 20 KB EBDA"
%endif

NVME_CLASS      equ 0x010802            ; mass storage, NVM, NVMe
NVME_NSID       equ 1

NVME_CAP        equ 0x00                ; qword
NVME_CC         equ 0x14
NVME_CSTS       equ 0x1C
NVME_AQA        equ 0x24
NVME_ASQ_REG    equ 0x28                ; qword
NVME_ACQ_REG    equ 0x30                ; qword
NVME_DOORBELL   equ 0x1000
CC_EN           equ 0x00000001
CC_QES          equ (6 << 16) | (4 << 20)   ; 64-byte SQ, 16-byte CQ entries
CSTS_RDY        equ 0x01
CSTS_CFS        equ 0x02

NVME_ADM_CREATE_SQ equ 0x01
NVME_ADM_CREATE_CQ equ 0x05
NVME_ADM_IDENTIFY  equ 0x06
NVME_IO_FLUSH      equ 0x00
NVME_IO_WRITE      equ 0x01
NVME_IO_READ       equ 0x02

; CF clear when DL 80h is the NVMe namespace and ATA_DEVSEL names it.
; Keeps every register but the flags.
nvme_owns:
    cmp byte [ss:DISK_KIND], DISK_NVME
    jne .no
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    jne .no
    clc
    ret
.no:
    stc
    ret

; POST, when no IDE or SATA disk answered: find an NVMe controller on
; bus 0 and bring namespace 1 up. On success CF is clear and DL 80h is
; that namespace (DISK_KIND, ATA_PRESENT bit 0, one hard disk in the
; BDA, ATA_DEVSEL on the master, BOOT_DL = 80h). On failure CF is set
; and every register is kept.
nvme_detect:
    pushad
    push gs
    pushf
    cli
    call enable_a20             ; the BAR may have bit 20 set
    call pci_present
    jc .none
    xor bx, bx
.fn:
    call pci_next
    jc .none
    mov di, 0x08                ; class code in bits 31:8
    call pci_read32
    shr eax, 8
    cmp eax, NVME_CLASS
    jne .next
    mov di, PCI_BAR0
    call pci_read32
    test al, 1                  ; must be a memory BAR
    jnz .next
    mov esi, eax
    and esi, 0xFFFFFFF0
    jz .next                    ; POST could not place it
    and al, 0x06
    cmp al, 0x04                ; 64-bit: the high half must be 0
    jne .bar_ok
    mov di, PCI_BAR0 + 4
    call pci_read32
    test eax, eax
    jnz .next
.bar_ok:
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_MEM | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    call flat_gs
    mov [gs:dword NVME_BAR], esi
    mov [gs:dword NVME_BDF], bx
    call nvme_init
    jnc .found
.next:
    inc bl
    jnz .fn
.none:
    popf
    pop gs
    popad
    stc
    ret
.found:
    mov byte [ss:DISK_KIND], DISK_NVME
    or byte [ss:ATA_PRESENT], 1
    mov byte [ss:BDA_HDCOUNT], 1
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    mov byte [ss:BOOT_DL], 0x80
    popf
    pop gs
    popad
    clc
    ret

; Reset and enable the controller at ESI, identify namespace 1 (512-byte
; blocks only) and create I/O queue pair 1. GS flat. CF set on failure.
; Clobbers EAX, ECX, EDX and EDI.
nvme_init:
    ; Doorbell stride: 4 << CAP.DSTRD (bits 35:32)
    mov ecx, [gs:esi + NVME_CAP + 4]
    and cl, 0x0F
    mov eax, 4
    shl eax, cl
    mov [gs:dword NVME_DBS], eax
    test dword [gs:esi + NVME_CAP + 4], 1 << (37 - 32)   ; NVM command set
    jz .fail
    ; Off first, then wait for RDY to drop
    and dword [gs:esi + NVME_CC], ~CC_EN
    mov ecx, ATA_TIMEOUT
.off:
    test dword [gs:esi + NVME_CSTS], CSTS_RDY
    jz .is_off
    dec ecx
    jnz .off
    jmp .fail
.is_off:
    ; Fresh queues: all of the four queue pages zeroed
    mov edi, NVME_ASQ
    mov ecx, 4 * 0x1000 / 4
.zero:
    mov dword [gs:edi], 0
    add edi, 4
    dec ecx
    jnz .zero
    mov dword [gs:dword NVME_ASQT], 0
    mov byte [gs:dword NVME_ICQH], 0
    mov byte [gs:dword NVME_APHASE], 1
    mov byte [gs:dword NVME_IPHASE], 1
    mov word [gs:dword NVME_CID], 0
    mov dword [gs:esi + NVME_AQA], ((NVME_QSIZE - 1) << 16) | (NVME_QSIZE - 1)
    mov dword [gs:esi + NVME_ASQ_REG], NVME_ASQ
    mov dword [gs:esi + NVME_ASQ_REG + 4], 0
    mov dword [gs:esi + NVME_ACQ_REG], NVME_ACQ
    mov dword [gs:esi + NVME_ACQ_REG + 4], 0
    mov dword [gs:esi + NVME_CC], CC_QES | CC_EN
    mov ecx, ATA_TIMEOUT
.on:
    mov eax, [gs:esi + NVME_CSTS]
    test al, CSTS_CFS
    jnz .fail
    test al, CSTS_RDY
    jnz .ready
    dec ecx
    jnz .on
    jmp .fail
.ready:
    ; Identify namespace 1 (CNS 0) into the I/O SQ page
    call nvme_clear_cmd
    mov byte [gs:dword NVME_CMD_BUF], NVME_ADM_IDENTIFY
    mov dword [gs:dword NVME_CMD_BUF + 4], NVME_NSID
    mov dword [gs:dword NVME_CMD_BUF + 24], NVME_IDBUF
    mov dword [gs:dword NVME_CMD_BUF + 40], 0
    call nvme_admin
    jc .fail
    mov eax, [gs:dword NVME_IDBUF]          ; NSZE
    mov edx, [gs:dword NVME_IDBUF + 4]
    mov ecx, eax
    or ecx, edx
    jz .fail                    ; namespace not there
    mov [gs:dword NVME_SECTORS], eax
    mov [gs:dword NVME_SECTORS + 4], edx
    movzx ecx, byte [gs:dword NVME_IDBUF + 26]  ; FLBAS: format in use
    and cl, 0x0F
    cmp byte [gs:dword NVME_IDBUF + 128 + 2 + ecx * 4], 9   ; LBADS: 512
    jne .fail
    ; The Identify data was in the I/O SQ page: clear it again
    mov edi, NVME_IOSQ
    mov ecx, 0x1000 / 4
.zero_sq:
    mov dword [gs:edi], 0
    add edi, 4
    dec ecx
    jnz .zero_sq
    ; Create I/O CQ 1, then I/O SQ 1 that completes into it
    call nvme_clear_cmd
    mov byte [gs:dword NVME_CMD_BUF], NVME_ADM_CREATE_CQ
    mov dword [gs:dword NVME_CMD_BUF + 24], NVME_IOCQ
    mov dword [gs:dword NVME_CMD_BUF + 40], ((NVME_QSIZE - 1) << 16) | 1
    mov dword [gs:dword NVME_CMD_BUF + 44], 1   ; physically contiguous, no IRQ
    call nvme_admin
    jc .fail
    call nvme_clear_cmd
    mov byte [gs:dword NVME_CMD_BUF], NVME_ADM_CREATE_SQ
    mov dword [gs:dword NVME_CMD_BUF + 24], NVME_IOSQ
    mov dword [gs:dword NVME_CMD_BUF + 40], ((NVME_QSIZE - 1) << 16) | 1
    mov dword [gs:dword NVME_CMD_BUF + 44], (1 << 16) | 1   ; CQ 1, contiguous
    call nvme_admin
    jc .fail
    clc
    ret
.fail:
    and dword [gs:esi + NVME_CC], ~CC_EN    ; leave nothing half set up
    stc
    ret

; The command being built: 64 bytes in the EBDA's first page.
NVME_CMD_BUF    equ EBDA_BASE + 0xC80

; Zero NVME_CMD_BUF. GS flat. Keeps every register.
nvme_clear_cmd:
    push ecx
    push edi
    mov edi, NVME_CMD_BUF
    mov ecx, 16
.zero:
    mov dword [gs:edi], 0
    add edi, 4
    dec ecx
    jnz .zero
    pop edi
    pop ecx
    ret

; Submit NVME_CMD_BUF on the admin queue (QID 0) and wait for it.
; ESI = registers, GS flat. CF set on failure, SI/BX as nvme_submit.
nvme_admin:
    push edx
    xor edx, edx                ; queue 0
    call nvme_submit
    pop edx
    ret

; Submit NVME_CMD_BUF on queue EDX (0 admin, 1 I/O) and poll its
; completion. ESI = registers, GS flat. Out: CF clear on success; CF set
; with SI = msg_readerr (BL = status code, BH = status code type) or
; msg_timeout (BX = 0). Clobbers EAX, and BX and SI on failure.
nvme_submit:
    push ecx
    push edi
    push ebp
    ; EBP = this queue's variables: SQ tail, CQ head, phase at +0/+1/+2
    mov ebp, NVME_ASQT
    test edx, edx
    jz .q
    mov ebp, NVME_ISQT
.q:
    ; Command identifier
    mov ax, [gs:dword NVME_CID]
    inc word [gs:dword NVME_CID]
    mov [gs:dword NVME_CMD_BUF + 2], ax
    ; Copy the command into the SQ slot at the tail
    movzx edi, byte [gs:ebp]
    shl edi, 6
    add edi, NVME_ASQ
    test edx, edx
    jz .copy
    add edi, NVME_IOSQ - NVME_ASQ
.copy:
    xor ecx, ecx
.word:
    mov eax, [gs:dword NVME_CMD_BUF + ecx * 4]
    mov [gs:edi + ecx * 4], eax
    inc ecx
    cmp ecx, 16
    jb .word
    ; New tail, then ring the SQ doorbell: 1000h + (2 * QID) * stride
    mov al, [gs:ebp]
    inc al
    cmp al, NVME_QSIZE
    jb .tail
    xor al, al
.tail:
    mov [gs:ebp], al
    movzx ecx, al
    mov eax, edx
    shl eax, 1
    imul eax, [gs:dword NVME_DBS]
    mov [gs:esi + eax + NVME_DOORBELL], ecx
    ; The completion entry at the CQ head, once its phase tag matches
    movzx edi, byte [gs:ebp + 1]
    shl edi, 4
    add edi, NVME_ACQ
    test edx, edx
    jz .poll_start
    add edi, NVME_IOCQ - NVME_ACQ
.poll_start:
    mov ecx, ATA_TIMEOUT
.poll:
    mov eax, [gs:edi + 12]      ; DW3: CID, phase, status
    shr eax, 16
    and al, 1
    cmp al, [gs:ebp + 2]
    je .done
    dec ecx
    jnz .poll
    mov si, msg_timeout
    xor bx, bx
    jmp .fail
.done:
    mov eax, [gs:edi + 12]
    shr eax, 17                 ; status field without the phase tag
    mov bx, ax                  ; BL = status code, BH bits 2:0 = type
    and bh, 0x07
    ; Consume it: new head (the phase flips on wrap), CQ doorbell
    mov al, [gs:ebp + 1]
    inc al
    cmp al, NVME_QSIZE
    jb .head
    xor al, al
    xor byte [gs:ebp + 2], 1
.head:
    mov [gs:ebp + 1], al
    movzx ecx, al
    mov eax, edx
    shl eax, 1
    inc eax
    imul eax, [gs:dword NVME_DBS]
    mov [gs:esi + eax + NVME_DOORBELL], ecx
    test bx, bx
    jz .ok
    mov si, msg_readerr
.fail:
    pop ebp
    pop edi
    pop ecx
    stc
    ret
.ok:
    pop ebp
    pop edi
    pop ecx
    clc
    ret

; ata_read for the NVMe namespace: EAX = LBA, CL = sectors (1-128), EBP
; = linear buffer (dword-aligned, below 1 MB). Same outputs as ata_read.
nvme_read:
    push edx
    mov dl, NVME_IO_READ
    call nvme_rw
    pop edx
    ret

; ata_write for the NVMe namespace, then a Flush. Same registers.
nvme_write:
    push edx
    mov dl, NVME_IO_WRITE
    call nvme_rw
    pop edx
    ret

; DL = the opcode. Every register but SI and BX (on failure) is kept.
nvme_rw:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push gs
    pushf
    cli
    call flat_gs
    movzx ecx, cl
    test cx, cx
    jz .badarg
    cmp cx, 128
    ja .badarg
    test bp, 3                  ; PRP entries are dword-aligned
    jnz .badarg
    mov esi, ecx                ; the last sector must still be below 2^32
    dec esi
    add esi, eax
    jc .badarg
    mov esi, [gs:dword NVME_BAR]
    call nvme_clear_cmd
    mov [gs:dword NVME_CMD_BUF], dl
    mov dword [gs:dword NVME_CMD_BUF + 4], NVME_NSID
    mov [gs:dword NVME_CMD_BUF + 40], eax       ; SLBA
    mov dword [gs:dword NVME_CMD_BUF + 44], 0
    lea eax, [ecx - 1]
    mov [gs:dword NVME_CMD_BUF + 48], eax       ; NLB, zero-based
    ; PRP1 = the buffer. The bytes past PRP1's page go to PRP2: the next
    ; page when one more is enough, else a list of the pages after it.
    mov [gs:dword NVME_CMD_BUF + 24], ebp
    mov edi, ebp
    and edi, 0xFFFFF000
    add edi, 0x1000             ; EDI = the next page
    shl ecx, 9
    add ecx, ebp                ; ECX = end of the buffer
    cmp ecx, edi
    jbe .prp_done               ; all in PRP1's page
    mov [gs:dword NVME_CMD_BUF + 32], edi
    lea eax, [edi + 0x1000]
    cmp ecx, eax
    jbe .prp_done               ; PRP2 is the second and last page
    mov dword [gs:dword NVME_CMD_BUF + 32], NVME_PRPL
    xor eax, eax
.list:
    mov [gs:dword NVME_PRPL + eax * 8], edi
    mov dword [gs:dword NVME_PRPL + eax * 8 + 4], 0
    inc eax
    add edi, 0x1000
    cmp edi, ecx
    jb .list
.prp_done:
    push edx
    mov edx, 1                  ; I/O queue
    call nvme_submit
    pop edx
    jc .fail
    cmp dl, NVME_IO_WRITE
    jne .ok
    call nvme_clear_cmd
    mov byte [gs:dword NVME_CMD_BUF], NVME_IO_FLUSH
    mov dword [gs:dword NVME_CMD_BUF + 4], NVME_NSID
    push edx
    mov edx, 1
    call nvme_submit
    pop edx
    jc .fail
.ok:
    popf
    pop gs
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret
.badarg:
    mov si, msg_readerr
    xor bx, bx
.fail:
    popf
    pop gs
    add sp, 8                   ; drop saved EBX and ESI; BX/SI stay
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc
    ret

; ata_capacity for the NVMe namespace: CF clear, EDX:EAX = sectors.
nvme_capacity:
    push es
    push word EBDA_BASE >> 4
    pop es
    mov eax, [es:NVME_SECTORS - EBDA_BASE]
    mov edx, [es:NVME_SECTORS - EBDA_BASE + 4]
    pop es
    clc
    ret

; ata_reset for the NVMe namespace: nothing to reset between commands;
; CF set if the controller is not ready or reports a fatal status.
; Keeps every register.
nvme_reset:
    push eax
    push gs
    pushf
    cli
    call flat_gs
    mov eax, [gs:dword NVME_BAR]
    mov eax, [gs:eax + NVME_CSTS]
    and al, CSTS_RDY | CSTS_CFS
    cmp al, CSTS_RDY
    je .ok
    popf
    pop gs
    pop eax
    stc
    ret
.ok:
    popf
    pop gs
    pop eax
    clc
    ret

; INT 13h AH=48h device path for the NVMe namespace, at DS:SI 24h-3Fh:
; host bus 'PCI' with the controller's bus, device and function,
; interface 'NVMe' with the namespace ID. Keeps every register.
nvme_edd_path:
    push ax
    push es
    push word EBDA_BASE >> 4
    pop es
    mov dword [si + 0x24], 'PCI '
    mov dword [si + 0x28], 'NVMe'
    mov dword [si + 0x2C], '    '
    mov ax, [es:NVME_BDF - EBDA_BASE]
    mov [si + 0x30], ah             ; bus
    shr al, 3
    mov [si + 0x31], al             ; device
    mov al, [es:NVME_BDF - EBDA_BASE]
    and al, 7
    mov [si + 0x32], al             ; function
    mov byte [si + 0x33], 0         ; channel
    mov dword [si + 0x34], 0
    mov dword [si + 0x38], NVME_NSID
    mov dword [si + 0x3C], 0
    pop es
    pop ax
    ret
