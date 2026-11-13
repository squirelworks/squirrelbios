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
; Slots are few (8): a device that is not a hub gives its slot back
; (Disable Slot) once it has been listed, so a bus of any size fits.
;
; What the controller reads and writes lives in one block, reached through
; the flat GS at XHCI_BASE. The block is in the EBDA (+5000h, nine pages)
; so that the controller can keep running after POST, unless a keyboard
; already runs from there or the controller wants more scratchpad pages
; than fit; then it is at XHCI_ALT, which is free only during POST.
; Offsets in the block:
;   0000h  device context base address array (slots 0-8)
;   0100h  event ring segment table (one entry)
;   0140h  scratchpad buffer array
;   0200h  command ring, 16 TRBs
;   0300h  event ring, 64 TRBs
;   0700h  the keyboard's interrupt ring: 15 TRBs and a link
;   0800h  endpoint 0 transfer rings, 16 TRBs per slot
;   1000h  input context (up to 33 contexts of 64 bytes)
;   1880h  the keyboard's report buffers, 15 of 8 bytes
;   1900h  the mouse's interrupt ring; 1A00h its report buffers
;   1B00h  the disk's bulk OUT ring; 1C00h its bulk IN ring
;   1D00h  its command block wrapper; 1D40h its status wrapper;
;          1D80h a buffer for INQUIRY, REQUEST SENSE, READ CAPACITY
;   2000h  output device contexts, 2 KB per slot (slots 1-8)
;   6000h  scratchpad pages: 3 in the EBDA, up to 10 at XHCI_ALT
; Software's own tables stay in USB_SEG (usb.inc):
;   B000h  ring state per slot: enqueue index, cycle bit
;   B100h  devices by address, 8 bytes each: slot, root port, speed ID,
;          hub depth, route string

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
TRB_NORMAL      equ 1
TRB_SETUP       equ 2
TRB_DATA        equ 3
TRB_STATUS      equ 4
TRB_LINK        equ 6
TRB_ENABLE_SLOT equ 9
TRB_DISABLE_SLOT equ 10
TRB_ADDRESS     equ 11
TRB_RESET_EP    equ 14
TRB_CONFIG_EP   equ 12
TRB_STOP_EP     equ 15
TRB_SET_DEQ     equ 16
TRB_EV_TRANSFER equ 32
TRB_EV_COMMAND  equ 33
TRB_LINK_CTRL   equ (TRB_LINK << 10) | 0x12     ; toggle cycle, chain
TRB_LINK_INT    equ (TRB_LINK << 10) | 0x02     ; toggle cycle
CC_SUCCESS      equ 1
CC_SHORT        equ 13

; The block (offsets from XHCI_BASE)
XHCI_DCBAA      equ 0x0000
XHCI_ERST       equ 0x0100
XHCI_SPARRAY    equ 0x0140
XHCI_CMDRING    equ 0x0200
XHCI_EVRING     equ 0x0300
XHCI_EV_TRBS    equ 64
XHCI_INTRING    equ 0x0700              ; the keyboard's interrupt ring
XHCI_INTBUF     equ 0x1880              ; and its report buffers
XHCI_MINTRING   equ 0x1900              ; the mouse's
XHCI_MINTBUF    equ 0x1A00
XHCI_BOUT       equ 0x1B00              ; the disk's bulk rings (msd.inc)
XHCI_BIN        equ 0x1C00
XHCI_CBW        equ 0x1D00
XHCI_CSW        equ 0x1D40
XHCI_SCSIBUF    equ 0x1D80
XHCI_DIN        equ 0x1E00              ; a UAS disk's data-in ring
XHCI_DOUT       equ 0x1F00              ; and data-out ring
; With streams, the UAS status and data pipes' stream context arrays, four
; entries each: in the gap after the input context, and in the one after
; the scratchpad array (a block in the EBDA has XHCI_EBDA_SP pages)
XHCI_SSTAT      equ 0x1840
XHCI_SDIN       equ 0x0160
XHCI_SDOUT      equ 0x01A0
XHCI_INT_TRBS   equ 16                  ; 15 reports, then the link
XHCI_RINGS      equ 0x0800
XHCI_RING_TRBS  equ 16
XHCI_INCTX      equ 0x1000
XHCI_DEVCTX     equ 0x2000
XHCI_SPPAGES    equ 0x6000
XHCI_SLOTS      equ 8
XHCI_EBDA       equ EBDA_BASE + 0x5000  ; the block, kept after POST
XHCI_EBDA_SP    equ 3                   ; scratchpad pages that fit there
%if XHCI_SPARRAY + XHCI_EBDA_SP * 8 > XHCI_SDIN || XHCI_SDOUT + 64 > XHCI_CMDRING
%error "the UAS stream context arrays overlap the block's scratchpad array or command ring"
%endif
XHCI_ALT        equ 0x40000             ; the block, POST only
XHCI_ALT_SP     equ 10
; In USB_SEG
XHCI_RSTATE     equ 0xB000
XHCI_ADDR       equ 0xB100

