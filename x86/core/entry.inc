; SquirrelBIOS x86 — POST sequence and the first boot decision (boot sector, SQBL, S2OK)
; Original code. Included by x86/squirrel-bios.asm.

entry:
    cli
    cld
    ; Mask both PICs until pit_pic_init programs them for the hand-off.
    mov al, 0xFF
    out PIC1_DATA, al
    out PIC2_DATA, al
    mov ax, cs                  ; strings live in the ROM segment
    mov ds, ax
    xor ax, ax
    mov ss, ax
    mov sp, FW_STACK
    mov ax, VGA_SEG
    mov es, ax

    call build_memmap
    call serial_init
    call vga_init
    call bda_init
%if CONFIG_FLOPPY
    call fdc_bda_init
%endif
%if CONFIG_PCI
    call pci_init
%endif
%if CONFIG_CDBOOT
    mov byte [ss:CD_SEL], 0     ; INT 13h has no CD until one boots
%endif
%if CONFIG_SMBIOS
    call smbios_init
%endif
%if CONFIG_ACPI
    call acpi_init
%endif

    ; Banner
    xor di, di
    mov si, banner
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline

%if CONFIG_USB
    call usb_init               ; lists the USB devices
%endif

%if CONFIG_FLOPPY
    call floppy_boot            ; returns only if A: did not boot
%endif

    ; Disk: sector 0 (LBA 0) to 0x7C00. Prefer the primary master; if
    ; nothing answered as master, try the primary slave and keep DL.
    ; ata_detect asked each unit IDENTIFY with fresh status; read from
    ; the first one that answered, and report why the master did not.
    call ata_detect
    mov byte [ss:ATA_DEVSEL], ATA_SEL_MASTER
    mov byte [ss:BOOT_DL], 0x80
    test byte [ss:ATA_PRESENT], 1
    jnz .read_boot
    call ata_probe              ; again, for the reason and registers
    jnc .read_boot
    cmp si, msg_nodrive
    jne .no_disk
    mov byte [ss:ATA_DEVSEL], ATA_SEL_SLAVE
    mov byte [ss:BOOT_DL], 0x81
    test byte [ss:ATA_PRESENT], 2
    jnz .read_boot
    call ata_probe
    jnc .read_boot
.no_disk:
%if CONFIG_AHCI
    call ahci_detect            ; CF clear: DL 80h is a SATA disk, AL = port
    jc .no_ahci
    mov si, msg_ahci
    mov bl, al
    mov ah, ATTR_NORMAL
    call con_print
    mov al, bl
    aam                         ; AH = tens, AL = units
    test ah, ah
    jz .units
    xchg al, ah
    add al, '0'
    push ax
    mov ah, ATTR_NORMAL
    call con_putc
    pop ax
    mov al, ah
.units:
    add al, '0'
    mov ah, ATTR_NORMAL
    call con_putc
    call con_newline
    jmp .read_boot
.no_ahci:
%endif
%if CONFIG_NVME
    call nvme_detect            ; CF clear: DL 80h is NVMe namespace 1
    jc .no_nvme
    mov si, msg_nvme
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    jmp .read_boot
.no_nvme:
%endif
%if CONFIG_USB
    call usbd_detect            ; CF clear: DL 80h is the USB disk
    jc .no_usbd
    call usbd_banner
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline
    jmp .read_boot
.no_usbd:
%endif
%if CONFIG_VIRTIO
    call vblk_detect            ; CF clear: DL 80h is a virtio disk
    jc .no_virtio
    mov si, msg_virtio
    mov ah, ATTR_NORMAL
    call con_print
    push es
    push word VBLK_SEG
    pop es
    cmp byte [es:VBLK_MODERN], 0
    pop es
    je .virtio_line
    mov si, msg_virtio_v1
    call con_print
.virtio_line:
    call con_newline
    jmp .read_boot
.no_virtio:
%endif
%if CONFIG_CDBOOT
    call cd_boot                ; returns only if no CD booted
%endif
%if CONFIG_NETBOOT
    call net_boot               ; returns only if nothing was booted
%endif
    jmp disk_fail
