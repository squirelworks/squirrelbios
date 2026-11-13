; SquirrelBIOS x86 — USB keyboard and mouse after POST (legacy emulation)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the USB Device Class Definition for HID 1.11 (boot
; protocol, appendix B) and the HID Usage Tables (keyboard page). The
; first boot-protocol keyboard and the first boot-protocol mouse that
; usb_enum finds, on one controller that can keep running, are
; configured, switched to the boot protocol and told to report only on
; changes. When that controller's bus has been listed, it is not halted:
; its schedule moves into the EBDA (pages at +5000h) and runs just their
; interrupt IN transfers (UHCI: a frame list whose every frame holds
; their queue heads, each with one TD; OHCI: an HCCA whose 32 interrupt
; lists all hold their endpoint descriptors, each with one TD and an
; empty tail). IRQ0 looks at the transfers 18.2 times a second. A
; finished keyboard report's newly pressed keys go into the BDA keyboard
; buffer as the 8042 path would put them; a mouse report goes to the
; handler a program set with INT 15h AX=C207h, as a PS/2 packet. The
; transfer is then armed again. A device holds its next report until it
; is asked, so nothing is lost between ticks.
;
; On xHCI the controller already works from the EBDA (xhci.inc) and each
; endpoint is added to its slot (Configure Endpoint). Each one's ring
; holds 15 transfers at once, each into its own buffer, and posts no
; events: IRQ0 takes every buffer that has been written and hands its
; transfer back, without touching the controller's registers.
;
; Two devices, 0 the keyboard and 1 the mouse. EBDA layout from +5000h
; (offsets in the EBDA segment), device D's area at 6000h + D * 80h:
;   5000h  UHCI frame list, 4 KB, every entry the first queue head;
;          OHCI: the HCCA (256 bytes)
;   +00h   queue head (UHCI) or endpoint descriptor (OHCI)
;   +20h   transfer descriptor; +30h OHCI's empty tail TD
;   +40h   report buffer (8 bytes); +48h previous report (keyboard)
; and the variables in the EBDA's first page, at 0F00h: an xHCI that
; works from the EBDA during POST (xhci.inc) overwrites every USB page,
; and must not be taken for a running device by IRQ0.

USBK_SEG        equ EBDA_BASE >> 4
USBK_FL         equ 0x5000
USBK_AREA       equ 0x6000          ; device 0; device 1 at +USBK_AREA_SZ
USBK_AREA_SZ    equ 0x80
USBK_QH         equ 0x00            ; in an area
USBK_TD         equ 0x20
USBK_TAIL       equ 0x30
USBK_RBUF       equ 0x40
USBK_BUF        equ USBK_AREA + USBK_RBUF           ; the keyboard's
USBK_PREV       equ USBK_AREA + 0x48
USBM_BUF        equ USBK_AREA + USBK_AREA_SZ + USBK_RBUF
USBK_KIND       equ 0x0F00          ; byte: 0 none, 1 UHCI, 2 OHCI, 3 xHCI
USBK_DEVS       equ 0x0F01          ; byte: bit D set when device D runs
USBK_VARS       equ 0x0F10          ; device D's at +D * 10h:
USBK_TOKEN      equ 0x00            ; dword: next TD token (UHCI)
USBK_STATUS     equ 0x04            ; dword: TD status to arm with (UHCI)
USBK_XIDX       equ 0x08            ; byte: next TRB to look at (xHCI)
USBK_XCYC       equ 0x09            ; byte: and the lap's cycle bit
USBK_XRING      equ 0x0A            ; word: its ring (EBDA offset, xHCI)
USBK_XBUF       equ 0x0C            ; word: its buffers (EBDA offset)
; INT 15h C2xx state
USBM_FLAGS      equ 0x0F30          ; byte: bit 0 enabled, bit 4 scaling 2:1
USBM_PACKET     equ 0x0F31          ; byte: packet size (C205h)
USBM_RATE       equ 0x0F32          ; byte: sample rate, reports per second
USBM_RES        equ 0x0F33          ; byte: resolution (0-3)
USBM_BUTTONS    equ 0x0F34          ; byte: the last report's buttons
USBM_HANDLER    equ 0x0F38          ; dword: far handler (0: none)
USBK_KIND_UHCI  equ 1
USBK_KIND_OHCI  equ 2
USBK_KIND_XHCI  equ 3

