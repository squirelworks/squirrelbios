; SquirrelBIOS x86 — virtio-net: send and receive Ethernet frames at POST
; Original code. Included by x86/squirrel-bios.asm when CONFIG_NETBOOT.
;
; Written from the Virtual I/O Device (VIRTIO) specification 1.1, its
; legacy interface (sections 4.1.4.8 and 5.1): the first PCI function
; with vendor 1AF4h and device 1000h (a transitional network device)
; whose BAR0 is an I/O range. Only the network boot (boot/netboot.inc)
; uses it, during POST: everything is in NET_SEG (64 KB of conventional
; memory that nothing else holds by then), reached through FS, and the
; device is reset again before anything is booted.
;
; Receive queue 0 holds 16 buffers of 2 KB, each two descriptors (the
; 10-byte virtio-net header, then the frame), handed back to the device
; once their frame has been looked at. Transmit queue 1 always sends
; descriptors 0 and 1: a zero header, then the frame in NET_TX. The only
; feature taken is MAC, for the device's address.
;
; NET_SEG layout:
;   0000h  receive queue (3 pages)     3000h  transmit queue (3 pages)
;   6000h  transmit header; 6010h the frame to send
;   6600h  variables (netboot.inc's after 6620h)
;   6800h  netboot's stack, down from 7000h
;   7000h  receive buffers, 16 of 800h: header at +0, frame at +10h

NET_SEG         equ 0x4000
NET_LIN         equ NET_SEG << 4
NET_RXQ         equ 0x0000
NET_TXQ         equ 0x3000
NET_TXHDR       equ 0x6000
NET_TX          equ 0x6010
NET_STACK       equ 0x7000
NET_RXBUF       equ 0x7000
NET_RXN         equ 16
NET_RXSZ        equ 0x800
NET_FRAME       equ 0x10                ; a frame's offset in its buffer
NET_HDRLEN      equ 10                  ; the legacy virtio-net header

NV_IO           equ 0x6600              ; word: I/O base
NV_BDF          equ 0x6602              ; word: its PCI address
NV_RXSIZE       equ 0x6604              ; word: receive queue size
NV_TXSIZE       equ 0x6606              ; word: transmit queue size
NV_RXSEEN       equ 0x6608              ; word: receive used index seen
NV_TXSEEN       equ 0x660A              ; word: transmit used index seen
NV_RXAVAIL      equ 0x660C              ; word: receive available ring
NV_RXUSED       equ 0x660E              ; word: receive used ring
NV_TXAVAIL      equ 0x6610
NV_TXUSED       equ 0x6612
NV_HELD         equ 0x6614              ; word: buffer being looked at
                                        ; (its first descriptor; FFFFh none)

VNET_DEVICE     equ 0x1000
VNET_F_MAC      equ 1 << 5
VNET_MAC        equ VIO_CONFIG          ; its 6 bytes, with VNET_F_MAC

; Find the device, reset it and set up both queues, its receive buffers
; offered. FS = NET_SEG. CF set if there is none or it cannot be set up.
; NN_MAC (netboot.inc) = its address. Keeps every register.
vnet_open:
    pushad
    call pci_present
    jc .none
    xor bx, bx
.fn:
    call pci_next
    jc .none
    xor di, di
    call pci_read32
    cmp eax, (VNET_DEVICE << 16) | VBLK_VENDOR
    jne .next
    mov di, PCI_BAR0
    call pci_read32
    test al, 1
    jz .next
    and ax, 0xFFFC
    jz .next
    mov si, ax
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_IO | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    push bx
    call vnet_init
    pop bx
    jnc .found
.next:
    inc bl
    jnz .fn
.none:
    popad
    stc
    ret
.found:
    popad
    clc
    ret

; The device at I/O base SI, PCI address BX. CF set on failure, and the
; device is reset. Clobbers EAX, ECX, EDX, DI, BP.
vnet_init:
    mov [fs:NV_IO], si
    mov [fs:NV_BDF], bx
    lea dx, [si + VIO_STATUS]
    xor al, al
    out dx, al
    mov al, VIO_ACK
    out dx, al
    or al, VIO_DRIVER
    out dx, al
    lea dx, [si + VIO_HOST_F]
    in eax, dx
    test eax, VNET_F_MAC
    jz .fail
    lea dx, [si + VIO_GUEST_F]
    mov eax, VNET_F_MAC
    out dx, eax
    ; The receive queue: NET_RXN buffers of two descriptors each
    xor ax, ax
    mov bx, NET_RXQ
    call vnet_queue
    jc .fail
    cmp cx, NET_RXN * 2
    jb .fail
    mov [fs:NV_RXSIZE], cx
    mov [fs:NV_RXAVAIL], dx
    mov [fs:NV_RXUSED], bp
    xor di, di                  ; descriptor 2k, then 2k+1
    mov eax, NET_LIN + NET_RXBUF
    xor cx, cx
.rx:
    mov [fs:NET_RXQ + di], eax
    mov dword [fs:NET_RXQ + di + 8], NET_HDRLEN
    mov word [fs:NET_RXQ + di + 12], VRING_NEXT | VRING_WRITE
    mov bx, cx
    shl bx, 1
    inc bx
    mov [fs:NET_RXQ + di + 14], bx
    add eax, NET_FRAME
    mov [fs:NET_RXQ + di + 16], eax
    mov dword [fs:NET_RXQ + di + 16 + 8], NET_RXSZ - NET_FRAME
    mov word [fs:NET_RXQ + di + 16 + 12], VRING_WRITE
    add eax, NET_RXSZ - NET_FRAME
    dec bx                      ; its first descriptor, on the ring
    mov bp, cx
    shl bp, 1
    add bp, dx
    mov [fs:bp + 4], bx
    add di, 32
    inc cx
    cmp cx, NET_RXN
    jb .rx
    mov bp, dx
    mov word [fs:bp + 2], NET_RXN
    mov word [fs:NV_RXSEEN], 0
    mov word [fs:NV_HELD], 0xFFFF
    ; The transmit queue: descriptors 0 and 1, header then frame
    mov ax, 1
    mov bx, NET_TXQ
    call vnet_queue
    jc .fail
    mov [fs:NV_TXSIZE], cx
    mov [fs:NV_TXAVAIL], dx
    mov [fs:NV_TXUSED], bp
    mov word [fs:NV_TXSEEN], 0
    mov dword [fs:NET_TXQ], NET_LIN + NET_TXHDR
    mov dword [fs:NET_TXQ + 8], NET_HDRLEN
    mov dword [fs:NET_TXQ + 12], (1 << 16) | VRING_NEXT
    mov dword [fs:NET_TXQ + 16], NET_LIN + NET_TX
    xor eax, eax
    mov [fs:NET_TXHDR], eax
    mov [fs:NET_TXHDR + 4], eax
    mov [fs:NET_TXHDR + 8], ax
    ; Its address, then running
    lea dx, [si + VNET_MAC]
    xor bx, bx
.mac:
    in al, dx
    mov [fs:NN_MAC + bx], al
    inc dx
    inc bx
    cmp bx, 6
    jb .mac
    lea dx, [si + VIO_STATUS]
    mov al, VIO_ACK | VIO_DRIVER | VIO_DRIVER_OK
    out dx, al
    lea dx, [si + VIO_QNOTIFY]
    xor ax, ax
    out dx, ax                  ; the receive buffers are there
    clc
    ret
.fail:
    lea dx, [si + VIO_STATUS]
    xor al, al
    out dx, al
    stc
    ret

; Queue AX of the device at I/O base SI, given the three pages at FS:BX
; (4 KB-aligned): CX = its size, DX = its available ring, BP = its used
; ring (offsets in FS). CF set if its size is not a power of two from 2
; to 256. Keeps every other register.
vnet_queue:
    push eax
    push di
    mov dx, si
    add dx, VIO_QSEL
    out dx, ax
    mov dx, si
    add dx, VIO_QSIZE
    in ax, dx
    cmp ax, 2
    jb .bad
    cmp ax, VBLK_QMAX
    ja .bad
    mov cx, ax
    dec cx
    test cx, ax
    jnz .bad
    mov cx, ax
    mov di, bx
.zero:
    mov dword [fs:di], 0
    add di, 4
    mov ax, di
    sub ax, bx
    cmp ax, 0x3000
    jb .zero
    mov dx, cx
    shl dx, 4
    add dx, bx                  ; DX = available ring
    mov bp, cx
    shl bp, 1
    add bp, dx
    add bp, 6 + 0xFFF
    and bp, 0xF000              ; BP = used ring, on the next page
    movzx eax, bx
    add eax, NET_LIN
    shr eax, 12
    push dx
    mov dx, si
    add dx, VIO_QPFN
    out dx, eax
    pop dx
    pop di
    pop eax
    clc
    ret
.bad:
    pop di
    pop eax
    stc
    ret

; Send the frame of CX bytes at FS:NET_TX (padded to 60), and wait up to
; 100 ms for the device to take it. CF set if it did not. Keeps every
; register.
vnet_send:
    pushad
    cmp cx, 60
    jae .len
    mov bx, cx
.pad:
    mov byte [fs:NET_TX + bx], 0
    inc bx
    cmp bx, 60
    jb .pad
    mov cx, 60
.len:
    movzx ecx, cx
    mov [fs:NET_TXQ + 16 + 8], ecx
    mov word [fs:NET_TXQ + 16 + 12], 0
    mov bx, [fs:NV_TXAVAIL]
    mov ax, [fs:bx + 2]
    mov cx, [fs:NV_TXSIZE]
    dec cx
    and cx, ax
    shl cx, 1
    mov di, bx
    add di, cx
    mov word [fs:di + 4], 0
    inc ax
    mov [fs:bx + 2], ax
    mov dx, [fs:NV_IO]
    add dx, VIO_QNOTIFY
    mov ax, 1
    out dx, ax
    mov bx, [fs:NV_TXUSED]
    mov si, 100
.wait:
    mov ax, [fs:bx + 2]
    cmp ax, [fs:NV_TXSEEN]
    jne .sent
    mov cx, 1
    call delay_ms
    dec si
    jnz .wait
    popad
    stc
    ret
.sent:
    mov [fs:NV_TXSEEN], ax
    popad
    clc
    ret

; A received frame, if there is one: CF clear, SI = its offset in FS, CX
; = its length; it stays the caller's until the next call, which hands
; its buffer back to the device. CF set if nothing has arrived. Keeps
; every other register.
vnet_recv:
    push ax
    push bx
    push dx
    push di
    mov bx, [fs:NV_RXAVAIL]
    mov ax, [fs:NV_HELD]
    cmp ax, 0xFFFF
    je .look
    ; The buffer looked at last time goes back on the available ring
    mov di, [fs:bx + 2]
    mov dx, [fs:NV_RXSIZE]
    dec dx
    and dx, di
    shl dx, 1
    push bx
    add bx, dx
    mov [fs:bx + 4], ax
    pop bx
    inc word [fs:bx + 2]
    mov word [fs:NV_HELD], 0xFFFF
    mov dx, [fs:NV_IO]
    add dx, VIO_QNOTIFY
    xor ax, ax
    out dx, ax
.look:
    mov bx, [fs:NV_RXUSED]
    mov ax, [fs:NV_RXSEEN]
    cmp ax, [fs:bx + 2]
    je .none
    inc word [fs:NV_RXSEEN]
    mov dx, [fs:NV_RXSIZE]
    dec dx
    and ax, dx
    shl ax, 3
    add bx, ax
    mov ax, [fs:bx + 4]         ; id: its first descriptor
    mov cx, [fs:bx + 8]         ; bytes written, with the header
    mov [fs:NV_HELD], ax
    shr ax, 1
    mov si, NET_RXSZ
    mul si
    add ax, NET_RXBUF + NET_FRAME
    mov si, ax
    sub cx, NET_HDRLEN
    jnc .ok
    xor cx, cx
.ok:
    pop di
    pop dx
    pop bx
    pop ax
    clc
    ret
.none:
    pop di
    pop dx
    pop bx
    pop ax
    stc
    ret

; Reset the device: it stops using its queues. FS = NET_SEG. Keeps every
; register.
vnet_close:
    push ax
    push dx
    mov dx, [fs:NV_IO]
    add dx, VIO_STATUS
    xor al, al
    out dx, al
    pop dx
    pop ax
    ret