.read_boot:
    xor eax, eax
    mov cl, 1
    mov ebp, BOOT_SECTOR
    call ata_read_retry
    jc disk_fail

; A clean read without the signature is not a boot sector. Reading it
; again would give the same bytes, so this is not retried.
    cmp word [ss:BOOT_SECTOR + 0x1FE], 0xAA55    ; SS = 0
    je .signed
    mov si, msg_nosig
    jmp boot_fail
.signed:

    ; No SQBL header: either a Squirrel-OS/2 image or one sector
    cmp dword [ss:SQBL_HDR], SQBL_MAGIC
    jne maybe_os

    ; Extra sector count, 1-128
    movzx ecx, word [ss:SQBL_HDR + 4]
    test cx, cx
    jz .range
    cmp cx, SQBL_MAX
    ja .range
    ; EAX = segment * 16 + offset, EDX = end of the load
    movzx eax, word [ss:SQBL_HDR + 6]
    shl eax, 4
    movzx edx, word [ss:SQBL_HDR + 8]
    add eax, edx
    cmp eax, LOAD_LIMIT
    jae .range
    mov edx, ecx
    shl edx, 9
    add edx, eax
    cmp edx, LOAD_LIMIT
    ja .range
    ; [EAX, EDX) must miss the memory map page and the boot sector
    cmp eax, MEMMAP_ADDR + 0x100
    jae .past_map
    cmp edx, MEMMAP_ADDR
    ja .overlap
.past_map:
    cmp eax, BOOT_SECTOR + 0x200
    jae .load
    cmp edx, BOOT_SECTOR
    ja .overlap
.load:
    mov ebp, eax
    mov eax, 1                  ; from LBA 1; CL is still the sector count
    call ata_read_retry
    jnc handoff
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_readerr_extra
    jmp disk_fail
.range:
    mov si, msg_range
    jmp boot_fail
.overlap:
    mov si, msg_overlap
    jmp boot_fail

; Linux bzImage (HdrS at 0x202) or a 96 KB 'S2OK' image, else one sector.
; HdrS lives in sector 1; setup_sects is in sector 0 at 0x1F1.
maybe_os:
    mov eax, 1
    mov cl, 1
    mov ebp, BOOT_SECTOR + 512
    call ata_read
    jnc .have_sect1
    call ata_reset
    jmp maybe_s2ok
.have_sect1:
%if CONFIG_BZIMAGE
    cmp dword [ss:BOOT_SECTOR + 0x202], 'HdrS'
    je bzimage_go
%endif

maybe_s2ok:
    mov eax, OS_SECTORS         ; LBA of the last kernel sector
    mov cl, 1
    mov ebp, OS_PROBE
    call ata_read
    jnc .probed
    call ata_reset              ; do not leave ERR set for the boot sector
    jmp handoff
.probed:
    cmp dword [ss:OS_PROBE + 508], OS_SIG
    jne handoff

    mov eax, 1
    mov cl, 128                 ; LBAs 1..128 -> 0x8000
    mov ebp, OS_LOAD
    call ata_read_retry
    jc kernel_fail
    mov eax, 129
    mov cl, OS_SECTORS - 128    ; LBAs 129..192 -> 0x18000
    mov ebp, OS_LOAD + 128 * 512
    call ata_read_retry
    jc kernel_fail
    push es
    mov ax, OS_SIG_LIN >> 4
    mov es, ax
    cmp dword [es:OS_SIG_LIN & 0x0F], OS_SIG
    pop es
    je kernel_go
    mov si, msg_nosig_os
    jmp boot_fail
kernel_fail:
    cmp si, msg_readerr
    jne disk_fail
    mov si, msg_kernel
    jmp disk_fail
kernel_go:
    call prepare_runtime
    mov si, msg_kload
    mov ah, ATTR_NORMAL
    call con_print
    call con_newline

; Registers match the 0x7C00 hand-off. CS:IP is the kernel.
kernel_entry:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, BOOT_SECTOR
    mov dl, [ss:BOOT_DL]
    mov di, MEMMAP_ADDR
    jmp 0x0000:OS_LOAD
