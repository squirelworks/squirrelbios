; SquirrelBIOS x86 — USB disk: boot and INT 13h from a USB mass storage device
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the USB Mass Storage Class Bulk-Only Transport 1.0 and the
; SCSI Block Commands (READ CAPACITY(10), READ(10), WRITE(10)). The first
; bulk-only SCSI interface that usb_enum finds on an xHCI whose block is
; in the EBDA is configured: both bulk endpoints get a ring in the block
; and are added to the device's slot. That controller then keeps running
; after POST (xhci.inc), alongside a USB keyboard or mouse on the same
; controller, and its state that the transfers need (registers, event
; ring position, the disk's rings) is copied into the EBDA's first page.
;
; At boot, when no IDE, SATA or NVMe disk answered, usbd_detect asks the
; disk whether it is ready (INQUIRY, TEST UNIT READY, REQUEST SENSE) and
; how big it is (READ CAPACITY(10), 512-byte blocks only). It then
; becomes DL = 80h: ata_read, ata_write, ata_capacity and ata_reset hand
; over to the routines here while DISK_KIND says USB, as for NVMe.
;
; Each command is three transfers, polled with interrupts off: the
; 31-byte command block wrapper (CBW) on the bulk OUT endpoint, the data
; (if any) straight to or from the caller's buffer, then the 13-byte
; status wrapper (CSW) on bulk IN. Each waits for its own transfer event,
; told apart by its TRB pointer: the keyboard's and mouse's transfers
; post none. A stalled endpoint is not recovered yet: the command fails.
;
; Runtime code reaches the copy with FS = USBD_FSEG, a segment whose
; offsets 1F00h-1F7Fh are the copy, so xhci.inc's xhci_put and
; xhci_event work unchanged on it.

; The disk's record (USB_SEG variables, copied into the EBDA)
USBD_ADDR       equ 0x1F60          ; byte: its address (0: none)
USBD_EPIN       equ 0x1F61          ; byte: bulk IN endpoint
USBD_EPOUT      equ 0x1F62          ; byte: bulk OUT endpoint
USBD_SLOT       equ 0x1F64          ; word: its xHCI slot
USBD_MPS        equ 0x1F66          ; word: bulk max packet
USBD_OUTST      equ 0x1F68          ; bulk OUT ring: index, cycle bit
USBD_INST       equ 0x1F6A          ; bulk IN ring: index, cycle bit
USBD_TAG        equ 0x1F6C          ; dword: last CBW tag
USBD_SECTORS    equ 0x1F70          ; dword: capacity (usbd_detect)

USBD_SNAP       equ 0x0F80          ; the copy of 1F00h-1F7Fh, in the EBDA
; Its own stack, in the EBDA too: POST's stack below 0600h is too shallow
; for the nested transfer code. Offsets in the EBDA.
USBD_SAVESP     equ 0x0CC0          ; word: the caller's SP, then its SS
USBD_SAVESS     equ 0x0CC2
USBD_SAVEES     equ 0x0CC4          ; word: the caller's ES, while switching
USBD_RFLAGS     equ 0x0CC6          ; word: the routine's flags
USBD_STACK      equ 0x0F00          ; top; 512 bytes down to 0D00h
USBD_FSEG       equ (EBDA_BASE + USBD_SNAP - 0x1F00) >> 4

%if (EBDA_BASE + USBD_SNAP - 0x1F00) & 15
%error "the USB disk's copy must sit at a paragraph from 1F00h"
%endif

DISK_USB        equ 3

USBD_CBW_SIG    equ 0x43425355      ; 'USBC'
USBD_CSW_SIG    equ 0x53425355      ; 'USBS'
USBD_IN         equ 0x80            ; CBW flags: data to the host

SCSI_TUR        equ 0x00
SCSI_SENSE      equ 0x03
SCSI_INQUIRY    equ 0x12
SCSI_CAPACITY   equ 0x25
SCSI_READ10     equ 0x28
SCSI_WRITE10    equ 0x2A

; From usb_enum, with its frame at BP: the device just listed has a
; bulk-only SCSI interface. If no disk is claimed yet, the keyboard or
; mouse claimed before (if any) is on this controller, and this is an
; xHCI that can keep running, add its bulk endpoints, configure it and
; remember it. Keeps every register.
usb_disk_claim:
    pushad
    cmp byte [fs:USB_HC], USB_HC_XHCI
    jne .out
    cmp byte [fs:USBD_ADDR], 0
    jne .out
    cmp byte [bp - 13], 0
    je .out
    cmp byte [bp - 14], 0
    je .out
    mov dx, [bp - 16]
    test dx, dx
    jz .out
    mov al, [fs:USB_KBD_ADDR]
    or al, [fs:USB_MOU_ADDR]
    jz .first
    mov ax, [fs:USB_BDF]
    cmp ax, [fs:USB_KBD_BDF]
    jne .out                    ; the keyboard or mouse runs elsewhere
.first:
    cmp dword [fs:XHCI_BASE], XHCI_EBDA
    jne .out                    ; its block will not survive POST
    mov al, [bp - 1]
    mov cl, [bp - 13]
    mov ch, [bp - 14]
    call xhci_msd_config
    jc .out
    ; SET_CONFIGURATION
    mov ax, USB_REQ_SET_CONFIG << 8
    movzx cx, byte [bp - 12]
    xor dx, dx
    xor bx, bx
    call usb_setup
    call usb_enum.ctrl
    jc .out
    mov al, [bp - 1]
    mov [fs:USBD_ADDR], al
    mov al, [bp - 13]
    mov [fs:USBD_EPIN], al
    mov al, [bp - 14]
    mov [fs:USBD_EPOUT], al
    mov ax, [bp - 16]
    mov [fs:USBD_MPS], ax
    mov dword [fs:USBD_TAG], 0
    mov dword [fs:USBD_SECTORS], 0
    mov ax, [fs:USB_BDF]
    mov [fs:USB_KBD_BDF], ax
.out:
    popad
    ret

; The controller with the disk keeps running (xhci_bus, FS = USB_SEG):
; copy its state for the runtime code. Keeps every register.
usbd_keep:
    cmp byte [fs:USBD_ADDR], 0
    je .ret
    pushad
    push es
    push word EBDA_BASE >> 4
    pop es
    mov si, 0x1F00
    mov di, USBD_SNAP
    mov cx, 0x80 / 4
.copy:
    mov eax, [fs:si]
    mov [es:di], eax
    add si, 4
    add di, 4
    loop .copy
    pop es
    popad
.ret:
    ret

; CF clear when DL 80h is the USB disk and ATA_DEVSEL names it. Keeps
; every register but the flags.
usbd_owns:
    cmp byte [ss:DISK_KIND], DISK_USB
    jne .no
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    jne .no
    clc
    ret
.no:
    stc
    ret

; Run the routine at CS:DI on the USB disk's stack, with interrupts off.
; Every register passes through both ways, and CF comes back from the
; routine; the caller's other flags are kept.
usbd_on_stack:
    pushf
    cli
    push es
    push word EBDA_BASE >> 4
    pop es
    pop word [es:USBD_SAVEES]
    mov [es:USBD_SAVESP], sp
    mov [es:USBD_SAVESS], ss
    push es
    pop ss
    mov sp, USBD_STACK
    mov es, [ss:USBD_SAVEES]
    call di
    pushf
    pop word [ss:USBD_RFLAGS]
    lss sp, [ss:USBD_SAVESP]
    popf
    push es
    push word EBDA_BASE >> 4
    pop es
    test byte [es:USBD_RFLAGS], 1
    pop es
    jnz .carry
    clc
    ret
.carry:
    stc
    ret

; POST, when no IDE, SATA or NVMe disk answered: if a USB disk was kept,
; wait for it to be ready and read its size. On success CF is clear and
; DL 80h is the USB disk (DISK_KIND, ATA_PRESENT bit 0, one hard disk in
; the BDA, ATA_DEVSEL on the master, BOOT_DL = 80h). On failure CF is set.
; Keeps every register.
usbd_detect:
    push di
    mov di, usbd_find
    call usbd_on_stack
    pop di
    ret

usbd_find:
    pushad
    push fs
    push gs
    pushf
    cli
    push word USBD_FSEG
    pop fs
    cmp byte [fs:USBD_ADDR], 0
    je .none
    call flat_gs
    mov ebx, [fs:XHCI_BASE]
    lea ebp, [ebx + XHCI_SCSIBUF]
    ; INQUIRY: some devices want it first
    mov al, SCSI_INQUIRY
    call usbd_cdb
    mov byte [gs:ebx + XHCI_CBW + 15 + 4], 36
    mov ax, (6 << 8) | USBD_IN
    mov ecx, 36
    call usbd_scsi
    jc .none
    ; TEST UNIT READY until it is, with REQUEST SENSE after each "not
    ; yet" (a fresh device reports a unit attention first): up to 2 s
    mov si, 20
.tur:
    mov al, SCSI_TUR
    call usbd_cdb
    mov ax, 6 << 8
    xor ecx, ecx
    call usbd_scsi
    jc .none
    test al, al
    jz .ready
    call usbd_sense
    mov cx, 100
    call delay_ms
    dec si
    jnz .tur
    jmp .none
.ready:
    ; READ CAPACITY(10): the last block and the block size
    mov si, 2
.cap:
    mov al, SCSI_CAPACITY
    call usbd_cdb
    mov ax, (10 << 8) | USBD_IN
    mov ecx, 8
    call usbd_scsi
    jc .none
    test al, al
    jz .size
    call usbd_sense
    dec si
    jnz .cap
    jmp .none
.size:
    mov eax, [gs:ebx + XHCI_SCSIBUF + 4]
    bswap eax
    cmp eax, 512
    jne .none                   ; 512-byte blocks only
    mov eax, [gs:ebx + XHCI_SCSIBUF]
    bswap eax
    add eax, 1
    jnc .count
    mov eax, 0xFFFFFFFF         ; 2 TB or more: what 32 bits hold
.count:
    mov [fs:USBD_SECTORS], eax
    push word 0
    pop fs
    mov byte [fs:DISK_KIND], DISK_USB
    or byte [fs:ATA_PRESENT], 1
    mov byte [fs:BDA_HDCOUNT], 1
    mov byte [fs:ATA_DEVSEL], ATA_SEL_MASTER
    mov byte [fs:BOOT_DL], 0x80
    popf
    pop gs
    pop fs
    popad
    clc
    ret
.none:
    popf
    pop gs
    pop fs
    popad
    stc
    ret

; REQUEST SENSE into the SCSI buffer, its result ignored: it clears the
; condition the last command reported. FS = USBD_FSEG, GS flat. Keeps
; every register.
usbd_sense:
    pushad
    mov ebx, [fs:XHCI_BASE]
    lea ebp, [ebx + XHCI_SCSIBUF]
    mov al, SCSI_SENSE
    call usbd_cdb
    mov byte [gs:ebx + XHCI_CBW + 15 + 4], 18
    mov ax, (6 << 8) | USBD_IN
    mov ecx, 18
    call usbd_scsi
    popad
    ret

; A fresh command block, all zero but its opcode AL. FS = USBD_FSEG, GS
; flat. Keeps every register.
usbd_cdb:
    push edi
    mov edi, [fs:XHCI_BASE]
    mov dword [gs:edi + XHCI_CBW + 15], 0
    mov dword [gs:edi + XHCI_CBW + 19], 0
    mov dword [gs:edi + XHCI_CBW + 23], 0
    mov dword [gs:edi + XHCI_CBW + 27], 0
    mov [gs:edi + XHCI_CBW + 15], al
    pop edi
    ret

; Run the SCSI command whose block usbd_cdb started: AH = its length, AL
; = CBW flags (USBD_IN: data to the host), ECX = bytes of data (up to
; 64 KB), EBP = their linear address. FS = USBD_FSEG, GS flat,
; interrupts off. CF set if a transfer failed or the status wrapper is
; not this command's; otherwise AL = its status (0 passed, 1 failed, 2
; phase error). Keeps every other register.
usbd_scsi:
    pushad
    mov ebx, [fs:XHCI_BASE]
    mov dword [gs:ebx + XHCI_CBW], USBD_CBW_SIG
    mov edx, [fs:USBD_TAG]
    inc edx
    mov [fs:USBD_TAG], edx
    mov [gs:ebx + XHCI_CBW + 4], edx
    mov [gs:ebx + XHCI_CBW + 8], ecx
    mov [gs:ebx + XHCI_CBW + 12], al
    mov byte [gs:ebx + XHCI_CBW + 13], 0        ; LUN 0
    mov [gs:ebx + XHCI_CBW + 14], ah
    mov dword [gs:ebx + XHCI_CSW], 0
    ; The command block, on bulk OUT
    push eax
    push ecx
    lea eax, [ebx + XHCI_CBW]
    mov ecx, 31
    lea esi, [ebx + XHCI_BOUT]
    mov di, USBD_OUTST
    call usbd_xfer
    pop ecx
    pop eax
    jc .fail
    ; The data, if any, in its direction
    test ecx, ecx
    jz .status
    lea esi, [ebx + XHCI_BOUT]
    mov di, USBD_OUTST
    test al, USBD_IN
    jz .data
    lea esi, [ebx + XHCI_BIN]
    mov di, USBD_INST