%if EBDA_SIZE < 0x7000
%error "USB keyboard needs a 28 KB or larger EBDA"
%endif

; The claimed devices (USB_SEG variables), device D's at +D * 10h
USB_KBD_ADDR    equ 0x1F40          ; byte: its address (0: none yet)
USB_KBD_EP      equ 0x1F41          ; byte: interrupt IN endpoint
USB_KBD_MPS     equ 0x1F42          ; byte: that endpoint's max packet
USB_KBD_SPEED   equ 0x1F43          ; byte: as usb_enum's AH
USB_KBD_BDF     equ 0x1F44          ; word: their controller (both devices)
USB_MOU_ADDR    equ 0x1F50          ; the mouse: as the keyboard's

HID_REQ_SET_IDLE     equ 0x0A
HID_REQ_SET_PROTOCOL equ 0x0B

; From usb_enum, with its frame at BP: the device just listed has a boot
; interface with an interrupt IN endpoint. If it is a keyboard or mouse,
; none of that kind is claimed yet, any device claimed before is on this
; controller, and this controller can keep running, configure it for
; boot reports and remember it. Keeps every register.
usb_hid_claim:
    pushad
    movzx bx, byte [bp - 8]     ; 1 keyboard, 2 mouse
    dec bx
    cmp bx, 1
    ja .out
    mov di, bx
    shl di, 4
    add di, USB_KBD_ADDR        ; FS:DI = its record
    cmp byte [fs:di], 0
    jne .out
    mov al, [fs:USB_KBD_ADDR]
    or al, [fs:USB_MOU_ADDR]
    or al, [fs:USBD_ADDR]
    jz .first
    mov ax, [fs:USB_BDF]
    cmp ax, [fs:USB_KBD_BDF]
    jne .out                    ; the other one runs elsewhere
.first:
    cmp byte [fs:USB_HC], USB_HC_UHCI
    je .can
    cmp byte [fs:USB_HC], USB_HC_OHCI
    je .can
    cmp byte [fs:USB_HC], USB_HC_XHCI
    jne .out
    cmp dword [fs:XHCI_BASE], XHCI_EBDA
    jne .out                    ; its block will not survive POST
    mov al, [bp - 1]
    mov cl, [bp - 10]
    mov dl, [bp - 11]
    call xhci_hid_config        ; the controller must know the endpoint
    jc .out
.can:
    ; SET_CONFIGURATION
    mov ax, USB_REQ_SET_CONFIG << 8
    movzx cx, byte [bp - 12]
    xor dx, dx
    push bx
    xor bx, bx
    call usb_setup
    pop bx
    call usb_enum.ctrl
    jc .out
    ; SET_PROTOCOL: boot
    mov ax, (HID_REQ_SET_PROTOCOL << 8) | 0x21
    xor cx, cx
    movzx dx, byte [bp - 9]
    push bx
    xor bx, bx
    call usb_setup
    pop bx
    call usb_enum.ctrl
    jc .out
    ; SET_IDLE 0: a report only when something changes. Optional.
    mov ax, (HID_REQ_SET_IDLE << 8) | 0x21
    push bx
    xor bx, bx
    call usb_setup
    pop bx
    call usb_enum.ctrl
    mov al, [bp - 1]
    mov [fs:di], al
    mov al, [bp - 10]
    mov [fs:di + 1], al
    mov al, [bp - 11]
    mov [fs:di + 2], al
    mov al, [bp - 2]
    mov [fs:di + 3], al
    mov ax, [fs:USB_BDF]
    mov [fs:USB_KBD_BDF], ax
.out:
    popad
    ret

; CF clear if the devices claimed on a bus (keyboard, mouse, disk) are
; on this controller (USB_BDF) and not running yet. Keeps every register.
usb_hid_here:
    push ax
    mov al, [fs:USB_KBD_ADDR]
    or al, [fs:USB_MOU_ADDR]
    or al, [fs:USBD_ADDR]
    jz .no
    push es
    push word USBK_SEG
    pop es
    cmp byte [es:USBK_KIND], 0
    pop es
    jne .no
    mov ax, [fs:USB_KBD_BDF]
    cmp ax, [fs:USB_BDF]
    jne .no
    pop ax
    clc
    ret
.no:
    pop ax
    stc
    ret

