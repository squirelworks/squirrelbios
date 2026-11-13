; SquirrelBIOS x86 — USB core: control transfers, enumeration, hubs
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the USB 2.0 specification, chapters 9 (device framework)
; and 11 (hubs). At POST, usb_init finds the host controllers on PCI bus
; 0, lets each one reset and enable its root ports, and enumerates what
; is attached: every device gets an address and has its device and
; configuration descriptors read; a hub is configured, its ports powered
; and reset, and the devices behind it enumerated in turn, to the
; depth USB allows. Each device gets one console line.
;
; Nothing here keeps running after POST: each controller is halted and
; reset once its bus has been listed, so it does no DMA into memory the
; OS will own. (USB keyboards and disks, B7 and B8, change that.)
;
; Work memory is USB_SEG, a 64 KB block of conventional memory that POST
; does not otherwise use before the boot loads. FS points at it in every
; routine here. Layout (offsets in USB_SEG):
;   0000h  controller-specific (UHCI: the 4 KB frame list)
;   1000h  controller-specific (UHCI: queue head, then transfer descriptors)
;   1800h  setup packet (8 bytes)
;   1900h  data buffer (USB_BUFLEN bytes)
;   1F00h  variables below

USB_SEG         equ 0x3000
USB_PHYS        equ USB_SEG << 4
USB_SETUP       equ 0x1800
USB_BUF         equ 0x1900
USB_BUFLEN      equ 256
USB_HC          equ 0x1F00          ; byte: controller type of this bus
USB_HC_UHCI     equ 1
USB_HC_OHCI     equ 2
USB_HC_EHCI     equ 3
USB_HC_XHCI     equ 4
USB_IOBASE      equ 0x1F02          ; word: controller I/O base
USB_NEXTADDR    equ 0x1F04          ; byte: next device address (1-127)
USB_BDF         equ 0x1F06          ; word: controller's PCI address
USB_TOGGLE      equ 0x1F08          ; dword: next data toggle (UHCI token bit)
USB_SAVESP      equ 0x1F0C          ; word: POST's stack pointer
USB_MMIO        equ 0x1F10          ; dword: operational registers (OHCI, EHCI, xHCI)
USB_PASS        equ 0x1F14          ; byte: 1 EHCI, 2 UHCI and OHCI
USB_CURSOR      equ 0x1F16          ; word: the console cursor (DI): the
                                    ; routines that print keep their
                                    ; registers, so each line starts and
                                    ; ends here
USB_STACK       equ 0x7C00          ; segment 0: enumeration nests deeper than
                                    ; the POST stack below 0x600 allows

USB_MAX_DEPTH   equ 5               ; hub tiers below the root hub

; Class, subclass, programming interface (PCI class 0C:03)
USB_CLASS_UHCI  equ 0x0C0300
USB_CLASS_OHCI  equ 0x0C0310
USB_CLASS_EHCI  equ 0x0C0320
USB_CLASS_XHCI  equ 0x0C0330

; Standard requests
USB_REQ_GET_STATUS     equ 0
USB_REQ_CLEAR_FEATURE  equ 1
USB_REQ_SET_FEATURE    equ 3
USB_REQ_SET_ADDRESS    equ 5
USB_REQ_GET_DESCRIPTOR equ 6
USB_REQ_SET_CONFIG     equ 9
USB_DT_DEVICE   equ 1
USB_DT_CONFIG   equ 2
USB_DT_HUB      equ 0x29
USB_CLASS_HID   equ 0x03
USB_CLASS_MSD   equ 0x08
USB_CLASS_HUB   equ 0x09

; Hub port features and status bits
HUB_PORT_RESET  equ 4
HUB_PORT_POWER  equ 8
HUB_C_CONNECT   equ 16
HUB_C_RESET     equ 20
HUB_PS_CONNECT  equ 0x0001
HUB_PS_ENABLE   equ 0x0002
HUB_PS_RESET    equ 0x0010
HUB_PS_LOWSPEED equ 0x0200
HUB_PS_HIGHSPEED equ 0x0400

