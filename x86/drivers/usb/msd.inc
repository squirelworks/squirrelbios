; SquirrelBIOS x86 — USB disk: boot and INT 13h from a USB mass storage device
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
;
; Written from the USB Mass Storage Class Bulk-Only Transport 1.0 and the
; SCSI Block Commands (READ CAPACITY(10), READ(10), WRITE(10)). The first
; bulk-only SCSI interface that usb_enum finds on an xHCI whose block is
; in the EBDA, or at full speed on a UHCI or an OHCI, is configured. On
; xHCI both bulk endpoints get a ring in the block and are added to the
; device's slot. On UHCI the disk gets a queue head of its own after the
; keyboard's and mouse's in the frame list hid.inc moves into the EBDA,
; and each transfer is a chain of TDs hung from it (usbd_uhci_xfer). On
; OHCI each bulk endpoint gets an endpoint descriptor on the bulk list,
; skipped except while a chain of general TDs runs on it
; (usbd_ohci_xfer). On EHCI (high speed) the disk can share no schedule
; with a keyboard or mouse, which go to the companion controllers: the
; asynchronous list is one queue head for the endpoint in use, built in
; the EBDA for each transfer with the qTDs after it and run with the
; schedule on only until it is done (usbd_ehci_xfer). A high-speed UAS
; interface (USB Attached SCSI, protocol 62h) is taken on EHCI too: its
; four pipes are four bulk endpoints, used one at a time without streams
; (usbd_uas_scsi). That
; controller then keeps running after POST, alongside a USB keyboard or
; mouse on the same controller, and its state that the transfers need
; (registers, event ring position, the disk's rings or data toggles) is
; copied into the EBDA's first page.
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
; post none (UHCI: polled TD by TD). A command that fails (a stalled
; endpoint, a stray status wrapper, a phase error) gets bulk-only reset
; recovery and one retry (usbd_recover): written from the BOT spec and
; tested in QEMU by a build that sends a bad command block (USBD_FAULT),
; not yet on real hardware.
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
USBD_BUFS       equ 0x1F74          ; dword: linear CBW; CSW and SCSI data after
USBD_BDF        equ 0x1F78          ; word: its controller
USBD_HC         equ 0x1F7A          ; byte: and that controller's type
USBD_EP0ST      equ 0x1F5A          ; word: xHCI, its endpoint 0 ring state
USBD_IFACE      equ 0x1F5C          ; byte: its interface (Bulk-Only Reset)
USBD_MPS0       equ 0x1F5D          ; byte: endpoint 0 max packet
USBD_FAULTN     equ 0x1F5E          ; byte: commands so far (USBD_FAULT builds)
USBD_SETUP      equ 0x38            ; from USBD_BUFS: a control request's 8 bytes
USBD_UAS        equ 0x1F63          ; byte: 1 if it speaks UAS, not bulk-only
USBD_EPDIN      equ 0x1F7B          ; byte: UAS data-in pipe's endpoint
USBD_EPDOUT     equ 0x1F7C          ; byte: UAS data-out pipe's endpoint
USBD_DINST      equ 0x1F7D          ; byte: data-in pipe's data toggle
USBD_DOUTST     equ 0x1F7E          ; byte: data-out pipe's data toggle
; On UAS, USBD_EPOUT is the command pipe and USBD_EPIN the status pipe
; On UHCI and EHCI, USBD_OUTST and USBD_INST hold each endpoint's next
; data toggle

; Offsets from USBD_BUFS: on xHCI they are the block's XHCI_CBW, XHCI_CSW
; and XHCI_SCSIBUF; on UHCI they are in the EBDA (USBD_UBUF), followed by
; the TDs. That EBDA range is NVMe's and virtio's, unused when DL 80h is
; a USB disk.
USBD_CBW        equ 0x00
USBD_CSW        equ 0x40
USBD_SCSIBUF    equ 0x80
USBD_UBUF       equ 0x1000              ; EBDA offset (UHCI)
USBD_UTD        equ 0x1100              ; EBDA offset: the TDs, 16 bytes each
USBD_UTD_MAX    equ 256                 ; TDs per pass: 16 KB at full speed
USBD_UQH        equ USBK_AREA + 2 * USBK_AREA_SZ   ; EBDA offset: the queue head

%if USBD_UTD + USBD_UTD_MAX * 16 > 0x5000
%error "the USB disk's TDs must end below the frame list"
%endif

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
    je .hc
    cmp byte [fs:USB_HC], USB_HC_EHCI
    je .ehci
    cmp byte [fs:USB_HC], USB_HC_UHCI
    je .usb11
    cmp byte [fs:USB_HC], USB_HC_OHCI
    jne .out
.usb11:
    cmp byte [bp - 2], 0
    jne .out                    ; USB 1.1: full speed only (no bulk at low)
    jmp .hc
.ehci:
    cmp byte [bp - 2], 2
    jne .out                    ; high speed only (no split transactions)
    cmp byte [fs:USBD_ADDR], 0
    jne .out
    cmp byte [bp - 17], 2
    jne .ehci_eps
    cmp byte [bp - 18], 0       ; UAS: all four pipes
    je .out
    cmp byte [bp - 19], 0
    je .out
.ehci_eps:
    cmp byte [bp - 13], 0
    je .out
    cmp byte [bp - 14], 0
    je .out
    cmp word [bp - 16], 0
    je .out
    jmp .usb_bufs               ; on its own: no keyboard shares it
.hc:
    cmp byte [bp - 17], 1
    jne .out                    ; UAS on EHCI only
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
    cmp byte [fs:USB_HC], USB_HC_XHCI
    je .xhci
.usb_bufs:
    mov dword [fs:USBD_BUFS], EBDA_BASE + USBD_UBUF
    mov word [fs:USBD_OUTST], 0         ; DATA0 on both after configuring
    mov word [fs:USBD_INST], 0
    mov word [fs:USBD_DINST], 0
    jmp .config
.xhci:
    cmp dword [fs:XHCI_BASE], XHCI_EBDA
    jne .out                    ; its block will not survive POST
    mov al, [bp - 1]
    mov cl, [bp - 13]
    mov ch, [bp - 14]
    call xhci_msd_config
    jc .out
    mov eax, [fs:XHCI_BASE]
    add eax, XHCI_CBW
    mov [fs:USBD_BUFS], eax
.config:
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
    mov al, [bp - 21]
    mov [fs:USBD_IFACE], al
    mov al, [bp - 3]
    mov [fs:USBD_MPS0], al
    mov al, [bp - 17]
    dec al
    mov [fs:USBD_UAS], al
    mov al, [bp - 18]
    mov [fs:USBD_EPDIN], al
    mov al, [bp - 19]
    mov [fs:USBD_EPDOUT], al
    mov dword [fs:USBD_TAG], 0
    mov dword [fs:USBD_SECTORS], 0
    mov ax, [fs:USB_BDF]
    mov [fs:USBD_BDF], ax
    mov cl, [fs:USB_HC]
    mov [fs:USBD_HC], cl
    cmp cl, USB_HC_EHCI
    je .out
    mov [fs:USB_KBD_BDF], ax    ; the keyboard and mouse must join it
.out:
    popad
    ret

; The controller with the disk keeps running (xhci_bus, FS = USB_SEG):
; copy its state for the runtime code. Keeps every register.
usbd_keep:
    cmp byte [fs:USBD_ADDR], 0
    je .ret
    push ax
    mov ax, [fs:USB_BDF]
    cmp ax, [fs:USBD_BDF]
    pop ax
    jne .ret                    ; the disk is on another controller
    pushad
    ; xHCI: endpoint 0's ring state too, for stall recovery's requests
    cmp byte [fs:USB_HC], USB_HC_XHCI
    jne .snap
    mov di, [fs:USBD_SLOT]
    shl di, 1
    mov ax, [fs:XHCI_RSTATE + di]
    mov [fs:USBD_EP0ST], ax
.snap:
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
    mov ebx, [fs:USBD_BUFS]
    lea ebp, [ebx + USBD_SCSIBUF]
    ; INQUIRY: some devices want it first
    mov al, SCSI_INQUIRY
    call usbd_cdb
    mov byte [gs:ebx + USBD_CBW + 15 + 4], 36
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
    mov eax, [gs:ebx + USBD_SCSIBUF + 4]
    bswap eax
    cmp eax, 512
    jne .none                   ; 512-byte blocks only
    mov eax, [gs:ebx + USBD_SCSIBUF]
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
    mov ebx, [fs:USBD_BUFS]
    lea ebp, [ebx + USBD_SCSIBUF]
    mov al, SCSI_SENSE
    call usbd_cdb
    mov byte [gs:ebx + USBD_CBW + 15 + 4], 18
    mov ax, (6 << 8) | USBD_IN
    mov ecx, 18
    call usbd_scsi
    popad
    ret

; A fresh command block, all zero but its opcode AL. FS = USBD_FSEG, GS
; flat. Keeps every register.
usbd_cdb:
    push edi
    mov edi, [fs:USBD_BUFS]
    mov dword [gs:edi + USBD_CBW + 15], 0
    mov dword [gs:edi + USBD_CBW + 19], 0
    mov dword [gs:edi + USBD_CBW + 23], 0
    mov dword [gs:edi + USBD_CBW + 27], 0
    mov [gs:edi + USBD_CBW + 15], al
    pop edi
    ret

; Run the SCSI command whose block usbd_cdb started: AH = its length, AL
; = CBW flags (USBD_IN: data to the host), ECX = bytes of data (up to
; 64 KB), EBP = their linear address. FS = USBD_FSEG, GS flat,
; interrupts off. CF set if a transfer failed or the status wrapper is
; not this command's; otherwise AL = its status (0 passed, 1 failed, 2
; phase error). Keeps every other register.
;
; Bulk-only: when a transfer fails (a stall, say), the status wrapper is
; not this command's, or the device reports a phase error, reset
; recovery (usbd_recover) and one more try.
usbd_scsi:
    cmp byte [fs:USBD_UAS], 0
    jne usbd_uas_scsi
    push eax
    call usbd_scsi_once
    jc .again
    cmp al, 2
    je .again
    clc                         ; (the compare left CF set for 0 and 1)
    jmp .done