; "usb: keyboard is device N", "usb: mouse is device N", "usb: disk is
; device N", for those claimed. Keeps every register but DI (the cursor).
usb_hid_announce:
    push ax
    push si
    mov al, [fs:USB_KBD_ADDR]
    test al, al
    jz .mouse
    mov si, msg_usb_kbd
    call usb_puts_head
    call usb_dec
    call usb_newline
.mouse:
    mov al, [fs:USB_MOU_ADDR]
    test al, al
    jz .disk
    mov si, msg_usb_mouse
    call usb_puts_head
    call usb_dec
    call usb_newline
.disk:
    mov al, [fs:USBD_ADDR]
    test al, al
    jz .out
    mov si, msg_usb_disk
    call usb_puts_head
    call usb_dec
    call usb_newline
.out:
    pop si
    pop ax
    ret

; Device BX (0 keyboard, 1 mouse): SI = its area in the EBDA, DI = its
; variables there. Keeps every other register.
usb_hid_dev:
    mov si, bx
    shl si, 7
    add si, USBK_AREA
    mov di, bx
    shl di, 4
    add di, USBK_VARS
    ret

; Common start, ES = USBK_SEG: no previous keyboard report, the mouse
; disabled with no handler and its defaults, kind AL and the devices
; claimed marked running. Keeps every register.
usb_hid_begin:
    push eax
    mov [es:USBK_KIND], al
    xor eax, eax
    mov [es:USBK_PREV], eax
    mov [es:USBK_PREV + 4], eax
    mov [es:USBM_HANDLER], eax
    mov byte [es:USBM_FLAGS], 0
    mov byte [es:USBM_PACKET], 3
    mov byte [es:USBM_RATE], 100
    mov byte [es:USBM_RES], 2
    mov byte [es:USBM_BUTTONS], 0
    xor al, al
    cmp byte [fs:USB_KBD_ADDR], 0
    je .mouse
    or al, 1
.mouse:
    cmp byte [fs:USB_MOU_ADDR], 0
    je .set
    or al, 2
.set:
    mov [es:USBK_DEVS], al
    pop eax
    ret

; UHCI at I/O base DX, its bus listed: move its schedule into the EBDA
; with the devices' interrupt transfers as the only work, and run it.
; Keeps every register.
uhci_hid_start:
    pushad
    push es
    push word USBK_SEG
    pop es
    ; Halt it while the frame list moves
    xor ax, ax
    out dx, ax
    push dx
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
    pop dx
    mov al, USBK_KIND_UHCI
    call usb_hid_begin
    ; A USB disk (msd.inc) gets the last queue head, with no work until a
    ; command hangs its TDs there
    mov ebp, TD_T               ; EBP = the next queue head's link
    cmp byte [fs:USBD_ADDR], 0
    je .devices
    mov dword [es:USBD_UQH], TD_T
    mov dword [es:USBD_UQH + 4], TD_T
    mov ebp, (EBDA_BASE + USBD_UQH) | TD_Q
.devices:
    ; Each device: a queue head with one TD, IN from its endpoint, DATA0
    ; first. The queue heads are chained, the last one ends the frame.
    mov bx, 1
.dev:
    push bx
    shl bx, 4
    add bx, USB_KBD_ADDR        ; FS:BX = its record
    cmp byte [fs:bx], 0
    pop bx
    je .next
    push bx
    call usb_hid_dev
    shl bx, 4
    add bx, USB_KBD_ADDR
    mov [es:si + USBK_QH], ebp
    mov eax, TD_ACTIVE | TD_CERR3
    cmp byte [fs:bx + 3], 1
    jne .fs
    or eax, TD_LS
.fs:
    mov [es:di + USBK_STATUS], eax
    movzx eax, byte [fs:bx + 2]
    cmp al, 8
    jbe .len
    mov al, 8                   ; a boot report is at most 8 bytes
.len:
    dec eax
    shl eax, 21
    movzx ecx, byte [fs:bx + 1]
    shl ecx, 15
    or eax, ecx
    movzx ecx, byte [fs:bx]
    shl ecx, 8
    or eax, ecx
    mov al, PID_IN
    mov [es:di + USBK_TOKEN], eax
    pop bx
    call usb_hid_arm
    movzx ebp, si
    add ebp, EBDA_BASE + USBK_QH
    or ebp, TD_Q