.data:
    mov eax, ebp
    call usbd_xfer
    jc .fail
.status:
    ; The status wrapper, on bulk IN
    lea eax, [ebx + XHCI_CSW]
    mov ecx, 13
    lea esi, [ebx + XHCI_BIN]
    mov di, USBD_INST
    call usbd_xfer
    jc .fail
    cmp dword [gs:ebx + XHCI_CSW], USBD_CSW_SIG
    jne .fail
    mov eax, [gs:ebx + XHCI_CSW + 4]
    cmp eax, [fs:USBD_TAG]
    jne .fail
    mov al, [gs:ebx + XHCI_CSW + 12]
    mov bp, sp
    mov [bp + 28], al           ; the caller's AL
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

; One bulk transfer of ECX bytes (1 to 64 KB) at linear EAX, on the ring
; at linear ESI with its state at FS:DI (USBD_OUTST or USBD_INST): one
; Normal TRB, or two chained when the buffer crosses a 64 KB boundary.
; Ring the endpoint's doorbell and wait for the transfer's event. CF set
; unless it ended with success or a short packet. Keeps every register.
usbd_xfer:
    pushad
    ; EBP = the first TRB, its cycle bit cleared
    xchg eax, ebp
    call xhci_ring_ptr
    and al, 0xF0
    xchg eax, ebp
    ; EDX = bytes up to the next 64 KB boundary
    mov edx, eax
    and edx, 0xFFFF
    neg edx
    add edx, 0x10000
    cmp ecx, edx
    jbe .last
    ; The first part, chained to the rest. Its TD size: the packets
    ; still to come after it, at most 31.
    push eax
    push edx
    lea eax, [ecx + 0]
    sub eax, edx
    movzx ebx, word [fs:USBD_MPS]
    add eax, ebx
    dec eax
    xor edx, edx
    div ebx
    cmp eax, 31
    jbe .td_size
    mov eax, 31
.td_size:
    shl eax, 17
    pop edx
    mov ebx, edx
    or ebx, eax                 ; EBX = length, TD size
    pop eax
    sub ecx, edx
    push ecx
    push edx
    mov ecx, ebx
    xor ebx, ebx
    mov edx, (TRB_NORMAL << 10) | 0x14  ; chain, interrupt on short
    call xhci_put
    pop edx
    pop ecx
    add eax, edx
.last:
    ; The last part, with an event when it is done
    push eax
    xor ebx, ebx
    mov edx, (TRB_NORMAL << 10) | 0x24  ; interrupt on completion, short
    ; (ECX = its length, TD size 0)
    push ecx
    push eax
    call xhci_ring_ptr
    and al, 0xF0
    mov [fs:XHCI_EV0], eax      ; for now: the last TRB
    pop eax
    pop ecx
    call xhci_put
    pop eax
    ; The doorbell: the slot, the endpoint's context index
    movzx eax, byte [fs:USBD_EPOUT]
    shl eax, 1
    cmp di, USBD_OUTST
    je .door
    movzx eax, byte [fs:USBD_EPIN]
    lea eax, [eax * 2 + 1]
.door:
    mov ecx, [fs:XHCI_EV0]      ; ECX = the last TRB
    movzx ebx, word [fs:USBD_SLOT]
    mov edx, [fs:XHCI_DB]
    mov [gs:edx + ebx * 4], eax
