; SquirrelBIOS x86 — AHCI (SATA) driver: one disk, DMA through the EBDA
; Original code. Included by x86/squirrel-bios.asm when CONFIG_AHCI.
;
; Written from the Serial ATA AHCI 1.3.1 specification. At POST, and only
; when no IDE hard disk answered, ahci_detect looks on PCI bus 0 for a
; class 01:06:01 function (AHCI 1.0), resets the HBA and takes the first
; implemented port with a link up and an ATA disk's signature. That disk
; becomes DL = 80h: ata_read, ata_write, ata_capacity and ata_reset hand
; over to the routines here while DISK_KIND says AHCI, so the boot path and
; INT 13h use it unchanged.
;
; One command slot, one PRD, polled with interrupts off. READ DMA EXT and
; WRITE DMA EXT for every transfer (a write is followed by FLUSH CACHE
; EXT). The command list, received FIS area and command table live in
; the EBDA, which the memory map reserves, so they stay put after the
; hand-off.
;
; The HBA registers sit above 1 MB, so every access goes through GS
; loaded as a flat 4 GB segment (unreal mode). flat_gs (core/post.inc)
; loads it and
; puts the caller's GDTR back; the caller's GS returns at the end of
; each routine, so a guest never keeps the flat GS. This needs real
; mode: INT 13h from virtual 8086 mode is not served on the AHCI disk.

AHCI_VARS       equ EBDA_BASE + 0x10
AHCI_ABAR       equ AHCI_VARS + 0       ; dword: HBA registers
AHCI_PORTB      equ AHCI_VARS + 4       ; dword: ABAR + 100h + port * 80h
AHCI_SECTORS    equ AHCI_VARS + 8       ; qword: from IDENTIFY DEVICE
AHCI_BDF        equ AHCI_VARS + 16      ; word: bus << 8 | dev << 3 | fn
AHCI_PORTNUM    equ AHCI_VARS + 18      ; byte
AHCI_CLB        equ EBDA_BASE + 0x400   ; command list (1 KB aligned)
AHCI_FB         equ EBDA_BASE + 0x800   ; received FISes (256 bytes)
AHCI_CTBA       equ EBDA_BASE + 0x900   ; command table: FIS, PRD at +80h
AHCI_IDBUF      equ EBDA_BASE + 0xA00   ; IDENTIFY DEVICE data (512 bytes)

AHCI_CLASS      equ 0x010601            ; mass storage, SATA, AHCI 1.0
AHCI_BAR        equ 0x24                ; BAR5: ABAR

HBA_CAP         equ 0x00
HBA_GHC         equ 0x04
HBA_PI          equ 0x0C
GHC_HR          equ 0x00000001
GHC_AE          equ 0x80000000
CAP_SSS         equ 0x08000000          ; staggered spin-up

P_CLB           equ 0x00
P_CLBU          equ 0x04
P_FB            equ 0x08
P_FBU           equ 0x0C
P_IS            equ 0x10
P_IE            equ 0x14
P_CMD           equ 0x18
P_TFD           equ 0x20
P_SIG           equ 0x24
P_SSTS          equ 0x28
P_SCTL          equ 0x2C
P_SERR          equ 0x30
P_CI            equ 0x38
P_IS_TFES       equ 0x40000000          ; task file error
PCMD_ST         equ 0x0001
PCMD_SUD        equ 0x0002
PCMD_POD        equ 0x0004
PCMD_FRE        equ 0x0010
PCMD_FR         equ 0x4000
PCMD_CR         equ 0x8000
SIG_ATA         equ 0x00000101

CMD_READ_DMA_EXT  equ 0x25
CMD_WRITE_DMA_EXT equ 0x35
CMD_FLUSH_EXT     equ 0xEA
FIS_H2D         equ 0x27
AHCI_LINK_POLLS equ 0x00010000          ; PxSSTS reads while the link comes up

; CF clear when DL 80h is the AHCI disk and ATA_DEVSEL names it.
; Keeps every register but the flags.
ahci_owns:
    cmp byte [ss:DISK_KIND], DISK_AHCI
    jne .no
    cmp byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    jne .no
    clc
    ret
