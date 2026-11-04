; SquirrelBIOS x86 — EHCI host controller (USB 2.0)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the Enhanced Host Controller Interface specification,
; revision 1.0. The registers are memory-mapped (BAR0) and reached through
; the flat GS from flat_gs, with interrupts off. EHCI runs only high-speed
; devices: at each root port, a low-speed device (seen on the line state)
; or one that does not come up enabled after the reset (full speed) is
; handed to the companion UHCI / OHCI function, which usb_init's second
; pass lists. CONFIGFLAG stays set after POST so that those ports keep
; going to the companions.
;
; One queue head is the whole asynchronous list, linked to itself. A
; control transfer is a chain of queue element transfer descriptors
; (setup, data, status) put on its overlay; the schedule is turned on,
; the last qTD polled until the controller retires it, and the schedule
; turned off again. Structures in USB_SEG (usb.inc):
;   1000h  the queue head, 32-byte aligned
;   1040h  qTDs, 32 bytes each

EHCI_BAR        equ 0x10            ; BAR0: capability registers
EHCI_CAPLENGTH  equ 0x00            ; byte: where the operational ones start
EHCI_HCSPARAMS  equ 0x04
EHCI_HCCPARAMS  equ 0x08
; Operational registers, from the operational base
EHCI_USBCMD     equ 0x00
EHCI_USBSTS     equ 0x04
EHCI_USBINTR    equ 0x08
EHCI_CTRLDSSEG  equ 0x10
EHCI_ASYNCLIST  equ 0x18
EHCI_CONFIGFLAG equ 0x40
EHCI_PORTSC     equ 0x44            ; + 4 * (port - 1)

ECMD_RS         equ 0x00000001
ECMD_HCRESET    equ 0x00000002
ECMD_ASE        equ 0x00000020
ECMD_ITC8       equ 0x00080000      ; interrupt threshold: 8 microframes
ESTS_HALTED     equ 0x00001000
ESTS_ASS        equ 0x00008000      ; asynchronous schedule running
HCS_PPC         equ 0x00000010      ; ports have power switches
HCC_64BIT       equ 0x00000001

EPS_CCS         equ 0x00000001
EPS_PE          equ 0x00000004
EPS_PR          equ 0x00000100
EPS_LINE        equ 0x00000C00
EPS_LINE_K      equ 0x00000400      ; K state: a low-speed device
EPS_PP          equ 0x00001000
EPS_PO          equ 0x00002000      ; owned by the companion
EPS_WC          equ 0x0000002A      ; CSC, PEC, OCC: write 1 to clear

USBLEGSUP_BIOS  equ 0x00010000
USBLEGSUP_OS    equ 0x01000000

EHCI_QH         equ 0x1000
EHCI_TD         equ 0x1040
EHCI_TD_SIZE    equ 32

QH_TYPE         equ 0x00000002      ; link: points at a queue head
QH_T            equ 0x00000001      ; link: terminate
QH_EPS_HIGH     equ 0x00002000
QH_DTC          equ 0x00004000      ; data toggle from the qTDs
QH_HEAD         equ 0x00008000      ; head of the reclamation list
QH_MULT1        equ 0x40000000
QTD_ACTIVE      equ 0x00000080
QTD_HALTED      equ 0x00000040
QTD_OUT         equ 0x00000000
QTD_IN          equ 0x00000100
QTD_SETUP       equ 0x00000200
QTD_CERR3       equ 0x00000C00
QTD_DATA1       equ 0x80000000

; The EHCI function at USB_BDF (BX): take it over, reset it, run it, then
; reset each root port with a device on it. High-speed devices are
; enumerated; the others go to the companion. The controller is halted at
; the end. FS = USB_SEG. Keeps every register.
ehci_bus:
    pushad
    push gs
    mov byte [fs:USB_HC], USB_HC_EHCI
    mov di, EHCI_BAR
    call pci_read32
    test al, 1                  ; must be a memory BAR
    jnz .out
    test al, 4                  ; 64-bit: the upper half must be 0
    jz .bar32
    push eax
    mov di, EHCI_BAR + 4
    call pci_read32
    test eax, eax
    pop eax
    jnz .out
