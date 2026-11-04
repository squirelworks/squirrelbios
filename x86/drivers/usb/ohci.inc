; SquirrelBIOS x86 — OHCI host controller (USB 1.1)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the Open Host Controller Interface specification, release
; 1.0a. The registers are memory-mapped (BAR0) and reached through the
; flat GS from flat_gs, with interrupts off. One endpoint descriptor sits
; on the control list for the whole listing; a control transfer is a
; chain of general transfer descriptors put on it, ending at an empty
; tail descriptor, and is done when the controller has moved the head up
; to the tail (or halted the endpoint). Structures in USB_SEG (usb.inc):
;   0000h  HCCA, 256 bytes, 256-byte aligned
;   1000h  the control endpoint descriptor
;   1020h  transfer descriptors, 16 bytes each, then the tail

OHCI_BAR        equ 0x10            ; BAR0: registers
OHCI_CONTROL    equ 0x04
OHCI_CMDSTATUS  equ 0x08
OHCI_INTSTATUS  equ 0x0C
OHCI_INTDISABLE equ 0x14
OHCI_HCCA       equ 0x18
OHCI_CTRLHEAD   equ 0x20
OHCI_CTRLCUR    equ 0x24
OHCI_FMINTERVAL equ 0x34
OHCI_PERSTART   equ 0x40
OHCI_RHDESCA    equ 0x48
OHCI_RHSTATUS   equ 0x50
OHCI_RHPORT     equ 0x54            ; + 4 * (port - 1)

OCTL_PLE        equ 0x00000004      ; periodic list enable
OCTL_CLE        equ 0x00000010      ; control list enable
OCTL_HCFS_OPER  equ 0x00000080      ; functional state: operational
OCTL_IR         equ 0x00000100      ; interrupts routed to SMM
OCS_HCR         equ 0x00000001      ; controller reset
OCS_CLF         equ 0x00000002      ; control list filled
OCS_OCR         equ 0x00000008      ; ownership change request
OHCI_FI         equ 11999           ; frame interval: 1 ms of 12 MHz bits
OHCI_FMI        equ (((OHCI_FI - 210) * 6 / 7) << 16) | OHCI_FI
RHA_NPS         equ 0x00000200      ; no power switching
RHS_LPSC        equ 0x00010000      ; write: set global power

ORP_CCS         equ 0x00000001
ORP_SET_RESET   equ 0x00000010      ; write
ORP_PES         equ 0x00000002
ORP_PRS         equ 0x00000010
ORP_SET_POWER   equ 0x00000100      ; write
ORP_LSDA        equ 0x00000200
ORP_CSC         equ 0x00010000
ORP_PRSC        equ 0x00100000

OHCI_HCCA_OFF   equ 0x0000
OHCI_ED         equ 0x1000
OHCI_TD         equ 0x1020
OHCI_TD_SIZE    equ 16

ED_SKIP         equ 0x00004000
ED_LOWSPEED     equ 0x00002000
ED_HALTED       equ 0x00000001
GTD_ROUNDING    equ 0x00040000      ; a short IN packet is not an error
GTD_SETUP       equ 0x00000000
GTD_OUT         equ 0x00080000
GTD_IN          equ 0x00100000
GTD_NOINT       equ 0x00E00000
GTD_DATA0       equ 0x02000000
GTD_DATA1       equ 0x03000000
GTD_CC_NEW      equ 0xF0000000      ; "not accessed"

; The OHCI function at USB_BDF (BX): take it over, reset it, run it, power
; and reset each root port with a device, enumerate, then reset it again.
; FS = USB_SEG. Keeps every register.
ohci_bus:
    pushad
    push gs
    mov byte [fs:USB_HC], USB_HC_OHCI
    mov di, OHCI_BAR
    call pci_read32
    test al, 1                  ; must be a memory BAR
    jnz .out
    and eax, 0xFFFFF000
    jz .out
    mov esi, eax
    mov [fs:USB_MMIO], esi
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_MEM | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    call enable_a20
    call flat_gs

    ; SMM owns it (legacy emulation): ask for it back
    test dword [gs:esi + OHCI_CONTROL], OCTL_IR
    jz .ours
    mov dword [gs:esi + OHCI_CMDSTATUS], OCS_OCR
    mov cx, 500
.own:
    test dword [gs:esi + OHCI_CONTROL], OCTL_IR
    jz .ours
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .own
    jmp .out
.ours:
    ; Controller reset (done within 10 us), then the frame timing again
    mov dword [gs:esi + OHCI_CMDSTATUS], OCS_HCR
    mov cx, 10