.next:
    dec bx
    jns .dev
    ; Every frame: the first queue head
    xor di, di
.fl:
    mov [es:USBK_FL + di], ebp
    add di, 4
    cmp di, 4096
    jb .fl
    ; Run from the new frame list
    push dx
    add dx, UHCI_FLBASE
    mov eax, EBDA_BASE + USBK_FL
    out dx, eax
    pop dx
    push dx
    add dx, UHCI_FRNUM
    xor ax, ax
    out dx, ax
    pop dx
    push dx
    add dx, UHCI_STS
    mov ax, 0x003F
    out dx, ax
    pop dx
    mov ax, CMD_RS | CMD_CF | CMD_MAXP
    out dx, ax
    pop es
    popad
    ret

; OHCI at GS:ESI (flat GS), its bus listed: point its HCCA at the EBDA,
; with the devices' endpoints on every interrupt list, and run only the
; periodic list. Keeps every register.
ohci_hid_start:
    pushad
    push es
    push word USBK_SEG
    pop es
    ; Stop the lists, then the new HCCA
    mov dword [gs:esi + OHCI_CONTROL], OCTL_HCFS_OPER
    mov cx, 2
    call delay_ms
    push esi
    mov al, USBK_KIND_OHCI
    call usb_hid_begin
    ; Each device: an endpoint descriptor with one TD and an empty tail,
    ; chained; EBP = the next one
    xor ebp, ebp
    mov bx, 1
.dev:
    push bx
    shl bx, 4
    add bx, USB_KBD_ADDR
    cmp byte [fs:bx], 0
    pop bx
    je .next
    push bx
    call usb_hid_dev
    shl bx, 4
    add bx, USB_KBD_ADDR
    ; Address, endpoint, IN, speed, max packet (8 at most)
    movzx eax, byte [fs:bx + 2]
    cmp al, 8
    jbe .mps
    mov al, 8
.mps:
    shl eax, 16
    or eax, 2 << 11             ; direction IN
    movzx ecx, byte [fs:bx + 1]
    shl ecx, 7
    or eax, ecx
    or al, [fs:bx]
    cmp byte [fs:bx + 3], 1
    jne .fs
    or eax, ED_LOWSPEED
.fs:
    mov [es:si + USBK_QH], eax
    movzx eax, si
    add eax, EBDA_BASE
    lea ecx, [eax + USBK_TAIL]
    mov [es:si + USBK_QH + 4], ecx              ; TailP
    lea ecx, [eax + USBK_TD]
    mov [es:si + USBK_QH + 8], ecx              ; HeadP: DATA0
    mov [es:si + USBK_QH + 12], ebp             ; NextED
    mov ebp, eax
    pop bx
    call usb_hid_arm
.next:
    dec bx
    jns .dev
    pop esi
    xor di, di
.hcca:
    mov [es:USBK_FL + di], ebp
    add di, 4
    cmp di, 32 * 4
    jb .hcca
.zero:
    mov dword [es:USBK_FL + di], 0
    add di, 4
    cmp di, 256
    jb .zero
    mov dword [gs:esi + OHCI_HCCA], EBDA_BASE + USBK_FL
    mov eax, OCTL_HCFS_OPER | OCTL_PLE
    cmp byte [fs:USBD_ADDR], 0
    je .run
    call usbd_ohci_eds          ; the USB disk's, on the bulk list
    or eax, OCTL_BLE
.run:
    mov [gs:esi + OHCI_CONTROL], eax
    pop es
    popad
    ret

; Arm device BX's transfer again (ES = USBK_SEG, UHCI or OHCI). Keeps
; every register.
usb_hid_arm:
    push eax
    push si
    push di
    call usb_hid_dev
    cmp byte [es:USBK_KIND], USBK_KIND_OHCI
    je .ohci
    mov dword [es:si + USBK_TD], TD_T
    mov eax, [es:di + USBK_TOKEN]
    mov [es:si + USBK_TD + 8], eax
    movzx eax, si
    add eax, EBDA_BASE + USBK_RBUF
    mov [es:si + USBK_TD + 12], eax
    mov eax, [es:di + USBK_STATUS]
    mov [es:si + USBK_TD + 4], eax
    movzx eax, si
    add eax, EBDA_BASE + USBK_TD
    mov [es:si + USBK_QH + 4], eax      ; last: now the HC's
    jmp .out