.again:
    pop eax
    push eax
    call usbd_recover
    call usbd_scsi_once
.done:
    inc sp                      ; drop the saved EAX, keeping CF
    inc sp
    inc sp
    inc sp
    ret

; One try of usbd_scsi on bulk-only.
usbd_scsi_once:
    pushad
    mov ebx, [fs:USBD_BUFS]
    mov dword [gs:ebx + USBD_CBW], USBD_CBW_SIG
%ifdef USBD_FAULT
    ; Test builds: the USBD_FAULT'th command goes out with a bad
    ; signature, which the device must stall
    inc byte [fs:USBD_FAULTN]
    cmp byte [fs:USBD_FAULTN], USBD_FAULT
    jne .sig
    mov byte [gs:ebx + USBD_CBW], 'X'
.sig:
%endif
    mov edx, [fs:USBD_TAG]
    inc edx
    mov [fs:USBD_TAG], edx
    mov [gs:ebx + USBD_CBW + 4], edx
    mov [gs:ebx + USBD_CBW + 8], ecx
    mov [gs:ebx + USBD_CBW + 12], al
    mov byte [gs:ebx + USBD_CBW + 13], 0        ; LUN 0
    mov [gs:ebx + USBD_CBW + 14], ah
    mov dword [gs:ebx + USBD_CSW], 0
    ; The command block, on bulk OUT
    push eax
    push ecx
    lea eax, [ebx + USBD_CBW]
    mov ecx, 31
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_BOUT
    mov di, USBD_OUTST
    call usbd_xfer
    pop ecx
    pop eax
    jc .fail
    ; The data, if any, in its direction
    test ecx, ecx
    jz .status
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_BOUT
    mov di, USBD_OUTST
    test al, USBD_IN
    jz .data
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_BIN
    mov di, USBD_INST
.data:
    mov eax, ebp
    call usbd_xfer
    jc .fail
.status:
    ; The status wrapper, on bulk IN
    lea eax, [ebx + USBD_CSW]
    mov ecx, 13
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_BIN
    mov di, USBD_INST
    call usbd_xfer
    jc .fail
    cmp dword [gs:ebx + USBD_CSW], USBD_CSW_SIG
    jne .fail
    mov eax, [gs:ebx + USBD_CSW + 4]
    cmp eax, [fs:USBD_TAG]
    jne .fail
    mov al, [gs:ebx + USBD_CSW + 12]
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
    cmp byte [fs:USB_HC], USB_HC_UHCI
    je usbd_uhci_xfer
    cmp byte [fs:USB_HC], USB_HC_OHCI
    je usbd_ohci_xfer
    cmp byte [fs:USB_HC], USB_HC_EHCI
    je usbd_ehci_xfer
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
    mov ebx, [fs:USBD_BUFS]
    push eax
    mov al, dl
    call usbd_cdb
    pop eax
    bswap eax
    mov [gs:ebx + USBD_CBW + 15 + 2], eax       ; the LBA, big-endian
    mov [gs:ebx + USBD_CBW + 15 + 8], cl        ; the count's low byte
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
    cmp byte [fs:USB_HC], USB_HC_OHCI
    je .ohci
    cmp byte [fs:USB_HC], USB_HC_UHCI
    jne .ehci
    push dx
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_STS
    in ax, dx
    pop dx
    test ax, STS_HALTED | 0x18  ; halted, process or host system error
    jz .ok
    jmp .bad
.ohci:
    ; Unrecoverable error, or no longer operational
    mov eax, [fs:USB_MMIO]
    test dword [gs:eax + OHCI_INTSTATUS], 0x10
    jnz .bad
    mov eax, [gs:eax + OHCI_CONTROL]
    and eax, 0xC0
    cmp eax, OCTL_HCFS_OPER
    je .ok
    jmp .bad
.ehci:
    cmp byte [fs:USB_HC], USB_HC_EHCI
    jne .xhci
    mov eax, [fs:USB_MMIO]
    test dword [gs:eax + EHCI_USBSTS], ESTS_HALTED | 0x10
    jz .ok
    jmp .bad
.xhci:
    mov eax, [fs:USB_MMIO]
    test dword [gs:eax + XHCI_USBSTS], XSTS_HCH | 0x04
    jz .ok
.bad:
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
    mov ax, [es:USBD_SNAP + USBD_BDF - 0x1F00]
    mov [si + 0x30], ah             ; bus
    shr al, 3
    mov [si + 0x31], al             ; device
    mov al, [es:USBD_SNAP + USBD_BDF - 0x1F00]
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

; usbd_xfer on UHCI: ECX bytes (1 to 64 KB) at linear EAX, IN when DI =
; USBD_INST, else OUT; FS:DI holds that endpoint's data toggle. Max-packet
; TDs, at most USBD_UTD_MAX per pass, hung from the disk's queue head and
; polled. An IN transfer stops at a short packet (SPD). CF set on a
; stall, an error or a 5 s timeout. Keeps every register.
usbd_uhci_xfer:
    pushad
.pass:
    ; The template: address, endpoint, PID; EDX = the status to arm with
    movzx ebx, byte [fs:USBD_ADDR]
    shl ebx, 8
    mov bl, PID_OUT
    movzx edx, byte [fs:USBD_EPOUT]
    cmp di, USBD_INST
    jne .tmpl
    mov bl, PID_IN
    movzx edx, byte [fs:USBD_EPIN]
.tmpl:
    shl edx, 15
    or ebx, edx                 ; EBX = token without length and toggle
    mov edx, TD_ACTIVE | TD_CERR3
    cmp di, USBD_INST
    jne .build
    or edx, TD_SPD
.build:
    ; ESI = the next TD (linear); EBP = bytes in this pass
    mov esi, EBDA_BASE + USBD_UTD
    xor ebp, ebp
.td:
    push eax
    push ebx
    movzx eax, word [fs:USBD_MPS]
    mov ebx, ecx
    sub ebx, ebp
    cmp ebx, eax
    jbe .len
    mov ebx, eax
.len:
    mov eax, esi
    add eax, 16
    or eax, TD_VF
    mov [gs:esi], eax           ; link: the next TD, depth first
    mov [gs:esi + 4], edx
    lea eax, [ebx - 1]
    shl eax, 21
    pop ebx
    push ebx
    or eax, ebx
    test byte [fs:di], 1
    jz .tog
    or eax, 1 << 19
.tog:
    xor byte [fs:di], 1
    mov [gs:esi + 8], eax
    pop ebx
    pop eax
    push eax
    add eax, ebp
    mov [gs:esi + 12], eax      ; the buffer
    pop eax
    push ebx
    mov ebx, [gs:esi + 8]
    shr ebx, 21
    inc ebx                     ; this TD's length
    add ebp, ebx
    pop ebx
    add esi, 16
    cmp ebp, ecx
    jae .hang
    cmp esi, EBDA_BASE + USBD_UTD + USBD_UTD_MAX * 16
    jb .td
.hang:
    mov dword [gs:esi - 16], TD_T       ; the chain ends at the last TD
    ; Hang it: the controller runs it in the next frames
    push esi
    mov esi, EBDA_BASE + USBD_UQH
    mov dword [gs:esi + 4], EBDA_BASE + USBD_UTD
    pop esi
    ; Poll up to 5 s: done when the last TD is inactive, or one is
    ; inactive with an error or a short packet
    push ecx
    mov ecx, 5000
    mov edx, esi                ; EDX = past the last TD
.poll:
    test dword [gs:esi - 16 + 4], TD_ACTIVE
    jz .scan
    push ecx
    mov cx, 1
    call delay_ms
    pop ecx
    ; an error or a short packet stops the queue: look for one
    push esi
    mov esi, EBDA_BASE + USBD_UTD
.peek:
    cmp esi, edx
    jae .peek_stop
    mov ebx, [gs:esi + 4]
    test ebx, TD_ACTIVE
    jnz .peek_done              ; the controller is still on this one
    test ebx, TD_ERRORS
    jnz .peek_stop
    call .short
    jc .peek_stop
    add esi, 16
    jmp .peek
.peek_stop:
    pop esi
    jmp .scan
.peek_done:
    pop esi
    dec ecx
    jnz .poll
    pop ecx
    jmp .fail
.scan:
    pop ecx
    ; Take the queue head's work away, then count what was done
    push esi
    mov esi, EBDA_BASE + USBD_UQH
    mov dword [gs:esi + 4], TD_T
    pop esi
    push ecx
    mov cx, 2                   ; let the controller leave the chain
    call delay_ms
    pop ecx
    mov edx, esi                ; EDX = past the last TD
    mov esi, EBDA_BASE + USBD_UTD
    xor ebp, ebp                ; EBP = bytes moved
.count:
    cmp esi, edx
    jae .whole
    mov ebx, [gs:esi + 4]
    test ebx, TD_ACTIVE
    jnz .undone
    test ebx, TD_ERRORS
    jnz .undone
    and ebx, 0x7FF
    inc ebx
    and ebx, 0x7FF              ; 7FFh + 1: no bytes
    add ebp, ebx
    call .short
    jc .short_end
    add esi, 16
    jmp .count
.undone:
    ; Not run (or failed): its toggle and every later one's were never
    ; used, so the endpoint's toggle is this TD's
    mov ebx, [gs:esi + 8]
    shr ebx, 19
    and bl, 1
    mov [fs:di], bl
    jmp .fail
.short_end:
    ; A short packet ends the transfer; the TDs after it never ran
    mov ebx, [gs:esi + 8]
    shr ebx, 19
    and bl, 1
    xor bl, 1
    mov [fs:di], bl
    jmp .ok
.whole:
    add eax, ebp
    sub ecx, ebp
    ja .pass                    ; more than one pass's worth
.ok:
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

; CF set if the inactive TD at GS:ESI moved fewer bytes than it asked
; for (a short packet). Keeps every register.
.short:
    push eax
    push ebx
    mov eax, [gs:esi + 4]
    inc eax
    and eax, 0x7FF
    mov ebx, [gs:esi + 8]
    shr ebx, 21
    inc ebx
    and ebx, 0x7FF
    cmp eax, ebx
    pop ebx
    pop eax
    jb .is_short
    clc
    ret
.is_short:
    stc
    ret

; OHCI (ohci_hid_start, ES = USBK_SEG, flat GS, ESI = its registers): the
; disk's endpoint descriptors, bulk OUT then bulk IN, empty and skipped,
; as the bulk list. Keeps every register.
usbd_ohci_eds:
    pushad
    movzx eax, word [fs:USBD_MPS]
    shl eax, 16
    or eax, ED_SKIP
    or al, [fs:USBD_ADDR]
    movzx ecx, byte [fs:USBD_EPOUT]
    shl ecx, 7
    or ecx, eax
    or ecx, 1 << 11             ; OUT
    mov [es:USBD_UQH], ecx
    movzx ecx, byte [fs:USBD_EPIN]
    shl ecx, 7
    or ecx, eax
    or ecx, 2 << 11             ; IN
    mov [es:USBD_UQH + 16], ecx
    xor eax, eax
    mov [es:USBD_UQH + 4], eax          ; TailP = HeadP: empty, DATA0
    mov [es:USBD_UQH + 8], eax
    mov dword [es:USBD_UQH + 12], EBDA_BASE + USBD_UQH + 16
    mov [es:USBD_UQH + 16 + 4], eax
    mov [es:USBD_UQH + 16 + 8], eax
    mov [es:USBD_UQH + 16 + 12], eax
    mov dword [gs:esi + OHCI_BULKHEAD], EBDA_BASE + USBD_UQH
    popad
    ret

; usbd_xfer on OHCI: ECX bytes (1 to 64 KB) at linear EAX, IN when DI =
; USBD_INST, else OUT. General TDs of up to 8 KB that cross at most one
; page boundary, then an empty tail, put on that endpoint's descriptor;
; the controller carries the data toggle in it. A short IN packet halts
; the endpoint with a data underrun: that ends the transfer, and the rest
; of the chain is dropped. CF set on any other error or a 5 s timeout
; (the descriptor is emptied and un-halted either way). Keeps every
; register.
usbd_ohci_xfer:
    pushad
    mov edx, EBDA_BASE + USBD_UQH       ; EDX = the endpoint descriptor
    mov ebx, GTD_CC_NEW | GTD_NOINT | GTD_OUT
    cmp di, USBD_INST
    jne .build
    add edx, 16
    mov ebx, GTD_CC_NEW | GTD_NOINT | GTD_IN
.build:
    mov edi, EBDA_BASE + USBD_UTD       ; EDI = the next TD
.td:
    mov [gs:edi], ebx
    mov [gs:edi + 4], eax               ; CBP
    lea ebp, [edi + 16]
    mov [gs:edi + 8], ebp               ; next TD
    ; ESI = this TD's bytes: to the end of the next page at most
    mov esi, eax
    and esi, 0xFFF
    neg esi
    add esi, 0x2000
    cmp esi, ecx
    jbe .len
    mov esi, ecx
.len:
    lea ebp, [eax + esi - 1]
    mov [gs:edi + 12], ebp              ; BE: the last byte
    add eax, esi
    sub ecx, esi
    add edi, 16
    test ecx, ecx
    jnz .td
    ; The empty tail
    xor eax, eax
    mov [gs:edi], eax
    mov [gs:edi + 4], eax
    mov [gs:edi + 8], eax
    mov [gs:edi + 12], eax
    ; On the descriptor (still skipped): TailP, HeadP with the toggle carry
    mov [gs:edx + 4], edi
    mov eax, [gs:edx + 8]
    and eax, 2
    or eax, EBDA_BASE + USBD_UTD
    mov [gs:edx + 8], eax
    and dword [gs:edx], ~ED_SKIP
    mov esi, [fs:USB_MMIO]
    mov dword [gs:esi + OHCI_CMDSTATUS], OCS_BLF
    ; Done when HeadP reaches the tail; halted on an error. Up to 5 s.
    mov ecx, 5000
.poll:
    mov eax, [gs:edx + 8]
    test al, ED_HALTED
    jnz .halted
    and eax, ~0x0F
    cmp eax, edi
    je .done
    push ecx
    mov cx, 1
    call delay_ms
    pop ecx
    dec ecx
    jnz .poll
    or dword [gs:edx], ED_SKIP
    jmp .drop                           ; timed out
.halted:
    or dword [gs:edx], ED_SKIP
    ; The TD it halted on: a data underrun (a short IN packet) is the end
    ; of the transfer, anything else an error
    and eax, ~0x0F
    mov ebx, [gs:eax]
    shr ebx, 28
    cmp bl, 9
    jne .drop
    call .empty
    popad
    clc
    ret
.drop:
    call .empty
    popad
    stc
    ret
.done:
    or dword [gs:edx], ED_SKIP
    popad
    clc
    ret

; Let the controller leave the skipped descriptor at GS:EDX, then empty
; it at the tail EDI, keeping the toggle carry and clearing a halt.
.empty:
    push eax
    push cx
    mov cx, 2
    call delay_ms
    pop cx
    mov eax, [gs:edx + 8]
    and eax, 2
    or eax, edi
    mov [gs:edx + 8], eax
    pop eax
    ret

; EHCI layout in the EBDA (USBD_UTD on): the queue head, the terminator
; every qTD's alternate pointer names (a short IN packet stops there),
; then the qTDs, 32 bytes each, up to 16 KB of data each
USBD_EQH        equ USBD_UTD
USBD_EDUMMY     equ USBD_UTD + 0x40
USBD_ETD        equ USBD_UTD + 0x60
USBD_ETD_BYTES  equ 0x4000

; usbd_xfer on EHCI: ECX bytes (1 to 64 KB) at linear EAX, IN when DI =
; USBD_INST, else OUT; FS:DI holds that endpoint's data toggle, which the
; queue head carries while the transfer runs. CF set on a halt, an error
; or a 5 s timeout. Keeps every register.
usbd_ehci_xfer:
    pushad
    ; The queue head: alone on the asynchronous list, the head of
    ; reclamation; address, endpoint, high speed, max packet; the toggle
    ; from the queue head
    mov esi, EBDA_BASE + USBD_EQH
    lea edx, [esi + QH_TYPE]
    mov [gs:esi], edx
    movzx edx, word [fs:USBD_MPS]
    shl edx, 16
    or edx, QH_EPS_HIGH | QH_HEAD
    or dl, [fs:USBD_ADDR]
    movzx ebx, byte [fs:USBD_EPOUT]
    mov ebp, QTD_OUT
    cmp di, USBD_OUTST
    je .ep
    movzx ebx, byte [fs:USBD_EPDOUT]
    cmp di, USBD_DOUTST
    je .ep
    mov ebp, QTD_IN
    movzx ebx, byte [fs:USBD_EPDIN]
    cmp di, USBD_DINST
    je .ep
    movzx ebx, byte [fs:USBD_EPIN]
.ep:
    shl ebx, 8
    or edx, ebx
    mov [gs:esi + 4], edx
    mov dword [gs:esi + 8], QH_MULT1
    mov dword [gs:esi + 12], 0                  ; current qTD
    mov dword [gs:esi + 16], EBDA_BASE + USBD_ETD
    mov dword [gs:esi + 20], QH_T
    xor edx, edx
    test byte [fs:di], 1
    jz .tog
    mov edx, QTD_DATA1
.tog:
    mov [gs:esi + 24], edx                      ; inactive, the toggle
    xor edx, edx
    mov [gs:esi + 28], edx
    mov [gs:esi + 32], edx
    mov [gs:esi + 36], edx
    mov [gs:esi + 40], edx
    mov [gs:esi + 44], edx
    ; The terminator: inactive, nothing after it
    mov dword [gs:esi + 0x40], QH_T
    mov dword [gs:esi + 0x40 + 4], QH_T
    mov [gs:esi + 0x40 + 8], edx
    ; The qTDs
    mov edi, EBDA_BASE + USBD_ETD
.td:
    mov ebx, ecx
    cmp ebx, USBD_ETD_BYTES
    jbe .len
    mov ebx, USBD_ETD_BYTES
.len:
    lea edx, [edi + 32]
    mov [gs:edi], edx                           ; next
    mov dword [gs:edi + 4], EBDA_BASE + USBD_EDUMMY
    mov edx, ebx
    shl edx, 16
    or edx, ebp
    or edx, QTD_ACTIVE | QTD_CERR3
    mov [gs:edi + 8], edx
    mov [gs:edi + 12], eax                      ; buffer 0
    mov edx, eax
    and edx, 0xFFFFF000
    add edx, 0x1000
    mov [gs:edi + 16], edx                      ; buffers 1-4: the next pages
    add edx, 0x1000
    mov [gs:edi + 20], edx
    add edx, 0x1000
    mov [gs:edi + 24], edx
    add edx, 0x1000
    mov [gs:edi + 28], edx
    add eax, ebx
    sub ecx, ebx
    add edi, 32
    test ecx, ecx
    jnz .td
    mov dword [gs:edi - 32], QH_T               ; the last one's next
    ; Run it
    mov edx, edi                                ; EDX = past the last qTD
    mov esi, [fs:USB_MMIO]
    mov dword [gs:esi + EHCI_ASYNCLIST], EBDA_BASE + USBD_EQH
    clc
    call ehci_async
    mov ecx, 5000
.poll:
    test dword [gs:edx - 32 + 8], QTD_ACTIVE
    jz .stop
    ; a halt or a short IN packet ends it early
    mov edi, EBDA_BASE + USBD_ETD
.peek:
    cmp edi, edx
    jae .wait
    mov ebx, [gs:edi + 8]
    test ebx, QTD_ACTIVE
    jnz .wait
    test ebx, QTD_HALTED
    jnz .stop
    test ebx, 0x7FFF0000                        ; bytes left: short
    jnz .stop
    add edi, 32
    jmp .peek
.wait:
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    dec ecx
    jnz .poll
    stc
    call ehci_async
    jmp .fail
.stop:
    stc
    call ehci_async
    ; The toggle for next time, from the overlay
    mov ebx, EBDA_BASE + USBD_EQH
    mov ebx, [gs:ebx + 24]
    shr ebx, 31
    mov bp, sp
    mov di, [bp]                ; the caller's DI, in the PUSHAD frame
    mov [fs:di], bl
    ; Every qTD done without a halt, or stopped by a short packet
    mov edi, EBDA_BASE + USBD_ETD
.check:
    cmp edi, edx
    jae .ok
    mov ebx, [gs:edi + 8]
    test ebx, QTD_HALTED | 0x78                 ; halted, buffer, babble, transaction
    jnz .fail
    test ebx, QTD_ACTIVE
    jnz .fail
    test ebx, 0x7FFF0000
    jnz .ok                                     ; short: the end
    add edi, 32
    jmp .check
.ok:
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

