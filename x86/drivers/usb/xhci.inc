; SquirrelBIOS x86 — xHCI host controller (USB 3)
; Original code. Included by x86/squirrel-bios.asm when CONFIG_USB.
;
; Written from the eXtensible Host Controller Interface specification,
; revision 1.2. The registers are memory-mapped (BAR0) and reached through
; the flat GS from flat_gs, with interrupts off. Software talks to the
; controller through rings of 16-byte transfer request blocks (TRBs): one
; command ring, one event ring (polled, no interrupts), and one transfer
; ring per device for endpoint 0. A ring ends in a link TRB back to its
; start that flips the cycle bit, which tells new TRBs from old ones.
;
; The controller, not software, gives devices their bus addresses: each
; device gets a slot (Enable Slot), is first set up without an address
; (Address Device with BSR set) so that usb_enum can read its first eight
; descriptor bytes, and its SET_ADDRESS request is turned into a second
; Address Device. A table maps usb_enum's address numbers to slots and
; keeps each device's route string for the devices behind it.
;
; Structures in USB_SEG (usb.inc):
;   0000h  device context base address array (slots 0-16)
;   0100h  event ring segment table (one entry)
;   0200h  command ring, 16 TRBs
;   0400h  event ring, 64 TRBs
;   0800h  input context (up to 3 contexts of 64 bytes)
;   2000h  output device contexts, 2 KB per slot (slots 1-16)
;   A000h  endpoint 0 transfer rings, 16 TRBs per slot
;   B000h  ring state per slot: enqueue index, cycle bit
;   B100h  devices by address, 8 bytes each: slot, root port, speed ID,
;          hub depth, route string
;   B800h  scratchpad buffer array; C000h-FFFFh: up to 4 scratchpad pages

XHCI_BAR        equ 0x10
; Capability registers
XHCI_HCSPARAMS1 equ 0x04
XHCI_HCSPARAMS2 equ 0x08
XHCI_HCCPARAMS1 equ 0x10
XHCI_DBOFF      equ 0x14
XHCI_RTSOFF     equ 0x18
; Operational registers
XHCI_USBCMD     equ 0x00
XHCI_USBSTS     equ 0x04
XHCI_CRCR       equ 0x18
XHCI_DCBAAP     equ 0x30
XHCI_CONFIG     equ 0x38
XHCI_PORTSC     equ 0x400           ; + 0x10 * (port - 1)
; Interrupter 0, from the runtime registers
XHCI_IMAN       equ 0x20
XHCI_ERSTSZ     equ 0x28
XHCI_ERSTBA     equ 0x30
XHCI_ERDP       equ 0x38

XCMD_RS         equ 0x00000001
XCMD_HCRST      equ 0x00000002
XSTS_HCH        equ 0x00000001
XSTS_CNR        equ 0x00000800
XHCC_CSZ        equ 0x00000004      ; 64-byte contexts

XPS_CCS         equ 0x00000001
XPS_PED         equ 0x00000002
XPS_PR          equ 0x00000010
XPS_PP          equ 0x00000200
XPS_CSC         equ 0x00020000
XPS_PRC         equ 0x00200000
XPS_KEEP        equ 0x7F00FFED      ; write back all but PED, PR, LWS, the
                                    ; change bits (write 1 to clear) and WPR

XLEG_BIOS       equ 0x00010000
XLEG_OS         equ 0x01000000

; TRB types
TRB_SETUP       equ 2
TRB_DATA        equ 3
TRB_STATUS      equ 4
TRB_LINK        equ 6
TRB_ENABLE_SLOT equ 9
TRB_ADDRESS     equ 11
TRB_RESET_EP    equ 14
TRB_STOP_EP     equ 15
TRB_SET_DEQ     equ 16
TRB_EV_TRANSFER equ 32
TRB_EV_COMMAND  equ 33
TRB_LINK_CTRL   equ (TRB_LINK << 10) | 0x12     ; toggle cycle, chain
CC_SUCCESS      equ 1
CC_SHORT        equ 13