.ohci:
    ; A general TD for 8 bytes IN, the toggle from the endpoint, then the
    ; empty tail; HeadP back to it, keeping the toggle carry and clearing
    ; a halt
    mov dword [es:si + USBK_TD], GTD_CC_NEW | GTD_NOINT | GTD_IN | GTD_ROUNDING
    movzx eax, si
    add eax, EBDA_BASE + USBK_RBUF
    mov [es:si + USBK_TD + 4], eax
    add eax, 7
    mov [es:si + USBK_TD + 12], eax
    movzx eax, si
    add eax, EBDA_BASE + USBK_TAIL
    mov [es:si + USBK_TD + 8], eax
    xor eax, eax
    mov [es:si + USBK_TAIL], eax
    mov [es:si + USBK_TAIL + 4], eax
    mov [es:si + USBK_TAIL + 8], eax
    mov [es:si + USBK_TAIL + 12], eax
    mov eax, [es:si + USBK_QH + 8]
    and eax, 2                  ; toggle carry
    or ax, si
    add eax, EBDA_BASE + USBK_TD
    mov [es:si + USBK_QH + 8], eax
.out:
    pop di
    pop si
    pop eax
    ret

; From IRQ0: for each device running, if its transfer has finished, take
; its report and arm it again. Runs with interrupts off, in real or
; virtual-8086 mode. Keeps every register and the flags.
usb_hid_poll:
    pushf
    pushad
    push es
    push word USBK_SEG
    pop es
    mov al, [es:USBK_KIND]
    test al, al
    jz .out
    mov bx, 1
.dev:
    bt word [es:USBK_DEVS], bx
    jnc .next
    cmp al, USBK_KIND_XHCI
    je .xhci
    call usb_hid_dev
    cmp al, USBK_KIND_OHCI
    je .ohci
    ; UHCI
    mov edx, [es:si + USBK_TD + 4]
    test edx, TD_ACTIVE
    jnz .next                   ; still waiting for the device
    test edx, TD_ERRORS
    jnz .again                  ; same toggle, try again
    xor dword [es:di + USBK_TOKEN], 1 << 19
.report:
    call usb_hid_take
.again:
    call usb_hid_arm
    jmp .next
.ohci:
    ; Done when the endpoint's head has moved on to the tail (or halted)
    mov edx, [es:si + USBK_QH + 8]
    test dl, ED_HALTED
    jnz .again
    and edx, ~0x0F
    movzx ecx, si
    add ecx, EBDA_BASE + USBK_TAIL
    cmp edx, ecx
    jne .next
    test dword [es:si + USBK_TD], 0xF0000000    ; condition code
    jnz .again
    jmp .report
.xhci:
    call usb_hid_xpoll
.next:
    dec bx
    jns .dev
.out:
    pop es
    popad
    popf
    ret

; Device BX's report is in its area's buffer: hand it on. Keeps every
; register.
usb_hid_take:
    test bx, bx
    jz usb_hid_report
    jmp usb_mouse_report

; usb_hid_poll for xHCI, device BX (ES = USBK_SEG). The controller fills
; the ring's buffers in order and posts no events; a buffer whose first
; byte is no longer FFh (a keyboard's modifiers or a mouse's buttons
; never are) holds a report. Each one is taken, its buffer set back to
; all ones, and its TRB handed back for the next lap (the other cycle
; bit). Once TRB 0 of a lap is done the controller has followed the
; link, so the link gets this lap's cycle bit for the next time round.
; Reading memory only, this works in virtual-8086 mode too. Keeps every
; register.
usb_hid_xpoll:
    pushad
    call usb_hid_dev            ; SI = its area, DI = its variables
    mov bp, si
    mov cx, XHCI_INT_TRBS - 1   ; at most one lap per tick
.look:
    movzx dx, byte [es:di + USBK_XIDX]
    mov si, dx
    shl si, 3
    add si, [es:di + USBK_XBUF]
    cmp byte [es:si], 0xFF
    je .out                     ; still waiting for the device
    mov eax, [es:si]
    mov [es:bp + USBK_RBUF], eax
    mov eax, [es:si + 4]
    mov [es:bp + USBK_RBUF + 4], eax
    mov dword [es:si], -1
    mov dword [es:si + 4], -1
    call usb_hid_take
    mov si, [es:di + USBK_XRING]
    movzx eax, byte [es:di + USBK_XCYC]
    test dx, dx
    jnz .repost
    or eax, TRB_LINK_INT
    mov [es:si + (XHCI_INT_TRBS - 1) * 16 + 12], eax
    movzx eax, byte [es:di + USBK_XCYC]