%if EBDA_SIZE < 0x5000 + XHCI_SPPAGES + XHCI_EBDA_SP * 0x1000
%error "the xHCI block needs a 56 KB or larger EBDA"
%endif

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
XHCI_EV0        equ 0x1F3C          ; dword: last event's TRB pointer
XHCI_BASE       equ 0x1F48          ; dword: the block's linear address
XHCI_HIDRING    equ 0x1F4C          ; dword: the ring xhci_hid_config fills

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
    ; Where its block goes: the EBDA, unless a keyboard already runs from
    ; there or the scratchpad pages do not fit
    pop cx                      ; scratchpad buffers wanted
    mov ebx, XHCI_EBDA
    cmp byte [gs:dword EBDA_BASE + USBK_KIND], 0
    jne .alt
    cmp cx, XHCI_EBDA_SP
    jbe .block
.alt:
    mov ebx, XHCI_ALT
    cmp cx, XHCI_ALT_SP
    ja .stop
.block:
    mov [fs:XHCI_BASE], ebx
    ; Fresh structures: the block up to the scratchpad pages, and the
    ; tables in USB_SEG
    mov edi, ebx
    push cx
    mov cx, XHCI_SPPAGES / 4
    call xhci_zero
    xor di, di
.tables:
    mov dword [fs:XHCI_RSTATE + di], 0
    add di, 4
    cmp di, 0x800
    jb .tables
    pop cx
    movzx eax, byte [fs:XHCI_MAXSLOT]
    mov [gs:esi + XHCI_CONFIG], eax
    lea eax, [ebx + XHCI_DCBAA]
    mov [gs:esi + XHCI_DCBAAP], eax
    mov dword [gs:esi + XHCI_DCBAAP + 4], 0

    ; Scratchpad buffers, if it wants a few
    jcxz .no_scratch
    lea edi, [ebx + XHCI_SPPAGES]
    lea edx, [ebx + XHCI_SPARRAY]
    mov [gs:ebx + XHCI_DCBAA], edx
.scratch:
    push cx
    mov cx, 1024
    call xhci_zero
    pop cx
    mov [gs:edx], edi
    mov dword [gs:edx + 4], 0
    add edi, 0x1000
    add edx, 8
    loop .scratch