; POST: find every USB host controller on bus 0 and list its devices.
; Keeps every register (ES:DI stays the console cursor).
usb_init:
    pushad
    push fs
    push word USB_SEG
    pop fs
    mov [fs:USB_SAVESP], sp
    mov sp, USB_STACK           ; SS = 0; nothing is loaded below it yet
    mov [fs:USB_CURSOR], di
    mov byte [fs:USB_NEXTADDR], 1
    xor ax, ax                  ; no keyboard, mouse or disk claimed yet
    mov bx, 0x1F40
.records:
    mov [fs:bx], ax
    add bx, 2
    cmp bx, 0x1F80
    jb .records
    call pci_present
    jc .out
    ; Two passes: EHCI first, so that it takes the high-speed devices and
    ; hands the rest to its companion UHCI / OHCI functions, which the
    ; second pass lists
    mov byte [fs:USB_PASS], 1
.pass:
    xor bx, bx
.fn:
    call pci_next
    jc .pass_done
    push di
    mov di, 0x08                ; class code in bits 31:8
    call pci_read32
    pop di
    shr eax, 8
    mov [fs:USB_BDF], bx
    cmp byte [fs:USB_PASS], 1
    jne .companions
    cmp eax, USB_CLASS_EHCI
    jne .next
    call ehci_bus
    jmp .next
.companions:
    cmp eax, USB_CLASS_UHCI
    jne .not_uhci
    call uhci_bus
    jmp .next
.not_uhci:
    cmp eax, USB_CLASS_OHCI
    jne .not_ohci
    call ohci_bus
    jmp .next
.not_ohci:
    cmp eax, USB_CLASS_XHCI
    jne .next
    call xhci_bus
.next:
    inc bl
    jnz .fn
.pass_done:
    inc byte [fs:USB_PASS]
    cmp byte [fs:USB_PASS], 2
    jbe .pass
.out:
    mov sp, [fs:USB_SAVESP]
    mov ax, [fs:USB_CURSOR]
    pop fs
    mov bp, sp
    mov [ss:bp], ax             ; DI in the PUSHAD frame: past the lines
    popad
    ret

; End a line: CR LF, and the cursor kept in USB_CURSOR. Keeps every
; register but DI.
usb_newline:
    call con_newline
    mov [fs:USB_CURSOR], di
    ret

; Start a line at USB_CURSOR: print "usb: " and the message at CS:SI.
; Keeps every register but DI (the cursor) and the flags.
usb_puts_head:
    mov di, [fs:USB_CURSOR]
    push si
    mov si, msg_usb
    call usb_puts
    pop si
usb_puts:
    push ax
    mov ah, ATTR_NORMAL
    call con_print
    pop ax
    ret

; Print AL as two hex digits, AX as four. Keep every register but DI.
usb_hex8:
    push ax
    mov ah, ATTR_NORMAL
    call con_hex8
    pop ax
    ret
usb_hex16:
    push ax
    xchg al, ah
    call usb_hex8
    xchg al, ah
    call usb_hex8
    pop ax
    ret

; Print AL (0-255) in decimal. Keeps every register but DI.
usb_dec:
    push ax
    push bx
    push cx
    xor ah, ah
    mov bl, 10
    xor cx, cx
.div:
    div bl
    push ax                     ; AH = this digit
    inc cx
    xor ah, ah
    test al, al
    jnz .div
.put:
    pop ax
    mov al, ah
    add al, '0'
    mov ah, ATTR_NORMAL
    call con_putc
    loop .put
    pop cx
    pop bx
    pop ax
    ret

; Print the controller's PCI address as "bb:dd.f". Keeps every register
; but DI.
usb_print_bdf:
    push ax
    mov ax, [fs:USB_BDF]
    xchg al, ah
    call usb_hex8               ; bus
    push ax
    mov al, ':'
    mov ah, ATTR_NORMAL
    call con_putc
    pop ax
    mov al, ah
    shr al, 3
    call usb_hex8               ; device
    mov al, '.'
    mov ah, ATTR_NORMAL
    call con_putc
    mov al, [fs:USB_BDF]
    and al, 7
    add al, '0'
    call con_putc               ; function
    pop ax
    ret

; Build a setup packet at USB_SETUP: AL = bmRequestType, AH = bRequest,
; CX = wValue, DX = wIndex, BX = wLength. Keeps every register.
usb_setup:
    mov [fs:USB_SETUP], ax
    mov [fs:USB_SETUP + 2], cx
    mov [fs:USB_SETUP + 4], dx
    mov [fs:USB_SETUP + 6], bx
    ret

