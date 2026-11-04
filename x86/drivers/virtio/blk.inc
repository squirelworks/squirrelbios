; SquirrelBIOS x86 — virtio-blk: a virtio block device as DL 80h
; Original code. Included by x86/squirrel-bios.asm when CONFIG_VIRTIO.
;
; Written from the Virtual I/O Device (VIRTIO) specification 1.1, its
; legacy interface (sections 4.1.4.8 and 2.4): the first PCI function
; with vendor 1AF4h and device 1001h (a transitional block device) whose
; BAR0 is an I/O range. At POST, when no IDE, SATA or NVMe disk answered,
; vblk_detect resets it, accepts no optional features (so writes go
; through to the disk) and gives its one queue three pages in the EBDA,
; the ones NVMe would have used. The disk then becomes DL = 80h:
; ata_read, ata_write, ata_capacity and ata_reset hand over to the
; routines here while DISK_KIND says virtio, as for NVMe.
;
; One request at a time, always on descriptors 0-2: the request header,
; the caller's buffer, then the status byte. It is put on the available
; ring, the queue is notified, and the used ring is polled with
; interrupts off. Everything is in the EBDA, reached through ES, so
; there is no unreal mode here.

VBLK_SEG        equ EBDA_BASE >> 4
VBLK_VARS       equ 0x0080              ; EBDA offsets
VBLK_IO         equ VBLK_VARS + 0       ; word: I/O base (BAR0)
VBLK_BDF        equ VBLK_VARS + 2       ; word: bus << 8 | dev << 3 | fn
VBLK_QSIZE      equ VBLK_VARS + 4       ; word: queue size
VBLK_LASTUSED   equ VBLK_VARS + 6       ; word: used ring index seen
VBLK_SECTORS    equ VBLK_VARS + 8       ; qword: capacity, 512-byte sectors
VBLK_AVAIL      equ VBLK_VARS + 16      ; word: the available ring (offset)
VBLK_USED       equ VBLK_VARS + 18      ; word: the used ring (offset)
VBLK_HDR        equ 0x00C0              ; request header: type, 0, sector
VBLK_STATUS     equ 0x00D0              ; status byte
VBLK_QUEUE      equ 0x1000              ; the queue: three pages
VBLK_QMAX       equ 256                 ; the largest queue that fits

%if EBDA_SIZE < VBLK_QUEUE + 0x3000
%error "virtio-blk needs its queue in the EBDA"
%endif

VBLK_VENDOR     equ 0x1AF4
VBLK_DEVICE     equ 0x1001

; Legacy registers, from BAR0 (no MSI-X)
VIO_HOST_F      equ 0x00
VIO_GUEST_F     equ 0x04
VIO_QPFN        equ 0x08
VIO_QSIZE       equ 0x0C
VIO_QSEL        equ 0x0E
VIO_QNOTIFY     equ 0x10
VIO_STATUS      equ 0x12
VIO_CONFIG      equ 0x14            ; virtio-blk: capacity, qword
VIO_ACK         equ 0x01
VIO_DRIVER      equ 0x02
VIO_DRIVER_OK   equ 0x04
VIO_FAILED      equ 0x80

VRING_NEXT      equ 1               ; descriptor flags
VRING_WRITE     equ 2               ; the device writes this buffer
VBLK_T_IN       equ 0               ; read
VBLK_T_OUT      equ 1               ; write
DISK_VIRTIO     equ 4

; CF clear when DL 80h is the virtio disk and ATA_DEVSEL names it.
; Keeps every register but the flags.
vblk_owns:
    cmp byte [ss:DISK_KIND], DISK_VIRTIO
    jne .no
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    jne .no
    clc
    ret
.no:
    stc
    ret

; POST, when no IDE, SATA or NVMe disk answered: find a virtio block
; device on bus 0 and bring it up. On success CF is clear and DL 80h is
; that disk (DISK_KIND, ATA_PRESENT bit 0, one hard disk in the BDA,
; ATA_DEVSEL on the master, BOOT_DL = 80h). On failure CF is set. Keeps
; every register.
vblk_detect:
    pushad
    push es
    pushf
    cli
    call pci_present
    jc .none
    xor bx, bx
.fn:
    call pci_next
    jc .none
    xor di, di
    call pci_read32
    cmp eax, (VBLK_DEVICE << 16) | VBLK_VENDOR
    jne .next
    mov di, PCI_BAR0
    call pci_read32
    test al, 1                  ; the legacy interface: an I/O BAR
    jz .next
    and ax, 0xFFFC
    jz .next                    ; POST could not place it
    mov si, ax
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_IO | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    call vblk_init
    jnc .found
.next:
    inc bl
    jnz .fn
.none:
    popf
    pop es
    popad
    stc
    ret
.found:
    mov byte [ss:DISK_KIND], DISK_VIRTIO
    or byte [ss:ATA_PRESENT], 1
    mov byte [ss:BDA_HDCOUNT], 1
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    mov byte [ss:BOOT_DL], 0x80
    popf
    pop es
    popad
    clc
    ret

; The device at I/O base SI, PCI address BX: reset it, set up queue 0 in
; the EBDA, and read its size. CF set on failure (the device is then
; reset again). Clobbers EAX, ECX, EDX, DI, ES.
vblk_init:
    push word VBLK_SEG
    pop es
    mov [es:VBLK_IO], si
    mov [es:VBLK_BDF], bx
    lea dx, [si + VIO_STATUS]
    xor al, al
    out dx, al                  ; reset
    mov al, VIO_ACK
    out dx, al
    or al, VIO_DRIVER
    out dx, al
    lea dx, [si + VIO_GUEST_F]
    xor eax, eax
    out dx, eax                 ; no optional features
    lea dx, [si + VIO_QSEL]
    out dx, ax                  ; queue 0
    lea dx, [si + VIO_QSIZE]
    in ax, dx
    cmp ax, 3
    jb .fail
    cmp ax, VBLK_QMAX
    ja .fail
    test ax, ax                 ; a power of two
    mov cx, ax
    dec cx
    test cx, ax
    jnz .fail
    mov [es:VBLK_QSIZE], ax
    ; The queue: descriptors (16 bytes each), the available ring (6 +
    ; 2 per entry), then on the next page the used ring (6 + 8 per entry)
    mov di, VBLK_QUEUE
    mov cx, 0x3000 / 2
    push ax
    xor ax, ax
    cld
    rep stosw
    pop ax
    mov cx, ax
    shl cx, 4
    add cx, VBLK_QUEUE
    mov [es:VBLK_AVAIL], cx
    shl ax, 1
    add ax, cx
    add ax, 6 + 0xFFF
    and ax, 0xF000
    mov [es:VBLK_USED], ax
    mov word [es:VBLK_LASTUSED], 0
    ; Descriptors 0-2 always chain: header, data, status
    mov dword [es:VBLK_QUEUE + 0], EBDA_BASE + VBLK_HDR
    mov word [es:VBLK_QUEUE + 8], 16
    mov dword [es:VBLK_QUEUE + 12], (1 << 16) | VRING_NEXT
    mov dword [es:VBLK_QUEUE + 16 + 12], (2 << 16) | VRING_NEXT
    mov dword [es:VBLK_QUEUE + 32], EBDA_BASE + VBLK_STATUS
    mov word [es:VBLK_QUEUE + 32 + 8], 1
    mov word [es:VBLK_QUEUE + 32 + 12], VRING_WRITE
    lea dx, [si + VIO_QPFN]
    mov eax, (EBDA_BASE + VBLK_QUEUE) >> 12
    out dx, eax
    lea dx, [si + VIO_CONFIG]
    in eax, dx
    mov [es:VBLK_SECTORS], eax
    add dx, 4
    in eax, dx
    mov [es:VBLK_SECTORS + 4], eax
    or eax, [es:VBLK_SECTORS]
    jz .fail                    ; no medium
    lea dx, [si + VIO_STATUS]
    mov al, VIO_ACK | VIO_DRIVER | VIO_DRIVER_OK
    out dx, al
    clc
    ret
.fail:
    lea dx, [si + VIO_STATUS]
    xor al, al
    out dx, al
    stc
    ret

; ata_read for the virtio disk: EAX = LBA, CL = sectors (1-128), EBP =
; linear buffer below 1 MB. Same outputs as ata_read.
vblk_read:
    push edx
    mov dl, VBLK_T_IN
    call vblk_rw
    pop edx
    ret

; ata_write for the virtio disk. Same registers.
vblk_write:
    push edx
    mov dl, VBLK_T_OUT
    call vblk_rw
    pop edx
    ret

; DL = the request type. Every register but SI and BX (on failure) is
; kept.
vblk_rw:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push es
    pushf
    cli
    push word VBLK_SEG
    pop es
    movzx ecx, cl
    test cx, cx
    jz .fail
    cmp cx, 128
    ja .fail
    mov esi, ecx                ; the last sector must still be below 2^32
    dec esi
    add esi, eax
    jc .fail
    ; The header, then the data descriptor: device-writable for a read
    movzx edx, dl
    mov [es:VBLK_HDR], edx
    mov dword [es:VBLK_HDR + 4], 0
    mov [es:VBLK_HDR + 8], eax
    mov dword [es:VBLK_HDR + 12], 0
    mov byte [es:VBLK_STATUS], 0xFF
    mov [es:VBLK_QUEUE + 16], ebp
    mov dword [es:VBLK_QUEUE + 16 + 4], 0
    shl ecx, 9
    mov [es:VBLK_QUEUE + 16 + 8], ecx
    mov ax, VRING_NEXT
    test dl, dl
    jnz .dir
    or ax, VRING_WRITE
.dir:
    mov [es:VBLK_QUEUE + 16 + 12], ax
    ; Offer descriptor 0 on the available ring, then notify queue 0
    mov di, [es:VBLK_AVAIL]
    mov ax, [es:di + 2]         ; its index
    mov cx, [es:VBLK_QSIZE]
    dec cx
    and cx, ax
    shl cx, 1
    mov bx, di
    add bx, cx
    mov word [es:bx + 4], 0
    inc ax
    mov [es:di + 2], ax
    mov dx, [es:VBLK_IO]
    add dx, VIO_QNOTIFY
    xor ax, ax
    out dx, ax
    ; Wait up to 5 s for the used ring to move
    mov di, [es:VBLK_USED]
    mov si, 5000
.poll:
    mov ax, [es:di + 2]
    cmp ax, [es:VBLK_LASTUSED]
    jne .done
    mov cx, 1
    call delay_ms
    dec si
    jnz .poll
    jmp .fail
.done:
    inc word [es:VBLK_LASTUSED]
    cmp byte [es:VBLK_STATUS], 0
    jne .fail
    popf
    pop es
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret
.fail:
    mov si, msg_readerr
    xor bx, bx
    popf
    pop es
    add sp, 8                   ; drop saved EBX and ESI; BX/SI stay
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc
    ret

; ata_capacity for the virtio disk: CF clear, EDX:EAX = sectors.
vblk_capacity:
    push es
    push word VBLK_SEG
    pop es
    mov eax, [es:VBLK_SECTORS]
    mov edx, [es:VBLK_SECTORS + 4]
    pop es
    clc
    ret

; ata_reset for the virtio disk: nothing to reset between requests; CF
; set if the device reports that it needs a reset. Keeps every register.
vblk_reset:
    push ax
    push dx
    push es
    push word VBLK_SEG
    pop es
    mov dx, [es:VBLK_IO]
    add dx, VIO_STATUS
    in al, dx
    pop es
    test al, VIO_FAILED | 0x40  ; failed, or DEVICE_NEEDS_RESET
    pop dx
    pop ax
    jnz .bad
    clc
    ret
.bad:
    stc
    ret

; INT 13h AH=48h device path for the virtio disk, at DS:SI 24h-3Fh: host
; bus 'PCI' with its bus, device and function, interface 'SCSI' (EDD
; 3.0's nearest) with target 0 and LUN 0. Keeps every register.
vblk_edd_path:
    push ax
    push es
    push word VBLK_SEG
    pop es
    mov dword [si + 0x24], 'PCI '
    mov dword [si + 0x28], 'SCSI'
    mov dword [si + 0x2C], '    '
    mov ax, [es:VBLK_BDF]
    mov [si + 0x30], ah             ; bus
    shr al, 3
    mov [si + 0x31], al             ; device
    mov al, [es:VBLK_BDF]
    and al, 7
    mov [si + 0x32], al             ; function
    mov byte [si + 0x33], 0
    mov dword [si + 0x34], 0
    mov dword [si + 0x38], 0        ; target 0, LUN 0
    mov dword [si + 0x3C], 0
    pop es
    pop ax
    ret
