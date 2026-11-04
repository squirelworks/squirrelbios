; SquirrelBIOS x86 — network boot: DHCP, then a TFTP download to 0x7C00
; Original code. Included by x86/squirrel-bios.asm when CONFIG_NETBOOT.
;
; Written from RFC 2131 and 2132 (DHCP and its options), RFC 951
; (BOOTP), RFC 1350 (TFTP), RFC 826 (ARP), RFC 791 (IPv4) and RFC 768
; (UDP). When no disk and no CD booted, net_boot brings up a virtio-net
; device (drivers/virtio/net.inc), asks for an address and a boot file
; (DHCPDISCOVER, DHCPREQUEST), finds the boot server's Ethernet address
; (ARP), and reads the file over TFTP (512-byte blocks) into memory from
; 0x7C00, as if its sectors had been read from a disk there. The device
; is reset, and the file is entered at 0000:7C00 with DL = FFh (no
; INT 13h drive) like a boot sector.
;
; The boot server must be on the local network: there is no routing.
; The file must end below NET_SEG (0x40000), where the network buffers
; are: up to 230400 bytes. Everything runs with interrupts off, on a
; stack in NET_SEG, and every wait is counted in milliseconds.

; Variables, in NET_SEG (net.inc's before 6620h). Addresses are kept in
; network byte order, ports and block numbers in the CPU's.
NN_MAC          equ 0x6620          ; 6 bytes: our Ethernet address
NN_IP           equ 0x6628          ; dword: our IP address (0: none)
NN_SRV          equ 0x662C          ; dword: the boot server's
NN_SRVMAC       equ 0x6630          ; 6 bytes: and its Ethernet address
NN_SRVID        equ 0x6638          ; dword: the DHCP server's identifier
NN_XID          equ 0x663C          ; dword: DHCP transaction ID
NN_TID          equ 0x6640          ; word: the TFTP server's port
NN_BLOCK        equ 0x6642          ; word: the next TFTP block expected
NN_DEST         equ 0x6644          ; dword: where it goes (linear)
NN_GOT          equ 0x6648          ; dword: bytes loaded
NN_RXIP         equ 0x664C          ; dword: the last datagram's source
NN_RXPORT       equ 0x6650          ; word: and its source port
NN_IPID         equ 0x6652          ; word: the next IP identification
NN_ARP          equ 0x6654          ; byte: 1 once NN_SRVMAC is known
NN_BCAST        equ 0x6655          ; byte: 1 to send from 0.0.0.0 to all
NN_TRIES        equ 0x6656          ; byte: retries left
NN_PTYPE        equ 0x6657          ; byte: DHCP message type parsed
NN_SAVESP       equ 0x6658          ; dword: POST's SP, then SS
NN_PSID         equ 0x665C          ; dword: server identifier parsed
NN_PFILE        equ 0x6660          ; word: option 67 parsed (offset; 0 none)
NN_PFLEN        equ 0x6662          ; byte: its length
NN_FILE         equ 0x6680          ; the boot file's name, NUL-ended

NET_PAY         equ NET_TX + 42     ; a UDP datagram's data
NET_PORT        equ 2070            ; our TFTP port
NET_TFTP        equ 69
NET_DHCPS       equ 67
NET_DHCPC       equ 68
NET_LOAD        equ BOOT_SECTOR     ; where the file goes
DHCP_COOKIE     equ 0x63538263      ; 63h 82h 53h 63h
DHCP_DISCOVER   equ 1
DHCP_OFFER      equ 2
DHCP_REQUEST    equ 3
DHCP_ACK        equ 5
DHCP_NAK        equ 6

; POST, when no disk or CD booted. Boots the file from the network, or
; returns with every register kept if there is no virtio-net device or
; the boot fails (after saying why).
net_boot:
    pushad
    push fs
    push gs
    cli
    push word NET_SEG
    pop fs
    mov [fs:NN_SAVESP], sp
    mov [fs:NN_SAVESP + 2], ss
    push fs
    pop ss
    mov sp, NET_STACK
    call net_try
    lss sp, [fs:NN_SAVESP]
    pop gs
    pop fs
    popad
    ret

; FS = NET_SEG, on its stack. Does not return if the file was loaded.
net_try:
    call vnet_open
    jc .ret
    ; "net: virtio-net, MAC 52:54:00:12:34:56"
    mov si, msg_net_mac
    mov ah, ATTR_NORMAL
    call con_print
    xor bx, bx
.mac:
    mov al, [fs:NN_MAC + bx]
    call con_hex8
    inc bx
    cmp bx, 6
    jae .mac_done
    mov al, ':'
    call con_putc
    jmp .mac
.mac_done:
    call con_newline
    call net_dhcp
    jc .fail
    ; "net: address 10.0.2.15 from 10.0.2.2, boot file NAME"
    mov si, msg_net_ip
    mov ah, ATTR_NORMAL
    call con_print
    mov bx, NN_IP
    call net_print_ip
    mov si, msg_net_from
    call con_print
    mov bx, NN_SRV
    call net_print_ip
    mov si, msg_net_file
    call con_print
    mov bx, NN_FILE
.name:
    mov al, [fs:bx]
    test al, al
    jz .named
    call con_putc
    inc bx
    jmp .name
.named:
    call con_newline
    call net_arp
    jc .fail
    call net_tftp
    jc .fail
    call vnet_close
    ; "net: loaded N bytes"
    mov si, msg_net_got
    mov ah, ATTR_NORMAL
    call con_print
    mov eax, [fs:NN_GOT]
    call net_dec32
    mov si, msg_net_bytes
    call con_print
    call con_newline
    push word 0
    pop gs
    mov byte [gs:BOOT_DL], 0xFF
    jmp handoff
.fail:
    mov ah, ATTR_ERROR
    call con_print
    call con_newline
    call vnet_close
.ret:
    ret

; Print the IP address at FS:BX as a.b.c.d. Keeps every register but DI
; (the cursor).
net_print_ip:
    push eax
    push bx
    push cx
    mov cx, 4
.byte:
    movzx eax, byte [fs:bx]
    call net_dec32
    inc bx
    dec cx
    jz .done
    mov al, '.'
    mov ah, ATTR_NORMAL
    call con_putc
    jmp .byte
.done:
    pop cx
    pop bx
    pop eax
    ret

; Print EAX in decimal. Keeps every register but DI (the cursor).
net_dec32:
    push eax
    push ebx
    push cx
    push edx
    xor cx, cx
    mov ebx, 10
.div:
    xor edx, edx
    div ebx
    push dx
    inc cx
    test eax, eax
    jnz .div
.out:
    pop ax
    add al, '0'
    mov ah, ATTR_NORMAL
    call con_putc
    loop .out
    pop edx
    pop cx
    pop ebx
    pop eax
    ret

; Send the CX bytes at FS:NET_PAY as a UDP datagram from port BX to port
; DX: to the boot server, or from 0.0.0.0 to everyone while NN_BCAST is
; set. CF set if the device did not take it. Keeps every register.
net_udp_send:
    pushad
    ; Ethernet: to everyone, or to the server
    mov eax, 0xFFFFFFFF
    mov si, 0xFFFF
    cmp byte [fs:NN_BCAST], 0
    jne .dst
    mov eax, [fs:NN_SRVMAC]
    mov si, [fs:NN_SRVMAC + 4]
.dst:
    mov [fs:NET_TX], eax
    mov [fs:NET_TX + 4], si
    mov eax, [fs:NN_MAC]
    mov [fs:NET_TX + 6], eax
    mov ax, [fs:NN_MAC + 4]
    mov [fs:NET_TX + 10], ax
    mov word [fs:NET_TX + 12], 0x0008       ; IPv4
    ; IPv4
    mov word [fs:NET_TX + 14], 0x0045       ; version 4, 20 bytes; TOS 0
    mov ax, cx
    add ax, 28
    xchg al, ah
    mov [fs:NET_TX + 16], ax
    mov ax, [fs:NN_IPID]
    inc word [fs:NN_IPID]
    xchg al, ah
    mov [fs:NET_TX + 18], ax
    mov word [fs:NET_TX + 20], 0
    mov word [fs:NET_TX + 22], 0x1140       ; TTL 64, UDP
    mov word [fs:NET_TX + 24], 0
    xor eax, eax
    dec eax
    mov [fs:NET_TX + 30], eax
    inc eax
    cmp byte [fs:NN_BCAST], 0
    jne .src
    mov eax, [fs:NN_SRV]
    mov [fs:NET_TX + 30], eax
    mov eax, [fs:NN_IP]
.src:
    mov [fs:NET_TX + 26], eax
    ; Its checksum: the ones' complement of the ones' complement sum
    xor eax, eax
    xor si, si
.sum:
    movzx ebp, word [fs:NET_TX + 14 + si]
    add eax, ebp
    add si, 2
    cmp si, 20
    jb .sum
    mov ebp, eax
    shr ebp, 16
    add ax, bp
    adc ax, 0
    not ax
    mov [fs:NET_TX + 24], ax
    ; UDP, without a checksum
    xchg bl, bh
    mov [fs:NET_TX + 34], bx
    xchg dl, dh
    mov [fs:NET_TX + 36], dx
    mov ax, cx
    add ax, 8
    xchg al, ah
    mov [fs:NET_TX + 38], ax
    mov word [fs:NET_TX + 40], 0
    add cx, 42
    call vnet_send
    popad
    ret

; Wait up to CX ms for a UDP datagram to port DX (DX = 0: until NN_ARP is
; set). Frames met on the way are dealt with (ARP) or dropped. CF clear:
; SI = its data (offset in FS), CX = its length, NN_RXIP and NN_RXPORT
; its source. CF set on a timeout. Keeps every other register.
net_wait:
    push bp
    mov bp, cx
.look:
    test dx, dx
    jnz .recv
    cmp byte [fs:NN_ARP], 0
    jne .done
.recv:
    call vnet_recv
    jc .idle
    call net_frame
    jc .look
    test dx, dx
    jz .look
.done:
    pop bp
    clc
    ret
.idle:
    test bp, bp
    jz .timeout
    dec bp
    push cx
    mov cx, 1
    call delay_ms
    pop cx
    jmp .look
.timeout:
    pop bp
    stc
    ret

; The frame of CX bytes at FS:SI: CF clear if it is a UDP datagram to
; port DX (DX nonzero) for us, with SI, CX, NN_RXIP and NN_RXPORT as
; net_wait returns them. An ARP reply from the boot server sets
; NN_SRVMAC; an ARP request for our address is answered. CF set
; otherwise. Keeps every other register.
net_frame:
    push eax
    push bx
    cmp cx, 42
    jb .no
    mov ax, [fs:si + 12]
    cmp ax, 0x0608
    je .arp
    cmp ax, 0x0008
    jne .no
    ; IPv4, unfragmented, UDP
    mov al, [fs:si + 14]
    mov ah, al
    and ah, 0xF0
    cmp ah, 0x40
    jne .no
    and ax, 0x0F
    shl ax, 2                   ; AX = header length
    cmp ax, 20
    jb .no
    cmp byte [fs:si + 23], 17
    jne .no
    test word [fs:si + 20], 0xFF3F
    jnz .no
    mov ebx, [fs:si + 30]       ; for us, for everyone, or anyone's
    cmp ebx, 0xFFFFFFFF         ; while we have no address
    je .ours
    cmp dword [fs:NN_IP], 0
    je .ours
    cmp ebx, [fs:NN_IP]
    jne .no
.ours:
    test dx, dx
    jz .no
    mov ebx, [fs:si + 26]
    mov [fs:NN_RXIP], ebx
    add si, 14
    add si, ax                  ; SI = the UDP header
    sub cx, 14 + 8
    sub cx, ax
    jb .no
    mov bx, [fs:si + 2]
    xchg bl, bh
    cmp bx, dx
    jne .no
    mov bx, [fs:si]
    xchg bl, bh
    mov [fs:NN_RXPORT], bx
    mov bx, [fs:si + 4]
    xchg bl, bh
    sub bx, 8
    jb .no
    cmp bx, cx
    ja .no
    mov cx, bx
    add si, 8
    pop bx
    pop eax
    clc
    ret
.arp:
    ; Ethernet and IPv4 addresses only
    cmp dword [fs:si + 14], 0x00080100
    jne .no
    cmp word [fs:si + 18], 0x0406
    jne .no
    ; From the boot server, request or reply: its address
    mov eax, [fs:si + 28]
    test eax, eax
    jz .request
    cmp eax, [fs:NN_SRV]
    jne .request
    mov eax, [fs:si + 22]
    mov [fs:NN_SRVMAC], eax
    mov ax, [fs:si + 26]
    mov [fs:NN_SRVMAC + 4], ax
    mov byte [fs:NN_ARP], 1
.request:
    cmp word [fs:si + 20], 0x0100
    jne .no
    mov eax, [fs:NN_IP]
    test eax, eax
    jz .no
    cmp eax, [fs:si + 38]
    jne .no
    ; Answer it: our address, to the asker
    mov eax, [fs:si + 22]
    mov [fs:NET_TX], eax
    mov [fs:NET_TX + 32], eax
    mov ax, [fs:si + 26]
    mov [fs:NET_TX + 4], ax
    mov [fs:NET_TX + 36], ax
    mov eax, [fs:si + 28]
    mov [fs:NET_TX + 38], eax
    mov eax, [fs:NN_MAC]
    mov [fs:NET_TX + 6], eax
    mov [fs:NET_TX + 22], eax
    mov ax, [fs:NN_MAC + 4]
    mov [fs:NET_TX + 10], ax
    mov [fs:NET_TX + 26], ax
    mov eax, [fs:NN_IP]
    mov [fs:NET_TX + 28], eax
    mov word [fs:NET_TX + 12], 0x0608
    mov dword [fs:NET_TX + 14], 0x00080100
    mov dword [fs:NET_TX + 18], 0x02000406  ; sizes 6, 4; reply
    push cx
    mov cx, 42
    call vnet_send
    pop cx
.no:
    pop bx
    pop eax
    stc
    ret

; DHCP: an offer, then the address requested and acknowledged. Sets
; NN_IP, NN_SRV, NN_SRVID and NN_FILE. CF set with SI = the reason on
; failure. Keeps every other register.
net_dhcp:
    push eax
    push bx
    push cx
    push dx
    mov eax, [fs:NN_MAC + 2]
    xor eax, 0x5351524C
    mov [fs:NN_XID], eax
    xor eax, eax
    mov [fs:NN_IP], eax
    mov [fs:NN_SRV], eax
    mov [fs:NN_SRVID], eax
    mov [fs:NN_FILE], al
    mov byte [fs:NN_ARP], 0
    mov byte [fs:NN_BCAST], 1
    mov byte [fs:NN_TRIES], 4
.discover:
    mov al, DHCP_DISCOVER
    call net_bootp
.offer:
    mov cx, 2000
    mov dx, NET_DHCPC
    call net_wait
    jc .again
    call net_bootp_parse
    jc .offer
    cmp byte [fs:NN_PTYPE], DHCP_OFFER
    jne .offer
    call net_bootp_take
    mov byte [fs:NN_TRIES], 4
.request:
    mov al, DHCP_REQUEST
    call net_bootp
.ack:
    mov cx, 2000
    mov dx, NET_DHCPC
    call net_wait
    jc .again_req
    call net_bootp_parse
    jc .ack
    cmp byte [fs:NN_PTYPE], DHCP_NAK
    je .none
    cmp byte [fs:NN_PTYPE], DHCP_ACK
    jne .ack
    call net_bootp_take
    mov byte [fs:NN_BCAST], 0
    mov eax, [fs:NN_SRV]
    test eax, eax
    jnz .server
    mov eax, [fs:NN_SRVID]
    mov [fs:NN_SRV], eax
    test eax, eax
    jz .none
.server:
    mov si, msg_net_nofile
    cmp byte [fs:NN_FILE], 0
    je .fail
    pop dx
    pop cx
    pop bx
    pop eax
    clc
    ret
.again:
    dec byte [fs:NN_TRIES]
    jnz .discover
    jmp .none
.again_req:
    dec byte [fs:NN_TRIES]
    jnz .request
.none:
    mov si, msg_net_nodhcp
.fail:
    mov byte [fs:NN_BCAST], 0
    pop dx
    pop cx
    pop bx
    pop eax
    stc
    ret

; Send a BOOTP request carrying DHCP message type AL: a request names the
; address offered (NN_IP) and the server (NN_SRVID). Keeps every
; register.
net_bootp:
    pushad
    xor bx, bx
.zero:
    mov dword [fs:NET_PAY + bx], 0
    add bx, 4
    cmp bx, 300
    jb .zero
    mov dword [fs:NET_PAY], 0x00060101      ; request, Ethernet, 6 bytes
    mov ebx, [fs:NN_XID]
    mov [fs:NET_PAY + 4], ebx
    mov byte [fs:NET_PAY + 10], 0x80        ; answer by broadcast
    mov ebx, [fs:NN_MAC]
    mov [fs:NET_PAY + 28], ebx
    mov bx, [fs:NN_MAC + 4]
    mov [fs:NET_PAY + 32], bx
    mov dword [fs:NET_PAY + 236], DHCP_COOKIE
    mov word [fs:NET_PAY + 240], 0x0135     ; 53: message type
    mov [fs:NET_PAY + 242], al
    mov bx, 243
    cmp al, DHCP_REQUEST
    jne .params
    mov word [fs:NET_PAY + bx], 0x0432      ; 50: the address
    mov eax, [fs:NN_IP]
    mov [fs:NET_PAY + bx + 2], eax
    mov word [fs:NET_PAY + bx + 6], 0x0436  ; 54: the server
    mov eax, [fs:NN_SRVID]
    mov [fs:NET_PAY + bx + 8], eax
    add bx, 12
.params:
    ; 55: subnet mask, router, TFTP server name, boot file name
    mov dword [fs:NET_PAY + bx], 0x03010437
    mov word [fs:NET_PAY + bx + 4], 0x4342
    mov byte [fs:NET_PAY + bx + 6], 0xFF    ; end
    mov cx, 300
    mov bx, NET_DHCPC
    mov dx, NET_DHCPS
    call net_udp_send
    popad
    ret

; The datagram of CX bytes at FS:SI: CF clear if it is a BOOTP reply to
; our transaction with a DHCP message type, which goes to NN_PTYPE, its
; server identifier to NN_PSID and its option 67 to NN_PFILE / NN_PFLEN.
; Keeps every register.
net_bootp_parse:
    pushad
    cmp cx, 240
    jb .no
    cmp byte [fs:si], 2
    jne .no
    mov eax, [fs:si + 4]
    cmp eax, [fs:NN_XID]
    jne .no
    cmp dword [fs:si + 236], DHCP_COOKIE
    jne .no
    mov byte [fs:NN_PTYPE], 0
    mov dword [fs:NN_PSID], 0
    mov word [fs:NN_PFILE], 0
    mov bx, si
    add bx, cx                  ; BX = its end
    add si, 240
.opt:
    cmp si, bx
    jae .done
    mov al, [fs:si]
    cmp al, 0xFF
    je .done
    inc si
    test al, al
    jz .opt                     ; pad
    cmp si, bx
    jae .done
    movzx cx, byte [fs:si]      ; its length
    inc si
    mov dx, si
    add dx, cx
    cmp dx, bx
    ja .done
    cmp al, 53
    jne .sid
    mov al, [fs:si]
    mov [fs:NN_PTYPE], al
    jmp .next
.sid:
    cmp al, 54
    jne .file
    cmp cx, 4
    jb .next
    mov eax, [fs:si]
    mov [fs:NN_PSID], eax
    jmp .next
.file:
    cmp al, 67
    jne .next
    mov [fs:NN_PFILE], si
    mov [fs:NN_PFLEN], cl
.next:
    mov si, dx
    jmp .opt
.done:
    cmp byte [fs:NN_PTYPE], 0
    je .no
    popad
    clc
    ret
.no:
    popad
    stc
    ret

; From the parsed reply at FS:SI: our address, the boot server (siaddr),
; the DHCP server, and the boot file (option 67, else the file field).
; Keeps every register.
net_bootp_take:
    pushad
    mov eax, [fs:si + 16]
    mov [fs:NN_IP], eax
    mov eax, [fs:si + 20]
    test eax, eax
    jz .sid
    mov [fs:NN_SRV], eax
.sid:
    mov eax, [fs:NN_PSID]
    test eax, eax
    jz .file
    mov [fs:NN_SRVID], eax
.file:
    mov bx, [fs:NN_PFILE]
    movzx cx, byte [fs:NN_PFLEN]
    test bx, bx
    jnz .copy
    cmp byte [fs:si + 108], 0
    je .out                     ; keep the offer's
    lea bx, [si + 108]
    mov cx, 127
.copy:
    cmp cx, 127
    jbe .len
    mov cx, 127
.len:
    xor di, di
.byte:
    jcxz .end
    mov al, [fs:bx + di]
    test al, al
    jz .end
    mov [fs:NN_FILE + di], al
    inc di
    dec cx
    jmp .byte
.end:
    mov byte [fs:NN_FILE + di], 0
.out:
    popad
    ret

; The boot server's Ethernet address, by ARP unless already known. CF
; set with SI = the reason if it does not answer. Keeps every other
; register.
net_arp:
    push eax
    push cx
    push dx
    mov byte [fs:NN_TRIES], 4
.ask:
    cmp byte [fs:NN_ARP], 0
    jne .known
    ; A request, to everyone
    mov eax, 0xFFFFFFFF
    mov [fs:NET_TX], eax
    mov [fs:NET_TX + 4], ax
    mov eax, [fs:NN_MAC]
    mov [fs:NET_TX + 6], eax
    mov [fs:NET_TX + 22], eax
    mov ax, [fs:NN_MAC + 4]
    mov [fs:NET_TX + 10], ax
    mov [fs:NET_TX + 26], ax
    mov word [fs:NET_TX + 12], 0x0608
    mov dword [fs:NET_TX + 14], 0x00080100
    mov dword [fs:NET_TX + 18], 0x01000406  ; sizes 6, 4; request
    mov eax, [fs:NN_IP]
    mov [fs:NET_TX + 28], eax
    xor eax, eax
    mov [fs:NET_TX + 32], eax
    mov [fs:NET_TX + 36], ax
    mov eax, [fs:NN_SRV]
    mov [fs:NET_TX + 38], eax
    mov cx, 42
    call vnet_send
    mov cx, 1000
    xor dx, dx
    call net_wait
    jnc .known
    dec byte [fs:NN_TRIES]
    jnz .ask
    mov si, msg_net_noarp
    pop dx
    pop cx
    pop eax
    stc
    ret
.known:
    pop dx
    pop cx
    pop eax
    clc
    ret

; Read the boot file over TFTP into NET_LOAD onwards. NN_GOT = its size.
; CF set with SI = the reason on failure. Keeps every other register.
net_tftp:
    push eax
    push ebx
    push cx
    push dx
    push di
    mov dword [fs:NN_DEST], NET_LOAD
    mov dword [fs:NN_GOT], 0
    mov word [fs:NN_BLOCK], 1
    mov word [fs:NN_TID], NET_TFTP
    mov byte [fs:NN_TRIES], 5
.rrq:
    ; Read request: opcode 1, the file's name, "octet"
    mov word [fs:NET_PAY], 0x0100
    xor bx, bx
.name:
    mov al, [fs:NN_FILE + bx]
    mov [fs:NET_PAY + 2 + bx], al
    inc bx
    test al, al
    jnz .name
    mov dword [fs:NET_PAY + 2 + bx], 'octe'
    mov word [fs:NET_PAY + 2 + bx + 4], 't'
    lea cx, [bx + 2 + 6]
    mov bx, NET_PORT
    mov dx, NET_TFTP
    call net_udp_send
.wait:
    mov cx, 1000
    mov dx, NET_PORT
    call net_wait
    jc .timeout
    mov eax, [fs:NN_RXIP]
    cmp eax, [fs:NN_SRV]
    jne .wait
    cmp cx, 4
    jb .wait
    mov ax, [fs:si]
    xchg al, ah
    cmp ax, 5
    je .refused
    cmp ax, 3
    jne .wait
    ; Data: the first block names the server's port for the transfer
    mov bx, [fs:NN_RXPORT]
    cmp word [fs:NN_TID], NET_TFTP
    jne .tid
    cmp word [fs:NN_BLOCK], 1
    jne .tid
    mov [fs:NN_TID], bx
.tid:
    cmp bx, [fs:NN_TID]
    jne .wait
    mov bx, [fs:si + 2]
    xchg bl, bh
    cmp bx, [fs:NN_BLOCK]
    je .block
    inc bx
    cmp bx, [fs:NN_BLOCK]
    jne .wait
    dec bx
    call net_tftp_ack           ; ours was lost: again
    jmp .wait
.block:
    sub cx, 4                   ; CX = its bytes
    movzx eax, cx
    add eax, [fs:NN_DEST]
    cmp eax, NET_LIN
    ja .big
    ; Copy them to NN_DEST, through GS
    mov eax, [fs:NN_DEST]
    push eax
    shr eax, 4
    mov gs, ax
    pop eax
    and ax, 0x0F
    mov di, ax
    push si
    add si, 4
    push cx
.copy:
    jcxz .copied
    mov al, [fs:si]
    mov [gs:di], al
    inc si
    inc di
    dec cx
    jmp .copy
.copied:
    pop cx
    pop si
    movzx eax, cx
    add [fs:NN_DEST], eax
    add [fs:NN_GOT], eax
    call net_tftp_ack
    inc word [fs:NN_BLOCK]
    mov byte [fs:NN_TRIES], 5
    cmp cx, 512
    jae .wait
    ; A short block is the last
    pop di
    pop dx
    pop cx
    pop ebx
    pop eax
    clc
    ret
.timeout:
    dec byte [fs:NN_TRIES]
    jz .timed_out
    mov bx, [fs:NN_BLOCK]
    dec bx
    jz .rrq                     ; nothing yet: ask again
    call net_tftp_ack
    jmp .wait
.refused:
    mov si, msg_net_refused
    jmp .fail
.big:
    mov si, msg_net_big
    jmp .fail
.timed_out:
    mov si, msg_net_timeout
.fail:
    pop di
    pop dx
    pop cx
    pop ebx
    pop eax
    stc
    ret

; Acknowledge TFTP block BX. Keeps every register.
net_tftp_ack:
    push bx
    push cx
    push dx
    mov word [fs:NET_PAY], 0x0400
    xchg bl, bh
    mov [fs:NET_PAY + 2], bx
    mov cx, 4
    mov bx, NET_PORT
    mov dx, [fs:NN_TID]
    call net_udp_send
    pop dx
    pop cx
    pop bx
    ret