.no:
    stc
    ret

; POST, when no IDE hard disk answered: find an AHCI controller on bus 0
; and a SATA disk behind it. On success CF is clear, AL = the port, and
; DL 80h is that disk (DISK_KIND, ATA_PRESENT bit 0, one hard disk in the
; BDA, ATA_DEVSEL on the master, BOOT_DL = 80h). On failure CF is set
; and every register is kept, so the IDE failure can still be reported.
ahci_detect:
    pushad
    push gs
    pushf
    cli
    call enable_a20             ; the ABAR may have bit 20 set
    call pci_present
    jc .none
    xor bx, bx
.fn:
    call pci_next
    jc .none
    mov di, 0x08                ; class code in bits 31:8
    call pci_read32
    shr eax, 8
    cmp eax, AHCI_CLASS
    jne .next
    mov di, AHCI_BAR
    call pci_read32
    test al, 1                  ; must be a memory BAR
    jnz .next
    and eax, 0xFFFFFFF0
    jz .next                    ; POST could not place it
    mov esi, eax
    mov di, PCI_CMD
    call pci_read16
    or ax, PCI_CMD_MEM | PCI_CMD_MASTER
    mov cx, ax
    call pci_write16
    call flat_gs
    mov [gs:dword AHCI_ABAR], esi
    mov [gs:dword AHCI_BDF], bx
    call ahci_hba_reset
    jc .next
    call ahci_find_disk         ; AL = port
    jnc .found
.next:
    inc bl
    jnz .fn
.none:
    popf
    pop gs
    popad
    stc
    ret
.found:
    mov [gs:dword AHCI_PORTNUM], al
    mov byte [ss:DISK_KIND], DISK_AHCI
    or byte [ss:ATA_PRESENT], 1
    mov byte [ss:BDA_HDCOUNT], 1
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    mov byte [ss:BOOT_DL], 0x80
    mov bp, sp
    mov [bp + 4 + 28], al       ; AL in the pushad frame, past FLAGS and GS
    popf
    pop gs
    popad
    clc
    ret

; HBA reset with AHCI mode on before and after it. ESI = ABAR, GS flat.
; CF set if the reset does not finish. Clobbers EAX and ECX.
ahci_hba_reset:
    or dword [gs:esi + HBA_GHC], GHC_AE
    or dword [gs:esi + HBA_GHC], GHC_HR
    mov ecx, ATA_TIMEOUT
.wait:
    test dword [gs:esi + HBA_GHC], GHC_HR
    jz .done
    dec ecx
    jnz .wait
    stc
    ret
.done:
    or dword [gs:esi + HBA_GHC], GHC_AE   ; interrupts stay off (IE = 0)
    clc
    ret

; The first implemented port, lowest number first, with a link up and
; an ATA disk's signature; it is started and its size read. ESI = ABAR,
; GS flat. Out: CF clear, AL = port, AHCI_PORTB and AHCI_SECTORS set;
; CF set if there is none. Keeps every register but EAX and the flags.
ahci_find_disk:
    push ecx
    push edx
    push edi
    xor dl, dl                  ; port number
.port:
    mov eax, [gs:esi + HBA_PI]
    mov cl, dl
    shr eax, cl
    test al, 1
    jz .next
    movzx edi, dl
    shl edi, 7
    lea edi, [esi + edi + 0x100]
    mov [gs:dword AHCI_PORTB], edi
    call ahci_port_stop
    jc .next
    mov dword [gs:edi + P_CLB], AHCI_CLB
    mov dword [gs:edi + P_CLBU], 0
    mov dword [gs:edi + P_FB], AHCI_FB
    mov dword [gs:edi + P_FBU], 0
    mov dword [gs:edi + P_IE], 0
    mov dword [gs:edi + P_SERR], 0xFFFFFFFF
    mov dword [gs:edi + P_IS], 0xFFFFFFFF
    or dword [gs:edi + P_CMD], PCMD_SUD | PCMD_POD
    mov ecx, AHCI_LINK_POLLS
.link:
    mov eax, [gs:edi + P_SSTS]
    and al, 0x0F
    cmp al, 3                   ; device present, phy communication up
    je .linked
    dec ecx
    jnz .link
    jmp .next