.no_scratch:

    ; The command ring, then the event ring on interrupter 0
    lea esi, [ebx + XHCI_CMDRING]
    mov di, XHCI_CMDIDX
    call xhci_ring_init
    or esi, 1                   ; RCS
    mov edi, [fs:USB_MMIO]
    mov [gs:edi + XHCI_CRCR], esi
    mov dword [gs:edi + XHCI_CRCR + 4], 0
    mov esi, edi
    lea eax, [ebx + XHCI_EVRING]
    mov [gs:ebx + XHCI_ERST], eax
    mov dword [gs:ebx + XHCI_ERST + 4], 0
    mov dword [gs:ebx + XHCI_ERST + 8], XHCI_EV_TRBS
    mov byte [fs:XHCI_EVIDX], 0
    mov byte [fs:XHCI_EVCYC], 1
    mov edi, [fs:XHCI_RT]
    mov dword [gs:edi + XHCI_IMAN], 1          ; no interrupts; clear IP
    mov dword [gs:edi + XHCI_ERSTSZ], 1
    mov [gs:edi + XHCI_ERDP], eax
    mov dword [gs:edi + XHCI_ERDP + 4], 0
    lea eax, [ebx + XHCI_ERST]
    mov [gs:edi + XHCI_ERSTBA], eax
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
    call usb_newline
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
    ; The keyboard kept for after POST is here: keep the controller
    ; running from the EBDA, with only that keyboard's transfers
    mov esi, [fs:USB_MMIO]
    call usb_hid_here
    jc .halt_it
    call xhci_hid_start
    call usbd_keep              ; the disk's copy of the controller's state
    call usb_hid_announce       ; after: it needs ES:DI, the console cursor
    jmp .out
.halt_it:
    ; Halt it and reset it: no DMA after POST
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

; Zero CX dwords at linear EDI (GS flat). Keeps every register.
xhci_zero:
    push cx
    push edi
.z:
    mov dword [gs:edi], 0
    add edi, 4
    loop .z
    pop edi
    pop cx
    ret

; Empty the ring at linear ESI with its state (index, cycle) at FS:DI: its
; last TRB links back to the start, not yet handed to the controller. GS
; flat. Keeps every register.
xhci_ring_init:
    push cx
    push edi
    mov edi, esi
    mov cx, XHCI_RING_TRBS * 4
    call xhci_zero
    pop edi
    mov [gs:esi + (XHCI_RING_TRBS - 1) * 16], esi
    mov dword [gs:esi + (XHCI_RING_TRBS - 1) * 16 + 12], TRB_LINK_CTRL
    mov byte [fs:di], 0
    mov byte [fs:di + 1], 1
    pop cx
    ret

; Slot SI: ESI = its endpoint 0 ring (linear), DI = that ring's state.
; Keeps every other register.
xhci_ring_of:
    mov di, si
    shl di, 1
    add di, XHCI_RSTATE
    and esi, 0xFFFF
    dec esi
    shl esi, 8
    add esi, [fs:XHCI_BASE]
    add esi, XHCI_RINGS
    ret

; Linear address of the next TRB on the ring at linear ESI, state at FS:DI,
; with its cycle bit in bit 0 (a dequeue pointer): EAX. Keeps every other
; register.
xhci_ring_ptr:
    movzx eax, byte [fs:di]
    shl eax, 4
    add eax, esi
    or al, [fs:di + 1]
    ret

; Put a TRB on the ring at linear ESI, state at FS:DI: EAX, EBX, ECX its
; first three dwords, EDX its control dword without the cycle bit. At the
; link TRB, hand it over and wrap. GS flat. Keeps every register.
xhci_put:
    pushad
    movzx ebp, byte [fs:di]
    shl ebp, 4
    add ebp, esi
    mov [gs:ebp], eax
    mov [gs:ebp + 4], ebx
    mov [gs:ebp + 8], ecx
    or dl, [fs:di + 1]
    mov [gs:ebp + 12], edx      ; the cycle bit last: now it is the HC's
    inc byte [fs:di]
    cmp byte [fs:di], XHCI_RING_TRBS - 1
    jb .out
    mov eax, edx                ; the link chains as the TRB before it
    and eax, 0x10
    or eax, TRB_LINK_INT
    or al, [fs:di + 1]
    mov [gs:esi + (XHCI_RING_TRBS - 1) * 16 + 12], eax
    mov byte [fs:di], 0
    xor byte [fs:di + 1], 1