.repost:
    xor al, 1
    or eax, TRB_NORMAL << 10
    push si
    shl dx, 4
    add si, dx
    shr dx, 4
    mov [es:si + 12], eax
    pop si
    inc dx
    cmp dx, XHCI_INT_TRBS - 1
    jb .store
    xor dx, dx
    xor byte [es:di + USBK_XCYC], 1
.store:
    mov [es:di + USBK_XIDX], dl
    dec cx
    jnz .look
.out:
    popad
    ret

; A boot mouse report at ES:USBM_BUF (buttons, X, Y): if the mouse is
; enabled and has a handler (INT 15h C200h, C207h), call it the way the
; PS/2 BIOS does, with four words pushed: status (buttons, bit 3 set,
; the X and Y sign bits), X, Y (up is positive, as on PS/2) and 0.
; Keeps every register.
usb_mouse_report:
    pushad
    push ds
    push es
    mov al, [es:USBM_BUF]
    and al, 7
    mov [es:USBM_BUTTONS], al
    test byte [es:USBM_FLAGS], 1
    jz .out
    cmp dword [es:USBM_HANDLER], 0
    je .out
    or al, 8
    movsx dx, byte [es:USBM_BUF + 1]    ; X
    movsx cx, byte [es:USBM_BUF + 2]
    neg cx                              ; Y: USB down, PS/2 up
    cmp cx, 127                         ; -128 up: as 127, so that a
    jle .yclamp                         ; handler reading only the low
    mov cx, 127                         ; byte still moves up
.yclamp:
    test dx, dx
    jns .xpos
    or al, 0x10
.xpos:
    test cx, cx
    jns .ypos
    or al, 0x20
.ypos:
    xor ah, ah
    xor dh, dh
    xor ch, ch
    push ax
    push dx
    push cx
    push word 0
    call far [es:USBM_HANDLER]
    add sp, 8
.out:
    pop es
    pop ds
    popad
    ret