.reset:
    test dword [gs:esi + OHCI_CMDSTATUS], OCS_HCR
    jz .reset_done
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .reset
    jmp .out
.reset_done:
    mov dword [gs:esi + OHCI_FMINTERVAL], OHCI_FMI
    mov dword [gs:esi + OHCI_PERSTART], OHCI_FI * 9 / 10
    mov dword [gs:esi + OHCI_INTDISABLE], 0xFFFFFFFF
    mov dword [gs:esi + OHCI_INTSTATUS], 0xFFFFFFFF

    ; HCCA zeroed; the control ED skipped until a transfer needs it
    xor di, di
.hcca:
    mov dword [fs:OHCI_HCCA_OFF + di], 0
    add di, 4
    cmp di, 256
    jb .hcca
    mov dword [fs:OHCI_ED], ED_SKIP
    mov dword [fs:OHCI_ED + 4], 0
    mov dword [fs:OHCI_ED + 8], 0
    mov dword [fs:OHCI_ED + 12], 0
    mov dword [gs:esi + OHCI_HCCA], USB_PHYS + OHCI_HCCA_OFF
    mov dword [gs:esi + OHCI_CTRLHEAD], USB_PHYS + OHCI_ED
    mov dword [gs:esi + OHCI_CTRLCUR], 0
    mov dword [gs:esi + OHCI_CONTROL], OCTL_HCFS_OPER | OCTL_CLE

    ; "usb: OHCI at bb:dd.f"
    mov si, msg_usb_ohci
    call usb_puts_head
    call usb_print_bdf
    call con_newline
    mov esi, [fs:USB_MMIO]

    ; Power the root ports, then wait POTPGT (2 ms units) and a margin
    mov eax, [gs:esi + OHCI_RHDESCA]
    test eax, RHA_NPS
    jnz .powered
    mov dword [gs:esi + OHCI_RHSTATUS], RHS_LPSC
    movzx cx, al                ; NDP: ports
    xor ebx, ebx
.port_power:
    cmp bx, cx
    jae .powered
    mov dword [gs:esi + OHCI_RHPORT + ebx * 4], ORP_SET_POWER
    inc bx
    jmp .port_power
.powered:
    mov eax, [gs:esi + OHCI_RHDESCA]
    shr eax, 24
    shl ax, 1
    add ax, 20
    mov cx, ax
    call delay_ms

    ; Each port with a device: reset it (the port enables itself), then
    ; enumerate
    mov eax, [gs:esi + OHCI_RHDESCA]
    movzx bp, al                ; ports, at most 15
    and bp, 0x0F
    xor ebx, ebx
.port:
    cmp bx, bp
    jae .stop
    mov eax, [gs:esi + OHCI_RHPORT + ebx * 4]
    test eax, ORP_CCS
    jz .next
    mov dword [gs:esi + OHCI_RHPORT + ebx * 4], ORP_SET_RESET
    mov cx, 50
.port_reset:
    push cx
    mov cx, 10
    call delay_ms
    pop cx
    test dword [gs:esi + OHCI_RHPORT + ebx * 4], ORP_PRSC
    jnz .port_done
    loop .port_reset
    jmp .next
.port_done:
    mov dword [gs:esi + OHCI_RHPORT + ebx * 4], ORP_PRSC | ORP_CSC
    mov cx, 10                  ; reset recovery
    call delay_ms
    mov eax, [gs:esi + OHCI_RHPORT + ebx * 4]
    test eax, ORP_PES
    jz .next
    mov ah, 0
    test eax, ORP_LSDA
    jz .speed
    mov ah, 1
.speed:
    push bx
    push bp
    mov dh, bl
    inc dh                      ; port number, from 1
    mov dl, 0                   ; parent: root hub
    xor cl, cl                  ; tier 0
    call usb_enum
    pop bp
    pop bx
    call flat_gs                ; ohci_control's POP GS left a real-mode GS
    mov esi, [fs:USB_MMIO]
.next:
    inc bx
    jmp .port

.stop:
    ; The keyboard kept for after POST is here: keep the controller
    ; running, with only that keyboard's transfer
    call usb_hid_here
    jc .halt_it
    call ohci_hid_start
    call usb_hid_announce       ; after: it needs ES:DI, the console cursor
    jmp .out
.halt_it:
    ; Back to the reset state: no lists, no DMA after POST
    mov dword [gs:esi + OHCI_CONTROL], 0
    mov dword [gs:esi + OHCI_CMDSTATUS], OCS_HCR
.out:
    pop gs
    popad
    ret

