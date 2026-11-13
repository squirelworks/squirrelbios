; SquirrelBIOS x86 — UHCI host controller (USB 1.1)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the Universal Host Controller Interface design guide,
; revision 1.1. Every register is an I/O port (BAR4). Every entry of the
; 1024-entry frame list points at one queue head; a control transfer is a
; chain of transfer descriptors (setup, data, status) hung from it, run
; depth first, and polled until the status stage completes. Structures
; live in USB_SEG (see usb.inc):
;   0000h  frame list, 4 KB aligned
;   1000h  queue head
;   1020h  transfer descriptors, 32 bytes apart (16 used by the HC)

UHCI_BAR        equ 0x20            ; BAR4: I/O base
UHCI_CMD        equ 0x00
UHCI_STS        equ 0x02
UHCI_INTR       equ 0x04
UHCI_FRNUM      equ 0x06
UHCI_FLBASE     equ 0x08
UHCI_SOFMOD     equ 0x0C
UHCI_PORTSC1    equ 0x10
UHCI_PORTS      equ 2               ; root ports

CMD_RS          equ 0x0001
CMD_HCRESET     equ 0x0002
CMD_GRESET      equ 0x0004
CMD_CF          equ 0x0040
CMD_MAXP        equ 0x0080          ; 64-byte full-speed bandwidth reclamation
STS_HALTED      equ 0x0020

PORT_CCS        equ 0x0001
PORT_CSC        equ 0x0002          ; write 1 to clear
PORT_PE         equ 0x0004
PORT_PEC        equ 0x0008          ; write 1 to clear
PORT_LSDA       equ 0x0100
PORT_PR         equ 0x0200
PORT_WC         equ PORT_CSC | PORT_PEC

UHCI_FL         equ 0x0000
UHCI_QH         equ 0x1000
UHCI_TD         equ 0x1020
UHCI_TD_SIZE    equ 32
UHCI_TD_MAX     equ 34              ; setup, 32 data, status

TD_T            equ 0x00000001      ; link: terminate
TD_Q            equ 0x00000002      ; link: points at a queue head
TD_VF           equ 0x00000004      ; link: depth first
TD_ACTIVE       equ 0x00800000
TD_ERRORS       equ 0x007E0000 & ~0x00080000   ; stalled, buffer, babble, CRC/timeout, bitstuff
TD_LS           equ 0x04000000
TD_CERR3        equ 0x18000000
TD_SPD          equ 0x20000000      ; short packet detect
PID_SETUP       equ 0x2D
PID_IN          equ 0x69
PID_OUT         equ 0xE1

; The UHCI function at USB_BDF (BX): reset it, then enumerate what each
; root port has. The controller is halted and reset again at the end.
; FS = USB_SEG. Keeps every register.
uhci_bus:
    pushad
    mov byte [fs:USB_HC], USB_HC_UHCI
    mov di, UHCI_BAR
    call pci_read32
    test al, 1                  ; must be an I/O BAR
    jz .out
    and ax, 0xFFE0
    jz .out
    mov [fs:USB_IOBASE], ax
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_IO | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    mov dx, [fs:USB_IOBASE]
    ; Global reset for 10 ms, then a controller reset
    mov ax, CMD_GRESET
    out dx, ax
    mov cx, 10
    call delay_ms
    xor ax, ax
    out dx, ax
    mov ax, CMD_HCRESET
    out dx, ax
    mov cx, 100
.hcreset:
    in ax, dx
    test ax, CMD_HCRESET
    jz .reset_done
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .hcreset
    jmp .out
.reset_done:
    ; Frame list: every frame points at the one queue head, which is empty
    xor di, di
    mov eax, (USB_PHYS + UHCI_QH) | TD_Q
.fl:
    mov [fs:UHCI_FL + di], eax
    add di, 4
    cmp di, 4096
    jb .fl
    mov dword [fs:UHCI_QH], TD_T            ; horizontal: nothing after it
    mov dword [fs:UHCI_QH + 4], TD_T        ; element: no transfer yet
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_INTR
    xor ax, ax
    out dx, ax                  ; no interrupts
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_FRNUM
    out dx, ax
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_FLBASE
    mov eax, USB_PHYS + UHCI_FL
    out dx, eax
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_SOFMOD
    mov al, 64                  ; 1 ms frames
    out dx, al
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_STS
    mov ax, 0x003F              ; clear every status bit
    out dx, ax
    mov dx, [fs:USB_IOBASE]
    mov ax, CMD_RS | CMD_CF | CMD_MAXP
    out dx, ax

    ; "usb: UHCI at bb:dd.f"
    mov si, msg_usb_uhci
    call usb_puts_head
    call usb_print_bdf
    call usb_newline

    ; Each root port with something connected: reset, enable, enumerate
    xor cx, cx
.port:
    cmp cx, UHCI_PORTS
    jae .stop
    mov dx, [fs:USB_IOBASE]
    add dx, UHCI_PORTSC1
    add dx, cx
    add dx, cx
    in ax, dx
    test ax, PORT_CCS
    jz .next
    call uhci_port_reset        ; AX = PORTSC after it
    jc .next
    push cx
    in ax, dx
    mov bl, 0
    test ax, PORT_LSDA
    jz .full
    mov bl, 1
.full:
    mov ah, bl                  ; speed
    mov dh, cl
    inc dh                      ; port number, from 1
    mov dl, 0                   ; parent: root hub
    xor cl, cl                  ; tier 0
    call usb_enum
    pop cx
.next:
    inc cx
    jmp .port

.stop:
    ; The keyboard kept for after POST is here: keep the controller
    ; running, with only that keyboard's transfer
    mov dx, [fs:USB_IOBASE]
    call usb_hid_here
    jc .halt_it
    call uhci_hid_start
    call usbd_keep              ; the disk's copy of the controller's state
    call usb_hid_announce       ; after: it needs ES:DI, the console cursor
    jmp .out