XHCI_DCBAA      equ 0x0000
XHCI_ERST       equ 0x0100
XHCI_CMDRING    equ 0x0200
XHCI_EVRING     equ 0x0400
XHCI_EV_TRBS    equ 64
XHCI_INCTX      equ 0x0800
XHCI_DEVCTX     equ 0x2000
XHCI_RINGS      equ 0xA000
XHCI_RING_TRBS  equ 16
XHCI_RSTATE     equ 0xB000
XHCI_ADDR       equ 0xB100
XHCI_SPARRAY    equ 0xB800
XHCI_SPPAGES    equ 0xC000
XHCI_SP_MAX     equ 4
XHCI_SLOTS      equ 16

; Variables (USB_SEG, after usb.inc's)
XHCI_RT         equ 0x1F18          ; dword: runtime registers
XHCI_DB         equ 0x1F1C          ; dword: doorbells
XHCI_CTXSZ      equ 0x1F20          ; byte: context size, 32 or 64
XHCI_MAXSLOT    equ 0x1F21          ; byte: slots enabled
XHCI_DEFSLOT    equ 0x1F22          ; byte: slot of the device at address 0
XHCI_CMDIDX     equ 0x1F24          ; byte: command ring enqueue index
XHCI_CMDCYC     equ 0x1F25          ; byte: and its cycle bit
XHCI_EVIDX      equ 0x1F26          ; byte: event ring dequeue index
XHCI_EVCYC      equ 0x1F27          ; byte: and its cycle bit
XHCI_EV2        equ 0x1F28          ; dword: last event's status
XHCI_EV3        equ 0x1F2C          ; dword: last event's control
XHCI_PEND       equ 0x1F30          ; the device being set up: as an
                                    ; XHCI_ADDR entry, then its TT dword

; The xHCI function at USB_BDF (BX): take it over, reset it, run it, then
; enumerate each root port with a device. The controller is halted and
; reset at the end. FS = USB_SEG. Keeps every register.
xhci_bus:
    pushad
    push gs
    mov byte [fs:USB_HC], USB_HC_XHCI
    mov di, XHCI_BAR
    call pci_read32
    test al, 1                  ; must be a memory BAR
    jnz .out
    test al, 4                  ; 64-bit: the upper half must be 0
    jz .bar32
    push eax
    mov di, XHCI_BAR + 4
    call pci_read32
    test eax, eax
    pop eax
    jnz .out
.bar32:
    and eax, 0xFFFFFFF0
    jz .out
    mov esi, eax                ; ESI = capability registers
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_MEM | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    call enable_a20
    call flat_gs

    ; SMM may own it (USB legacy support capability): ask for it back
    mov eax, [gs:esi + XHCI_HCCPARAMS1]
    shr eax, 16                 ; xECP, in dwords
.cap:
    test eax, eax
    jz .ours
    lea edi, [esi + eax * 4]
    mov eax, [gs:edi]
    cmp al, 1
    je .legacy
    movzx eax, ah               ; next, in dwords from this one
    test eax, eax
    jz .ours
    sub edi, esi
    shr edi, 2
    add eax, edi
    jmp .cap
.legacy:
    test eax, XLEG_BIOS
    jz .ours
    or eax, XLEG_OS
    mov [gs:edi], eax
    mov cx, 1000
.own:
    test dword [gs:edi], XLEG_BIOS
    jz .ours
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .own
    jmp .out
.ours:
    ; Where the register blocks are, and the controller's limits
    mov eax, [gs:esi + XHCI_RTSOFF]
    and eax, ~0x1F
    add eax, esi
    mov [fs:XHCI_RT], eax
    mov eax, [gs:esi + XHCI_DBOFF]
    and eax, ~0x03
    add eax, esi
    mov [fs:XHCI_DB], eax
    mov byte [fs:XHCI_CTXSZ], 32
    test dword [gs:esi + XHCI_HCCPARAMS1], XHCC_CSZ
    jz .ctx
    mov byte [fs:XHCI_CTXSZ], 64
.ctx:
    ; (dword reads only: some controllers ignore narrower ones)
    mov eax, [gs:esi + XHCI_HCSPARAMS1]
    mov ebp, eax
    shr ebp, 24                 ; BP = MaxPorts
    cmp al, XHCI_SLOTS          ; MaxSlots
    jbe .slots
    mov al, XHCI_SLOTS
.slots:
    mov [fs:XHCI_MAXSLOT], al
    mov eax, [gs:esi + XHCI_HCSPARAMS2]
    mov edx, eax
    shr eax, 27                 ; scratchpad buffers, low 5 bits
    shr edx, 21
    and edx, 0x1F
    shl edx, 5
    or eax, edx
    push ax                     ; scratchpad buffers wanted
    mov eax, [gs:esi]
    movzx eax, al               ; CAPLENGTH
    add esi, eax                ; ESI = operational registers from here on
    mov [fs:USB_MMIO], esi

    ; Stop it, then reset it
    and dword [gs:esi + XHCI_USBCMD], ~XCMD_RS
    call xhci_halt
    mov dword [gs:esi + XHCI_USBCMD], XCMD_HCRST
    mov cx, 1000
.reset:
    test dword [gs:esi + XHCI_USBCMD], XCMD_HCRST
    jnz .resetting
    test dword [gs:esi + XHCI_USBSTS], XSTS_CNR
    jz .reset_done
.resetting:
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .reset
    pop ax
    jmp .out
.reset_done:
    ; Fresh structures
    xor di, di
    mov cx, XHCI_INCTX / 4
    call xhci_zero
    mov di, XHCI_DEVCTX
    mov cx, (XHCI_SPARRAY - XHCI_DEVCTX) / 4
    call xhci_zero
    movzx eax, byte [fs:XHCI_MAXSLOT]
    mov [gs:esi + XHCI_CONFIG], eax
    mov dword [gs:esi + XHCI_DCBAAP], USB_PHYS + XHCI_DCBAA
    mov dword [gs:esi + XHCI_DCBAAP + 4], 0

    ; Scratchpad buffers, if it wants a few
    pop cx
    jcxz .no_scratch
    cmp cx, XHCI_SP_MAX
    ja .stop
    mov di, XHCI_SPPAGES
    xor bx, bx
.scratch:
    push cx
    mov cx, 1024
    call xhci_zero
    pop cx
    movzx eax, di
    add eax, USB_PHYS
    mov [fs:XHCI_SPARRAY + bx], eax
    mov dword [fs:XHCI_SPARRAY + bx + 4], 0
    add di, 0x1000
    add bx, 8
    loop .scratch
    mov dword [fs:XHCI_DCBAA], USB_PHYS + XHCI_SPARRAY
.no_scratch:

    ; The command ring, then the event ring on interrupter 0
    mov si, XHCI_CMDRING
    mov di, XHCI_CMDIDX
    call xhci_ring_init
    mov esi, [fs:USB_MMIO]
    mov dword [gs:esi + XHCI_CRCR], (USB_PHYS + XHCI_CMDRING) | 1
    mov dword [gs:esi + XHCI_CRCR + 4], 0
    mov dword [fs:XHCI_ERST], USB_PHYS + XHCI_EVRING
    mov dword [fs:XHCI_ERST + 4], 0
    mov dword [fs:XHCI_ERST + 8], XHCI_EV_TRBS
    mov byte [fs:XHCI_EVIDX], 0
    mov byte [fs:XHCI_EVCYC], 1
    mov edi, [fs:XHCI_RT]
    mov dword [gs:edi + XHCI_IMAN], 1          ; no interrupts; clear IP
    mov dword [gs:edi + XHCI_ERSTSZ], 1
    mov dword [gs:edi + XHCI_ERDP], USB_PHYS + XHCI_EVRING
    mov dword [gs:edi + XHCI_ERDP + 4], 0
    mov dword [gs:edi + XHCI_ERSTBA], USB_PHYS + XHCI_ERST
    mov dword [gs:edi + XHCI_ERSTBA + 4], 0
    mov dword [gs:esi + XHCI_USBCMD], XCMD_RS
    mov cx, 20
.run:
    test dword [gs:esi + XHCI_USBSTS], XSTS_HCH
    jz .running
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .run
    jmp .stop
.running:

    ; "usb: xHCI at bb:dd.f"
    mov si, msg_usb_xhci
    call usb_puts_head
    call usb_print_bdf
    call con_newline
    mov esi, [fs:USB_MMIO]

    ; Power every port, then 20 ms for it to settle
    xor ebx, ebx
.power:
    cmp bx, bp
    jae .powered
    mov edi, ebx
    shl edi, 4
    mov eax, [gs:esi + XHCI_PORTSC + edi]
    and eax, XPS_KEEP
    or eax, XPS_PP
    mov [gs:esi + XHCI_PORTSC + edi], eax
    inc bx
    jmp .power
.powered:
    mov cx, 20
    call delay_ms

    ; Each port with a device: a USB 2 port is reset to enable it (a USB 3
    ; port enables itself), then the device is enumerated at the speed the
    ; port reports
    xor ebx, ebx
.port:
    cmp bx, bp
    jae .stop
    mov edi, ebx
    shl edi, 4
    add edi, esi
    mov eax, [gs:edi + XHCI_PORTSC]
    test eax, XPS_CCS
    jz .next
    test eax, XPS_PED
    jnz .enabled
    and eax, XPS_KEEP
    or eax, XPS_PR
    mov [gs:edi + XHCI_PORTSC], eax
    mov cx, 50
.port_reset:
    push cx
    mov cx, 10
    call delay_ms
    pop cx
    test dword [gs:edi + XHCI_PORTSC], XPS_PRC
    jnz .reset_ended
    loop .port_reset
    jmp .next
.reset_ended:
    mov cx, 10                  ; reset recovery
    call delay_ms
.enabled:
    mov eax, [gs:edi + XHCI_PORTSC]
    mov edx, eax
    and eax, XPS_KEEP
    or eax, XPS_CSC | XPS_PRC   ; clear the change bits
    mov [gs:edi + XHCI_PORTSC], eax
    test edx, XPS_PED
    jz .next
    shr edx, 10
    and dl, 0x0F                ; speed: 1 full, 2 low, 3 high, 4+ super
    jz .next
    mov ah, dl
    dec ah
    cmp ah, 3
    jbe .speed
    mov ah, 3
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
    call flat_gs                ; xhci_control's POP GS left a real-mode GS
    mov esi, [fs:USB_MMIO]
.next:
    inc bx
    jmp .port

.stop:
    ; Halt it and reset it: no DMA after POST
    mov esi, [fs:USB_MMIO]
    and dword [gs:esi + XHCI_USBCMD], ~XCMD_RS
    call xhci_halt
    mov dword [gs:esi + XHCI_USBCMD], XCMD_HCRST
.out:
    pop gs
    popad
    ret

; Wait up to 20 ms for the controller at GS:ESI (operational registers) to
; halt. Keeps every register.
xhci_halt:
    push cx
    mov cx, 20
.wait:
    test dword [gs:esi + XHCI_USBSTS], XSTS_HCH
    jnz .out
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    loop .wait
.out:
    pop cx
    ret

; Zero CX dwords at FS:DI. Keeps every register.
xhci_zero:
    push cx
    push di
.z:
    mov dword [fs:di], 0
    add di, 4
    loop .z
    pop di
    pop cx
    ret

; Empty the ring at FS:SI with its state (index, cycle) at FS:DI: its last
; TRB links back to the start, not yet handed to the controller. Keeps
; every register.
xhci_ring_init:
    push eax
    push cx
    push di
    mov di, si
    mov cx, XHCI_RING_TRBS * 4
    call xhci_zero
    pop di
    movzx eax, si
    add eax, USB_PHYS
    mov [fs:si + (XHCI_RING_TRBS - 1) * 16], eax
    mov dword [fs:si + (XHCI_RING_TRBS - 1) * 16 + 12], TRB_LINK_CTRL
    mov byte [fs:di], 0
    mov byte [fs:di + 1], 1
    pop cx
    pop eax
    ret

; Slot SI: SI = its endpoint 0 ring, DI = that ring's state. Keeps every
; other register.
xhci_ring_of:
    mov di, si
    shl di, 1
    add di, XHCI_RSTATE
    dec si
    shl si, 8
    add si, XHCI_RINGS
    ret

; Put a TRB on the ring at FS:SI, state at FS:DI: EAX, EBX, ECX its first
; three dwords, EDX its control dword without the cycle bit. At the link
; TRB, hand it over and wrap. Keeps every register.
xhci_put:
    pushad
    movzx bp, byte [fs:di]
    shl bp, 4
    add bp, si
    mov [fs:bp], eax
    mov [fs:bp + 4], ebx
    mov [fs:bp + 8], ecx
    or dl, [fs:di + 1]
    mov [fs:bp + 12], edx       ; the cycle bit last: now it is the HC's
    inc byte [fs:di]
    cmp byte [fs:di], XHCI_RING_TRBS - 1
    jb .out
    mov eax, TRB_LINK_CTRL
    or al, [fs:di + 1]
    mov [fs:si + (XHCI_RING_TRBS - 1) * 16 + 12], eax
    mov byte [fs:di], 0
    xor byte [fs:di + 1], 1
.out:
    popad
    ret

; Wait up to 1000 ms for an event of type AL, skipping others (port status
; changes). Its status and control dwords go to XHCI_EV2 and XHCI_EV3. CF
; set on a timeout. GS flat. Keeps every register.
xhci_event:
    pushad
    movzx bp, al
    mov cx, 1000
.look:
    movzx di, byte [fs:XHCI_EVIDX]
    shl di, 4
    add di, XHCI_EVRING
    mov edx, [fs:di + 12]
    mov al, dl
    and al, 1
    cmp al, [fs:XHCI_EVCYC]
    jne .wait
    mov ebx, [fs:di + 8]
    ; Take it off the ring and tell the controller
    inc byte [fs:XHCI_EVIDX]
    cmp byte [fs:XHCI_EVIDX], XHCI_EV_TRBS
    jb .erdp
    mov byte [fs:XHCI_EVIDX], 0
    xor byte [fs:XHCI_EVCYC], 1
.erdp:
    movzx eax, byte [fs:XHCI_EVIDX]
    shl eax, 4
    add eax, USB_PHYS + XHCI_EVRING
    or al, 8                    ; EHB: write 1 to clear
    mov esi, [fs:XHCI_RT]
    mov [gs:esi + XHCI_ERDP], eax
    mov dword [gs:esi + XHCI_ERDP + 4], 0
    mov eax, edx
    shr eax, 10
    and ax, 0x3F
    cmp ax, bp
    jne .look
    mov [fs:XHCI_EV2], ebx
    mov [fs:XHCI_EV3], edx
    popad
    clc
    ret
.wait:
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    dec cx
    jnz .look
    popad
    stc
    ret

; Run a command: EAX, EBX its parameter dwords, EDX its control dword
; without the cycle bit. CF set unless it completed with success; the
; completion event is in XHCI_EV2 / XHCI_EV3. Keeps every register.
xhci_cmd:
    pushad
    xor ecx, ecx
    mov si, XHCI_CMDRING
    mov di, XHCI_CMDIDX
    call xhci_put
    mov esi, [fs:XHCI_DB]
    mov dword [gs:esi], 0       ; doorbell 0: the command ring
    mov al, TRB_EV_COMMAND
    call xhci_event
    jc .out
    cmp byte [fs:XHCI_EV2 + 3], CC_SUCCESS
    je .out                     ; CF clear
    stc
.out:
    popad
    ret

; Address Device for the device in XHCI_PEND, endpoint 0 max packet AX,
; BL = 1 to only set up the slot (BSR: no SET_ADDRESS on the bus). The
; endpoint 0 dequeue pointer is where its ring stands now. CF set on
; failure. Keeps every register.
xhci_address:
    pushad
    push bx
    movzx ebp, ax
    movzx cx, byte [fs:XHCI_CTXSZ]
    imul cx, cx, 3
    shr cx, 2
    mov di, XHCI_INCTX
    call xhci_zero
    mov dword [fs:XHCI_INCTX + 4], 3            ; add: slot and EP0
    ; Slot context: route string, speed, one entry, root port, TT
    movzx bx, byte [fs:XHCI_CTXSZ]
    add di, bx
    movzx eax, byte [fs:XHCI_PEND + 2]
    shl eax, 20
    or eax, [fs:XHCI_PEND + 4]
    or eax, 1 << 27
    mov [fs:di], eax
    movzx eax, byte [fs:XHCI_PEND + 1]
    shl eax, 16
    mov [fs:di + 4], eax
    mov eax, [fs:XHCI_PEND + 8]
    mov [fs:di + 8], eax
    ; Endpoint 0 context: control, 3 retries, max packet, dequeue
    add di, bx
    mov eax, ebp
    shl eax, 16
    or eax, (3 << 1) | (4 << 3)
    mov [fs:di + 4], eax
    mov dword [fs:di + 16], 8   ; average TRB length
    push di
    movzx si, byte [fs:XHCI_PEND]
    call xhci_ring_of
    movzx eax, byte [fs:di]
    shl ax, 4
    add ax, si
    add eax, USB_PHYS
    or al, [fs:di + 1]
    pop di
    mov [fs:di + 8], eax
    mov dword [fs:di + 12], 0
    ; The command
    movzx edx, byte [fs:XHCI_PEND]
    shl edx, 24
    or edx, TRB_ADDRESS << 10
    pop bx
    test bl, 1
    jz .bsr
    or edx, 1 << 9
.bsr:
    mov eax, USB_PHYS + XHCI_INCTX
    xor ebx, ebx
    call xhci_cmd
    popad
    ret

; usb_attach for xHCI: a new device at port DH of the hub at address DL
; (0: root port DH), speed AH. Give it a slot and set the slot up, still
; without an address; it becomes XHCI_DEFSLOT, the slot of address 0.
; CF set on failure. Keeps every register.
xhci_attach:
    pushad
    push gs
    call flat_gs
    mov al, ah
    inc al                      ; speed ID: full 1, low 2, high 3, super 4
    mov [fs:XHCI_PEND + 2], al
    mov dword [fs:XHCI_PEND + 8], 0
    test dl, dl
    jnz .hub
    mov [fs:XHCI_PEND + 1], dh  ; root port
    mov byte [fs:XHCI_PEND + 3], 0
    mov dword [fs:XHCI_PEND + 4], 0
    jmp .slot
.hub:
    ; Behind a hub: the hub's root port, its route string with this port
    ; added at its depth
    movzx bx, dl
    shl bx, 3
    add bx, XHCI_ADDR
    cmp byte [fs:bx], 0
    je .fail                    ; the hub has no slot
    mov al, [fs:bx + 1]
    mov [fs:XHCI_PEND + 1], al
    mov cl, [fs:bx + 3]
    mov al, cl
    inc al
    mov [fs:XHCI_PEND + 3], al
    movzx eax, dh
    cmp al, 15
    jbe .nibble
    mov al, 15
.nibble:
    shl cl, 2
    shl eax, cl
    or eax, [fs:bx + 4]
    mov [fs:XHCI_PEND + 4], eax
    ; A full- or low-speed device behind a high-speed hub: the hub's
    ; transaction translator
    cmp byte [fs:bx + 2], 3
    jne .slot
    cmp byte [fs:XHCI_PEND + 2], 3
    jae .slot
    movzx eax, dh
    shl eax, 8
    mov al, [fs:bx]
    mov [fs:XHCI_PEND + 8], eax
.slot:
    xor eax, eax
    xor ebx, ebx
    mov edx, TRB_ENABLE_SLOT << 10
    call xhci_cmd
    jc .fail
    movzx si, byte [fs:XHCI_EV3 + 3]
    mov ax, si
    test al, al
    jz .fail
    cmp al, [fs:XHCI_MAXSLOT]
    ja .fail
    mov [fs:XHCI_PEND], al
    mov [fs:XHCI_DEFSLOT], al
    ; Its output device context
    mov di, si
    dec di
    shl di, 11
    add di, XHCI_DEVCTX
    mov cx, 0x800 / 4
    call xhci_zero
    movzx eax, di
    add eax, USB_PHYS
    mov bx, si
    shl bx, 3
    mov [fs:XHCI_DCBAA + bx], eax
    mov dword [fs:XHCI_DCBAA + bx + 4], 0
    ; Its endpoint 0 ring
    call xhci_ring_of
    call xhci_ring_init
    ; Set up at the default max packet for its speed
    mov ax, 8
    cmp byte [fs:XHCI_PEND + 2], 2
    je .mps
    mov ax, 64
    cmp byte [fs:XHCI_PEND + 2], 4
    jne .mps
    mov ax, 512
.mps:
    mov bl, 1
    call xhci_address
    jc .fail
    pop gs
    popad
    clc
    ret
.fail:
    mov byte [fs:XHCI_DEFSLOT], 0
    pop gs
    popad
    stc
    ret

; usb_control for xHCI: AL = address (0: the slot xhci_attach set up),
; BL = max packet size of endpoint 0 (used by SET_ADDRESS; 512 at super
; speed); setup packet at USB_SETUP, data at USB_BUF. SET_ADDRESS is done
; by the controller (Address Device). CF set on a stall, an error or a
; timeout. Keeps every register.
xhci_control:
    pushad
    push gs
    call flat_gs
    cmp word [fs:USB_SETUP], USB_REQ_SET_ADDRESS << 8
    jne .transfer
    ; SET_ADDRESS
    movzx ax, bl
    cmp byte [fs:XHCI_PEND + 2], 4
    jne .addr
    mov ax, 512
.addr:
    mov bl, 0
    call xhci_address
    jc .fail
    movzx di, byte [fs:USB_SETUP + 2]   ; usb_enum's number for it
    shl di, 3
    add di, XHCI_ADDR
    mov eax, [fs:XHCI_PEND]
    mov [fs:di], eax
    mov eax, [fs:XHCI_PEND + 4]
    mov [fs:di + 4], eax
    jmp .ok

.transfer:
    movzx si, byte [fs:XHCI_DEFSLOT]
    test al, al
    jz .slot
    movzx di, al
    shl di, 3
    movzx si, byte [fs:XHCI_ADDR + di]
.slot:
    test si, si
    jz .fail
    movzx ecx, word [fs:USB_SETUP + 6]  ; ECX = bytes of data
    cmp ecx, USB_BUFLEN
    ja .fail
    push si                     ; the slot
    call xhci_ring_of
    ; Setup stage: the 8 bytes inside the TRB
    mov eax, [fs:USB_SETUP]
    mov ebx, [fs:USB_SETUP + 4]
    mov edx, (TRB_SETUP << 10) | (1 << 6)
    test ecx, ecx
    jz .setup
    or edx, 2 << 16             ; an OUT data stage follows
    test byte [fs:USB_SETUP], 0x80
    jz .setup
    or edx, 3 << 16             ; an IN data stage follows
.setup:
    push ecx
    mov ecx, 8
    call xhci_put
    pop ecx
    ; Data stage
    test ecx, ecx
    jz .status
    mov eax, USB_PHYS + USB_BUF
    xor ebx, ebx
    mov edx, TRB_DATA << 10
    test byte [fs:USB_SETUP], 0x80
    jz .data
    or edx, 1 << 16             ; IN
.data:
    call xhci_put
.status:
    ; Status stage: the other direction (IN after OUT or no data), with
    ; an event when it completes
    mov edx, (TRB_STATUS << 10) | (1 << 5) | (1 << 16)
    test ecx, ecx
    jz .last
    test byte [fs:USB_SETUP], 0x80
    jz .last
    and edx, ~(1 << 16)
.last:
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    call xhci_put
    pop bx                      ; the slot
    push bx
    movzx ebx, bx
    mov eax, [fs:XHCI_DB]
    mov dword [gs:eax + ebx * 4], 1     ; endpoint 0
    ; Its transfer event
.wait:
    mov al, TRB_EV_TRANSFER
    call xhci_event
    jc .recover
    pop ax
    push ax
    cmp [fs:XHCI_EV3 + 3], al
    jne .wait
    mov al, [fs:XHCI_EV2 + 3]
    cmp al, CC_SUCCESS
    je .done
    cmp al, CC_SHORT
    je .done
.recover:
    ; Stalled, failed or timed out: stop or reset endpoint 0 (whichever
    ; its state allows), then move its dequeue pointer past the TD
    pop ax
    movzx edx, ax
    shl edx, 24
    or edx, 1 << 16             ; endpoint 0 (DCI 1)
    xor eax, eax
    xor ebx, ebx
    push edx
    or edx, TRB_STOP_EP << 10
    call xhci_cmd
    pop edx
    push edx
    or edx, TRB_RESET_EP << 10
    call xhci_cmd
    pop edx
    or edx, TRB_SET_DEQ << 10
    movzx eax, byte [fs:di]
    shl ax, 4
    add ax, si
    add eax, USB_PHYS
    or al, [fs:di + 1]
    call xhci_cmd
    jmp .fail
.done:
    pop ax
.ok:
    pop gs
    popad
    clc
    ret
.fail:
    pop gs
    popad
    stc
    ret