; INT 15h AH=C2h, the PS/2 pointing device interface, for the USB mouse
; (from int15_handler: BP pushed, the caller's FLAGS at [BP+6]). AL picks
; the function: 00h enable (BH=1) or disable (BH=0), 01h reset, 02h
; sample rate (BH 0-6), 03h resolution (BH 0-3), 04h device type, 05h
; initialize (BH = packet size 1-8), 06h status (BH=0) or scaling 1:1 /
; 2:1 (BH=1/2), 07h handler ES:BX. CF clear and AH=0 on success; CF set
; and AH = 1 bad function, 2 bad input, 3 no mouse, 5 no handler.
usb_mouse_int15:
    push ds
    push word USBK_SEG
    pop ds
    test byte [USBK_DEVS], 2
    jz .no_mouse
    cmp al, 7
    ja .bad_fn
    cmp al, 0
    je .enable
    cmp al, 1
    je .reset
    cmp al, 2
    je .rate
    cmp al, 3
    je .res
    cmp al, 4
    je .type
    cmp al, 5
    je .init
    cmp al, 6
    je .ext
    ; 07h: the handler (0:0 removes it)
    mov [USBM_HANDLER], bx
    mov [USBM_HANDLER + 2], es
    jmp .ok
.enable:
    cmp bh, 1
    ja .bad_input
    jb .disable
    cmp dword [USBM_HANDLER], 0
    je .no_handler
    or byte [USBM_FLAGS], 1
    jmp .ok
.disable:
    and byte [USBM_FLAGS], ~1
    jmp .ok
.reset:
    call .defaults
    mov bx, 0x00AA              ; device ID 0, then the reset answer
    jmp .ok
.rate:
    cmp bh, 6
    ja .bad_input
    push bx
    movzx bx, bh
    mov bl, [cs:.rates + bx]
    mov [USBM_RATE], bl
    pop bx
    jmp .ok
.res:
    cmp bh, 3
    ja .bad_input
    mov [USBM_RES], bh
    jmp .ok
.type:
    xor bh, bh                  ; a standard mouse
    jmp .ok
.init:
    cmp bh, 1
    jb .bad_input
    cmp bh, 8
    ja .bad_input
    mov [USBM_PACKET], bh
    call .defaults
    jmp .ok
.ext:
    cmp bh, 0
    jne .scaling
    ; BL: bit 5 enabled, bit 4 scaling 2:1, bit 2 left, bit 0 right
    mov bl, [USBM_FLAGS]
    shl bl, 5
    and bl, 0x20
    test byte [USBM_FLAGS], 0x10
    jz .ext_btn
    or bl, 0x10
.ext_btn:
    test byte [USBM_BUTTONS], 1
    jz .ext_right
    or bl, 0x04
.ext_right:
    test byte [USBM_BUTTONS], 2
    jz .ext_out
    or bl, 0x01
.ext_out:
    mov cl, [USBM_RES]
    mov dl, [USBM_RATE]
    jmp .ok
.scaling:
    cmp bh, 1
    jne .scale21
    and byte [USBM_FLAGS], ~0x10
    jmp .ok
.scale21:
    cmp bh, 2
    jne .bad_input
    or byte [USBM_FLAGS], 0x10
    jmp .ok

.ok:
    xor ah, ah
    and word [bp + 6], 0xFFFE
    pop ds
    pop bp
    iret
.bad_fn:
    mov ah, 1
    jmp .err
.bad_input:
    mov ah, 2
    jmp .err
.no_mouse:
    mov ah, 3
    jmp .err
.no_handler:
    mov ah, 5
.err:
    or word [bp + 6], 1
    pop ds
    pop bp
    iret

; Reset state: disabled, 100 reports a second, 4 counts per mm (2),
; scaling 1:1. DS = USBK_SEG.
.defaults:
    mov byte [USBM_FLAGS], 0
    mov byte [USBM_RATE], 100
    mov byte [USBM_RES], 2
    ret

.rates:
    db 10, 20, 40, 60, 80, 100, 200

; A boot keyboard report at ES:USBK_BUF: every key in it that was not in
; the previous one is a key press, put in the BDA buffer with the
; modifiers applied (Shift and Ctrl on letters). Keeps every register.
usb_hid_report:
    pusha
    cmp byte [es:USBK_BUF + 2], 1       ; phantom state (rollover): skip
    je .out
    mov si, 2
.key:
    mov al, [es:USBK_BUF + si]
    cmp al, 4
    jb .next
    ; Already down in the previous report?
    mov di, 2
.prev:
    cmp al, [es:USBK_PREV + di]
    je .next
    inc di
    cmp di, 8
    jb .prev
    call usb_hid_press
.next:
    inc si
    cmp si, 8
    jb .key
    mov eax, [es:USBK_BUF]
    mov [es:USBK_PREV], eax
    mov eax, [es:USBK_BUF + 4]
    mov [es:USBK_PREV + 4], eax
.out:
    popa
    ret

; Key usage AL pressed, modifiers at ES:USBK_BUF. Keeps every register.
usb_hid_press:
    pusha
    cmp al, usb_hid_map_end - usb_hid_map + 4
    jae .out
    movzx bx, al
    mov al, [cs:usb_hid_map + bx - 4]   ; set-1 make code
    test al, al
    jz .out
    call kbd_lookup                     ; AL = ASCII, AH = scancode
    jc .out
    mov bl, [es:USBK_BUF]               ; modifiers
    cmp al, 'a'
    jb .put
    cmp al, 'z'
    ja .put
    test bl, 0x11                       ; Ctrl
    jz .shift
    and al, 0x1F
    jmp .put
.shift:
    test bl, 0x22                       ; Shift
    jz .put
    sub al, 'a' - 'A'
.put:
    call kbd_enqueue
.out:
    popa
    ret

; Keyboard page usages 04h-38h -> set-1 make codes (0: not mapped)
usb_hid_map:
    db 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23  ; 04-0B a-h
    db 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19  ; 0C-13 i-p
    db 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D  ; 14-1B q-x
    db 0x15, 0x2C, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07  ; 1C-23 y z 1-6
    db 0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F  ; 24-2B 7-0 Enter Esc BS Tab
    db 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27  ; 2C-33 Space - = [ ] \ # ;
    db 0x28, 0x29, 0x33, 0x34, 0x35                    ; 34-38 ' ` , . /
usb_hid_map_end:
