; SquirrelBIOS x86 — USB keyboard after POST (legacy emulation)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the USB Device Class Definition for HID 1.11 (boot
; protocol, appendix B) and the HID Usage Tables (keyboard page). The
; first boot-protocol keyboard that usb_enum finds on a controller that
; can keep running is configured, switched to the boot protocol and told
; to report only on changes. When that controller's bus has been listed,
; it is not halted: its schedule moves into the EBDA (pages at +5000h)
; and runs just one transfer, an interrupt IN from the keyboard (UHCI:
; a frame list whose every frame holds one queue head with one TD; OHCI:
; an HCCA whose 32 interrupt lists all hold one endpoint descriptor with
; one TD and an empty tail). IRQ0
; looks at that transfer 18.2 times a second; a finished one is an
; 8-byte report, whose newly pressed keys go into the BDA keyboard buffer
; as the 8042 path would put them, and the transfer is armed again. The
; keyboard holds its next report until it is asked, so nothing is lost
; between ticks.
;
; On xHCI the controller already works from the EBDA (xhci.inc) and the
; keyboard's endpoint is added to its slot (Configure Endpoint). Its
; ring holds 15 transfers at once, each into its own buffer, and posts
; no events: IRQ0 takes every buffer that has been written and hands its
; transfer back, without touching the controller's registers.
;
; EBDA layout from +5000h (USBK_*, offsets in the EBDA segment):
;   5000h  UHCI frame list, 4 KB, every entry the queue head;
;          OHCI: the HCCA (256 bytes)
;   6000h  queue head (UHCI) or endpoint descriptor (OHCI)
;   6020h  transfer descriptor; 6030h OHCI's empty tail TD
;   6040h  report buffer (8 bytes); 6048h previous report
; and its variables in the EBDA's first page, at 0F00h: an xHCI that
; works from the EBDA during POST (xhci.inc) overwrites every USB page,
; and must not be taken for a running keyboard by IRQ0.

USBK_SEG        equ EBDA_BASE >> 4
USBK_FL         equ 0x5000
USBK_QH         equ 0x6000
USBK_TD         equ 0x6020
USBK_TAIL       equ 0x6030
USBK_BUF        equ 0x6040
USBK_PREV       equ 0x6048
USBK_KIND       equ 0x0F00          ; byte: 0 none, 1 UHCI, 2 OHCI, 3 xHCI
USBK_TOKEN      equ 0x0F04          ; dword: next TD token (UHCI)
USBK_STATUS     equ 0x0F08          ; dword: TD status to arm with (UHCI)
USBK_XIDX       equ 0x0F0C          ; byte: next TRB to look at (xHCI)
USBK_XCYC       equ 0x0F0D          ; byte: and the lap's cycle bit
USBK_XRING      equ XHCI_EBDA - EBDA_BASE + XHCI_INTRING
USBK_XBUF       equ XHCI_EBDA - EBDA_BASE + XHCI_INTBUF
USBK_KIND_UHCI  equ 1
USBK_KIND_OHCI  equ 2
USBK_KIND_XHCI  equ 3

%if EBDA_SIZE < 0x7000
%error "USB keyboard needs a 28 KB or larger EBDA"
%endif

; The claimed keyboard (USB_SEG variables)
USB_KBD_ADDR    equ 0x1F40          ; byte: its address (0: none yet)
USB_KBD_EP      equ 0x1F41          ; byte: interrupt IN endpoint
USB_KBD_MPS     equ 0x1F42          ; byte: that endpoint's max packet
USB_KBD_SPEED   equ 0x1F43          ; byte: as usb_enum's AH
USB_KBD_BDF     equ 0x1F44          ; word: its controller

HID_REQ_SET_IDLE     equ 0x0A
HID_REQ_SET_PROTOCOL equ 0x0B

; From usb_enum, with its frame at BP: the device just listed has a boot
; interface with an interrupt IN endpoint. If it is a keyboard, none is
; claimed yet, and this controller can keep one running, configure it for
; boot reports and remember it. Keeps every register.
usb_hid_claim:
    pushad
    cmp byte [bp - 8], 1        ; keyboard
    jne .out
    cmp byte [fs:USB_KBD_ADDR], 0
    jne .out
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
    xor bx, bx
    call usb_setup
    call usb_enum.ctrl
    jc .out
    ; SET_PROTOCOL: boot
    mov ax, (HID_REQ_SET_PROTOCOL << 8) | 0x21
    xor cx, cx
    movzx dx, byte [bp - 9]
    call usb_setup
    call usb_enum.ctrl
    jc .out
    ; SET_IDLE 0: a report only when something changes. Optional.
    mov ax, (HID_REQ_SET_IDLE << 8) | 0x21
    call usb_setup
    call usb_enum.ctrl
    mov al, [bp - 1]
    mov [fs:USB_KBD_ADDR], al
    mov al, [bp - 10]
    mov [fs:USB_KBD_EP], al
    mov al, [bp - 11]
    mov [fs:USB_KBD_MPS], al
    mov al, [bp - 2]
    mov [fs:USB_KBD_SPEED], al
    mov ax, [fs:USB_BDF]
    mov [fs:USB_KBD_BDF], ax