.linked:
    or dword [gs:edi + P_CMD], PCMD_FRE
    call ahci_wait_idle         ; the device's first D2H FIS sets PxSIG
    jc .drop
    cmp dword [gs:edi + P_SIG], SIG_ATA
    jne .drop
    mov dword [gs:edi + P_SERR], 0xFFFFFFFF
    or dword [gs:edi + P_CMD], PCMD_ST
    call ahci_identify
    jc .drop
    mov al, dl
    pop edi
    pop edx
    pop ecx
    clc
    ret
.drop:
    call ahci_port_stop         ; leave nothing running on a port we skip
.next:
    inc dl
    cmp dl, 32
    jb .port
    pop edi
    pop edx
    pop ecx
    stc
    ret

; Stop the port at EDI: ST off and wait for CR, then FRE off and wait for
; FR (each within ATA_TIMEOUT polls). GS flat. CF set if it does not stop.
; Clobbers EAX.
ahci_port_stop:
    push ecx
    and dword [gs:edi + P_CMD], ~PCMD_ST
    mov ecx, ATA_TIMEOUT
.cr:
    test dword [gs:edi + P_CMD], PCMD_CR
    jz .fre
    dec ecx
    jnz .cr
    jmp .fail
.fre:
    and dword [gs:edi + P_CMD], ~PCMD_FRE
    mov ecx, ATA_TIMEOUT
.fr:
    test dword [gs:edi + P_CMD], PCMD_FR
    jz .ok
    dec ecx
    jnz .fr
.fail:
    pop ecx
    stc
    ret
.ok:
    pop ecx
    clc
    ret

; Wait until the task file at EDI shows neither BSY nor DRQ. GS flat.
; Out: AL = status, CF on timeout. Clobbers EAX.
ahci_wait_idle:
    push ecx
    mov ecx, ATA_TIMEOUT
.poll:
    mov eax, [gs:edi + P_TFD]
    test al, ST_BSY | ST_DRQ
    jz .ok
    dec ecx
    jnz .poll
    pop ecx
    stc
    ret
.ok:
    pop ecx
    clc
    ret

; IDENTIFY DEVICE into AHCI_IDBUF; AHCI_SECTORS from words 100-103 when
; word 83 bit 10 says LBA48, else words 60-61. EDI = port, GS flat.
; CF set on failure. Clobbers EAX.
ahci_identify:
    push ebx
    push ecx
    push esi
    mov dword [gs:dword AHCI_CLB], 5 | (1 << 16)    ; 5-dword FIS, 1 PRD
    mov dword [gs:dword AHCI_CLB + 4], 0
    mov dword [gs:dword AHCI_CLB + 8], AHCI_CTBA
    mov dword [gs:dword AHCI_CLB + 12], 0
    mov ebx, AHCI_IDBUF
    mov ecx, 512
    call ahci_prd
    xor eax, eax
    mov bl, CMD_IDENTIFY
    xor cx, cx
    call ahci_fis
    mov byte [gs:dword AHCI_CTBA + 7], 0          ; device: no LBA bit
    call ahci_issue
    jc .out
    mov eax, [gs:dword AHCI_IDBUF + 60 * 2]
    xor ecx, ecx
    test word [gs:dword AHCI_IDBUF + 83 * 2], 1 << 10
    jz .size
    mov eax, [gs:dword AHCI_IDBUF + 100 * 2]
    mov ecx, [gs:dword AHCI_IDBUF + 102 * 2]
.size:
    mov [gs:dword AHCI_SECTORS], eax
    mov [gs:dword AHCI_SECTORS + 4], ecx
    clc
.out:
    pop esi
    pop ecx
    pop ebx
    ret

; The single PRD: EBX = buffer (linear, even), ECX = bytes. GS flat.
; Keeps every register.
ahci_prd:
    push ecx
    mov [gs:dword AHCI_CTBA + 0x80], ebx
    mov dword [gs:dword AHCI_CTBA + 0x84], 0
    mov dword [gs:dword AHCI_CTBA + 0x88], 0
    dec ecx
    mov [gs:dword AHCI_CTBA + 0x8C], ecx
    pop ecx
    ret

; The H2D register FIS in the command table: command BL, LBA EAX (48-bit,
; upper 16 bits 0), count CX, device 40h (LBA). GS flat. Keeps every
; register.
ahci_fis:
    push eax
    push edx
    movzx edx, bl
    shl edx, 16
    or edx, 0x8000 | FIS_H2D    ; C bit: this is a command
    mov [gs:dword AHCI_CTBA], edx
    mov edx, eax
    and edx, 0x00FFFFFF
    or edx, 0x40000000
    mov [gs:dword AHCI_CTBA + 4], edx
    shr eax, 24                 ; LBA bits 31:24, then 47:32 = 0
    mov [gs:dword AHCI_CTBA + 8], eax
    movzx edx, cx
    mov [gs:dword AHCI_CTBA + 12], edx
    mov dword [gs:dword AHCI_CTBA + 16], 0
    pop edx
    pop eax
    ret

; Run the command in slot 0 of the port at EDI and wait for it. GS flat.
; Out: CF clear on success. CF set on failure with SI = msg_timeout or
; msg_readerr, BL = status, BH = error (from PxTFD); the port is then
; restarted. Clobbers EAX, and BX and SI on failure.
ahci_issue:
    push ecx
    call ahci_wait_idle
    jc .timeout
    mov dword [gs:edi + P_IS], 0xFFFFFFFF
    mov dword [gs:edi + P_CI], 1
    mov ecx, ATA_TIMEOUT
.wait:
    test dword [gs:edi + P_IS], P_IS_TFES
    jnz .error
    test dword [gs:edi + P_CI], 1
    jz .done
    dec ecx
    jnz .wait
.timeout:
    mov si, msg_timeout
    jmp .fail
.done:
    mov eax, [gs:edi + P_TFD]
    test al, ST_ERR
    jnz .error
    pop ecx
    clc
    ret
.error:
    mov si, msg_readerr
.fail:
    mov eax, [gs:edi + P_TFD]
    mov bx, ax
    call ahci_restart
    pop ecx
    stc
    ret

; Error recovery (AHCI 1.3.1 section 6.2.2): stop the port, clear its
; error bits, COMRESET if the device still holds BSY or DRQ, start it
; again. EDI = port, GS flat. CF set if the port does not come back.
; Clobbers EAX.
ahci_restart:
    push ecx
    and dword [gs:edi + P_CMD], ~PCMD_ST
    mov ecx, ATA_TIMEOUT
.cr:
    test dword [gs:edi + P_CMD], PCMD_CR
    jz .stopped
    dec ecx
    jnz .cr
.stopped:
    mov dword [gs:edi + P_SERR], 0xFFFFFFFF
    mov dword [gs:edi + P_IS], 0xFFFFFFFF
    test dword [gs:edi + P_TFD], ST_BSY | ST_DRQ
    jz .start
    ; COMRESET: DET = 1 for at least 1 ms, then 0, then wait for the link
    or dword [gs:edi + P_SCTL], 1
    mov ecx, 2000
.hold:
    out 0x80, al                ; ~1 us each on an ISA-timed port
    loop .hold, ecx
    and dword [gs:edi + P_SCTL], ~0x0F
    mov ecx, AHCI_LINK_POLLS
.link:
    mov eax, [gs:edi + P_SSTS]
    and al, 0x0F
    cmp al, 3
    je .relinked
    dec ecx
    jnz .link
.relinked:
    mov dword [gs:edi + P_SERR], 0xFFFFFFFF
    call ahci_wait_idle
    jc .fail
.start:
    or dword [gs:edi + P_CMD], PCMD_ST
    pop ecx
    clc
    ret
.fail:
    pop ecx
    stc
    ret

; ata_read for the AHCI disk: EAX = LBA, CL = sectors (1-128), EBP =
; linear buffer (even, below 1 MB). Same outputs as ata_read.
ahci_read:
    push edx
    mov dl, CMD_READ_DMA_EXT
    call ahci_rw
    pop edx
    ret

; ata_write for the AHCI disk, then FLUSH CACHE EXT. Same registers.
ahci_write:
    push edx
    mov dl, CMD_WRITE_DMA_EXT
    call ahci_rw
    pop edx
    ret

