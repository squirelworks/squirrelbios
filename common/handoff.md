# Hand-off contract

This file is the single source of truth for what SquirrelBIOS guarantees
when it passes control to the OS, and for what the OS must not assume.
If the firmware and this file disagree, the firmware has a bug.

## At a glance

| | x86 PC | AArch64 (QEMU virt) | RISC-V (QEMU virt) |
|---|---|---|---|
| Image source | LBA 0 of the primary master ATA disk, or the primary slave when the master is absent; sectors 1–192 as well when they are a `S2OK` image; a Linux `bzImage` (`HdrS`) loads its protected-mode kernel at 1 MB | `-kernel`, read through fw_cfg DMA | `-kernel`, loaded by QEMU |
| Entry | `0x0000:0x7C00`, or `0x0000:0x8000` for a `S2OK` image, or 32-bit PM at `code32_start` (usually `0x100000`) for a `bzImage` | `0x40200000` | `0x80200000` (rv64) |
| CPU mode | 16-bit real mode | EL1 (EL2 with `virtualization=on`), MMU off | S-mode (QEMU's `next_mode`), `satp = 0` |
| Boot argument | `DL = 0x80` (master) or `0x81` (slave) | `x0` = device tree | `a0` = hart ID, `a1` = device tree |
| Memory description | Memory map at `ES:DI = 0x0000:0x0500` | Device tree `/memory` | Device tree `/memory` |
| Console | COM1 `0x3F8`, 115200 8N1 | PL011 `0x09000000` | NS16550A `0x10000000`, 115200 8N1 |
| Interrupts | `IF = 0`; PIT/PIC programmed, IRQ0 unmasked | `DAIF` masked | `mstatus.MIE`/`SIE` = 0, `mie = 0` |
| CPUs running | Boot CPU only | Boot CPU only | The boot hart (`fw_dynamic_info.boot_hart`, else hart 0); others through `sbi_hart_start` |
| Firmware services | `INT 13h` `AH=00/02/03/08/41/42/43/44/47/48` (on an IDE, SATA, NVMe, USB or virtio disk; diskettes `00/02/03/08/15`; boot CD `00/41/42/48/4B`), `INT 10h` `AH=02/03/06/07/0E/0F/4F` (VBE `4F00..4F03/06/07`), `INT 15h` `E820`/`AX=2400..2403`/`AH=C2` (USB mouse), `INT 16h` `AH=00/01`, `INT 1Ah` `AH=00/02`, PCI BIOS `AX=B101/B102/B103/B108..B10D` | None | SBI from S-mode: base, TIME, IPI, RFENCE, HSM, SRST |

## Rules for every architecture

The firmware guarantees:

1. It does only early init, the image load and the jump. Nothing runs
   after the jump, except the RISC-V SBI when the OS calls it.
2. Interrupts are masked at the CPU on entry.
3. The serial console is initialised and has already printed the banner,
   so the OS can write to it straight away.
4. If the image cannot be loaded, the firmware prints the reason on the
   console and halts. It never jumps to a partial or missing image.

The OS must not assume:

1. **Firmware services.** There are none after hand-off, apart from the
   RISC-V SBI listed below and the x86 calls below. `INT 15h` with
   `EAX = 0xE820` returns the memory map. `INT 13h` `AH = 0x00`, `0x02`,
   `0x08` and `0x42` read the primary master (`DL = 0x80`) or primary
   slave (`DL = 0x81`); `AH = 0x03` and `0x43` write the same drives.
   `AH = 0x41`, `0x44`, `0x47` and `0x48` are the rest of EDD 3.0's
   fixed disk access subset.
   With `DL = 0x00` / `0x01`, `INT 13h` `AH = 0x00`, `0x02`, `0x03`,
   `0x08` and `0x15` work on 1.44 MB diskettes in A: / B:.
   `INT 10h` `AH = 0x0E`
   writes one character to the VGA text screen; `AH = 0x02`/`0x03` set
   or get the cursor, `AH = 0x06`/`0x07` scroll, and `AH = 0x0F` reports
   the 80x25 mode. `INT 16h` `AH = 0x00` and `0x01` read or check the
   keyboard buffer. `INT 1Ah` `AH = 0x00` returns the tick count;
   `AH = 0x02` reads the RTC time in BCD; `AH = 0xB1` is the PCI BIOS
   (real mode). Any other function on those
   vectors returns with CF set (`INT 16h` check uses ZF instead). No
   other software interrupt is serviced. No UEFI. Anything else the OS
   needs, it drives itself.
2. **A usable stack.** Set your own stack before using it heavily.
3. **Trap or interrupt vectors.** Install your own before unmasking
   interrupts.
4. **Register values not listed below.** Treat them as undefined.
5. **That firmware data stays put.** Hand-off data (the x86 memory map,
   the device tree) lives in RAM the OS may reuse. Copy or parse it before
   overwriting that RAM.
6. **Other CPUs.** Only the boot CPU enters the OS. Secondary CPUs are
   not started (AArch64) or are parked until the OS starts them with
   `sbi_hart_start` (RISC-V).

### Network boot

The firmware does not boot over the network, on any architecture, and
has no network driver. It starts the image that is already there: the
one QEMU loaded with `-kernel` (RISC-V, AArch64) or the one on the
primary disk (x86). A diskless node, such as one of the many identical
nodes of a cluster, gets its OS in two steps. First it boots a small
kernel and initramfs from wherever the machine keeps them (QEMU's
`-kernel` and `-initrd`, or a small local disk). That kernel then brings
up the network with its own drivers and fetches the real system: an NFS
or network-block root, or a second kernel started with `kexec`.

The reasons:

- **Size and risk.** A network loader means a NIC driver (virtio-net
  queues), DHCP, and TFTP or HTTP, all in assembly, for each
  architecture. That is more code than the rest of the firmware, and it
  would run before anything can check it.
- **Linux does it better.** Linux already has every NIC driver, DHCP,
  NFS root and `kexec`, and it can retry and report errors.
- **The contract stays the same.** The hand-off stays "load the image
  that is there, then jump", which `make test` checks on every
  architecture.

Revisit this only if a target machine has no way to supply the first
kernel. That would be a separate TODO item with its own stand-in, a
`make test` case that loads an image over QEMU's user-mode network.

## x86 PC

### Registers

| Item | Value |
|------|-------|
| `CS:IP` | `0x0000:0x7C00` (far jump) |
| Mode | 16-bit real mode |
| `DS`, `ES`, `SS` | `0` |
| `SP` | `0x7C00` |
| `DL` | `0x80` (primary master) or `0x81` (primary slave, when that is the boot drive) |
| `DI` | `0x0500` (memory map header, so `ES:DI` points at it) |
| `EFLAGS` | `IF = 0`, `DF = 0` |

### Memory

| Range | Contents |
|-------|----------|
| `0x0500`–`0x05FF` | Memory map (below). Always at this address. The firmware's own stack sits at the top of this page while it runs, and is free after the jump. |
| `0x7C00`–`0x7DFF` | Sector 1 of the disk. The one-sector and SQBL paths jump here |
| `0x8000`–`0x1FFFF` | The kernel, when the image is the `S2OK` one below. That path jumps to `0x8000` |
| SQBL load address | The extra sectors, when sector 1 has an SQBL header (below) |

`SP` is `0x7C00`, so the first push lands at `0x7BFE`. An image that loads
extra sectors right up to `0x7C00` should move the stack before it relies
on the last bytes it loaded.

### Extra sectors (SQBL header)

The firmware reads one sector unless that sector asks for more. To ask,
put this header at the end of sector 1, immediately before the
signature. All fields are little-endian.

| Offset | Size | Field |
|--------|------|-------|
| `0x1F0` | 4 | Magic bytes `53 51 42 4C` (`SQBL`). Absent means one sector. |
| `0x1F4` | 2 | Extra sector count, 1–128. |
| `0x1F6` | 2 | Load segment. |
| `0x1F8` | 2 | Load offset. |
| `0x1FA` | 4 | Reserved, zero. The firmware does not read it. |
| `0x1FE` | 2 | `0xAA55` |

With the magic present, the firmware reads that many sectors starting at
LBA 1 (the sector after the boot sector) to linear address
`segment * 16 + offset`, and only then jumps to `0x0000:0x7C00`, with the
same registers and memory map as a one-sector boot. The loaded bytes are
contiguous in linear memory; the firmware normalises the segment per
sector, so a load may cross 64 KB boundaries.

The load must start below the EBDA (`0x92000`) and end at or before it, and must not
touch `0x0500`–`0x05FF` or `0x7C00`–`0x7DFF`. Anything else, including a
count of 0 or above 128, is refused before the read (see Failures).
Memory below `0x0500` is allowed. Every path fills the IVT before it
jumps; see below.

### Squirrel-OS/2 image (`S2OK`)

Squirrel-OS/2's own boot sector does not use the SQBL header. When
sector 0 has no SQBL magic and LBA 192's last dword is `S2OK` (bytes
`53 32 4F 4B`), the firmware reads LBAs 1–192 (96 KB) to `0x8000` and
jumps to `0x0000:0x8000`. The registers are the same as the `0x7C00`
hand-off (`DL = 0x80`, `DS = ES = SS = 0`, `SP = 0x7C00`,
`DI = 0x0500`, `IF = 0`, `DF = 0`). It prints `kernel loaded at
0x00008000, jumping` first. A disk that is too small, or whose LBA 192
does not end in `S2OK`, takes the one-sector path: the firmware jumps
to `0x0000:0x7C00` and the boot sector loads the kernel itself. That
probe is a single read and is not reported as a disk error. A failed
probe resets the drive so the boot sector's first read does not see a
stale error bit.

The boot sector's calls, which the firmware serves on every path:

| Call | Behaviour |
|------|-----------|
| `INT 13h` `AH = 0x42` | Extended read. `DS:SI` points at a 16-byte disk address packet (size byte `0x10` or more, reserved, word count 1–128, word offset, word segment, qword LBA; the 64-bit flat buffer of a larger packet is not used). The buffer must lie entirely below the EBDA (`0x92000`). The LBA's upper dword must be 0 (disks up to 2 TB). A range that reaches sector 2^28 (128 GiB) or beyond goes to the drive as `READ SECTORS EXT` (LBA48), anything below as LBA28. CF clear and `AH = 0` on success. |
| `INT 13h` `AH = 0x43` | Extended write. Same packet rules as `AH = 0x42` (`WRITE SECTORS EXT` past 2^28). CF clear and `AH = 0` on success. |
| `INT 13h` `AH = 0x41` | EDD installation check: with `BX = 0x55AA` and a drive that answered at POST, CF clear, `BX = 0xAA55`, `AH = 0x30` (EDD 3.0), `CX = 0x0001` (fixed disk access subset: `42h`, `43h`, `44h`, `47h`, `48h`). Otherwise CF set, `AH = 0x01`. |
| `INT 13h` `AH = 0x44` / `0x47` | Verify / seek. Same packet rules as `AH = 0x42`; nothing is transferred or moved. CF clear and `AH = 0` when every sector named is on the disk, CF set and `AH = 0x04` when not. |
| `INT 13h` `AH = 0x48` | Drive parameters into the buffer at `DS:SI`, whose first word is its size (`0x1A` or more). Fills total sectors (a qword, from `IDENTIFY DEVICE`: words 100–103 with LBA48, else 60–61), 512 bytes per sector, and the INT 13h CHS translation (1024 / 16 / 63, flags `0`: not the drive's own geometry). With `0x1E` or more, the DPTE pointer is `FFFF:FFFF` (none). With `0x42` or more, the EDD 3.0 device path: key `0xBEDD`, length `0x24`, host bus `ISA `, interface `ATA     `, base port `0x1F0`, device 0 (master) or 1 (slave), checksum. The size word is set to `0x1A`, `0x1E` or `0x42` to match. A buffer under `0x1A`: CF set, `AH = 0x01`. |
| `INT 13h` `AH = 0x08` | Drive parameters for `DL = 0x80` or `0x81`. CF clear, `AH = 0`, `DH = 15` (16 heads), `CL` bits 5–0 `= 63` (sectors per track), `CH = 0xFF` and `CL` bits 7–6 `= 3` (1023 cylinders), `DL` = number of hard disks that answered `IDENTIFY` at POST (1 or 2). `ES:DI` and `BL` are left alone. A drive that did not answer: CF set, `AH = 0x07`. |
| `INT 13h` `AH = 0x02` | CHS read of `AL` sectors (1–128) to `ES:BX`, using that same geometry. Cylinder is `CH` plus `CL` bits 7–6, sector is `CL` bits 5–0, head is `DH`. CF clear, `AH = 0`, `AL` unchanged on success. |
| `INT 13h` `AH = 0x03` | CHS write of `AL` sectors from `ES:BX`, same geometry and status rules as `AH = 0x02`. |
| `INT 13h` `AH = 0x00` | Reset the primary channel. CF clear and `AH = 0` when the drive comes ready. |
| `INT 10h` `AH = 0x02` | Set cursor: `DH` = row, `DL` = column (clamped to 80x25). Page `BH` ignored. CF clear. |
| `INT 10h` `AH = 0x03` | Get cursor: `DH`/`DL` = row/column, `CH`/`CL` = `0x06`/`0x07`, `BH` = 0. CF clear. |
| `INT 10h` `AH = 0x06` / `0x07` | Scroll up / down. `AL` = lines (`0` blanks the window), `BH` = blank attribute, `CH`/`CL`–`DH`/`DL` = window. Full-screen one-line scrolls move rows; other windows are blanked. CF clear. |
| `INT 10h` `AH = 0x0E` | Teletype `AL` onto the VGA text screen in the normal attribute, from the cursor at `0x0450` (column, then row). CR, LF, BS and BEL behave as a teletype; the screen scrolls at row 25. Other registers are kept. CF clear. |
| `INT 10h` `AH = 0x0F` | Get mode: `AL` = 3 (80x25 colour), `AH` = 80 columns, `BH` = 0. CF clear. |
| `INT 10h` `AH = 0x4F` | VBE 3.0 on QEMU's standard VGA (PCI `1234:1111`, through its `1CEh`/`1CFh` display registers). `AL = 0x00` controller information at `ES:DI` (512 bytes if it starts `VBE2`): `VESA`, version 3.0, the video memory, and the mode list and strings inside the block. `0x01` mode information for `CX` at `ES:DI`. `0x02` set mode `BX`: bit 14 (linear frame buffer) is required, bit 15 keeps the video memory; `BX = 3` returns to 80x25 text with a blank screen and the cursor at (0, 0). `0x03` current mode in `BX` (`3` in text mode). `0x06` with `BL = 1`: bytes per line in `BX`, pixels in `CX`, lines the memory holds in `DX`. `0x07` display start: `BL = 0x00`/`0x80` set from `CX`, `DX`; `BL = 1` get. Modes `111h`/`112h` 640x480, `114h`/`115h` 800x600, `117h`/`118h` 1024x768, `11Ah`/`11Bh` 1280x1024, each at 16 bits (5:6:5) then 32 bits (8:8:8 with 8 reserved); the frame buffer is the VGA function's BAR0. `AX = 0x004F` on success, `0x014F` on failure (an unknown mode, no bit 14), `0x0100` for other functions or without that VGA. There are no banked windows, palette or protected-mode interface. |
| `INT 16h` `AH = 0x00` | Wait for a keystroke. Returns `AL` = ASCII, `AH` = set-1 make scancode. CF clear. |
| `INT 16h` `AH = 0x01` | Check the buffer. ZF clear and `AX` as above when a key is ready (not removed); ZF set when empty. CF clear. |
| `INT 16h` other `AH` | CF set. |
| `INT 1Ah` `AH = 0x00` | Tick count since midnight: `CX:DX` from `0x046C`, `AL` = midnight overflow at `0x0470` (then cleared). CF clear. |
| `INT 1Ah` `AH = 0x02` | RTC time from CMOS in BCD: `CH` = hours, `CL` = minutes, `DH` = seconds, `DL` = 0 (no daylight flag). Waits for Update-In-Progress to clear. CF clear. (`AH = 0x04` date is not implemented.) |
| `INT 1Ah` other `AH` | CF set. |

`DL` must be `0x80` (primary master) or `0x81` (primary slave) for the
disk calls. Anything else, a packet that is not 16 bytes, a zero count,
a count above 128, or a buffer that is not entirely below the EBDA (`0x92000`),
returns with CF set and `AH = 0x01`. A sector that is not on the disk
returns with CF set and `AH = 0x04` (read) or `AH = 0x05` (reset failed)
or `AH = 0x80` (timeout). `AH = 0x02` reports `AL = 0` when nothing was
transferred. The firmware boots from the master when
it answers; if the master is absent it tries the slave and hands off with
`DL = 0x81`. When no IDE hard disk answered (or the master is ATAPI),
it looks for a SATA disk, then an NVMe disk, then a USB disk, then a
virtio disk, then tries a CD, then the network, before it halts.

**SATA (AHCI).** With no IDE hard disk, the firmware looks on PCI bus 0
for an AHCI controller (class `01:06:01`, such as q35's ICH9 at
`00:1F.2`), resets it, and takes the lowest-numbered implemented port
whose link is up and whose signature is an ATA disk (`00000101h`);
ATAPI and empty ports are passed over. It prints `disk: SATA disk on
AHCI port N`, and that disk is then `DL = 0x80`: it boots the same way
as an IDE master (boot sector, SQBL, S2OK, bzImage), and the `INT 13h`
calls above serve it, with `READ DMA EXT` and `WRITE DMA EXT` (each
write followed by `FLUSH CACHE EXT`). Their buffers must also start at
an even address. `AH = 0x48` names it as host bus `PCI` with the
controller's bus, device and function, and interface `SATA` with the
port number as the device path (EDD 4.0). `DL = 0x81` is not a drive
then. Only one SATA disk is served, and none when an IDE hard disk
answered. The firmware keeps the port running for the hand-off, with
interrupts off (`GHC.IE = 0`); its command list, received-FIS area and
command table are in the EBDA (`0x92400`, `0x92800`, `0x92900`), and
the byte at `0x04FF` is 1 while `DL = 0x80` is the SATA disk (2 for
NVMe, 0 for IDE). The
`INT 13h` calls reach the controller's registers through unreal mode, so
they are served from real mode only, not from virtual 8086 mode.

**NVMe.** With no IDE or SATA disk, the firmware looks on PCI bus 0 for
an NVMe controller (class `01:08:02`) whose BAR0 it placed below 4 GB.
It resets and enables it with 8-entry admin queues, identifies
namespace 1, which must use 512-byte blocks, and creates one 8-entry
I/O queue pair. It prints `disk: NVMe disk (namespace 1)`, and that
namespace is then `DL = 0x80`, booting and served by `INT 13h` as above
with NVMe Read and Write (each write followed by Flush). Their buffers
must start on a 4-byte boundary. A transfer that spans more than two
4 KB pages uses a PRP list. `AH = 0x48` names it as host bus `PCI` with
the controller's bus, device and function, and interface `NVMe` with
namespace ID 1 as the device path. The four queues are the EBDA's pages
at `0x93000` (admin submission), `0x94000` (admin completion),
`0x95000` (I/O submission) and `0x96000` (I/O completion); the
controller is left enabled with interrupts unused. As for SATA, the
`INT 13h` calls are served from real mode only.

**virtio-blk.** With no IDE, SATA or NVMe disk, the firmware looks on
PCI bus 0 for a virtio block device with the legacy interface (vendor
`1AF4h`, device `1001h`, BAR0 an I/O range). It resets the device,
accepts no optional features (so the disk writes through), sets up
queue 0 in the three EBDA pages at `0x93000` that NVMe would have used
(queues of up to 256 entries fit), and reads the capacity. It prints
`disk: virtio disk`, and the device is then `DL = 0x80`, booting and
served by `INT 13h` as above, one request at a time with the caller's
buffer as its data descriptor; the used ring is polled with interrupts
off. `AH = 0x48` names it as host bus `PCI` with its bus, device and
function, and interface `SCSI`, the nearest EDD 3.0 has. The device is
left running with no interrupts unmasked. A modern-only device (ID
`1042h`) is not used.

**Network boot.** When no disk or CD booted, the firmware looks on PCI
bus 0 for a virtio network device with the legacy interface (vendor
`1AF4h`, device `1000h`) and prints its address (`net: virtio-net, MAC
52:54:00:12:34:56`). It asks for an address by DHCP (DISCOVER, then
REQUEST; up to four tries of 2 s each) and takes the boot server from
the reply's `siaddr` (else the DHCP server) and the boot file from
option 67 (else the `file` field): `net: address 10.0.2.15 from
10.0.2.2, boot file NAME`. It finds the server's Ethernet address by ARP
and answers ARP requests for its own address. It then reads the file
over TFTP (octet mode, 512-byte blocks, each wait 1 s and five tries)
into memory from `0x7C00`, as if its sectors had been read there from a
disk, prints `net: loaded N bytes`, resets the device and enters the
file at `0000:7C00` like a boot sector, with `DL = 0xFF`: there is no
`INT 13h` drive behind it. The file must end below `0x40000`, which
allows 230400 bytes: the network buffers and queues are at
`0x40000`–`0x4FFFF` while it runs. The server must be on the local
network, as there is no routing. When something fails it says why
(`net: no DHCP answer`, `net: DHCP gave no boot file`, `net: the boot
server does not answer ARP`, `net: TFTP timed out`, `net: the TFTP
server refused the file`, `net: the boot file does not fit below
0x40000`) and the boot halts as before. Everything runs during POST
with interrupts off; no network service is left behind.

**USB.** Before it looks for a boot disk, POST lists the USB devices on
every EHCI (class `0C:03:20`), UHCI (`0C:03:00`), OHCI (`0C:03:10`)
and xHCI (`0C:03:30`) controller on PCI bus 0. EHCI controllers go first. Each one keeps its
high-speed devices and hands every low- or full-speed device to its
companion UHCI or OHCI function, which is listed after it. It takes an
OHCI or EHCI controller back from SMM if SMM owns it. It then resets
the controller and each root port that has a device. It then gives every
device an address, starting at 1 and counting on across controllers,
and reads its device and configuration descriptors. A hub is
configured, its ports are powered and reset, and the devices behind it
are listed in turn, down to five hub tiers. Each device gets one line:

```
usb: UHCI at 00:05.0
usb: device 1 on root port 1: 0409:55AA, hub, 8 ports
usb: device 2 on hub 1 port 3: 0627:0001, HID
```

The class is the device's own class, or its first interface's class
when the device leaves it to the interfaces. It is printed as `HID`,
`mass storage`, `hub`, or `class` with its number in hex.

Apart from one keyboard, one mouse and one disk, nothing USB runs after
POST. Each controller
is halted and reset once its devices are listed, so no device is
configured at hand-off and no controller does DMA.

The exception is the first boot-protocol keyboard and the first
boot-protocol mouse found on one UHCI, OHCI or xHCI controller (`usb:
keyboard is device N`, `usb: mouse is device N`); a device on an EHCI
port is on its companion, and a mouse on another controller than the
keyboard's (or a keyboard on another than the mouse's, whichever was
found first) is not kept. Each is configured and set to the boot
protocol, and the controller keeps running with a schedule in the EBDA
(`0x97000`–`0x980FF`) that holds only their interrupt transfers (OHCI:
only the periodic list runs). An xHCI keeps its whole block in the EBDA
(`0x97000`–`0x9FFFF`), with each endpoint on a ring of 15 transfers
that post no events; an xHCI whose block had to go to `0x40000` keeps
neither. If an xHCI endpoint stops on a transfer error after POST, its
device stays silent: restarting it takes controller commands, which
IRQ0 does not send. IRQ0 checks the transfers on every tick. A key
pressed on the keyboard goes into the BDA keyboard buffer like a key
from the 8042, with the same ASCII table, plus Shift and Ctrl on
letters, so `INT 16h` reads it. A mouse report goes to the handler set
with `INT 15h` `AX = 0xC207` while the mouse is enabled (`AX = 0xC200`),
called as the PS/2 BIOS calls it: a far call with four words pushed,
status (buttons in bits 0–2, bit 3 set, the X and Y sign bits in 4 and
5), X, Y (up is positive; a USB report of 128 up is passed as 127) and 0,
which the BIOS pops. The handler runs
from IRQ0 with interrupts off. An OS that drives the controller itself
resets it, and the BIOS then gets nothing from it. An OS that replaces
the INT 08h vector stops the polling, and one that reads the 8042
itself (as Squirrel-OS/2 does) sees neither device. An EHCI controller
is halted but not reset, so `CONFIGFLAG` stays set and the ports it
handed over stay with the companions. Low- and full-speed devices
behind a high-speed hub would need split transactions, which are not
supported.