.out:
    popad
    ret

; CF clear if the keyboard claimed on a bus is on this controller
; (USB_BDF) and not running yet. Keeps every register.
usb_hid_here:
    push ax
    cmp byte [fs:USB_KBD_ADDR], 0
    je .no
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

; "usb: keyboard is device N". Keeps every register but DI (the cursor).
usb_hid_announce:
    push ax
    push si
    mov si, msg_usb_kbd
    call usb_puts_head
    mov al, [fs:USB_KBD_ADDR]
    call usb_dec
    call usb_newline
    pop si
    pop ax
    ret

; UHCI at I/O base DX, its bus listed: move its schedule into the EBDA
; with the keyboard's interrupt transfer as the only work, and run it.
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
    ; Every frame: the queue head, which holds the one TD
    mov eax, (EBDA_BASE + USBK_QH) | TD_Q
    xor di, di
.fl:
    mov [es:USBK_FL + di], eax
    add di, 4
    cmp di, 4096
    jb .fl
    mov dword [es:USBK_QH], TD_T
    ; The TD: IN from the keyboard's endpoint, DATA0 first
    mov esi, TD_ACTIVE | TD_CERR3
    cmp byte [fs:USB_KBD_SPEED], 1
    jne .fs
    or esi, TD_LS
.fs:
    mov [es:USBK_STATUS], esi
    movzx eax, byte [fs:USB_KBD_MPS]
    cmp al, 8
    jbe .len
    mov al, 8                   ; a boot report is 8 bytes
.len:
    dec eax
    shl eax, 21
    movzx ebx, byte [fs:USB_KBD_EP]
    shl ebx, 15
    or eax, ebx
    movzx ebx, byte [fs:USB_KBD_ADDR]
    shl ebx, 8
    or eax, ebx
    mov al, PID_IN
    mov [es:USBK_TOKEN], eax
    xor eax, eax
    mov [es:USBK_PREV], eax
    mov [es:USBK_PREV + 4], eax
    mov byte [es:USBK_KIND], USBK_KIND_UHCI
    call usb_hid_arm
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
; with the keyboard's endpoint on every interrupt list, and run only the
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
    xor di, di
.hcca:
    mov dword [es:USBK_FL + di], EBDA_BASE + USBK_QH
    add di, 4
    cmp di, 32 * 4
    jb .hcca
.zero:
    mov dword [es:USBK_FL + di], 0
    add di, 4
    cmp di, 256
    jb .zero
    ; The endpoint: address, endpoint, IN, speed, max packet (8 at most)
    movzx eax, byte [fs:USB_KBD_MPS]
    cmp al, 8
    jbe .mps
    mov al, 8
.mps:
    shl eax, 16
    or eax, 2 << 11             ; direction IN
    movzx ebx, byte [fs:USB_KBD_EP]
    shl ebx, 7
    or eax, ebx
    or al, [fs:USB_KBD_ADDR]
    cmp byte [fs:USB_KBD_SPEED], 1
    jne .fs
    or eax, ED_LOWSPEED
.fs:
    mov [es:USBK_QH], eax
    mov dword [es:USBK_QH + 4], EBDA_BASE + USBK_TAIL   ; TailP
    mov dword [es:USBK_QH + 8], EBDA_BASE + USBK_TD     ; HeadP: DATA0
    mov dword [es:USBK_QH + 12], 0
    xor eax, eax
    mov [es:USBK_PREV], eax
    mov [es:USBK_PREV + 4], eax
    mov byte [es:USBK_KIND], USBK_KIND_OHCI
    call usb_hid_arm
    mov dword [gs:esi + OHCI_HCCA], EBDA_BASE + USBK_FL
    mov dword [gs:esi + OHCI_CONTROL], OCTL_HCFS_OPER | OCTL_PLE
    pop es
    popad
    ret

; Arm the keyboard's transfer again (ES = USBK_SEG). Keeps every register.
usb_hid_arm:
    cmp byte [es:USBK_KIND], USBK_KIND_OHCI
    je .ohci
    push eax
    mov dword [es:USBK_TD], TD_T
    mov eax, [es:USBK_TOKEN]
    mov [es:USBK_TD + 8], eax
    mov dword [es:USBK_TD + 12], EBDA_BASE + USBK_BUF
    mov eax, [es:USBK_STATUS]
    mov [es:USBK_TD + 4], eax
    mov dword [es:USBK_QH + 4], EBDA_BASE + USBK_TD     ; last: now the HC's
    pop eax
    ret