.out:
    popad
    ret

; Wait up to 1000 ms for an event of type AL, skipping others (port status
; changes). Its TRB pointer (low half), status and control dwords go to
; XHCI_EV0, XHCI_EV2 and XHCI_EV3. CF
; set on a timeout. GS flat. Keeps every register.
xhci_event:
    pushad
    movzx bp, al
    mov cx, 1000
.look:
    movzx edi, byte [fs:XHCI_EVIDX]
    shl edi, 4
    add edi, [fs:XHCI_BASE]
    mov edx, [gs:edi + XHCI_EVRING + 12]
    mov al, dl
    and al, 1
    cmp al, [fs:XHCI_EVCYC]
    jne .wait
    mov ebx, [gs:edi + XHCI_EVRING + 8]
    ; Take it off the ring and tell the controller
    inc byte [fs:XHCI_EVIDX]
    cmp byte [fs:XHCI_EVIDX], XHCI_EV_TRBS
    jb .erdp
    mov byte [fs:XHCI_EVIDX], 0
    xor byte [fs:XHCI_EVCYC], 1
.erdp:
    movzx eax, byte [fs:XHCI_EVIDX]
    shl eax, 4
    add eax, [fs:XHCI_BASE]
    add eax, XHCI_EVRING
    or al, 8                    ; EHB: write 1 to clear
    mov esi, [fs:XHCI_RT]
    mov [gs:esi + XHCI_ERDP], eax
    mov dword [gs:esi + XHCI_ERDP + 4], 0
    mov eax, edx
    shr eax, 10
    and ax, 0x3F
    cmp ax, bp
    jne .look
    mov eax, [gs:edi + XHCI_EVRING]
    mov [fs:XHCI_EV0], eax
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
    mov esi, [fs:XHCI_BASE]
    add esi, XHCI_CMDRING
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
    mov edi, [fs:XHCI_BASE]
    add edi, XHCI_INCTX
    call xhci_zero
    mov dword [gs:edi + 4], 3   ; add: slot and EP0
    ; Slot context: route string, speed, one entry, root port, TT
    movzx ebx, byte [fs:XHCI_CTXSZ]
    add edi, ebx
    movzx eax, byte [fs:XHCI_PEND + 2]
    shl eax, 20
    or eax, [fs:XHCI_PEND + 4]
    or eax, 1 << 27
    mov [gs:edi], eax
    movzx eax, byte [fs:XHCI_PEND + 1]
    shl eax, 16
    mov [gs:edi + 4], eax
    mov eax, [fs:XHCI_PEND + 8]
    mov [gs:edi + 8], eax
    ; Endpoint 0 context: control, 3 retries, max packet, dequeue
    add edi, ebx
    mov eax, ebp
    shl eax, 16
    or eax, (3 << 1) | (4 << 3)
    mov [gs:edi + 4], eax
    mov dword [gs:edi + 16], 8  ; average TRB length
    push edi
    movzx si, byte [fs:XHCI_PEND]
    call xhci_ring_of
    call xhci_ring_ptr
    pop edi
    mov [gs:edi + 8], eax
    mov dword [gs:edi + 12], 0
    ; The command
    movzx edx, byte [fs:XHCI_PEND]
    shl edx, 24
    or edx, TRB_ADDRESS << 10
    pop bx
    test bl, 1
    jz .bsr
    or edx, 1 << 9
.bsr:
    mov eax, [fs:XHCI_BASE]
    add eax, XHCI_INCTX
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
    mov byte [fs:XHCI_PEND], 0
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
    movzx edi, si
    dec edi
    shl edi, 11
    mov ebx, [fs:XHCI_BASE]
    lea edi, [ebx + edi + XHCI_DEVCTX]
    mov cx, 0x800 / 4
    call xhci_zero
    movzx eax, si
    lea ebx, [ebx + eax * 8 + XHCI_DCBAA]
    mov [gs:ebx], edi
    mov dword [gs:ebx + 4], 0
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
    movzx edx, byte [fs:XHCI_PEND]
    test dl, dl
    jz .failed
    call xhci_disable           ; a slot it got but could not set up