.wait:
    mov al, TRB_EV_TRANSFER
    call xhci_event
    jc .fail
    cmp [fs:XHCI_EV3 + 3], bl
    jne .wait
    mov eax, [fs:XHCI_EV0]
    cmp eax, ebp
    je .done
    cmp eax, ecx
    jne .wait                   ; an older transfer's
.done:
    mov al, [fs:XHCI_EV2 + 3]
    cmp al, CC_SUCCESS
    je .ok
    cmp al, CC_SHORT
    jne .fail
.ok:
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

; ata_read for the USB disk: EAX = LBA, CL = sectors (1-128), EBP =
; linear buffer below 1 MB. Same outputs as ata_read.
usbd_read:
    push edx
    push di
    mov dl, SCSI_READ10
    mov di, usbd_rw
    call usbd_on_stack
    pop di
    pop edx
    ret

; ata_write for the USB disk. Same registers.
usbd_write:
    push edx
    push di
    mov dl, SCSI_WRITE10
    mov di, usbd_rw
    call usbd_on_stack
    pop di
    pop edx
    ret

; DL = the opcode. Every register but SI and BX (on failure) is kept.
usbd_rw:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push fs
    push gs
    pushf
    cli
    push word USBD_FSEG
    pop fs
    call flat_gs
    movzx ecx, cl
    test cx, cx
    jz .fail
    cmp cx, 128
    ja .fail
    mov esi, ecx                ; the last sector must still be below 2^32
    dec esi
    add esi, eax
    jc .fail
    mov ebx, [fs:XHCI_BASE]
    push eax
    mov al, dl
    call usbd_cdb
    pop eax
    bswap eax
    mov [gs:ebx + XHCI_CBW + 15 + 2], eax       ; the LBA, big-endian
    mov [gs:ebx + XHCI_CBW + 15 + 8], cl        ; the count's low byte
    shl ecx, 9
    mov ax, 10 << 8
    cmp dl, SCSI_READ10
    jne .go
    mov al, USBD_IN