; Run the control transfer whose setup packet is at USB_SETUP, data (if
; any) at USB_BUF, on this bus's controller. In: AL = device address,
; AH = speed (0 full, 1 low, 2 high), BL = endpoint 0 max packet size.
; CF set on failure. Keeps every register.
usb_control:
    cmp byte [fs:USB_HC], USB_HC_UHCI
    je uhci_control
    cmp byte [fs:USB_HC], USB_HC_OHCI
    je ohci_control
    cmp byte [fs:USB_HC], USB_HC_EHCI
    je ehci_control
    cmp byte [fs:USB_HC], USB_HC_XHCI
    je xhci_control
    stc
    ret

; A new device answers at address 0 on this bus: tell the controller, if
; it keeps its own state per device (xHCI). In: as usb_enum. CF set on
; failure. Keeps every register.
usb_attach:
    cmp byte [fs:USB_HC], USB_HC_XHCI
    je xhci_attach
    clc
    ret

; The device at address AL (or still at address 0) is listed and is not a
; hub: the controller may forget it, if it keeps state per device (xHCI)
; and the device is not the keyboard kept for after POST. Keeps every
; register.
usb_detach:
    cmp byte [fs:USB_HC], USB_HC_XHCI
    je xhci_detach
    ret

; A device was just reset on a port and answers at address 0. Give it an
; address, read what it is, print it, and enumerate a hub's ports.
; In: AH = speed (0 full, 1 low, 2 high, 3 super), DL = parent hub's address
; (0: root hub), DH = port number, CL = hub tier (0 for a root port).
; Keeps every register.
usb_enum:
    pushad
    mov bp, sp
    sub sp, 24
    ; [bp-1] address  [bp-2] speed  [bp-3] max packet  [bp-4] class
    ; [bp-5] tier     [bp-6] parent [bp-7] port
    ; A boot keyboard or mouse (B7): [bp-8] boot protocol (1 keyboard,
    ; 2 mouse, 0 neither), [bp-9] its interface, [bp-10] its interrupt IN
    ; endpoint (0: none), [bp-11] that endpoint's max packet,
    ; [bp-12] configuration value
    ; A bulk-only mass storage interface (B8): [bp-13] its bulk IN
    ; endpoint, [bp-14] its bulk OUT endpoint (0: none), [bp-16] word:
    ; their max packet, [bp-17] 1 if the interface is bulk-only SCSI,
    ; 2 if it is UAS: then [bp-13] is its status pipe, [bp-14] its
    ; command pipe, [bp-18] its data-in pipe, [bp-19] its data-out
    ; pipe, and [bp-20] the endpoint a pipe usage descriptor follows
    mov dword [bp - 12], 0
    mov dword [bp - 16], 0
    mov dword [bp - 20], 0
    mov byte [bp - 17], 0
    mov byte [bp - 8], 0
    mov [bp - 2], ah
    mov [bp - 5], cl
    mov [bp - 6], dl
    mov [bp - 7], dh
    mov al, [fs:USB_NEXTADDR]
    cmp al, 127
    ja .out                     ; the bus is full
    mov [bp - 1], al
    mov ah, [bp - 2]
    call usb_attach
    jc .fail

    ; The first 8 bytes of the device descriptor at address 0, with an
    ; 8-byte max packet: they hold bMaxPacketSize0
    mov ax, (USB_REQ_GET_DESCRIPTOR << 8) | 0x80
    mov cx, USB_DT_DEVICE << 8
    xor dx, dx
    mov bx, 8
    call usb_setup
    xor al, al
    mov ah, [bp - 2]
    mov bl, 8
    call usb_control
    jc .fail
    mov al, [fs:USB_BUF + 7]
    cmp al, 8
    jae .mps
    mov al, 8