.failed:
    pop gs
    popad
    stc
    ret

; usb_detach for xHCI: give back the slot of the device at address AL, or
; of the one at address 0 if it never got AL, unless it is the keyboard or
; mouse kept for after POST. Keeps every register.
xhci_detach:
    pushad
    push gs
    call flat_gs
    mov dx, [fs:USB_BDF]
    cmp dx, [fs:USB_KBD_BDF]
    jne .not_kept
    cmp al, [fs:USB_KBD_ADDR]
    je .out
    cmp al, [fs:USB_MOU_ADDR]
    je .out
    cmp al, [fs:USBD_ADDR]
    je .out
.not_kept:
    movzx bx, al
    shl bx, 3
    movzx edx, byte [fs:XHCI_ADDR + bx]
    mov byte [fs:XHCI_ADDR + bx], 0
    test dl, dl
    jnz .slot
    movzx edx, byte [fs:XHCI_DEFSLOT]
    test dl, dl
    jz .out
.slot:
    cmp dl, [fs:XHCI_DEFSLOT]
    jne .disable
    mov byte [fs:XHCI_DEFSLOT], 0
.disable:
    call xhci_disable
.out:
    pop gs
    popad
    ret

; Disable Slot EDX (GS flat). Keeps every register.
xhci_disable:
    pushad
    shl edx, 24
    or edx, TRB_DISABLE_SLOT << 10
    xor eax, eax
    xor ebx, ebx
    call xhci_cmd
    popad
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
    call xhci_ring_ptr
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

; Device BX (0 the keyboard, 1 the mouse) at address AL, interrupt IN
; endpoint CL, max packet DL is being claimed on this xHCI, whose block is
; in the EBDA: fill its interrupt ring with 15 Normal TRBs (no events: a
; finished one is seen by its buffer, which starts as all ones) and add
; the endpoint to its slot (Configure Endpoint). CF set on failure. Keeps
; every register.
xhci_hid_config:
    pushad
    push gs
    call flat_gs
    shl bx, 2
    mov si, [cs:xhci_hid_rings + bx]        ; its ring, then its buffers
    mov di, [cs:xhci_hid_rings + bx + 2]
    movzx bx, al
    shl bx, 3
    movzx ebp, byte [fs:XHCI_ADDR + bx]     ; EBP = its slot
    test bp, bp
    jz .fail
    movzx edx, dl                           ; EDX = max packet
    movzx ecx, cl
    lea ecx, [ecx * 2 + 1]                  ; ECX = device context index
    mov ebx, [fs:XHCI_BASE]
    ; The ring: TRB i reads 8 bytes into buffer i; the last one links back
    movzx eax, si
    movzx esi, di
    add esi, ebx
    lea edi, [ebx + eax]
    mov [fs:XHCI_HIDRING], edi
    push cx
    mov cx, XHCI_INT_TRBS - 1