; usbd_scsi for a UAS disk (on EHCI, without streams): the command IU on
; the command pipe; with data, the device's READ READY or WRITE READY IU
; on the status pipe, then the data on the data-in or data-out pipe; then
; the SENSE IU on the status pipe. The IUs use the block's CBW and CSW
; space: the command at +40h (its CDB taken from usbd_cdb's block), what
; the status pipe sends at +0. Same registers and results as usbd_scsi.
usbd_uas_scsi:
    pushad
    mov ebx, [fs:USBD_BUFS]
    ; A new tag, never 0; SI = it big-endian, as the IUs carry it
    mov edx, [fs:USBD_TAG]
    inc dx
    jnz .tag
    inc dx
.tag:
    mov [fs:USBD_TAG], edx
    xchg dl, dh
    mov si, dx
    ; The command IU: ID 1, the tag, a simple task, LUN 0, the CDB
    movzx edx, dx
    shl edx, 16
    or edx, 1
    mov [gs:ebx + 0x40], edx
    xor edx, edx
    mov [gs:ebx + 0x44], edx
    mov [gs:ebx + 0x48], edx
    mov [gs:ebx + 0x4C], edx
    mov edx, [gs:ebx + USBD_CBW + 15]
    mov [gs:ebx + 0x50], edx
    mov edx, [gs:ebx + USBD_CBW + 19]
    mov [gs:ebx + 0x54], edx
    mov edx, [gs:ebx + USBD_CBW + 23]
    mov [gs:ebx + 0x58], edx
    mov edx, [gs:ebx + USBD_CBW + 27]
    mov [gs:ebx + 0x5C], edx
    push eax
    push ecx
    lea eax, [ebx + 0x40]
    mov ecx, 32
    mov di, USBD_OUTST
    call usbd_xfer
    pop ecx
    pop eax
    jc .fail
    test ecx, ecx
    jz .status
    ; READ READY (6) or WRITE READY (7); a SENSE IU instead means the
    ; command ended before any data
    call .iu
    jc .fail
    mov dl, [gs:ebx]
    cmp dl, 3
    je .sense
    mov dh, 6
    mov di, USBD_DINST
    test al, USBD_IN
    jnz .ready
    mov dh, 7
    mov di, USBD_DOUTST
.ready:
    cmp dl, dh
    jne .fail
    mov eax, ebp
    call usbd_xfer
    jc .fail
.status:
    call .iu
    jc .fail
.sense:
    ; The SENSE IU (3): its status byte, GOOD or not
    cmp byte [gs:ebx], 3
    jne .fail
    mov al, [gs:ebx + 6]
    test al, al
    jz .done
    mov al, 1
.done:
    mov bp, sp
    mov [bp + 28], al           ; the caller's AL
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

; The next IU from the status pipe, to +0: CF set unless it arrived and
; carries this command's tag (SI). Keeps every register.
.iu:
    push eax
    push ecx
    push di
    mov dword [gs:ebx], 0
    mov eax, ebx
    mov ecx, 0x40
    mov di, USBD_INST
    call usbd_xfer
    jc .iu_out
    cmp [gs:ebx + 2], si
    je .iu_out                  ; CF clear
    stc
.iu_out:
    pop di
    pop ecx
    pop eax
    ret

; The POST line for the USB disk: SI = it. Keeps every other register.
usbd_banner:
    push es
    push word EBDA_BASE >> 4
    pop es
    mov si, msg_usbd
    cmp byte [es:USBD_SNAP + USBD_UAS - 0x1F00], 0
    je .out
    mov si, msg_usbd_uas
.out:
    pop es
    ret

; Stall recovery (bulk-only reset recovery, BOT 5.3.4): a Bulk-Only Mass
; Storage Reset to the disk's interface, then CLEAR_FEATURE(ENDPOINT_HALT)
; to bulk IN and to bulk OUT; on the host side both endpoints start over
; at DATA0 and un-halted (usbd_host_reset). Errors are ignored: the retry
; tells. FS = USBD_FSEG, GS flat. Keeps every register.
usbd_recover:
    pushad
    mov eax, 0x0000FF21         ; class, to the interface: reset (FFh)
    movzx ebx, byte [fs:USBD_IFACE]
    call usbd_ctl
    mov eax, 0x00000102         ; to an endpoint: CLEAR_FEATURE, ENDPOINT_HALT
    movzx ebx, byte [fs:USBD_EPIN]
    or bl, 0x80
    call usbd_ctl
    movzx ebx, byte [fs:USBD_EPOUT]
    call usbd_ctl
    call usbd_host_reset
    popad
    ret

; A control request without a data stage to the disk's endpoint 0: EAX
; the setup packet's first four bytes, EBX its last four. CF set on
; failure. FS = USBD_FSEG, GS flat. Keeps every register.
usbd_ctl:
    pushad
    mov edi, [fs:USBD_BUFS]
    add edi, USBD_SETUP
    mov [gs:edi], eax
    mov [gs:edi + 4], ebx
    cmp byte [fs:USB_HC], USB_HC_UHCI
    je .uhci
    cmp byte [fs:USB_HC], USB_HC_OHCI
    je .ohci
    cmp byte [fs:USB_HC], USB_HC_EHCI
    je .ehci
    call usbd_xhci_ctl
    jmp .out
.uhci:
    call usbd_uhci_ctl
    jmp .out
.ohci:
    call usbd_ohci_ctl
    jmp .out
.ehci:
    call usbd_ehci_ctl
.out:
    popad                       ; CF as it was
    ret

; UHCI: the SETUP TD (DATA0, 8 bytes at linear EDI) and the status TD (IN,
; DATA1, no data) hung from the disk's queue head, up to 1 s. CF set on
; failure. Clobbers EAX, EBX, ECX, EDX, ESI.
usbd_uhci_ctl:
    mov esi, EBDA_BASE + USBD_UTD
    movzx ebx, byte [fs:USBD_ADDR]
    shl ebx, 8                  ; endpoint 0
    lea eax, [esi + 16]
    or eax, TD_VF
    mov [gs:esi], eax
    mov dword [gs:esi + 4], TD_ACTIVE | TD_CERR3
    mov eax, ebx
    or eax, (7 << 21) | PID_SETUP
    mov [gs:esi + 8], eax
    mov [gs:esi + 12], edi
    mov dword [gs:esi + 16], TD_T
    mov dword [gs:esi + 20], TD_ACTIVE | TD_CERR3
    mov eax, ebx
    or eax, (0x7FF << 21) | (1 << 19) | PID_IN
    mov [gs:esi + 24], eax
    mov dword [gs:esi + 28], 0
    mov edx, EBDA_BASE + USBD_UQH
    mov [gs:edx + 4], esi
    mov ecx, 1000
.poll:
    test dword [gs:esi + 20], TD_ACTIVE
    jz .end
    test dword [gs:esi + 4], TD_ACTIVE
    jnz .wait
    test dword [gs:esi + 4], TD_ERRORS
    jnz .end                    ; the SETUP failed
.wait:
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    dec ecx
    jnz .poll
.end:
    mov dword [gs:edx + 4], TD_T
    push cx
    mov cx, 2                   ; let the controller leave the TDs
    call delay_ms
    pop cx
    test dword [gs:esi + 20], TD_ACTIVE | TD_ERRORS
    jnz .fail
    clc
    ret
.fail:
    stc
    ret

; OHCI: the SETUP TD (DATA0, 8 bytes at linear EDI), the status TD (IN,
; DATA1) and an empty tail on an endpoint descriptor of its own after
; the disk's two, alone on the control list, up to 1 s; the control list
; is off again after. CF set on failure. Clobbers EAX, EBX, ECX, EDX,
; ESI.
usbd_ohci_ctl:
    mov edx, EBDA_BASE + USBD_UQH + 32
    mov esi, EBDA_BASE + USBD_UTD
    mov dword [gs:esi], GTD_CC_NEW | GTD_NOINT | (2 << 24)     ; SETUP, DATA0
    mov [gs:esi + 4], edi
    lea eax, [esi + 16]
    mov [gs:esi + 8], eax
    lea eax, [edi + 7]
    mov [gs:esi + 12], eax
    mov dword [gs:esi + 16], GTD_CC_NEW | GTD_NOINT | GTD_IN | (3 << 24)  ; DATA1
    xor eax, eax
    mov [gs:esi + 20], eax
    mov [gs:esi + 28], eax
    mov [gs:esi + 32], eax      ; the tail
    mov [gs:esi + 36], eax
    mov [gs:esi + 40], eax
    mov [gs:esi + 44], eax
    lea eax, [esi + 32]
    mov [gs:esi + 24], eax
    ; The descriptor: address, endpoint 0, direction from the TDs, full
    ; speed, endpoint 0's max packet
    movzx eax, byte [fs:USBD_MPS0]
    shl eax, 16
    or al, [fs:USBD_ADDR]
    mov [gs:edx], eax
    lea eax, [esi + 32]
    mov [gs:edx + 4], eax       ; TailP
    mov [gs:edx + 8], esi       ; HeadP, toggle carry 0
    mov dword [gs:edx + 12], 0
    mov ebx, [fs:USB_MMIO]
    mov [gs:ebx + OHCI_CTRLHEAD], edx
    mov dword [gs:ebx + OHCI_CTRLCUR], 0
    or dword [gs:ebx + OHCI_CONTROL], OCTL_CLE
    mov dword [gs:ebx + OHCI_CMDSTATUS], OCS_CLF
    mov ecx, 1000
.poll:
    call .state
    jc .end
    jz .end
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    dec ecx
    jnz .poll
.end:
    or dword [gs:edx], ED_SKIP
    and dword [gs:ebx + OHCI_CONTROL], ~OCTL_CLE
    push cx
    mov cx, 2
    call delay_ms
    pop cx
    call .state
    jc .fail
    jnz .fail
    clc
    ret
.fail:
    stc
    ret
; CF set if the descriptor at GS:EDX halted, else ZF set once its TDs
; are all done. Clobbers EAX.
.state:
    mov eax, [gs:edx + 8]
    test al, ED_HALTED
    jnz .halted
    and eax, ~0x0F
    cmp eax, [gs:edx + 4]       ; CF clear when equal
    clc
    ret
.halted:
    stc
    ret

; EHCI: a queue head for endpoint 0 (toggles from the qTDs) alone on the
; asynchronous list, with the SETUP qTD (DATA0, 8 bytes at linear EDI)
; and the status qTD (IN, DATA1), up to 1 s; the schedule is off again
; after. CF set on failure. Clobbers EAX, EBX, ECX, EDX, ESI.
usbd_ehci_ctl:
    mov edx, EBDA_BASE + USBD_EQH
    lea eax, [edx + QH_TYPE]
    mov [gs:edx], eax
    movzx eax, byte [fs:USBD_MPS0]
    shl eax, 16
    or eax, QH_EPS_HIGH | QH_DTC | QH_HEAD
    or al, [fs:USBD_ADDR]       ; endpoint 0
    mov [gs:edx + 4], eax
    mov dword [gs:edx + 8], QH_MULT1
    xor eax, eax
    mov [gs:edx + 12], eax
    mov dword [gs:edx + 16], EBDA_BASE + USBD_ETD
    mov dword [gs:edx + 20], QH_T
    mov ecx, 24
.qh:
    mov [gs:edx + ecx], eax     ; the overlay: inactive
    add ecx, 4
    cmp ecx, 48
    jb .qh
    mov ebx, EBDA_BASE + USBD_ETD
    xor ecx, ecx
.zero:
    mov [gs:ebx + ecx], eax
    add ecx, 4
    cmp ecx, 64
    jb .zero
    lea eax, [ebx + 32]
    mov [gs:ebx], eax
    mov dword [gs:ebx + 4], QH_T
    mov dword [gs:ebx + 8], (8 << 16) | QTD_SETUP | QTD_ACTIVE | QTD_CERR3
    mov [gs:ebx + 12], edi
    mov dword [gs:ebx + 32], QH_T
    mov dword [gs:ebx + 36], QH_T
    mov dword [gs:ebx + 40], QTD_DATA1 | QTD_IN | QTD_ACTIVE | QTD_CERR3
    mov esi, [fs:USB_MMIO]
    mov [gs:esi + EHCI_ASYNCLIST], edx
    clc
    call ehci_async
    mov ecx, 1000
.poll:
    test dword [gs:ebx + 40], QTD_ACTIVE
    jz .end
    test dword [gs:ebx + 8], QTD_HALTED
    jnz .end
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    dec ecx
    jnz .poll
.end:
    stc
    call ehci_async
    test dword [gs:ebx + 8], QTD_ACTIVE | QTD_HALTED
    jnz .fail
    test dword [gs:ebx + 40], QTD_ACTIVE | QTD_HALTED
    jnz .fail
    clc
    ret
.fail:
    stc
    ret

; xHCI: a Setup TRB (the 8 bytes inside it) and an IN Status TRB on the
; slot's endpoint 0 ring, state USBD_EP0ST; wait for endpoint 0's event.
; CF set unless it succeeded. EAX, EBX: the setup packet. Clobbers ECX,
; EDX, ESI, EDI, EBP.
usbd_xhci_ctl:
    movzx ebp, word [fs:USBD_SLOT]
    lea esi, [ebp - 1]
    shl esi, 8
    add esi, [fs:XHCI_BASE]
    add esi, XHCI_RINGS
    mov di, USBD_EP0ST
    mov ecx, 8
    mov edx, (TRB_SETUP << 10) | (1 << 6)       ; immediate data
    call xhci_put
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    mov edx, (TRB_STATUS << 10) | (1 << 5) | (1 << 16)  ; IN, an event
    call xhci_put
    mov edx, [fs:XHCI_DB]
    mov dword [gs:edx + ebp * 4], 1             ; endpoint 0
.wait:
    mov al, TRB_EV_TRANSFER
    call xhci_event
    jc .fail
    mov eax, ebp
    cmp [fs:XHCI_EV3 + 3], al
    jne .wait                   ; another slot's
    mov al, [fs:XHCI_EV3 + 2]
    and al, 0x1F
    cmp al, 1
    jne .wait                   ; another endpoint's
    cmp byte [fs:XHCI_EV2 + 3], CC_SUCCESS
    jne .fail
    clc
    ret
.fail:
    stc
    ret

; The host side of reset recovery: both bulk endpoints at DATA0 with no
; halt. UHCI, EHCI: the toggles. OHCI: each endpoint descriptor's halt
; and toggle carry. xHCI: Reset Endpoint, Stop Endpoint and Set TR
; Dequeue Pointer on each (usbd_xhci_ep_reset). Keeps every register.
usbd_host_reset:
    pushad
    cmp byte [fs:USB_HC], USB_HC_OHCI
    je .ohci
    cmp byte [fs:USB_HC], USB_HC_XHCI
    je .xhci
    mov byte [fs:USBD_INST], 0
    mov byte [fs:USBD_OUTST], 0
    jmp .out
.ohci:
    mov edx, EBDA_BASE + USBD_UQH
    and dword [gs:edx + 8], ~3
    and dword [gs:edx + 16 + 8], ~3
    jmp .out
.xhci:
    movzx eax, byte [fs:USBD_EPOUT]
    shl eax, 1
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_BOUT
    mov di, USBD_OUTST
    call usbd_xhci_ep_reset
    movzx eax, byte [fs:USBD_EPIN]
    lea eax, [eax * 2 + 1]
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_BIN
    mov di, USBD_INST
    call usbd_xhci_ep_reset
.out:
    popad
    ret

; xHCI: the disk's endpoint context EAX starts over at the next TRB of
; its ring (linear ESI, state FS:DI): Reset Endpoint (a halted one stops),
; Stop Endpoint (a running one stops), then Set TR Dequeue Pointer there,
; which also resets its data toggle. A command that does not apply to
; the endpoint's state fails, harmlessly. Keeps every register.
usbd_xhci_ep_reset:
    pushad
    mov edx, eax
    shl edx, 16
    movzx ebx, word [fs:USBD_SLOT]
    shl ebx, 24
    or edx, ebx
    xor eax, eax
    xor ebx, ebx
    push edx
    or edx, TRB_RESET_EP << 10
    call xhci_cmd
    pop edx
    push edx
    or edx, TRB_STOP_EP << 10
    call xhci_cmd
    pop edx
    or edx, TRB_SET_DEQ << 10
    call xhci_ring_ptr          ; EAX = there, with its cycle bit (DCS)
    call xhci_cmd
    popad
    ret
