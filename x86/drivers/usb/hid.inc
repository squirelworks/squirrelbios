; SquirrelBIOS x86 — USB keyboard after POST (legacy emulation)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the USB Device Class Definition for HID 1.11 (boot
; protocol, appendix B) and the HID Usage Tables (keyboard page). The
; first boot-protocol keyboard that usb_enum finds on a controller that
; can keep running is configured, switched to the boot protocol and told
; to report only on changes. When that controller's bus has been listed,
; it is not halted: its schedule moves into the EBDA (pages at +5000h)
; and runs just one transfer, an interrupt IN from the keyboard. IRQ0
; looks at that transfer 18.2 times a second; a finished one is an
; 8-byte report, whose newly pressed keys go into the BDA keyboard buffer
; as the 8042 path would put them, and the transfer is armed again. The
; keyboard holds its next report until it is asked, so nothing is lost
; between ticks.
;
; EBDA layout from +5000h (USBK_*, offsets in the EBDA segment):
;   5000h  UHCI frame list, 4 KB, every entry the queue head
;   6000h  queue head; 6020h transfer descriptor
;   6040h  report buffer (8 bytes); 6048h previous report
;   6050h  runtime variables

USBK_SEG        equ EBDA_BASE >> 4
USBK_FL         equ 0x5000
USBK_QH         equ 0x6000
USBK_TD         equ 0x6020
USBK_BUF        equ 0x6040
USBK_PREV       equ 0x6048
USBK_KIND       equ 0x6050          ; byte: 0 none, 1 UHCI
USBK_TOKEN      equ 0x6054          ; dword: next TD token (UHCI)
USBK_STATUS     equ 0x6058          ; dword: TD status to arm with (UHCI)
USBK_KIND_UHCI  equ 1

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
    jne .out
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
    call con_newline
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
    call usb_hid_announce
    pop es
    popad
    ret

; Arm the keyboard's transfer again (ES = USBK_SEG). Keeps every register.
usb_hid_arm:
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

; From IRQ0: if the keyboard's transfer has finished, take its report and
; arm it again. Runs with interrupts off, in real or virtual-8086 mode.
; Keeps every register and the flags.
usb_hid_poll:
    pushf
    push eax
    push es
    push word USBK_SEG
    pop es
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
.out:
    pop es
    pop eax
    popf
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