.trb:
    mov dword [gs:esi], -1
    mov dword [gs:esi + 4], -1
    mov [gs:edi], esi
    mov dword [gs:edi + 4], 0
    mov dword [gs:edi + 8], 8
    mov dword [gs:edi + 12], (TRB_NORMAL << 10) | 1
    add esi, 8
    add edi, 16
    loop .trb
    pop cx
    mov eax, [fs:XHCI_HIDRING]
    mov [gs:edi], eax
    mov dword [gs:edi + 4], 0
    mov dword [gs:edi + 8], 0
    mov dword [gs:edi + 12], TRB_LINK_INT | 1
    ; Input context: add the slot and this endpoint
    lea edi, [ebx + XHCI_INCTX]
    push cx
    mov cx, 0x840 / 4
    call xhci_zero
    pop cx
    mov eax, 1
    shl eax, cl
    or al, 1
    mov [gs:edi + 4], eax
    ; Slot context: as the controller has it, with the entries up to this
    ; endpoint
    lea esi, [ebp - 1]
    shl esi, 11
    lea esi, [ebx + esi + XHCI_DEVCTX]
    movzx eax, byte [fs:XHCI_CTXSZ]
    push edi
    add edi, eax
    mov eax, [gs:esi]
    and eax, ~(0x1F << 27)
    push ecx
    shl ecx, 27
    or eax, ecx
    pop ecx
    mov [gs:edi], eax
    mov eax, [gs:esi + 4]
    mov [gs:edi + 4], eax
    mov eax, [gs:esi + 8]
    mov [gs:edi + 8], eax
    pop edi
    ; Endpoint context: interrupt IN, every 8 ms (2^6 x 125 us), 3 retries
    movzx eax, byte [fs:XHCI_CTXSZ]
    lea esi, [ecx + 1]
    imul esi, eax
    add esi, edi
    mov dword [gs:esi], 6 << 16
    mov eax, edx
    shl eax, 16
    or eax, (3 << 1) | (7 << 3)
    mov [gs:esi + 4], eax
    mov eax, [fs:XHCI_HIDRING]
    inc eax                                 ; dequeue, cycle 1
    mov [gs:esi + 8], eax
    mov dword [gs:esi + 12], 0
    mov eax, edx
    shl eax, 16
    or eax, 8                               ; average TRB length, max ESIT payload
    mov [gs:esi + 16], eax
    ; The command
    mov eax, edi
    xor ebx, ebx
    mov edx, ebp
    shl edx, 24
    or edx, TRB_CONFIG_EP << 10
    call xhci_cmd
    jc .fail
    pop gs
    popad
    clc
    ret
.fail:
    pop gs
    popad
    stc
    ret

; The disk at address AL, bulk IN endpoint CL, bulk OUT endpoint CH, max
; packet DX, is being claimed on this xHCI, whose block is in the EBDA:
; give each bulk endpoint an empty ring (state at USBD_INST, USBD_OUTST)
; and add both to its slot (Configure Endpoint). For a UAS disk
; (USBD_UAS, USBD_EPDIN, USBD_EPDOUT already set) CL is the status pipe
; and CH the command pipe, and the data pipes are added too (.uas).
; USBD_SLOT = its slot. CF set on failure. Keeps every register.
xhci_msd_config:
    pushad
    push gs
    call flat_gs
    movzx bx, al
    shl bx, 3
    movzx ebp, byte [fs:XHCI_ADDR + bx]     ; EBP = its slot
    test bp, bp
    jz .fail
    mov [fs:USBD_SLOT], bp
    mov ebx, [fs:XHCI_BASE]
    lea esi, [ebx + XHCI_BOUT]
    mov di, USBD_OUTST
    call xhci_ring_init
    lea esi, [ebx + XHCI_BIN]
    mov di, USBD_INST
    call xhci_ring_init
    ; Input context: add the slot and both endpoints
    lea edi, [ebx + XHCI_INCTX]
    push cx
    mov cx, 0x840 / 4
    call xhci_zero
    pop cx
    movzx eax, cl
    lea eax, [eax * 2 + 1]                  ; EAX = IN's context index
    movzx esi, ch
    shl esi, 1                              ; ESI = OUT's
    bts dword [gs:edi + 4], eax
    bts dword [gs:edi + 4], esi
    or dword [gs:edi + 4], 1
    cmp eax, esi                            ; ESI = the higher of the two
    jb .last
    mov esi, eax