.bar32:
    and eax, 0xFFFFFF00
    jz .out
    mov esi, eax                ; ESI = capability registers for now
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_MEM | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    call enable_a20
    call flat_gs

    ; SMM may own it (legacy support capability): ask for it back
    mov eax, [gs:esi + EHCI_HCCPARAMS]
    movzx di, ah                ; EECP: config offset of the capability
    cmp di, 0x40
    jb .ours
    call pci_read32
    cmp al, 1                   ; USBLEGSUP
    jne .ours
    test eax, USBLEGSUP_BIOS
    jz .ours
    mov ecx, eax
    or ecx, USBLEGSUP_OS
    call pci_write32
    mov cx, 1000
.own:
    call pci_read32
    test eax, USBLEGSUP_BIOS
    jz .ours
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .own
    jmp .out
.ours:
    mov eax, [gs:esi + EHCI_HCCPARAMS]
    push eax
    mov eax, [gs:esi + EHCI_HCSPARAMS]
    push eax
    movzx eax, byte [gs:esi + EHCI_CAPLENGTH]
    add esi, eax                ; ESI = operational registers from here on
    mov [fs:USB_MMIO], esi

    ; Stop it, wait for it to halt, then reset it
    and dword [gs:esi + EHCI_USBCMD], ~(ECMD_RS | ECMD_ASE)
    call ehci_halt
    mov dword [gs:esi + EHCI_USBCMD], ECMD_HCRESET
    mov cx, 250
.reset:
    test dword [gs:esi + EHCI_USBCMD], ECMD_HCRESET
    jz .reset_done
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .reset
    add sp, 8
    jmp .out
.reset_done:
    pop edx                     ; EDX = HCSPARAMS
    pop eax                     ; HCCPARAMS
    test eax, HCC_64BIT
    jz .seg
    mov dword [gs:esi + EHCI_CTRLDSSEG], 0
.seg:
    mov dword [gs:esi + EHCI_USBINTR], 0
    mov dword [gs:esi + EHCI_USBSTS], 0x3F     ; clear every change bit

    ; The queue head: alone on the asynchronous list, no transfer on it
    mov eax, (USB_PHYS + EHCI_QH) | QH_TYPE
    mov [fs:EHCI_QH], eax
    mov dword [fs:EHCI_QH + 4], QH_HEAD
    mov dword [fs:EHCI_QH + 8], QH_MULT1
    xor di, di
.qh:
    mov dword [fs:EHCI_QH + 12 + di], 0
    add di, 4
    cmp di, 36
    jb .qh
    mov dword [fs:EHCI_QH + 16], QH_T          ; next qTD: none
    mov dword [fs:EHCI_QH + 20], QH_T          ; alternate: none
    mov dword [gs:esi + EHCI_ASYNCLIST], USB_PHYS + EHCI_QH
    mov dword [gs:esi + EHCI_USBCMD], ECMD_RS | ECMD_ITC8
    ; Every port to EHCI
    mov dword [gs:esi + EHCI_CONFIGFLAG], 1
    mov cx, 5
    call delay_ms

    ; "usb: EHCI at bb:dd.f"
    mov si, msg_usb_ehci
    call usb_puts_head
    call usb_print_bdf
    call usb_newline
    mov esi, [fs:USB_MMIO]

    ; Power the ports if they switch, then 20 ms for it to settle
    mov ebp, edx
    and bp, 0x0F                ; N_PORTS
    xor ebx, ebx
    test edx, HCS_PPC
    jz .powered
.port_power:
    cmp bx, bp
    jae .power_wait
    mov eax, [gs:esi + EHCI_PORTSC + ebx * 4]
    and eax, ~EPS_WC
    or eax, EPS_PP
    mov [gs:esi + EHCI_PORTSC + ebx * 4], eax
    inc bx
    jmp .port_power
.power_wait:
    mov cx, 20
    call delay_ms
.powered:

    ; Each port with a device: low speed goes to the companion; the rest
    ; are reset, and high speed comes out enabled
    xor ebx, ebx