; DL = the command. Every register but SI and BX (on failure) is kept.
ahci_rw:
    push eax
    push ecx
    push edx
    push ebp
    push edi
    push esi
    push ebx
    push gs
    pushf
    cli
    call flat_gs
    movzx ecx, cl
    test cx, cx
    jz .badarg
    cmp cx, 128
    ja .badarg
    test bp, 1                  ; a PRD's buffer must be word-aligned
    jnz .badarg
    mov esi, ecx                ; the last sector must still be below 2^32
    dec esi
    add esi, eax
    jc .badarg
    mov edi, [gs:dword AHCI_PORTB]
    mov esi, 5 | (1 << 16)      ; 5-dword FIS, 1 PRD
    cmp dl, CMD_WRITE_DMA_EXT
    jne .header
    or si, 0x40                 ; W: host to device
.header:
    mov [gs:dword AHCI_CLB], esi
    mov dword [gs:dword AHCI_CLB + 4], 0
    mov dword [gs:dword AHCI_CLB + 8], AHCI_CTBA
    mov dword [gs:dword AHCI_CLB + 12], 0
    mov ebx, ebp
    push ecx
    shl ecx, 9
    call ahci_prd
    pop ecx
    mov bl, dl
    call ahci_fis               ; EAX = LBA, CX = count
    call ahci_issue
    jc .fail
    cmp dl, CMD_WRITE_DMA_EXT
    jne .ok
    ; FLUSH CACHE EXT: no data, no PRD
    mov dword [gs:dword AHCI_CLB], 5
    xor eax, eax
    xor cx, cx
    mov bl, CMD_FLUSH_EXT
    call ahci_fis
    call ahci_issue
    jc .fail
.ok:
    popf
    pop gs
    pop ebx
    pop esi
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    clc
    ret
.badarg:
    mov si, msg_readerr
    xor bx, bx
.fail:
    popf
    pop gs
    add sp, 8                   ; drop saved EBX and ESI; BX/SI stay
    pop edi
    pop ebp
    pop edx
    pop ecx
    pop eax
    stc
    ret

; ata_capacity for the AHCI disk: CF clear, EDX:EAX = sectors.
ahci_capacity:
    push es
    push word EBDA_BASE >> 4
    pop es
    mov eax, [es:AHCI_SECTORS - EBDA_BASE]
    mov edx, [es:AHCI_SECTORS - EBDA_BASE + 4]
    pop es
    clc
    ret

; ata_reset for the AHCI disk: port recovery. CF set if it fails. Keeps
; every register.
ahci_reset:
    push eax
    push edi
    push gs
    pushf
    cli
    call flat_gs
    mov edi, [gs:dword AHCI_PORTB]
    call ahci_restart
    jc .fail
    popf
    pop gs
    pop edi
    pop eax
    clc
    ret
.fail:
    popf
    pop gs
    pop edi
    pop eax
    stc
    ret

; INT 13h AH=48h device path for the AHCI disk, at DS:SI 24h-3Fh: host
; bus 'PCI' with the controller's bus, device and function, interface
; 'SATA' (EDD 4.0) with the port number. Keeps every register.
ahci_edd_path:
    push ax
    push es
    push word EBDA_BASE >> 4
    pop es
    mov dword [si + 0x24], 'PCI '
    mov dword [si + 0x28], 'SATA'
    mov dword [si + 0x2C], '    '
    mov ax, [es:AHCI_BDF - EBDA_BASE]
    mov [si + 0x30], ah             ; bus
    shr al, 3
    mov [si + 0x31], al             ; device
    mov al, [es:AHCI_BDF - EBDA_BASE]
    and al, 7
    mov [si + 0x32], al             ; function
    mov byte [si + 0x33], 0         ; channel
    mov dword [si + 0x34], 0
    mov al, [es:AHCI_PORTNUM - EBDA_BASE]
    xor ah, ah
    mov [si + 0x38], ax             ; port, port multiplier port 0
    mov word [si + 0x3A], 0
    mov dword [si + 0x3C], 0
    pop es
    pop ax
    ret