.last:
    ; Slot context: as the controller has it, with the entries up to the
    ; last endpoint
    push edi
    movzx eax, byte [fs:XHCI_CTXSZ]
    add edi, eax
    lea eax, [ebp - 1]
    shl eax, 11
    lea eax, [ebx + eax + XHCI_DEVCTX]
    push ecx
    mov ecx, [gs:eax]
    and ecx, ~(0x1F << 27)
    shl esi, 27
    or ecx, esi
    mov [gs:edi], ecx
    mov ecx, [gs:eax + 4]
    mov [gs:edi + 4], ecx
    mov ecx, [gs:eax + 8]
    mov [gs:edi + 8], ecx
    pop ecx
    pop edi
    ; The two endpoint contexts: bulk OUT (type 2), bulk IN (type 6)
    movzx eax, ch
    shl eax, 1
    lea esi, [ebx + XHCI_BOUT + 1]          ; dequeue, cycle 1
    push ecx
    mov cl, 2
    call .ep
    pop ecx
    movzx eax, cl
    lea eax, [eax * 2 + 1]
    lea esi, [ebx + XHCI_BIN + 1]
    push ecx
    mov cl, 6
    call .ep
    pop ecx                                 ; CL = the IN endpoint, for .uas
    ; A UAS disk: its data pipes too, and streams
    cmp byte [fs:USBD_UAS], 0
    je .cmd
    call .uas
.cmd:
    ; The command
    mov eax, edi
    xor ebx, ebx
    mov edx, ebp
    shl edx, 24
    or edx, TRB_CONFIG_EP << 10
    call xhci_cmd
    jc .fail
    pop gs
    popad
    clc
    ret
.fail:
    pop gs
    popad
    stc
    ret

; UAS (input context at EDI, block at EBX, status pipe CL, max packet
; DX): the data-in and data-out pipes get rings (XHCI_DIN, XHCI_DOUT) and
; endpoint contexts too, and the slot context counts up to the highest.
; With streams (USBD_UAS = 2), the status and both data pipes each get a
; stream context array whose stream 1 is their ring: every command uses
; tag 1 (a controller without streams fails the Configure Endpoint).
; Keeps every register.
.uas:
    pushad
    push edi
    lea esi, [ebx + XHCI_DIN]
    mov di, USBD_DINST
    call xhci_ring_init
    lea esi, [ebx + XHCI_DOUT]
    mov di, USBD_DOUTST
    call xhci_ring_init
    pop edi
    ; Their contexts: add flags, then the slot's last context index
    movzx eax, byte [fs:USBD_EPDIN]
    lea eax, [eax * 2 + 1]
    bts dword [gs:edi + 4], eax
    call .uas_last
    push ecx
    lea esi, [ebx + XHCI_DIN + 1]
    mov cl, 6
    call .ep
    pop ecx
    movzx eax, byte [fs:USBD_EPDOUT]
    shl eax, 1
    bts dword [gs:edi + 4], eax
    call .uas_last
    push ecx
    lea esi, [ebx + XHCI_DOUT + 1]
    mov cl, 2
    call .ep
    pop ecx
    cmp byte [fs:USBD_UAS], 2
    jne .uas_ok
    ; Streams on the status pipe and both data pipes
    movzx eax, cl
    lea eax, [eax * 2 + 1]
    lea esi, [ebx + XHCI_BIN]
    lea edx, [ebx + XHCI_SSTAT]
    call .streams
    movzx eax, byte [fs:USBD_EPDIN]
    lea eax, [eax * 2 + 1]
    lea esi, [ebx + XHCI_DIN]
    lea edx, [ebx + XHCI_SDIN]
    call .streams
    movzx eax, byte [fs:USBD_EPDOUT]
    shl eax, 1
    lea esi, [ebx + XHCI_DOUT]
    lea edx, [ebx + XHCI_SDOUT]
    call .streams
.uas_ok:
    popad
    clc
    ret