.go:
    call usbd_scsi
    jc .fail
    test al, al
    jnz .fail
    popf
    pop gs
    pop fs
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret
.fail:
    mov si, msg_readerr
    xor bx, bx
    popf
    pop gs
    pop fs
    add sp, 8                   ; drop saved EBX and ESI; BX/SI stay
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc
    ret

; ata_capacity for the USB disk: CF clear, EDX:EAX = sectors.
usbd_capacity:
    push es
    push word EBDA_BASE >> 4
    pop es
    mov eax, [es:USBD_SNAP + USBD_SECTORS - 0x1F00]
    xor edx, edx
    pop es
    clc
    ret

; ata_reset for the USB disk: nothing to reset between commands; CF set
; if the controller has halted or reports a host system error. Keeps
; every register.
usbd_reset:
    push eax
    push fs
    push gs
    pushf
    cli
    push word USBD_FSEG
    pop fs
    call flat_gs
    mov eax, [fs:USB_MMIO]
    test dword [gs:eax + XHCI_USBSTS], XSTS_HCH | 0x04
    jz .ok
    popf
    pop gs
    pop fs
    pop eax
    stc
    ret
.ok:
    popf
    pop gs
    pop fs
    pop eax
    clc
    ret

; INT 13h AH=48h device path for the USB disk, at DS:SI 24h-3Fh: host
; bus 'PCI' with the controller's bus, device and function, interface
; 'USB' with the device's address. Keeps every register.
usbd_edd_path:
    push eax
    push es
    push word EBDA_BASE >> 4
    pop es
    mov dword [si + 0x24], 'PCI '
    mov dword [si + 0x28], 'USB '
    mov dword [si + 0x2C], '    '
    mov ax, [es:USBD_SNAP + USB_KBD_BDF - 0x1F00]
    mov [si + 0x30], ah             ; bus
    shr al, 3
    mov [si + 0x31], al             ; device
    mov al, [es:USBD_SNAP + USB_KBD_BDF - 0x1F00]
    and al, 7
    mov [si + 0x32], al             ; function
    mov byte [si + 0x33], 0
    mov dword [si + 0x34], 0
    movzx eax, byte [es:USBD_SNAP + USBD_ADDR - 0x1F00]
    mov [si + 0x38], eax            ; device path: its USB address
    mov dword [si + 0x3C], 0
    pop es
    pop eax
    ret