.port:
    cmp bx, bp
    jae .stop
    mov eax, [gs:esi + EHCI_PORTSC + ebx * 4]
    test eax, EPS_CCS
    jz .next
    test eax, EPS_PO
    jnz .next
    and eax, EPS_LINE
    cmp eax, EPS_LINE_K
    je .release
    mov eax, [gs:esi + EHCI_PORTSC + ebx * 4]
    and eax, ~(EPS_WC | EPS_PE)
    or eax, EPS_PR
    mov [gs:esi + EHCI_PORTSC + ebx * 4], eax
    mov cx, 50                  ; reset signalling, 50 ms
    call delay_ms
    mov eax, [gs:esi + EHCI_PORTSC + ebx * 4]
    and eax, ~(EPS_WC | EPS_PR)
    mov [gs:esi + EHCI_PORTSC + ebx * 4], eax
    mov cx, 10                  ; the controller ends the reset in 2 ms
.reset_end:
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    test dword [gs:esi + EHCI_PORTSC + ebx * 4], EPS_PR
    jz .reset_ended
    loop .reset_end
    jmp .next
.reset_ended:
    mov eax, [gs:esi + EHCI_PORTSC + ebx * 4]
    test eax, EPS_CCS
    jz .next                    ; unplugged meanwhile
    test eax, EPS_PE
    jz .release                 ; full speed
    or eax, EPS_WC              ; clear the change bits
    mov [gs:esi + EHCI_PORTSC + ebx * 4], eax
    mov cx, 10                  ; reset recovery
    call delay_ms
    push bx
    push bp
    mov ah, 2                   ; high speed
    mov dh, bl
    inc dh                      ; port number, from 1
    mov dl, 0                   ; parent: root hub
    xor cl, cl                  ; tier 0
    call usb_enum
    pop bp
    pop bx
    call flat_gs                ; ehci_control's POP GS left a real-mode GS
    mov esi, [fs:USB_MMIO]
    jmp .next
.release:
    mov eax, [gs:esi + EHCI_PORTSC + ebx * 4]
    and eax, ~EPS_WC
    or eax, EPS_PO
    mov [gs:esi + EHCI_PORTSC + ebx * 4], eax
.next:
    inc bx
    jmp .port

.stop:
    ; Halt it, no schedules: no DMA after POST. No reset, which would
    ; clear CONFIGFLAG and give the high-speed devices to the companions
    mov dword [gs:esi + EHCI_USBCMD], 0
    call ehci_halt
    mov cx, 20                  ; let the companions see their ports
    call delay_ms
.out:
    pop gs
    popad
    ret

; Wait up to 20 ms for the controller at GS:ESI (operational registers) to
; halt. Keeps every register.
ehci_halt:
    push cx
    mov cx, 20
.wait:
    test dword [gs:esi + EHCI_USBSTS], ESTS_HALTED
    jnz .out
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .wait
.out:
    pop cx
    ret

; Turn the asynchronous schedule of the controller at GS:ESI on (CF clear)
; or off (CF set) and wait up to 20 ms for it to follow. Keeps every
; register.
ehci_async:
    push eax
    push cx
    pushf
    mov eax, [gs:esi + EHCI_USBCMD]
    or eax, ECMD_ASE
    jnc .set
    and eax, ~ECMD_ASE
.set:
    mov [gs:esi + EHCI_USBCMD], eax
    mov cx, 20
.wait:
    mov eax, [gs:esi + EHCI_USBSTS]
    popf
    pushf
    jc .off
    test eax, ESTS_ASS
    jnz .out
    jmp .again
.off:
    test eax, ESTS_ASS
    jz .out
.again:
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .wait
.out:
    popf
    pop cx
    pop eax
    ret