; The input slot context (after the control context at EDI) counts at
; least up to context index EAX. Keeps every register.
.uas_last:
    push ebx
    push ecx
    movzx ebx, byte [fs:XHCI_CTXSZ]
    mov ecx, [gs:edi + ebx]
    shr ecx, 27
    cmp ecx, eax
    jae .uas_last_out
    mov ecx, eax
    shl ecx, 27
    and dword [gs:edi + ebx], ~(0x1F << 27)
    or [gs:edi + ebx], ecx
.uas_last_out:
    pop ecx
    pop ebx
    ret

; Endpoint context EAX of the input context at EDI takes the stream
; context array at linear EDX (four entries: MaxPStreams 1, linear), whose
; stream 1 is the ring at linear ESI (cycle 1, a primary ring). Keeps
; every register.
.streams:
    pushad
    push edi
    mov edi, edx
    mov cx, 64 / 4
    call xhci_zero
    pop edi
    or esi, 3                   ; DCS 1, SCT 1: a primary transfer ring
    mov [gs:edx + 16], esi
    inc eax
    movzx ebx, byte [fs:XHCI_CTXSZ]
    imul eax, ebx
    add edi, eax
    or dword [gs:edi], (1 << 10) | (1 << 15)    ; MaxPStreams 1, LSA
    mov [gs:edi + 8], edx       ; the array, not a ring: no cycle bit
    mov dword [gs:edi + 12], 0
    popad
    ret
; Endpoint context EAX of the input context at EDI: type CL, max packet
; DX, 3 retries, dequeue pointer ESI. Keeps EBX, EDI, EDX.
.ep:
    push ebx
    push edi
    inc eax
    movzx ebx, byte [fs:XHCI_CTXSZ]
    imul eax, ebx
    add edi, eax
    mov dword [gs:edi], 0
    movzx eax, dx
    shl eax, 16
    shl cl, 3
    or al, cl
    or al, 3 << 1
    mov [gs:edi + 4], eax
    mov [gs:edi + 8], esi
    mov dword [gs:edi + 12], 0
    mov dword [gs:edi + 16], 512            ; average TRB length
    pop edi
    pop ebx
    ret

; Ring and buffer offsets in the block, per device (keyboard, mouse)
xhci_hid_rings:
    dw XHCI_INTRING, XHCI_INTBUF
    dw XHCI_MINTRING, XHCI_MINTBUF

; The bus with the keyboard or mouse is listed: leave the controller
; running and start their endpoints (their doorbells). From here on IRQ0
; reads their reports (usb_hid_poll). GS flat. Keeps every register.
xhci_hid_start:
    pushad
    push es
    push word USBK_SEG
    pop es
    mov al, USBK_KIND_XHCI
    call usb_hid_begin
    mov bx, 1
.dev:
    bt word [es:USBK_DEVS], bx
    jnc .next
    call usb_hid_dev                    ; DI = its variables
    mov byte [es:di + USBK_XIDX], 0
    mov byte [es:di + USBK_XCYC], 1
    push bx
    shl bx, 2
    mov ax, [cs:xhci_hid_rings + bx]
    add ax, XHCI_EBDA - EBDA_BASE
    mov [es:di + USBK_XRING], ax
    mov ax, [cs:xhci_hid_rings + bx + 2]
    add ax, XHCI_EBDA - EBDA_BASE
    mov [es:di + USBK_XBUF], ax
    pop bx
    ; Its doorbell: slot, endpoint's context index
    push bx
    shl bx, 4
    movzx ax, byte [fs:USB_KBD_ADDR + bx]
    movzx ecx, byte [fs:USB_KBD_EP + bx]
    mov bx, ax
    shl bx, 3
    movzx ebx, byte [fs:XHCI_ADDR + bx]
    lea eax, [ecx * 2 + 1]
    mov edx, [fs:XHCI_DB]
    mov [gs:edx + ebx * 4], eax
    pop bx
.next:
    dec bx
    jns .dev
    pop es
    popad
    ret