.ohci:
    ; A general TD for 8 bytes IN, the toggle from the endpoint, then the
    ; empty tail; HeadP back to it, keeping the toggle carry and clearing
    ; a halt
    push eax
    mov dword [es:USBK_TD], GTD_CC_NEW | GTD_NOINT | GTD_IN | GTD_ROUNDING
    mov dword [es:USBK_TD + 4], EBDA_BASE + USBK_BUF
    mov dword [es:USBK_TD + 8], EBDA_BASE + USBK_TAIL
    mov dword [es:USBK_TD + 12], EBDA_BASE + USBK_BUF + 7
    xor eax, eax
    mov [es:USBK_TAIL], eax
    mov [es:USBK_TAIL + 4], eax
    mov [es:USBK_TAIL + 8], eax
    mov [es:USBK_TAIL + 12], eax
    mov eax, [es:USBK_QH + 8]
    and eax, 2                  ; toggle carry
    or eax, EBDA_BASE + USBK_TD
    mov [es:USBK_QH + 8], eax
    pop eax
    ret

; From IRQ0: if the keyboard's transfer has finished, take its report and
; arm it again. Runs with interrupts off, in real or virtual-8086 mode.
; Keeps every register and the flags.
usb_hid_poll:
    pushf
    push eax
    push es
    push word USBK_SEG
    pop es
    cmp byte [es:USBK_KIND], USBK_KIND_OHCI
    je .ohci
    cmp byte [es:USBK_KIND], USBK_KIND_XHCI
    je .xhci
    cmp byte [es:USBK_KIND], USBK_KIND_UHCI
    jne .out
    mov eax, [es:USBK_TD + 4]
    test eax, TD_ACTIVE
    jnz .out                    ; still waiting for the keyboard
    test eax, TD_ERRORS
    jnz .again                  ; same toggle, try again
    xor dword [es:USBK_TOKEN], 1 << 19
    call usb_hid_report
.again:
    call usb_hid_arm
    jmp .out
.xhci:
    call usb_hid_xpoll
.out:
    pop es
    pop eax
    popf
    ret
.ohci:
    ; Done when the endpoint's head has moved on to the tail (or halted)
    mov eax, [es:USBK_QH + 8]
    test al, ED_HALTED
    jnz .again
    and eax, ~0x0F
    cmp eax, EBDA_BASE + USBK_TAIL
    jne .out
    test dword [es:USBK_TD], 0xF0000000     ; condition code
    jnz .again
    call usb_hid_report
    jmp .again

; usb_hid_poll for xHCI (ES = USBK_SEG). The controller fills the ring's
; buffers in order and posts no events; a buffer that is no longer all
; ones holds a report. Each one is taken, its buffer set back to all
; ones, and its TRB handed back for the next lap (the other cycle bit).
; Once TRB 0 of a lap is done the controller has followed the link, so
; the link gets this lap's cycle bit for the next time round. Reading
; memory only, this works in virtual-8086 mode too. Keeps every register.
usb_hid_xpoll:
    pushad
    mov cx, XHCI_INT_TRBS - 1   ; at most one lap per tick
.look:
    movzx bx, byte [es:USBK_XIDX]
    mov si, bx
    shl si, 3
    add si, USBK_XBUF
    cmp dword [es:si], -1
    jne .report
    cmp dword [es:si + 4], -1
    je .out                     ; still waiting for the keyboard
.report:
    mov eax, [es:si]
    mov [es:USBK_BUF], eax
    mov eax, [es:si + 4]
    mov [es:USBK_BUF + 4], eax
    mov dword [es:si], -1
    mov dword [es:si + 4], -1
    call usb_hid_report
    movzx eax, byte [es:USBK_XCYC]
    test bx, bx
    jnz .repost
    or eax, TRB_LINK_INT
    mov [es:USBK_XRING + (XHCI_INT_TRBS - 1) * 16 + 12], eax
    movzx eax, byte [es:USBK_XCYC]
.repost:
    xor al, 1
    or eax, TRB_NORMAL << 10
    mov di, bx
    shl di, 4
    mov [es:USBK_XRING + di + 12], eax
    inc bx
    cmp bx, XHCI_INT_TRBS - 1
    jb .store
    xor bx, bx
    xor byte [es:USBK_XCYC], 1
.store:
    mov [es:USBK_XIDX], bl
    dec cx
    jnz .look
.out:
    popad
    ret

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