; usb_control for EHCI: AL = address, AH = 2 (high speed; full- and
; low-speed devices behind a high-speed hub would need split transactions,
; which this does not do), BL = max packet size of endpoint 0; setup
; packet at USB_SETUP, data at USB_BUF. CF set on a stall, an error or a
; timeout. Keeps every register.
ehci_control:
    pushad
    push gs
    cmp ah, 2
    jne .fail
    call flat_gs
    ; The queue head: address, endpoint 0, high speed, toggles from the
    ; qTDs, max packet
    movzx edx, al
    movzx ecx, bl
    shl ecx, 16
    or edx, ecx
    or edx, QH_EPS_HIGH | QH_DTC | QH_HEAD
    mov [fs:EHCI_QH + 4], edx
    movzx edx, word [fs:USB_SETUP + 6]  ; EDX = bytes of data
    cmp edx, USB_BUFLEN
    ja .fail
    mov di, EHCI_TD
    ; Setup stage: 8 bytes, DATA0
    mov eax, (8 << 16) | QTD_SETUP
    mov ebx, USB_PHYS + USB_SETUP
    call .td
    ; Data stage: one qTD for all of it, from DATA1; the controller cuts
    ; it into max-packet pieces and carries the toggle
    test edx, edx
    jz .status
    mov eax, edx
    shl eax, 16
    or eax, QTD_DATA1 | QTD_OUT
    test byte [fs:USB_SETUP], 0x80
    jz .dir
    or eax, QTD_IN
.dir:
    mov ebx, USB_PHYS + USB_BUF
    call .td
.status:
    ; Status stage: no data, DATA1, the other direction
    mov eax, QTD_DATA1 | QTD_IN
    test byte [fs:USB_SETUP], 0x80
    jz .last
    test edx, edx
    jz .last
    mov eax, QTD_DATA1 | QTD_OUT
.last:
    xor ebx, ebx
    call .td
    sub di, EHCI_TD_SIZE
    mov dword [fs:di], QH_T     ; the chain ends here
    ; On the overlay: the controller fetches the first qTD
    mov dword [fs:EHCI_QH + 12], 0
    mov dword [fs:EHCI_QH + 16], USB_PHYS + EHCI_TD
    mov dword [fs:EHCI_QH + 20], QH_T
    mov dword [fs:EHCI_QH + 24], 0
    mov esi, [fs:USB_MMIO]
    clc
    call ehci_async
    ; Done when the last qTD is retired; a halted qTD stops it early. Up
    ; to 1000 ms.
    mov cx, 1000
.poll:
    test dword [fs:EHCI_QH + 24], QTD_HALTED
    jnz .stop_fail
    test dword [fs:di + 8], QTD_ACTIVE
    jz .finished
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .poll
    jmp .stop_fail
.finished:
    stc
    call ehci_async
    ; Every qTD must have finished without a halt
    mov bx, EHCI_TD
.check:
    test dword [fs:bx + 8], QTD_HALTED | QTD_ACTIVE
    jnz .unhang
    add bx, EHCI_TD_SIZE
    cmp bx, di
    jbe .check
    mov dword [fs:EHCI_QH + 16], QH_T
    pop gs
    popad
    clc
    ret
.stop_fail:
    stc
    call ehci_async
.unhang:
    mov dword [fs:EHCI_QH + 16], QH_T
    mov dword [fs:EHCI_QH + 24], 0          ; clear the halt
.fail:
    pop gs
    popad
    stc
    ret

; Write a qTD at FS:DI and move DI to the next: token EAX (active and
; three retries added), buffer EBX (0: none). Its next pointer is the
; next slot. Keeps every register but DI.
.td:
    push eax
    movzx eax, di
    add eax, USB_PHYS + EHCI_TD_SIZE
    mov [fs:di], eax            ; next qTD
    mov dword [fs:di + 4], QH_T ; alternate next: none
    pop eax
    push eax
    or eax, QTD_ACTIVE | QTD_CERR3
    mov [fs:di + 8], eax
    mov [fs:di + 12], ebx       ; buffer 0: the address
    mov eax, ebx
    and eax, 0xFFFFF000
    add eax, 0x1000
    mov [fs:di + 16], eax       ; buffer 1: the next page
    xor eax, eax
    mov [fs:di + 20], eax
    mov [fs:di + 24], eax
    mov [fs:di + 28], eax
    add di, EHCI_TD_SIZE
    pop eax
    ret