.halt_it:
    ; Halt, wait for it, then reset the controller: no DMA after POST
    xor ax, ax
    out dx, ax
    add dx, UHCI_STS
    mov cx, 10
.halt:
    in ax, dx
    test ax, STS_HALTED
    jnz .halted
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .halt
.halted:
    mov dx, [fs:USB_IOBASE]
    mov ax, CMD_HCRESET
    out dx, ax
.out:
    popad
    ret

; Reset the root port whose PORTSC is at DX and enable it. CF set if it
; does not come up enabled. Keeps every register.
uhci_port_reset:
    push ax
    push cx
    in ax, dx
    and ax, ~PORT_WC
    or ax, PORT_PR
    out dx, ax
    mov cx, 50                  ; reset signalling, 50 ms
    call delay_ms
    in ax, dx
    and ax, ~(PORT_WC | PORT_PR)
    out dx, ax
    mov cx, 1
    call delay_ms
    mov cx, 10
.enable:
    in ax, dx
    test ax, PORT_CCS
    jz .fail                    ; unplugged meanwhile
    test ax, PORT_PE
    jnz .enabled
    or ax, PORT_PE              ; also clears CSC / PEC if set
    out dx, ax
    push cx
    mov cx, 10
    call delay_ms
    pop cx
    loop .enable
.fail:
    pop cx
    pop ax
    stc
    ret
.enabled:
    or ax, PORT_WC              ; clear the change bits
    and ax, ~PORT_PR
    out dx, ax
    mov cx, 10                  ; reset recovery
    call delay_ms
    pop cx
    pop ax
    clc
    ret

; usb_control for UHCI: AL = address, AH bit 0 = low speed, BL = max
; packet size of endpoint 0; setup packet at USB_SETUP, data at USB_BUF.
; CF set on a stall, an error or a timeout. Keeps every register.
uhci_control:
    pushad
    ; EBP = the token's address field, ESI = the status field template
    movzx ebp, al
    shl ebp, 8
    mov esi, TD_ACTIVE | TD_CERR3
    test ah, 1
    jz .fs
    or esi, TD_LS
.fs:
    movzx ecx, bl               ; ECX = max packet
    movzx edx, word [fs:USB_SETUP + 6]  ; EDX = bytes of data
    cmp edx, USB_BUFLEN
    ja .fail
    mov di, UHCI_TD
    ; Setup stage: 8 bytes, DATA0
    mov eax, (7 << 21) | PID_SETUP
    or eax, ebp
    mov ebx, USB_PHYS + USB_SETUP
    call .td
    ; Data stage: max-packet pieces from DATA1 on
    mov ebx, USB_PHYS + USB_BUF
    mov dword [fs:USB_TOGGLE], 1 << 19
.data:
    test edx, edx
    jz .status
    mov eax, ecx
    cmp eax, edx
    jbe .piece
    mov eax, edx
.piece:
    sub edx, eax
    push eax
    dec eax
    shl eax, 21
    or eax, ebp
    or eax, [fs:USB_TOGGLE]
    xor dword [fs:USB_TOGGLE], 1 << 19
    test byte [fs:USB_SETUP], 0x80
    mov al, PID_IN
    jnz .pid
    mov al, PID_OUT
.pid:
    call .td
    pop eax
    add ebx, eax
    jmp .data
.status:
    ; Status stage: no data, DATA1, the other direction (IN after OUT or
    ; no data)
    mov eax, (0x7FF << 21) | (1 << 19)
    or eax, ebp
    mov al, PID_IN
    test byte [fs:USB_SETUP], 0x80
    jz .last
    cmp word [fs:USB_SETUP + 6], 0
    je .last
    mov al, PID_OUT
.last:
    xor ebx, ebx
    call .td
    sub di, UHCI_TD_SIZE
    mov dword [fs:di], TD_T     ; the chain ends here
    push di                     ; the status TD
    ; Hang the chain from the queue head: the controller runs it
    mov dword [fs:UHCI_QH + 4], USB_PHYS + UHCI_TD
    ; Poll: done when the status TD is inactive; any TD inactive with an
    ; error bit stops it early. Up to 1000 ms.
    mov cx, 1000
.poll:
    pop di
    push di
    test dword [fs:di + 4], TD_ACTIVE
    jz .finished
    mov bx, UHCI_TD
.scan:
    mov eax, [fs:bx + 4]
    test eax, TD_ACTIVE
    jnz .scan_next
    test eax, TD_ERRORS
    jnz .stop_fail
.scan_next:
    add bx, UHCI_TD_SIZE
    cmp bx, di
    jbe .scan
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .poll
    jmp .stop_fail
.finished:
    pop di
    test dword [fs:di + 4], TD_ERRORS
    jnz .unhang
    mov dword [fs:UHCI_QH + 4], TD_T
    popad
    clc
    ret
.stop_fail:
    pop di
.unhang:
    mov dword [fs:UHCI_QH + 4], TD_T
    mov cx, 2                   ; let the controller leave the chain
    call delay_ms
.fail:
    popad
    stc
    ret

; Write a TD at FS:DI and move DI to the next: token EAX (PID in AL),
; buffer EBX, status from ESI. Its link points at the next slot, depth
; first. Keeps every register but DI.
.td:
    push eax
    movzx eax, di
    add eax, USB_PHYS + UHCI_TD_SIZE    ; next slot, physical
    or eax, TD_VF
    mov [fs:di], eax
    mov [fs:di + 4], esi
    pop eax
    mov [fs:di + 8], eax
    mov [fs:di + 12], ebx
    add di, UHCI_TD_SIZE
    ret