On xHCI the controller assigns the bus addresses, so the address
numbers in the listing are the BIOS's own. The controller has eight
device slots; a device that is not a hub gives its slot back once it is
listed, so only hubs keep one. What the controller reads and writes is
in the EBDA's USB pages (`0x97000`–`0x9FFFF`), with room for three
scratchpad pages. When a keyboard on another controller already runs
from there, or the controller wants up to ten scratchpad pages, it works
from `0x40000`–`0x4FFFF` instead, which is free only during POST; one
that wants more is skipped. Hubs are enumerated through their route strings, but their
slot contexts are not marked as hubs. Low- and full-speed devices behind
a high-speed hub on xHCI get the hub's transaction translator, but this
is untested, as QEMU has no high-speed hub. The work memory is `0x30000`–`0x3FFFF`, and the
listing runs on a stack just below `0x7C00`. OHCI, EHCI and xHCI registers
are reached through unreal mode, as for AHCI. Both are free, since
nothing has been loaded yet. The reset timings come from PIT
channel 2, which is left gated off.

**USB disk.** The first USB mass storage device with a bulk-only SCSI
interface (class 08h, subclass 06h, protocol 50h) on an xHCI whose
block is in the EBDA is kept (`usb: disk is device N`), on the same
controller as a kept keyboard or mouse if there is one: its two bulk
endpoints get rings in the block, it is configured, and the controller
keeps running. A disk on a UHCI, OHCI or EHCI controller, or on an
xHCI working from `0x40000`, is only listed, and a USB Attached SCSI
interface is not used yet. With no IDE, SATA or NVMe disk, POST sends it
INQUIRY, then TEST UNIT READY (with REQUEST SENSE after each "not
ready", for up to 2 s), then READ CAPACITY(10); it must have 512-byte
blocks. It prints `disk: USB disk (bulk-only)`, and the disk is then
`DL = 0x80`, booting and served by `INT 13h` as above with READ(10) and
WRITE(10), up to 128 sectors a call, straight to and from the caller's
buffer. `AH = 0x48` names it as host bus `PCI` with the controller's
bus, device and function, and interface `USB` with the device's address
as the device path. Each command waits for its transfer events with
interrupts off, on a 512-byte stack of its own in the EBDA's first page
(`0x92D00`–`0x92EFF`); the controller state it needs is copied to
`0x92F80`–`0x92FFF` when the bus has been listed. A command that stalls
an endpoint fails, and the endpoint is not recovered, so the disk then
stays failed. As for NVMe, the `INT 13h` calls are served from real
mode only.

**CD-ROM (El Torito).** The firmware looks for ATAPI devices on the
primary and secondary IDE channels, master before slave (QEMU's
`-cdrom` is the secondary master). For each one it reads block 17 (2048
bytes) for the Boot Record Volume Descriptor (`CD001`, `EL TORITO
SPECIFICATION`), then the boot catalog it names: a validation entry
(header `01h`, key `55h AAh`, words summing to 0) and a default entry
that must be bootable (`88h`) with media type 0, no emulation. The
image's sector count (512-byte units) is loaded at its load segment
(`0` means `0x07C0`), in whole 2048-byte blocks, which must lie between
`0x7C00` and `0xA0000`. The firmware prints `booting from CD-ROM (El
Torito, no emulation)` and enters it at load segment:0 with
`DL = 0xE0`. A unit that cannot boot gets one `cd:` line with the reason
(no disc or a read error, no boot record, a bad catalog, not bootable,
an emulation image, or a load out of range), and the hard disk failure
is then reported as before.

| `INT 13h`, `DL = 0xE0` (after a CD boot) | Result |
|------|--------|
| `AH = 0x00` | CF clear, `AH = 0`. |
| `AH = 0x41` | With `BX = 0x55AA`: CF clear, `BX = 0xAA55`, `AH = 0x30`, `CX = 0x0001`. |
| `AH = 0x42` | Read, packet as for a hard disk but in 2048-byte blocks: count 1–32, LBA upper dword 0, buffer below `0xA0000`. CF set and `AH = 0x04` if the drive fails (each command is tried 3 times). |
| `AH = 0x48` | Buffer of `0x1A` or more: total blocks (READ CAPACITY), 2048 bytes per block, no CHS (flags 0); with `0x1E`, DPTE `FFFF:FFFF`. |
| `AH = 0x4B`, `AL = 0x00` / `0x01` | The 19-byte specification packet to `DS:SI`: size `0x13`, media 0 (no emulation), drive `0xE0`, controller 0 primary / 1 secondary, image block, device 0 master / 1 slave, load segment, sector count. Terminating changes nothing, since nothing is emulated. |

Any other `AH` on `DL = 0xE0` returns CF set and `AH = 0x01`. Before a
CD boot `DL = 0xE0` is not a drive. The firmware keeps the CD's port,
unit, image block, load segment and sector count at `0x04F0`–`0x04FA`.

**Diskettes.** Drives come from CMOS register `0x10` (high nibble A:,
low nibble B:); the BDA equipment word gets bit 0 and bits 6–7 to match.
Before any hard disk, the firmware reads cylinder 0, head 0, sector 1 of
A: to `0x7C00`. With the `0xAA55` signature it prints
`booting from floppy A:` and hands off with `DL = 0x00`; without it, it
prints `floppy: boot sector has no 0xAA55 signature, trying the hard
disk` and goes on to the ATA boot. An empty drive is skipped quietly.
Media is 1.44 MB (80 cylinders, 2 heads, 18 sectors). Transfers use
ISA DMA channel 2 with IRQ 6 left masked. `INT 1Eh` points at an
11-byte diskette parameter table in the ROM.

| Call (`DL = 0x00` or `0x01`) | Result |
|------|--------|
| `AH = 0x00` | Reset the controller. CF clear, `AH = 0`. |
| `AH = 0x02` / `0x03` | Read / write `AL` sectors at `CH`/`DH`/`CL` (sector 1–18) to / from `ES:BX`, continuing onto the next head and cylinder. `AL` = sectors done. CF clear and `AH = 0` on success; otherwise CF set and `AH` = `0x01` (bad call), `0x03` (write-protected), `0x04` (sector not found), `0x09` (a sector's buffer crosses a 64 KB physical boundary), `0x10` (CRC), `0x20` (controller), `0x40` (seek) or `0x80` (timeout, e.g. no media). Each sector is tried twice, with a reset in between. |
| `AH = 0x08` | `BL` = CMOS drive type, `CH = 79`, `CL = 18`, `DH = 1`, `DL` = number of diskette drives, `ES:DI` = the parameter table. CF clear, `AX = 0`. A drive CMOS does not report: CF set, `AH = 0x07`. |
| `AH = 0x15` | `AH = 1` (diskette, no change line) for a drive CMOS reports, `0` otherwise. CF clear. |

The last diskette status is kept at `0x0441`. Other `AH`, or a drive CMOS
does not report, returns CF set and `AH = 0x01`.

**PCI.** At POST the firmware walks bus 0 through configuration
mechanism #1 (`0xCF8`/`0xCFC`); bridges are not followed yet. Every
memory BAR gets an address from `0xE0000000` up and every I/O BAR a port
from `0xC000` up, each aligned to its size; 64-bit BARs go below 4 GB.
Memory and I/O decoding are turned on for the functions that got one;
expansion ROM BARs stay off. On i440fx the PIIX3 (00:01.0) routes PIRQA–D
to IRQ 10, 10, 11, 11, level-triggered in the ELCR, and each function's
Interrupt Line holds the IRQ its pin reaches (device `d`, INTx: PIRQ
`(d − 1 + x − 1) mod 4`). Without a PIIX3 (q35) Interrupt Line is
`0xFF`. A `$PIR` table for the PIIX3 sits on a 16-byte boundary in
`0xF0000`–`0xFFFFF`.

| PCI BIOS call (`INT 1Ah`) | Result |
|------|--------|
| `AX = 0xB101` | Installation check: `AH = 0`, `AL = 0x01` (mechanism #1), `BX = 0x0210` (2.10), `CL = 0` (last bus), `EDX = 'PCI '`. CF clear. |
| `AX = 0xB102` | Find device `CX` (device ID) of vendor `DX`, the `SI`'th match from 0: `BH` = bus, `BL` = device << 3 \| function. |
| `AX = 0xB103` | Find class code `ECX` bits 23–0, the `SI`'th match: `BH`, `BL` as above. |
| `AX = 0xB108` / `0xB109` / `0xB10A` | Read the configuration byte / word / dword at register `DI` of `BH`/`BL` into `CL` / `CX` / `ECX`. |
| `AX = 0xB10B` / `0xB10C` / `0xB10D` | Write `CL` / `CX` / `ECX` there. |

CF clear and `AH = 0` on success. Otherwise CF set and `AH` = `0x81`
(function not supported, including `0xB106`, `0xB10E`, `0xB10F`),
`0x83` (vendor `0xFFFF`), `0x86` (device not found) or `0x87` (register
above `0xFF`, or not aligned to the access size). There is no BIOS32
(`_32_`) protected-mode entry.

At POST the firmware selects each unit and sends `IDENTIFY DEVICE`, so
presence is judged from the status that command produces, not from
whatever an earlier transfer or the other unit left behind. A unit is
present when it returns its 512 identify bytes; an ATAPI device aborts
the command and leaves `14h EBh`, and is reported as such. The result is
kept at `0x04FB` (bit 0 master, bit 1 slave) and the BDA hard-disk count
at `0x0475` is set from it. `AH = 0x08` on a unit that did not answer
returns CF set and `AH = 0x07`.

Before the jump the firmware fills the real-mode IVT. The vectors for
`INT 08h` (IRQ0), `INT 09h` (IRQ1), `INT 10h`, `INT 13h`, `INT 15h`,
`INT 16h` and `INT 1Ah` point at those services in the ROM, and `INT 1Eh`
at the diskette parameter table (`0xF000`,
where this 64 KB image is mapped). Every other vector is an `iret` that
does not change flags, so a stray IRQ cannot flip CF. `IF` stays clear;
IRQ0 and IRQ1 are unmasked on the PIC so a later `STI` receives timer
and keyboard IRQs. The i8042 command byte has XLAT set so the guest sees
set-1 scancodes.

`INT 15h` on every path:

| Call | Result |
|------|--------|
| `INT 15h`, `EAX = 0xE820`, `EDX = 'SMAP'`, `ECX >= 20`, `EBX` an entry index | CF clear, `EAX = 'SMAP'`, `ECX = 20`, that SQMM entry copied to `ES:DI`. `EBX` is the next index, or 0 after the last entry |
| `INT 15h`, `AX = 0x2400` | Disable A20 (port `0x92` bit 1 clear). CF clear, `AH = 0`. |
| `INT 15h`, `AX = 0x2401` | Enable A20 (same path as hand-off). CF clear, `AH = 0`. |
| `INT 15h`, `AX = 0x2402` | Query A20: `AL = 1` if port `0x92` bit 1 set, else `0`. CF clear, `AH = 0`. |
| `INT 15h`, `AX = 0x2403` | A20 support: `BX` bit 1 set (port `0x92`). CF clear, `AH = 0`. |
| `INT 15h`, `AH = 0xC2` | The pointing device interface, on a USB mouse kept after POST. `AL = 0x00` enable (`BH = 1`, a handler must be set) or disable (`BH = 0`); `0x01` reset: disabled, rate 100, resolution 2, scaling 1:1, `BX = 0x00AA`; `0x02` sample rate (`BH = 0`–`6`: 10, 20, 40, 60, 80, 100, 200); `0x03` resolution (`BH = 0`–`3`); `0x04` device type, `BH = 0`; `0x05` initialize, `BH` = packet size 1–8, then as reset; `0x06` with `BH = 0` status (`BL` bit 5 enabled, bit 4 scaling 2:1, bit 2 left button, bit 0 right button; `CL` resolution, `DL` sample rate), `BH = 1` / `2` scaling 1:1 / 2:1; `0x07` handler `ES:BX` (`0:0` removes it). Rate, resolution and scaling are kept and reported but do not change the reports. CF clear and `AH = 0` on success; CF set and `AH = 1` (bad function), `2` (bad input), `3` (no USB mouse), `5` (enable without a handler) |
| Any other `INT 15h` | CF set. No register changes |

That `E820` data is the memory map already built at `0x0500`, one entry
per call, in the same order. The kernel asks for the map that way (24
bytes; the firmware writes the 20-byte entry and leaves the caller's
extra dword). A CPU that fails the kernel's own check prints its reason
with `INT 10h` `AH = 0x0E`.

### Devices

| Device | State |
|--------|-------|
| COM1 (`0x3F8`) | 115200 8N1, FIFOs on, UART interrupts off |
| CMOS / RTC (`0x70`/`0x71`) | Index `0x0D` (status register D), NMI enabled (bit 7 of `0x70` clear). The firmware masks NMI only while it reads a register. |
| 8259 PICs (`0x20`, `0xA0`) | IBM PC AT layout: master IRQs at `INT 08h`–`0Fh`, slave at `INT 70h`–`77h`, slave on IRQ2. Master mask `0xFC` (IRQ0 and IRQ1 open); slave mask `0xFF`. |
| 8253/8254 PIT | Channel 0 mode 3, divisor 65536 (~18.2 Hz). IRQ0 increments `0x046C`. |
| i8042 keyboard (`0x60`/`0x64`) | Command byte `0x65` (IRQ1 + SYS + XLAT). IRQ1 enqueues set-1 make codes into the BDA ring buffer. |
| Local APIC | Virtual-wire mode when CPUID reports an APIC: spurious vector enabled, LVT0 = ExtINT, LVT1 = NMI, so the 8259 delivers IRQs on QEMU's default CPU. |
| Primary ATA channel | Idle. `nIEN` is set if a retry or an `INT 13h` reset touched the channel. |
| VGA | 80x25 colour text at `0xB8000` (the standard 720x400 text timings, 9x16 cells), CRTC at `0x3D4`. The font in plane 2 is the firmware's own 8x16 font (printable ASCII; other codes are blank). The DAC holds the 16 usual text colours. The firmware's messages are on screen in light grey on black, red for errors. The teletype cursor starts at the top left and the CRTC cursor follows it. |

### ACPI

Minimal ACPI 1.0 tables live in the BIOS ROM (mapped at `0xF0000`):

| Physical | Table |
|----------|-------|
| `0xFE000` | RSDP (`"RSD PTR "`, revision 0, 20 bytes). OEMID `SQRRL `. |
| `0xFE020` | RSDT (one entry). |
| `0xFE050` | FADT (`FACP`, length 116). `DSDT` = `0xFE0D0`; no FACS; PM blocks are zero. |
| `0xFE0D0` | DSDT (36 bytes): a header with no AML objects. |

The RSDP checksum (sum of the 20 bytes is 0) and each SDT checksum are
valid. Linux loads its ACPI namespace from the DSDT the FADT names and
faults when there is none, so the DSDT is always present, even empty.
With the PM blocks still zero, Linux reports that it cannot start the
ACPI interpreter and carries on without it. Find the RSDP by scanning `0xE0000`–`0xFFFFF` on a 16-byte stride
(the usual BIOS search). No UEFI. No XSDT.

### SMBIOS

A SMBIOS 2.4 entry point sits in the BIOS ROM:

| Physical | Content |
|----------|---------|
| `0xFD000` | Entry point (`_SM_` / `_DMI_`), length `0x1F`. |
| `0xFD020` | Structure table: Type 0 (BIOS), Type 1 (System), Type 127 (end). |

Both EPS checksums are valid. Find the entry point by scanning
`0xF0000`–`0xFFFFF` on a 16-byte stride for `_SM_`.

### Linux bzImage

When sector 0 has `0xAA55`, no `SQBL` header, and offset `0x202` is
`HdrS`, the firmware treats the disk as a bzImage:

1. `setup_sects` at `0x1F1` (0 means 4) and `syssize` at `0x1F4`
   (16-byte units) size the protected-mode image. Its size is limited
   only by the usable memory-map entry at `0x100000`; an image that does
   not fit is refused before anything is read.
2. That image is read from LBA `setup_sects+1` to linear `0x100000` in
   64-sector pieces through a bounce buffer at `0x8000` (ATA PIO stays
   below 1 MB), so kernels of any size that fits load whole.
3. boot_params (the "zero page") at `0x90000` is zeroed, then gets the
   image's setup header (`0x1F1` up to `0x202` + the byte at `0x201`) and:

   | Offset | Field | Value |
   |---|---|---|
   | `0x1E0` | `alt_mem_k` | KB of RAM from 1 MB (the memory-map entry there) |
   | `0x1E8` | `e820_entries` | number of SQMM entries (at most 128) |
   | `0x210` | `type_of_loader` | `0xFF` (undefined loader, per the boot protocol) |
   | `0x214` | `code32_start` | from the image, or `0x100000` if it is zero |
   | `0x218`, `0x21C` | `ramdisk_image`, `ramdisk_size` | 0 (no initrd) |
   | `0x228` | `cmd_line_ptr` | `0x91000`: `console=ttyS0,115200` |
   | `0x2D0` | `e820_table` | the SQMM entries, which already use the E820 layout |

4. The CPU enters 32-bit protected mode with a flat GDT that has
   `__BOOT_CS` at `0x10` and `__BOOT_DS` at `0x18`: `CS = 0x10`,
   `DS = ES = FS = GS = SS = 0x18`, `ESI = 0x90000`, `EBX = EBP = EDI =
   0`, `ESP = 0x90000`, interrupts off, `DF` clear. Control jumps to
   `code32_start`.

`SQBL` and `S2OK` are checked before `HdrS`. No initrd yet. Linux's
own 32-bit boot protocol (`Documentation/arch/x86/boot.rst`) is the
specification followed here.

### Option ROMs

Before the hand-off the firmware scans `0xD0000`–`0xEFFFF` on 2 KB
strides (VGA at `0xC0000` is left alone). A ROM with bytes `55 AA`, a
non-zero size byte (512-byte units, at most 64 KB), and a checksum of 0
over that span is initialised with a far call to offset 3. Bad
signatures or checksums are skipped. The size byte advances the scan.

### BIOS data area

Filled after the memory map and VGA init, before the banner. Absolute
addresses (segment 0):

| Address | Size | Value |
|---------|------|-------|
| `0x0410` | word | Equipment list `0x0220`: 80x25 colour (bits 4–5 = `10`), one serial port (bits 9–11 = 1). No floppies or parallel ports. |
| `0x040E` | word | Segment of the extended BIOS data area: `0x9200`. The 56 KB EBDA at `0x92000`–`0x9FFFF` is the firmware's; its first byte is its size in KB (`56`). Its first 4 KB hold firmware data; the four 4 KB pages after it are kept for disk controller queues (NVMe or virtio), and the last nine for the USB controller that keeps a keyboard, mouse or disk running after POST. The AHCI driver keeps its state and DMA structures there. |
| `0x0413` | word | Conventional memory size in KB: `584` (`0x00000`–`0x91FFF`, matching the SQMM usable entry). |
| `0x0450` | 2 bytes | Page-0 cursor: column then row. Starts at `(0, 0)`; `INT 10h` `AH=0x0E` keeps both inside 80x25. |
| `0x046C` | dword | Timer ticks since midnight. IRQ0 (`INT 08h`) increments it; wraps at `0x1800B0` (~24 h at 18.2 Hz) and sets `0x0470` to 1. Starts at 0. |
| `0x0470` | byte | 24-hour rollover flag for the tick count. Starts at 0. |
| `0x041A` | word | Keyboard buffer head (offset; starts at `0x041E`). |
| `0x041C` | word | Keyboard buffer tail (offset; starts at `0x041E`). |
| `0x041E`–`0x043D` | 16 words | Keyboard ring: low byte ASCII, high byte set-1 make scancode. |

Other BDA bytes are not filled by this firmware yet.

### A20

Before the hand-off jump the firmware enables the A20 gate through the
fast gate at port `0x92` (set bit 1, leave the reset bit clear). QEMU
honours that port. The line stays enabled across the jump. `INT 15h`
`AX = 0x2400`/`0x2401`/`0x2402`/`0x2403` control and query that same gate.

Do not assume: BIOS interrupt services other than those listed above, or
VGA graphics modes. Legacy option ROMs in `0xD0000`–`0xEFFFF` are scanned
and initialised; the VGA ROM at `0xC0000` is not.

### Memory map

The firmware builds the table below before jumping, from the RAM sizes
QEMU stores in CMOS (`0x30/0x31`, `0x34/0x35`, `0x5B`–`0x5D`).
`INT 15h` `E820` returns these same entries.

| Offset | Size | Field |
|--------|------|-------|
| `0x0500` | 4 | Magic `'SQMM'` (bytes `53 51 4D 4D`) |
| `0x0504` | 2 | Entry count |
| `0x0506` | 2 | Entry size in bytes (20) |
| `0x0508` | 20 × count | Entries |

Each entry uses the E820 layout: 64-bit base, 64-bit length, 32-bit type
(`1` usable, `2` reserved). Entries are sorted by base and do not overlap.

| Range | Type | Present |
|-------|------|---------|
| `0x00000000`–`0x00091FFF` | usable | always |
| `0x00092000`–`0x0009FFFF` | reserved (EBDA) | always |
| `0x000A0000`–`0x000FFFFF` | reserved (VGA window, ROMs) | always |
| `0x00100000`–top of low RAM | usable | when RAM > 1 MB |
| `0xFEC00000`–`0xFFFFFFFF` | reserved (IOAPIC, LAPIC, firmware) | always |
| `0x100000000`–… | usable | when RAM is mapped above 4 GB |

The table lives in usable memory. Copy it, or check the magic first, before
reusing `0x0500`–`0x05FF`. Memory not covered by an entry (for example the
PCI hole below 4 GB) is not RAM. Do not assume it is free.

### Failures

The jump only happens after a clean read: the drive reported no `ERR`/`DF`,
delivered exactly the sectors asked for and went idle, and sector 1 ends in the
boot signature (bytes `55 AA` at offset `0x1FE`). Timeouts and read errors are
retried up to 3 times, with an ATA soft reset (`SRST`) between attempts.
On failure the firmware prints the reason and the ATA status and error
registers on COM1 (and the VGA text buffer), then halts:

| Message | Meaning |
|---------|---------|
| `no drive on primary master` | Floating bus, or nothing answered as master (before a slave retry). Not retried on that unit. |
| `no drive on primary slave` | Nothing answered as slave after the master was absent (also the final line when both units are empty). Not retried. |
| `primary master is ATAPI (CD/DVD), not a hard disk` | ATAPI signature found on the master. Not retried; the slave is not tried. The CD boot is tried first (see El Torito). |
| `primary slave is ATAPI (CD/DVD), not a hard disk` | ATAPI signature found on the slave. Not retried. The CD boot is tried first. |
| `timeout waiting for drive` | `BSY` never cleared or the expected bit never came up |
| `read of LBA 0 failed` | Drive set `ERR`/`DF`, or the transfer ended in a bad state |
| `boot sector has no 0xAA55 signature` | The read was clean but the last word of the sector is not `0xAA55`. Not retried, and no status or error registers are printed. |
| `load overlaps firmware data` | The SQBL load would touch the memory map page or the boot sector. Nothing is read. No registers printed. |
| `load address is out of range` | The SQBL count is not 1–128, the load address is not below the EBDA (`0x92000`), or the load runs past it. Nothing is read. No registers printed. |
| `read of the extra sectors failed` | As `read of LBA 0 failed`, for the SQBL sectors. Retried like the first read. |
| `read of the kernel image failed` | As `read of LBA 0 failed`, for the `S2OK` sectors after the signature probe. Retried like the first read. |
| `bzImage header is invalid` | `syssize` is 0. No registers printed. |
| `bzImage does not fit in RAM` | The protected-mode kernel would not fit in the usable RAM at 1 MB. Nothing is read. No registers printed. |
| `read of the bzImage failed` | As `read of LBA 0 failed`, for the bzImage. Retried like the first read. |
| `kernel image has no S2OK signature` | The signature was present in the probe and absent after the load. Not retried, and no status or error registers are printed. |

### How to test

`make test` runs `x86-boot`, `x86-no-drive`, `x86-mem-low`,
`x86-mem-high`, `x86-atapi`, `x86-bad-sector`, `x86-extra`,
`x86-extra-map`, `x86-extra-boot`, `x86-extra-range`, `x86-osload`,
`x86-int13`, `x86-bda`, `x86-a20`, `x86-pit`, `x86-kbd`, `x86-time`,
`x86-vid`, `x86-write`, `x86-int15a20`, `x86-slave`, `x86-acpi`,
`x86-smbios`, `x86-bzimage`, `x86-bzimage-big`, `x86-bzimage-toobig`,
`x86-slave-absent`, `x86-params-slave` and `x86-optrom`. In `x86-boot` the boot sector checks
`DL`, that `IF` and `DF` are clear, and that the memory map has the
magic, 20-byte entries, ends by `0x0600`, contains the conventional,
EBDA and the other two reserved regions above exactly, requires an entry at `0x00100000`
to be usable when one is present (its length is not checked), and is
sorted without overlaps. Its
SQBL header loads a second sector, which reads the VGA registers and
prints `vga=ok` when they describe 80x25 text at `0xB8000` (colour I/O,
text mode in the graphics and attribute controllers, 80 characters,
400 lines of 16-line characters). It then lists every entry on COM1 as `mm <base> <length> <type>`, which
`x86-mem-low` uses to check the extended-memory entry for `-m 8` and
`x86-mem-high` to check the entry at 4 GB for `-m 5120`. `x86-one-sector`
is the same image's first sector alone, without the SQBL header, for the
one-sector hand-off. By hand:

```bash
make x86
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio
```

Expect `SquirrelBIOS x86` on the terminal, then whatever `boot.bin`
prints. `tests/x86-boot.asm` is a minimal boot sector that reports the
hand-off state on COM1. `tests/x86-extra.asm` has an SQBL header that
loads three sectors to `0x1000:0x0010`; its boot sector checks `DL` and
the memory map pointer and jumps to them, and they print `payload=ok`
when the last of them arrived too. Built with `-DOVERLAP_MAP`,
`-DOVERLAP_BOOT` or `-DOUT_OF_RANGE`, its header asks for a load the
firmware must refuse. `x86-osload` is a disk with no SQBL header and
192 sectors signed `S2OK`. The firmware enters `0x8000`, the image
checks `DL`, the flags, the map pointer and the signature, walks
`INT 15h` `E820` against the SQMM entries, and checks that an
`INT 13h` `AH = 0x42` whose packet is not 16 bytes long returns with
CF set (`disk=absent`). It must not print `entered-boot-sector`.
`x86-int13` is a disk with no SQBL header and no `S2OK` signature, so
the firmware enters `0x7C00`. That sector performs the same three
64-sector `AH = 0x42` reads the Squirrel-OS/2 boot sector does, then
`AH = 0x08`, one `AH = 0x02` sector, and `AH = 0x00`, and checks that
`INT 10h` `AH = 0x0E` wrote the cells it asked for. It prints
`via-boot-sector` and `int13=ok tty=ok`.
`x86-bda` checks the equipment word, the conventional KB, and that the
cursor at `0x0450` is still inside 80x25 after the banner, and prints
`bda=ok`.
`x86-a20` writes at `0x500000`, checks that `0x100500` and `0x000500`
are independent (A20 open), and prints `a20=ok`.
`x86-pit` finds `IF` clear, `STI`s until `0x046C` advances, `CLI`s, and
prints `pit=ok` only when `IF` is clear again.
`x86-kbd` (via `tests/x86-kbd-run.sh`) injects `a` through QMP after
`OS-ENTRY`, checks `INT 16h` `AH=0x01`/`0x00` for ASCII `a` and scancode
`0x1E`, refuses an unknown `AH` with CF set, and prints `kbd=ok`.
`x86-time` waits for a non-zero `INT 1Ah` `AH=0x00` tick count, checks
`AH=0x02` BCD time fields, and prints `time=ok`.
`x86-vid` exercises `INT 10h` set/get cursor, get mode, teletype, and
scroll, and prints `vid=ok`.
`x86-write` writes LBA 1 with `AH=0x43` and LBA 2 with `AH=0x03`, reads
them back, and prints `write=ok`.
`x86-int15a20` toggles A20 with `AX=2400`/`2401`/`2402`, checks
`AX=2403`, confirms E820, and prints `int15a20=ok`.

A Squirrel-OS/2 hard-disk image is not part of `make test`. Build
`os.img` in that repo and run `tests/os-boot.sh path/to/os.img` (or
`make os-boot OS_IMG=path/to/os.img`). It passes when the VGA text
buffer shows the `Squirrel-OS/2` banner and a line that is the `>`
prompt. By hand, the same image with a display:

```bash
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=os.img,if=ide -serial stdio
```

The firmware line is on COM1. The banner and the prompt are on the VGA.

## AArch64 (QEMU virt)

The firmware loads the image QEMU was given with `-kernel`, read through
fw_cfg DMA, and jumps to its first byte. The register state follows the
Linux arm64 boot protocol, so a raw Linux `Image` can be booted the same way.

### Registers

| Item | Value |
|------|-------|
| `PC` | First byte of the image, at its load address (below): `0x40200000` (RAM base + 2 MB) unless an Image header fixes it elsewhere |
| Exception level | Whatever QEMU started the firmware at: EL1 by default, EL2 with `virtualization=on` |
| MMU, D-cache | Off, set by the firmware: `SCTLR_ELx.M` and `.C` are cleared at the EL above just before the jump. No page tables are built. |
| Endianness | Little-endian: `SCTLR_ELx.EE` (and `E0E` at EL1) cleared |
| I-cache | Invalidated (`ic iallu`, then `dsb ish; isb`) after the image was loaded. `SCTLR_ELx.I` is left as it was. |
| `DAIF` | All masked |
| `x0` | Device tree address (`0x40000000`) if a valid FDT is there, otherwise `0` |
| `x1`–`x3` | `0` |
| `SP` | Inside firmware scratch below `0x40200000` |

### Memory

| Range | Contents |
|-------|----------|
| `0x40000000`–`0x400FFFFF` | Device tree from QEMU |
| `0x40100000`–`0x401FFFFF` | Firmware stack and scratch, free after the jump |
| `0x40200000`–… | The image, `-kernel` size bytes (or at the address its `Image` header asks for) |
| next 4 KB boundary after the image's footprint | The `-initrd`, when QEMU was given one |

RAM size and layout come from the device tree (`/memory`).

### Initrd

When fw_cfg reports an initrd (key `0x0b`, its size, is non-zero), the
firmware reads it (key `0x12`) to the first 4 KB boundary after the
image's footprint (`image_size` when the `Image` header gives one,
otherwise the file size), so it overlaps neither the kernel, its BSS, nor
the device tree. It then adds `linux,initrd-start` and
`linux,initrd-end` (64-bit each, end exclusive) to `/chosen` and prints
`initrd at 0x<start>, end 0x<end>`. The two properties go first in
`/chosen`; the rest of the tree moves up by 40 bytes inside the blob's
`totalsize`, and their names are appended to the strings block. With no
initrd the device tree is left exactly as QEMU provided it.

### Load address

If the image starts with a Linux arm64 `Image` header (magic bytes
`41 52 4D 64` at offset `0x38`), the firmware follows it:

| `flags` bit 3 | Load and entry address |
|---------------|------------------------|
| set (placement anywhere; every Linux since 4.6) | `0x40200000` |
| clear (placement fixed) | the 2 MB-aligned RAM base plus `text_offset` (offset 8), i.e. `0x40000000 + text_offset` |

`image_size` (offset 16) is how much memory the image needs at that
address, BSS included, so the fit check below uses it when it is
non-zero. A non-zero `image_size` smaller than the file is a broken
header and is refused. Without the magic the image is loaded raw at
`0x40200000`, as before.

Before loading, the firmware checks that the image fits at its load
address: it must start at or above `0x40200000` (clear of the device tree
and the firmware scratch) and end, after `image_size` bytes when the
header gives one, at or before the end of RAM. A fixed-placement image
with a `text_offset` below 2 MB (such as `0x80000` from Linux before 4.6)
is therefore refused. The RAM size is fw_cfg key `0x03`
(8 bytes, little-endian) when QEMU fills it in. QEMU's `virt` machine
leaves that key at 0 (checked on QEMU 8.2), so the firmware then takes the
length of the device tree `/memory` range that starts at `0x40000000`.

### Devices

| Device | State |
|--------|-------|
| PL011 (`0x09000000`) | Enabled (`UARTEN`, `TXE`, `RXE`), baud rate as QEMU left it |
| GIC, timers | Not touched |

Do not assume: whether the I-cache is enabled, a configured GIC or timer, PSCI beyond what
the device tree advertises, or that the image was relocated or
decompressed. It is copied as raw bytes, so link it at its load
address. An uncompressed Linux `Image` boots as it is; a `vmlinuz` must
be decompressed first.

### Failures

The firmware prints one of these on the PL011, then `boot halted`, and halts:

| Message | Meaning |
|---------|---------|
| `fw_cfg with DMA not found` | Not on QEMU virt, or a QEMU too old for fw_cfg DMA |
| `no kernel image (pass -kernel to QEMU)` | fw_cfg reports a kernel size of 0 |
| `kernel does not fit in RAM` | The image would run past the end of RAM or start below `0x40200000`. Nothing is loaded. |
| `Image header image_size is smaller than the image` | The `Image` header's non-zero `image_size` is below the number of bytes fw_cfg holds |
| `initrd does not fit in RAM` | The initrd, placed after the image, would run past the end of RAM |
| `fw_cfg DMA read of the initrd failed` | The initrd transfer reported an error or did not complete |
| `device tree has no /chosen for the initrd` | There is an initrd but no device tree, or the tree has no `/chosen` |
| `device tree has no room for the initrd` | The blob's `totalsize` has no room for the two properties, or its strings block is not last |
| `RAM size not found (fw_cfg or device tree /memory)` | fw_cfg key `0x03` is 0 and there is no device tree, or it has no `/memory` range at `0x40000000` |
| `fw_cfg DMA read of the kernel failed` | The DMA transfer reported an error |
| `fw_cfg DMA read of the kernel did not complete` | The transfer neither finished nor reported an error within 1 s (by the generic counter, `CNTPCT_EL0` against `CNTFRQ_EL0`) |

### How to test

`make test` runs `arm-boot` (an image checks `x0`–`x3`, the device
tree, and that the `SCTLR` of its EL has `M`, `C` and `EE` clear with
`DAIF` fully masked), `arm-boot-el2` (the same image entered at EL2 with
`virtualization=on`), `arm-too-big` (a 31 MB image with `-m 32`, built
under `build/tests/` and never committed, is refused), `arm-image-fixed`
(an `Image` header with placement fixed and `text_offset = 0x400000` runs
at `0x40400000`), `arm-image-reloc` (bit 3 set, the same `text_offset`,
runs at `0x40200000`), `arm-image-short` (a header whose `image_size` is
below the file size is refused), `arm-initrd` (with `-initrd`, the
firmware reports where it put it and the image still finds a valid device
tree) and `arm-no-kernel`. A real check, by hand: decompress an arm64
`vmlinuz`, build a busybox initramfs, and boot both with
`-kernel Image -initrd initrd.gz -append console=ttyAMA0`; Ubuntu's 6.11
kernel unpacks it and runs its `/init`. By hand:

```bash
make arm
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin -kernel image.bin
```

Expect `SquirrelBIOS ARM`, then `kernel loaded at 0x40200000, jumping`
(or the address an `Image` header asked for),
then whatever the image prints. `tests/arm-kernel.S` is a minimal image.

## RISC-V (QEMU virt)

QEMU loads the `-kernel` image into RAM itself and tells the firmware
where through the `fw_dynamic_info` block it passes in `a2` (magic
`"OSBI"`, `next_addr`, `next_mode`). The firmware enters the image in
the mode QEMU asks for. With `-kernel` that is S-mode (`next_mode = 1`),
entered through `mret`.

### Registers

| Item | Value |
|------|-------|
| `pc` | `next_addr` from QEMU, `0x80200000` on rv64 (RAM base + 2 MB) |
| Privilege | S-mode when `next_mode` is `1` (what `-kernel` gives), M-mode when it is `3`, or when `fw_dynamic_info` is older than version 2 and has no `next_mode` |
| PMP | S-mode entry: entry 0 is NAPOT over the firmware region `0x80000000`–`0x801FFFFF` with R, W and X all clear, so any S-mode load, store or fetch there takes an access fault (`5`, `7` or `1`, delegated to S-mode). Entry 1 is NAPOT over the whole physical address space with R, W and X. Both are unlocked, so neither applies to M-mode. Other entries are untouched. M-mode entry: untouched. |
| `medeleg` | S-mode entry: misaligned fetch, load and store (`0`, `4`, `6`), the access faults (`1`, `5`, `7`), illegal instruction (`2`), breakpoint (`3`), `ecall` from U-mode (`8`) and the page faults (`12`, `13`, `15`). Not `ecall` from S-mode (`9`). M-mode entry: untouched. |
| `mideleg` | S-mode entry: supervisor software, timer and external interrupts (`SSIP`, `STIP`, `SEIP`). M-mode entry: untouched. |
| `satp` | S-mode entry: `0` (bare, translation off), written by the firmware on every entry, including a restart after `hart_stop`. M-mode entry: untouched (bare after reset). |
| `mtvec` | Firmware trap handler, direct mode (below) |
| `mscratch` | Firmware trap stack. Owned by the firmware. |
| `menvcfg` | S-mode entry, on every hart: the firmware sets `CBIE` = `01` (`cbo.inval` acts as a flush), `CBCFE` (`cbo.clean`, `cbo.flush`), `CBZE` (`cbo.zero`) and `STCE` (S-mode `stimecmp`). A bit for an extension the hart lacks stays `0`, and a hart without `menvcfg` is left as it is. Other bits are untouched. M-mode entry: untouched. |
| `mcounteren` | S-mode entry: `CY`, `TM` and `IR` set, so S-mode can read `cycle`, `time` and `instret`. M-mode entry: untouched. |
| `mstatus.MIE`, `mstatus.SIE` | `0` |
| `mie` | S-mode entry: only `MSIE`, the M-mode software interrupt through which other harts' IPIs and remote fences reach this one (it is not delegated, and S-mode cannot see it). M-mode entry: `0`. |
| `a0` | Hart ID (`0`) |
| `a1` | Device tree address, as QEMU passed it to the firmware |
| `a2` | `0` |
| `sp` | S-mode entry: `0`, so an image that uses it before setting its own stack faults. M-mode entry: inside the firmware image. |

### Memory

| Range | Contents |
|-------|----------|
| `0x80000000`–`0x801FFFFF` | Firmware image, stack and SBI. Stays in use after the jump, and S-mode cannot read, write or execute it (PMP above). The device tree marks it reserved (below). |
| `0x80200000`–… | The image |
| `a1` | Device tree, placed by QEMU near the top of RAM |

RAM size and layout come from the device tree (`/memory`).

Before an S-mode entry, the boot hart adds this to the device tree, at
the end of the root node, so an OS that reads `/reserved-memory` (Linux
does) leaves the firmware region alone and does not map it:

```dts
reserved-memory {
	#address-cells = <2>;
	#size-cells = <2>;
	ranges;

	squirrelbios@80000000 {
		reg = <0x0 0x80000000 0x0 0x200000>;
		no-map;
	};
};
```

The tree grows in place by 185 bytes, `totalsize` and the other header
sizes are updated, and the firmware prints `dt: firmware region
0x80000000-0x801fffff reserved (no-map)`. It leaves the tree unchanged,
and prints `dt: firmware region not reserved:` and the reason, when the
tree already has a `/reserved-memory` node (a tree passed with `-dtb`
must then reserve the region itself), is not laid out as QEMU writes it
(strings block last, right after the structure block), or has no room to
grow before the next 1 MB boundary. The boot goes on either way. An
M-mode entry gets the tree as QEMU wrote it.

### Devices and harts

| Item | State |
|------|-------|
| NS16550A (`0x10000000`) | 115200 8N1 (divisor 2 from `virt`'s 3.6864 MHz UART clock), FIFOs on, interrupts off |
| Other harts | Parked in `wfi` in M-mode, with `mstatus.MIE = 0` and only `mie.MSIE` set, until `sbi_hart_start` (below). The firmware tracks hart IDs 0 to 511 (QEMU `virt`'s limit, across all sockets), so harts in every socket can be started, each through its own socket's CLINT (below); higher IDs are never started. |
| CLINT | One per socket. With `-numa`, QEMU makes each NUMA node a socket whose CLINT is `0x10000` above the previous one, with slots counted from the socket's lowest hart ID; the boot hart reads each `cpu` node's `reg` and `numa-node-id` and uses the right CLINT for every hart (up to 8 sockets). A hart's `mtimecmp` is written only by `sbi_set_timer` and the firmware's timer interrupt. A parked hart's `msip` is written by `sbi_hart_start`. `mtime` runs at 10 MHz (the device tree's `timebase-frequency`). |
| PLIC | Not touched |
| APLIC (`aia=aplic`, `aia=aplic-imsic`) | Before an S-mode entry, every source the root (M-level) APLIC's `riscv,delegate` names is delegated to its S-level child (`sourcecfg` = D plus the child's index), in each socket. The root APLIC node, and any interrupt controller whose `interrupts-extended` names only machine external interrupts (the M-level IMSIC, or the root's per-hart contexts in direct mode), are overwritten with `FDT_NOP` in the device tree, so the OS sees only the S-level ones. The firmware prints `aia: APLIC sources delegated to S-mode, M-level interrupt controllers hidden`. MSI mode's `smsiaddrcfg` is not programmed; see `riscv-aia` in TODO.md. IPIs and timers still go through the CLINT. |
| Test device (`0x100000`) | Written only by `sbi_system_reset`. S-mode can reach it too (PMP entry 1 opens everything outside the firmware region), but an OS should use SRST. |

Do not assume: SBI extensions other than base, TIME, IPI, RFENCE, HSM and SRST,
a set `stvec` (install one before the first delegated trap), secondary
harts being available, or IPIs and remote fences reaching a hart that is
not started (they are skipped; a hart that starts again fences itself
first).

### Traps

An image entered in S-mode takes the delegated exceptions and interrupts
above at its own `stvec`. Everything else, `ecall` from S-mode included,
reaches M-mode.

`mtvec` points at a firmware handler. An `ecall` from S-mode (cause `9`)
is an SBI call and returns (below). The M-mode timer interrupt is how
`sbi_set_timer` fires: the firmware raises the supervisor timer interrupt
(`mip.STIP`) and returns to where S-mode was. Any other trap that reaches
M-mode is reported instead of jumping to address 0, and does not return:
the handler prints one line

```
ras: hart=<16 hex> mcause=<16 hex> mepc=<16 hex> mtval=<16 hex> fatal
```

(all hex lowercase, no `0x`), then parks every other started hart with a
CLINT software interrupt so those harts leave S-mode and sit in `wfi`,
then prints `boot halted` and shuts the machine down through the same
path as `sbi_system_reset` with type shutdown and reason system failure
(QEMU's test finisher, exit status 1). A short wait after the park lets
those harts leave S-mode before the finisher stops the machine. With the
delegation above, an S-mode image only gets there through a firmware bug
(or the smoke-only `ecall` with `a7 = 0x53515241` (`"SQRA"`), which is
not an SBI extension and is not reported by `probe_extension`: it loads
from address 0 in M-mode so `make test` can exercise this path). An image
entered in M-mode installs its own `mtvec` before it relies on traps (an
M-mode `ecall` is cause `000000000000000b`).

### SBI

An S-mode image calls the firmware with `ecall`, following the RISC-V SBI
calling convention: `a7` is the extension ID, `a6` the function ID, and
`a0`–`a5` the arguments. The firmware returns the error in `a0` and the
value in `a1`, preserves every other register (`sp` included), and
resumes at the instruction after the `ecall`. An unknown extension or
function returns `SBI_ERR_NOT_SUPPORTED` (`-2`).

| Item | Value |
|------|-------|
| Spec version | `0x02000000` (SBI 2.0; Linux probes DBCN only from 2.0) |
| Implementation ID | `0x5351` (`"SQ"`, SquirrelBIOS). Not registered with RISC-V International. |
| Implementation version | `1` |

| Extension | ID | Functions |
|---|---|---|
| Base | `0x10` | `get_spec_version` (0), `get_impl_id` (1), `get_impl_version` (2), `probe_extension` (3), `get_mvendorid` (4), `get_marchid` (5), `get_mimpid` (6) |
| TIME | `0x54494D45` | `set_timer` (0) |
| IPI | `0x735049` | `send_ipi` (0) |
| RFENCE | `0x52464E43` | `remote_fence_i` (0), `remote_sfence_vma` (1), `remote_sfence_vma_asid` (2) |
| HSM | `0x48534D` | `hart_start` (0), `hart_stop` (1), `hart_get_status` (2) |
| SRST | `0x53525354` | `system_reset` (0) |
| DBCN | `0x4442434E` | `console_write` (0), `console_read` (1), `console_write_byte` (2): on the UART; `a0` = bytes, `a1`/`a2` = physical address low/high (high must be 0), returns the bytes done in `a1` |

`set_timer(a0)`, on a hart with Sstc (`menvcfg.STCE` set, as on QEMU's
default CPU), writes the absolute time `a0` to `stimecmp`: the
supervisor timer interrupt is pending while `time >= stimecmp`, so it
clears when S-mode sets a later time, and S-mode may write `stimecmp`
itself instead of calling the SBI (Linux does). On a hart without Sstc,
it programs the calling hart's `mtimecmp` with `a0`, clears a pending
supervisor timer interrupt and enables the M-mode timer interrupt. When `mtime` reaches it, the firmware sets
`mip.STIP` and pushes `mtimecmp` to the maximum, so the supervisor timer
interrupt stays pending, once, until S-mode calls `set_timer` again. To
stop the timer, call `set_timer` with a time that is never reached
(`-1`).

`send_ipi(a0, a1)` takes the SBI hart mask: bit N of `a0` selects hart
`a1 + N`, and `a1 = -1` selects every started hart (the boot hart and
any `hart_start` started). Each hart selected gets a supervisor software
interrupt (`mip.SSIP`; S-mode clears `sip.SSIP` in its handler): the
caller at once, another hart through its M-mode software interrupt
(CLINT `msip`), which the firmware turns into `SSIP` on that hart. The
call returns once the interrupts are sent, not taken. A stopped hart
(parked, or after `hart_stop`) is a valid hart and is skipped: it gets
nothing, and the call still succeeds for the others. Linux names stopped
harts after CPU hotplug, because an offline CPU stays in an address
space's CPU mask. A mask that names a hart the machine does not have
(one that never reached its park loop, or an ID of 512 or more) returns
`SBI_ERR_INVALID_PARAM` (`-3`) and raises nothing, on that hart or any
other. An empty mask succeeds and does nothing.

The RFENCE functions take the same hart mask in `a0` and `a1`, with the
same rules. Each hart selected runs `fence.i` (`remote_fence_i`) or a
full `sfence.vma` (both `remote_sfence_vma` functions), which covers any
`start`, `size` and ASID: the caller at once, the others in their M-mode
software interrupt. The call returns when every hart selected has run
it after the call was made: a target takes all its requests at once and
numbers each such pass, and the caller waits until its request has been
taken and the pass that took it has finished, so a pass already under way
when the request arrived does not count. Two harts may fence each other at
the same time: while one waits, it also runs the fences asked of it. A
hart that stops drops what was asked of it; the call still returns. The hypervisor
fences (functions 3–6) return `SBI_ERR_NOT_SUPPORTED`.

`hart_start(a0 = hart, a1 = start address, a2 = opaque)` takes a hart
that is stopped (parked) and enters it in S-mode at the start address,
set up like the boot hart (same PMP entries, delegation, `mcounteren` and
`mtvec`, its own trap stack, `satp = 0`, `sp = 0`), with `a0` = its hart
ID and `a1` = the opaque value. `a1` is the device tree only if the caller passed the
device tree as the opaque value. The call returns once the hart has been
told; `hart_get_status` reports `START_PENDING` (`2`) until the hart has
left `wfi`, then `STARTED` (`0`). Starting a hart that is not stopped
returns `SBI_ERR_ALREADY_AVAILABLE` (`-6`).

`hart_stop()` stops the calling hart. It does not return: the hart goes
back to its park loop, its registers are not kept (a later start has
`satp = 0` again, whatever paging the hart had on), its pending supervisor
software and timer interrupts are cleared (with Sstc, by setting
`stimecmp` to the maximum), and `hart_get_status` reports
it `STOPPED` (`1`). `hart_start` can start it again, at any address.

`hart_get_status(a0 = hart)` returns `STARTED` (`0`), `STOPPED` (`1`) or
`START_PENDING` (`2`) in `a1`. The boot hart is `STARTED`.

Each secondary hart marks itself stopped when it reaches its park loop,
which on QEMU can happen after the OS is already running. For a hart
that has not done so yet, `hart_start` and `hart_get_status` wait up to
100 ms (by `mtime`) for it, then treat it as absent: a hart ID with no
hart behind it, or 512 and above, returns `SBI_ERR_INVALID_PARAM`. An
IPI or RFENCE mask that names such a hart waits the same way, once per
call, before it decides the hart does not exist.

`system_reset(a0 = type, a1 = reason)` takes type shutdown (`0`), cold
reboot (`1`) or warm reboot (`2`), and reason no reason (`0`) or system
failure (`1`). It goes through QEMU `virt`'s test device at `0x100000`:
a shutdown powers the machine off and QEMU exits with status 0, or 1 for
a system failure; both reboots reset the whole machine (all harts) and
the firmware boots again, so a warm reboot keeps no more state than a
cold one. With `-no-reboot`, QEMU exits with status 0 instead. On
success the call does not return. Any other type or reason, vendor ones
included, returns `SBI_ERR_INVALID_PARAM` (`-3`). If the machine is
still running 1 s (by `mtime`) after the request, the call returns
`SBI_ERR_FAILED` (`-1`).

`probe_extension` returns `a1 = 1` for the extensions in this table and
`0` for every other one.

### Failures

The firmware prints one of these on the UART, then `boot halted`, and halts:

| Message | Meaning |
|---------|---------|
| `boot: no kernel image (pass -kernel to QEMU)` | No `-kernel`, or QEMU passed no valid `fw_dynamic_info` |
| `boot: unsupported next_mode <16 hex digits>` | `next_mode` is not `1` (S) or `3` (M), for example U-mode (`0`) |

### How to test

`make test` runs `riscv-boot` (the firmware reports `next_mode=1`, and an
image checks `a0`, the device tree in `a1` and `a2 = 0`, with `-smp 2`;
its UART stores and device-tree load pass PMP in S-mode, and the log must
not show a trap), `riscv-ecall` (an image executes `ecall` and
gets `SBI_ERR_NOT_SUPPORTED` back for an extension that does not exist),
`riscv-deleg` (an image sets `stvec`, executes an illegal instruction and
prints `deleg=ok` from its S-mode handler, then its `ecall` still reaches
the SBI), `riscv-sbi` (an image calls the base functions, probes base,
TIME, IPI, RFENCE, HSM and SRST, and checks that only `a0`
and `a1` changed), `riscv-timer` (an image sets a timer 10 ms ahead and
prints `timer=ok` from its supervisor timer interrupt, which must not
come back after it pushed the timer out), `riscv-ipi` (with `-smp 2`, an
IPI to the parked hart 1 succeeds and raises nothing on the caller, one to
hart 2, which does not exist, fails, and an IPI to itself
prints `ipi=ok` from its software-interrupt handler), `riscv-ipi-smp`
(with `-smp 2`, hart 1 is started and takes IPIs by mask bit, by mask base and with base `-1`, which also reaches hart 0;
`remote_fence_i` and `remote_sfence_vma` on both harts return; then both
harts fence each other 20000 times at once without the firmware
deadlocking), `riscv-fence-smp3` (with `-smp 3`, harts 0 and 1 both
fence hart 2 20000 times at once, then hart 2 takes an IPI from each),
`riscv-boot-hart` (with `-smp 4` and a shim that hands the firmware
QEMU's `fw_dynamic_info` with `boot_hart = 2`: hart 2 enters the image,
HSM reports it started and hart 0 stopped, and it starts hart 0),
`riscv-aia` (on `aia=aplic`, an image that drives the S-level APLIC
gets the UART's transmit interrupt, source 10, through it: `aia=ok`),
`riscv-aia-ipi` (`riscv-ipi-smp` on `aia=aplic-imsic`),
`riscv-numa` (`riscv-harts` on `-smp 4` in two NUMA nodes, so harts 2
and 3 sit behind the second socket's CLINT: all start and take a fence,
`harts=ok`), `riscv-harts` (with `-smp 64`, hart 0 starts harts 1 to 63 one at a
time, `hart_get_status` reports each started, and one `remote_fence_i`
whose mask names all 64 harts succeeds: `harts=64`),
`riscv-rfence`
(with `-smp 2`, a fence whose mask also names the stopped hart 1
succeeds, one that names hart 2, which does not exist, fails, and
`remote_sfence_vma` and `remote_fence_i` for itself succeed),
`riscv-hsm` (with `-smp 2`, hart 1 is stopped, hart 7 does not exist,
`hart_start` brings hart 1 up and a second start is refused; hart 1
prints `hart1=ok a1=5157000012345678`, the opaque value, and is then
reported started), `riscv-hsm-stop` (with `-smp 2`, hart 1 is started,
turns on Sv39 paging, sets a timer that is already due, calls `hart_stop` and is reported stopped, then is
started again with `satp` and `sp` both 0 and no timer interrupt pending, and `hart_stop` never
returns), `riscv-envcfg` (an image runs `cbo.zero` on a cache block
and checks it is zero, runs `cbo.clean`, `cbo.flush` and `cbo.inval`
without a trap, and gets the supervisor timer interrupt from a direct
`stimecmp` write, which goes away when it pushes `stimecmp` out),
`riscv-dt` (with `-smp 2`, an image walks the device tree and
finds `/reserved-memory` with `#address-cells` and `#size-cells` 2,
`ranges`, and a child with `reg` over the firmware region and `no-map`;
the `/memory` node, the header sizes and the end token are still right),
`riscv-pmp` (an image stores to the first and last dword of
the firmware region, loads from it and jumps into it, and gets an access
fault at its own `stvec` each time, with `stval` the address; a store to
its own memory and an `ecall` still work), `riscv-srst` (an image probes SRST, gets bad types, reasons
and functions refused, prints `reset=ok` and shuts down; QEMU must exit
with status 0 and the call must not return), `riscv-srst-reboot` (the
same with a cold reboot under `-no-reboot`, status 0), `riscv-srst-fail`
(a shutdown for a system failure, status 1), `riscv-ras` (with `-smp 2`,
hart 0 starts hart 1, then `ecall`s `a7 = 0x53515241` so the SBI path
faults: the log has `hart1=up`, a `ras: hart=... mcause=0000000000000005
... mtval=0000000000000000 fatal` line, and `boot halted`, must not show
`hart1=still`, which hart 1 would print after 100 ms if it were still
running, and QEMU must exit with status 1 from the system-failure
shutdown) and `riscv-no-kernel`. By hand:

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

Expect `SquirrelBIOS RISC-V`, then `jumping to kernel at
0x0000000080200000, next_mode=1`, then whatever the image prints. For a
Linux kernel, see "Trying Linux" in `riscv/README.md`.
`tests/riscv-kernel.S` is a minimal image.