.mps:
    mov [bp - 3], al

    ; SET_ADDRESS, then 2 ms for the device to take it
    mov ax, USB_REQ_SET_ADDRESS << 8
    movzx cx, byte [bp - 1]
    xor dx, dx
    xor bx, bx
    call usb_setup
    xor al, al
    mov ah, [bp - 2]
    mov bl, [bp - 3]
    call usb_control
    jc .fail
    inc byte [fs:USB_NEXTADDR]
    mov cx, 2
    call delay_ms

    ; The whole device descriptor at the new address
    mov ax, (USB_REQ_GET_DESCRIPTOR << 8) | 0x80
    mov cx, USB_DT_DEVICE << 8
    xor dx, dx
    mov bx, 18
    call usb_setup
    call .ctrl
    jc .fail
    mov al, [fs:USB_BUF + 4]    ; bDeviceClass
    mov [bp - 4], al
    push word [fs:USB_BUF + 10] ; idProduct
    push word [fs:USB_BUF + 8]  ; idVendor

    ; The configuration descriptor: 9 bytes for wTotalLength, then all of
    ; it (up to the buffer) for the first interface's class
    mov ax, (USB_REQ_GET_DESCRIPTOR << 8) | 0x80
    mov cx, USB_DT_CONFIG << 8
    xor dx, dx
    mov bx, 9
    call usb_setup
    call .ctrl
    jc .print                   ; listed with the device class only
    mov bx, [fs:USB_BUF + 2]
    cmp bx, USB_BUFLEN
    jbe .total
    mov bx, USB_BUFLEN
.total:
    mov ax, (USB_REQ_GET_DESCRIPTOR << 8) | 0x80
    call usb_setup
    mov cx, bx                  ; bytes read, for the walk
    call .ctrl
    jc .print
    mov al, [fs:USB_BUF + 5]    ; bConfigurationValue
    mov [bp - 12], al
    cmp byte [bp - 4], 0        ; class per interface
    jne .print
    ; Walk the descriptors for the first interface (type 4)
    xor si, si
.walk:
    movzx ax, byte [fs:USB_BUF + si]
    test ax, ax
    jz .print
    cmp byte [fs:USB_BUF + si + 1], 4
    je .iface
    add si, ax
    cmp si, cx
    jb .walk
    jmp .print
.iface:
    mov al, [fs:USB_BUF + si + 5]   ; bInterfaceClass
    mov [bp - 4], al
    cmp al, USB_CLASS_MSD
    je .msd
    cmp al, USB_CLASS_HID
    jne .print
    cmp byte [fs:USB_BUF + si + 6], 1   ; boot interface subclass
    jne .print
    mov al, [fs:USB_BUF + si + 7]       ; protocol: 1 keyboard, 2 mouse
    cmp al, 2
    ja .print
    mov [bp - 8], al
    mov al, [fs:USB_BUF + si + 2]
    mov [bp - 9], al
    ; Its first interrupt IN endpoint, before the next interface
.ep:
    movzx ax, byte [fs:USB_BUF + si]
    test ax, ax
    jz .print
    add si, ax
    cmp si, cx
    jae .print
    cmp byte [fs:USB_BUF + si + 1], 4
    je .print
    cmp byte [fs:USB_BUF + si + 1], 5
    jne .ep
    mov al, [fs:USB_BUF + si + 3]       ; bmAttributes
    and al, 3
    cmp al, 3                           ; interrupt
    jne .ep
    mov al, [fs:USB_BUF + si + 2]       ; bEndpointAddress
    test al, 0x80
    jz .ep
    and al, 0x0F
    mov [bp - 10], al
    mov al, [fs:USB_BUF + si + 4]
    mov [bp - 11], al
    jmp .print

.msd:
    ; Mass storage: SCSI commands (subclass 6) over bulk-only transport
    ; (protocol 50h). Its bulk endpoints, before the next interface.
    cmp word [fs:USB_BUF + si + 6], 0x6206
    je .uas
    cmp word [fs:USB_BUF + si + 6], 0x5006
    jne .print
    mov byte [bp - 17], 1
.bulk:
    movzx ax, byte [fs:USB_BUF + si]
    test ax, ax
    jz .print
    add si, ax
    cmp si, cx
    jae .print
    cmp byte [fs:USB_BUF + si + 1], 4
    je .print
    cmp byte [fs:USB_BUF + si + 1], 5
    jne .bulk
    mov al, [fs:USB_BUF + si + 3]       ; bmAttributes
    and al, 3
    cmp al, 2                           ; bulk
    jne .bulk
    mov ax, [fs:USB_BUF + si + 4]       ; wMaxPacketSize
    and ax, 0x07FF
    mov [bp - 16], ax
    mov al, [fs:USB_BUF + si + 2]       ; bEndpointAddress
    test al, 0x80
    jz .bulk_out
    and al, 0x0F
    mov [bp - 13], al
    jmp .bulk