; usb_control for OHCI: AL = address, AH bit 0 = low speed, BL = max
; packet size of endpoint 0; setup packet at USB_SETUP, data at USB_BUF.
; CF set on a stall, an error or a timeout. Keeps every register.
ohci_control:
    pushad
    push gs
    call flat_gs
    ; The endpoint: address, endpoint 0, direction from the TDs, speed,
    ; max packet; skipped while the chain is built
    movzx edx, al
    movzx ecx, bl
    shl ecx, 16
    or edx, ecx
    or edx, ED_SKIP
    test ah, 1
    jz .fs
    or edx, ED_LOWSPEED
.fs:
    mov [fs:OHCI_ED], edx
    movzx ecx, bl               ; ECX = max packet
    movzx edx, word [fs:USB_SETUP + 6]  ; EDX = bytes of data
    cmp edx, USB_BUFLEN
    ja .fail
    mov di, OHCI_TD
    ; Setup stage: 8 bytes, DATA0
    mov eax, GTD_CC_NEW | GTD_DATA0 | GTD_NOINT | GTD_SETUP
    mov ebx, USB_PHYS + USB_SETUP
    mov esi, 8
    call .td
    ; Data stage: one TD for all of it, from DATA1; the controller cuts it
    ; into max-packet pieces and carries the toggle
    test edx, edx
    jz .status
    mov eax, GTD_CC_NEW | GTD_DATA1 | GTD_NOINT | GTD_OUT
    test byte [fs:USB_SETUP], 0x80
    jz .dir
    mov eax, GTD_CC_NEW | GTD_DATA1 | GTD_NOINT | GTD_IN
.dir:
    mov ebx, USB_PHYS + USB_BUF
    mov esi, edx
    call .td
.status:
    ; Status stage: no data, DATA1, the other direction
    mov eax, GTD_CC_NEW | GTD_DATA1 | GTD_NOINT | GTD_IN
    test byte [fs:USB_SETUP], 0x80
    jz .last
    test edx, edx
    jz .last
    mov eax, GTD_CC_NEW | GTD_DATA1 | GTD_NOINT | GTD_OUT
.last:
    xor ebx, ebx
    xor esi, esi
    call .td
    ; DI = the empty tail TD
    mov dword [fs:di], 0
    mov dword [fs:di + 4], 0
    mov dword [fs:di + 8], 0
    mov dword [fs:di + 12], 0
    movzx eax, di
    add eax, USB_PHYS
    mov [fs:OHCI_ED + 4], eax                   ; TailP
    mov dword [fs:OHCI_ED + 8], USB_PHYS + OHCI_TD  ; HeadP, not halted
    and dword [fs:OHCI_ED], ~ED_SKIP
    mov esi, [fs:USB_MMIO]
    mov dword [gs:esi + OHCI_CMDSTATUS], OCS_CLF
    ; Done when HeadP reaches TailP; halted on an error. Up to 1000 ms.
    mov cx, 1000
.poll:
    mov eax, [fs:OHCI_ED + 8]
    test eax, ED_HALTED
    jnz .halted
    and eax, ~0x0F
    cmp eax, [fs:OHCI_ED + 4]
    je .done
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .poll
.halted:
    or dword [fs:OHCI_ED], ED_SKIP
    mov cx, 2                   ; let the controller leave the endpoint
    call delay_ms
.fail:
    pop gs
    popad
    stc
    ret
.done:
    or dword [fs:OHCI_ED], ED_SKIP
    ; Every TD's condition code must be 0 (no error)
    mov bx, OHCI_TD
.check:
    cmp bx, di
    jae .ok
    test dword [fs:bx], 0xF0000000
    jnz .fail
    add bx, OHCI_TD_SIZE
    jmp .check
.ok:
    pop gs
    popad
    clc
    ret

; Write a general TD at FS:DI and move DI to the next: control word EAX,
; buffer EBX, ESI bytes (0: none). Its next pointer is the next slot.
; Keeps every register but DI.
.td:
    push eax
    test esi, esi
    jz .nobuf
    or eax, GTD_ROUNDING
.nobuf:
    mov [fs:di], eax
    mov [fs:di + 4], ebx        ; current buffer pointer (0: no data)
    movzx eax, di
    add eax, USB_PHYS + OHCI_TD_SIZE
    mov [fs:di + 8], eax        ; next TD
    xor eax, eax
    test esi, esi
    jz .end
    lea eax, [ebx + esi - 1]    ; buffer end: its last byte
.end:
    mov [fs:di + 12], eax
    add di, OHCI_TD_SIZE
    pop eax
    ret