.bulk_out:
    and al, 0x0F
    mov [bp - 14], al
    jmp .bulk

.uas:
    ; USB Attached SCSI (protocol 62h): four bulk endpoints, each named
    ; by the pipe usage descriptor (24h) after it
    mov byte [bp - 17], 2
.uas_next:
    movzx ax, byte [fs:USB_BUF + si]
    test ax, ax
    jz .print
    add si, ax
    cmp si, cx
    jae .print
    mov al, [fs:USB_BUF + si + 1]
    cmp al, 4
    je .print
    cmp al, 5
    je .uas_ep
    cmp al, 0x24
    jne .uas_next
    mov al, [bp - 20]
    mov ah, [fs:USB_BUF + si + 2]       ; bPipeID
    mov di, 14                          ; 1: command
    cmp ah, 1
    je .uas_pipe
    mov di, 13                          ; 2: status
    cmp ah, 2
    je .uas_pipe
    mov di, 18                          ; 3: data in
    cmp ah, 3
    je .uas_pipe
    mov di, 19                          ; 4: data out
    cmp ah, 4
    jne .uas_next
.uas_pipe:
    neg di
    mov [bp + di], al
    jmp .uas_next
.uas_ep:
    mov byte [bp - 20], 0
    mov al, [fs:USB_BUF + si + 3]       ; bmAttributes
    and al, 3
    cmp al, 2                           ; bulk
    jne .uas_next
    mov ax, [fs:USB_BUF + si + 4]       ; wMaxPacketSize
    and ax, 0x07FF
    mov [bp - 16], ax
    mov al, [fs:USB_BUF + si + 2]       ; bEndpointAddress
    and al, 0x0F
    mov [bp - 20], al
    jmp .uas_next

.print:
    ; "usb: device A on root port P: VVVV:PPPP, <class>"
    ; "usb: device A on hub H port P: ..."
    mov si, msg_usb_dev
    call usb_puts_head
    mov al, [bp - 1]
    call usb_dec
    mov si, msg_usb_on_root
    cmp byte [bp - 6], 0
    je .parent
    mov si, msg_usb_on_hub
    call usb_puts
    mov al, [bp - 6]
    call usb_dec
    mov si, msg_usb_port
.parent:
    call usb_puts
    mov al, [bp - 7]
    call usb_dec
    mov si, msg_usb_colon
    call usb_puts
    pop ax                      ; idVendor
    call usb_hex16
    push ax
    mov al, ':'
    mov ah, ATTR_NORMAL
    call con_putc
    pop ax
    pop ax                      ; idProduct
    call usb_hex16
    mov si, msg_usb_comma
    call usb_puts
    mov al, [bp - 4]
    mov si, msg_usb_hid
    cmp al, USB_CLASS_HID
    je .class_name
    mov si, msg_usb_msd
    cmp al, USB_CLASS_MSD
    je .class_name
    mov si, msg_usb_hub
    cmp al, USB_CLASS_HUB
    je .class_name
    mov si, msg_usb_class
    call usb_puts
    call usb_hex8
    jmp .printed
.class_name:
    call usb_puts
.printed:
    cmp byte [bp - 4], USB_CLASS_HUB
    jne .line
    call usb_hub                ; finishes the line with the port count
    jmp .out
.line:
    call usb_newline
    cmp byte [bp - 10], 0
    je .disk
    call usb_hid_claim          ; a boot keyboard: keep it for after POST
.disk:
    cmp byte [bp - 17], 0
    je .detach
    call usb_disk_claim         ; a USB disk: keep it to boot from
.detach:
    mov al, [bp - 1]
    call usb_detach
    jmp .out

.fail:
    mov si, msg_usb_fail
    call usb_puts_head
    mov al, [bp - 7]
    call usb_dec
    cmp byte [bp - 6], 0
    je .fail_nl
    mov si, msg_usb_on_hub
    call usb_puts
    mov al, [bp - 6]
    call usb_dec
.fail_nl:
    call usb_newline
    mov al, [bp - 1]
    call usb_detach
.out:
    mov sp, bp
    popad
    ret

; usb_control at this device's address, speed and max packet size.
.ctrl:
    mov al, [bp - 1]
    mov ah, [bp - 2]
    mov bl, [bp - 3]
    jmp usb_control

; The hub at [bp-1] (usb_enum's frame, BP unchanged): configure it, power
; its ports, then reset and enumerate each one with a device on it.
; Ends the hub's console line with ", N ports". Keeps every register.
usb_hub:
    pushad
    ; SET_CONFIGURATION 1
    mov ax, USB_REQ_SET_CONFIG << 8
    mov cx, 1
    xor dx, dx
    xor bx, bx
    call usb_setup
    call usb_enum.ctrl
    jc .bad
    ; Hub descriptor: bNbrPorts at 2, bPwrOn2PwrGood (2 ms units) at 5
    mov ax, (USB_REQ_GET_DESCRIPTOR << 8) | 0xA0
    mov cx, USB_DT_HUB << 8
    xor dx, dx
    mov bx, 7
    call usb_setup
    call usb_enum.ctrl
    jc .bad
    mov si, msg_usb_comma
    call usb_puts
    mov al, [fs:USB_BUF + 2]
    call usb_dec
    mov si, msg_usb_ports
    call usb_puts
    call usb_newline
    movzx si, byte [fs:USB_BUF + 2]     ; ports
    movzx ax, byte [fs:USB_BUF + 5]
    shl ax, 1
    add ax, 20                          ; and a margin
    push ax                             ; power-good wait, ms

    ; Power every port
    mov dx, 1
.power:
    cmp dx, si
    ja .powered
    mov ax, (USB_REQ_SET_FEATURE << 8) | 0x23
    mov cx, HUB_PORT_POWER
    xor bx, bx
    call usb_setup
    call usb_enum.ctrl
    inc dx
    jmp .power
.powered:
    pop cx
    call delay_ms

    ; Each port: a device there? Reset it and enumerate it
    mov dx, 1
.port:
    cmp dx, si
    ja .out
    call hub_port_status        ; AX = wPortStatus
    jc .next
    test al, HUB_PS_CONNECT
    jz .next
    cmp byte [bp - 5], USB_MAX_DEPTH
    jae .next                   ; too deep for USB
    mov ax, (USB_REQ_SET_FEATURE << 8) | 0x23
    mov cx, HUB_PORT_RESET
    xor bx, bx
    call usb_setup
    call usb_enum.ctrl
    jc .next
    mov cx, 50                  ; poll the reset for up to 500 ms
.reset:
    push cx
    mov cx, 10
    call delay_ms
    pop cx
    call hub_port_status
    jc .next
    test al, HUB_PS_RESET
    jz .reset_done
    loop .reset
    jmp .next
.reset_done:
    test al, HUB_PS_ENABLE
    jz .next
    push ax
    ; Clear C_PORT_RESET and C_PORT_CONNECTION
    mov ax, (USB_REQ_CLEAR_FEATURE << 8) | 0x23
    mov cx, HUB_C_RESET
    xor bx, bx
    call usb_setup
    call usb_enum.ctrl
    mov ax, (USB_REQ_CLEAR_FEATURE << 8) | 0x23
    mov cx, HUB_C_CONNECT
    call usb_setup
    call usb_enum.ctrl
    mov cx, 10                  ; reset recovery
    call delay_ms
    pop ax
    ; Enumerate: speed from the port status, parent = this hub
    mov cl, [bp - 5]
    inc cl
    mov bx, ax
    mov ah, 1
    test bx, HUB_PS_LOWSPEED
    jnz .speed
    mov ah, 2
    test bx, HUB_PS_HIGHSPEED
    jnz .speed
    mov ah, 0
.speed:
    push dx
    mov dh, dl                  ; port
    mov dl, [bp - 1]            ; this hub's address
    call usb_enum
    pop dx
.next:
    inc dx
    jmp .port
.bad:
    call usb_newline
.out:
    popad
    ret

; GET_PORT_STATUS for port DX of the hub at [bp-1]. Out: CF clear and AX
; = wPortStatus. Keeps every register but AX.
hub_port_status:
    push bx
    push cx
    mov ax, (USB_REQ_GET_STATUS << 8) | 0xA3
    xor cx, cx
    mov bx, 4
    call usb_setup
    call usb_enum.ctrl
    jc .out
    mov ax, [fs:USB_BUF]
.out:
    pop cx
    pop bx
    ret
