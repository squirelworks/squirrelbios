# Hand-off contract

This file is the single source of truth for what SquirrelBIOS guarantees
when it passes control to the OS, and for what the OS must not assume.
If the firmware and this file disagree, the firmware has a bug.

## At a glance

| | x86 PC | AArch64 (QEMU virt) | RISC-V (QEMU virt) |
|---|---|---|---|
| Image source | LBA 0 of the primary master ATA disk | `-kernel`, read through fw_cfg DMA | `-kernel`, loaded by QEMU |
| Entry | `0x0000:0x7C00` | `0x40200000` | `0x80200000` (rv64) |
| CPU mode | 16-bit real mode | EL1 (EL2 with `virtualization=on`), MMU off | S-mode (QEMU's `next_mode`), `satp = 0` |
| Boot argument | `DL = 0x80` | `x0` = device tree | `a0` = hart ID, `a1` = device tree |
| Memory description | Memory map at `ES:DI = 0x0000:0x0500` | Device tree `/memory` | Device tree `/memory` |
| Console | COM1 `0x3F8`, 115200 8N1 | PL011 `0x09000000` | NS16550A `0x10000000`, 115200 8N1 |
| Interrupts | `IF = 0` | `DAIF` masked | `mstatus.MIE`/`SIE` = 0, `mie = 0` |
| CPUs running | Boot CPU only | Boot CPU only | Hart 0; others through `sbi_hart_start` |
| Firmware services | None | None | SBI from S-mode: base, TIME, IPI, RFENCE, HSM, SRST |

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
   RISC-V SBI listed below: no BIOS interrupts on x86 (`INT 10h`,
   `INT 13h`, `INT 15h` do not exist), and no UEFI. Anything else the OS
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

## x86 PC

### Registers

| Item | Value |
|------|-------|
| `CS:IP` | `0x0000:0x7C00` (far jump) |
| Mode | 16-bit real mode |
| `DS`, `ES`, `SS` | `0` |
| `SP` | `0x7C00` |
| `DL` | `0x80` (boot drive) |
| `DI` | `0x0500` (memory map header, so `ES:DI` points at it) |
| `EFLAGS` | `IF = 0`, `DF = 0` |

### Memory

| Range | Contents |
|-------|----------|
| `0x0500`–`0x05FF` | Memory map (below). Always at this address. The firmware's own stack sits at the top of this page while it runs, and is free after the jump. |
| `0x7C00`–`0x7DFF` | Sector 1 of the disk, the image that was jumped to |
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

The load must start below `0xA0000` and end at or before it, and must not
touch `0x0500`–`0x05FF` or `0x7C00`–`0x7DFF`. Anything else, including a
count of 0 or above 128, is refused before the read (see Failures).
Memory below `0x0500` is allowed: the firmware leaves no IVT or BIOS data
there.

### Devices

| Device | State |
|--------|-------|
| COM1 (`0x3F8`) | 115200 8N1, FIFOs on, UART interrupts off |
| CMOS / RTC (`0x70`/`0x71`) | Index `0x0D` (status register D), NMI enabled (bit 7 of `0x70` clear). The firmware masks NMI only while it reads a register. |
| Primary ATA channel | Idle. `nIEN` is set if a retry reset the channel. |
| VGA | 80x25 colour text at `0xB8000` (the standard 720x400 text timings, 9x16 cells), CRTC at `0x3D4`. The font in plane 2 is the firmware's own 8x16 font (printable ASCII; other codes are blank). The DAC holds the 16 usual text colours. The firmware's messages are on screen in light grey on black, red for errors; the cursor is at the top left. |

Do not assume: BIOS interrupt services, an initialised IVT, A20 state
(enabled on QEMU, but the firmware does not touch it), or VGA graphics
modes and a VGA BIOS (no option ROM is run).

### Memory map

There is no `INT 15h, E820` after hand-off, so the firmware builds the
same kind of table in memory before jumping. It is derived from the RAM
sizes QEMU stores in CMOS (`0x30/0x31`, `0x34/0x35`, `0x5B`–`0x5D`).

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
| `0x00000000`–`0x0009FFFF` | usable | always |
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
| `no drive on primary master` | Floating bus or nothing answered as master. Not retried. |
| `primary master is ATAPI (CD/DVD), not supported` | ATAPI signature found. Not retried. |
| `timeout waiting for drive` | `BSY` never cleared or the expected bit never came up |
| `read of LBA 0 failed` | Drive set `ERR`/`DF`, or the transfer ended in a bad state |
| `boot sector has no 0xAA55 signature` | The read was clean but the last word of the sector is not `0xAA55`. Not retried, and no status or error registers are printed. |
| `load overlaps firmware data` | The SQBL load would touch the memory map page or the boot sector. Nothing is read. No registers printed. |
| `load address is out of range` | The SQBL count is not 1–128, the load address is not below `0xA0000`, or the load runs past it. Nothing is read. No registers printed. |
| `read of the extra sectors failed` | As `read of LBA 0 failed`, for the SQBL sectors. Retried like the first read. |

### How to test

`make test` runs `x86-boot`, `x86-no-drive`, `x86-mem-low`,
`x86-mem-high`, `x86-atapi`, `x86-bad-sector`, `x86-extra`,
`x86-extra-map`, `x86-extra-boot` and `x86-extra-range`. In `x86-boot` the boot sector checks
`DL`, that `IF` and `DF` are clear, and that the memory map has the
magic, 20-byte entries, ends by `0x0600`, contains the conventional and
both reserved regions above exactly, and is sorted without overlaps. Its
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
firmware must refuse.

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
| Other harts | Parked in `wfi` in M-mode, with `mstatus.MIE = 0` and only `mie.MSIE` set, until `sbi_hart_start` (below). Hart IDs 8 and up are never started. |
| CLINT | A hart's `mtimecmp` is written only by `sbi_set_timer` and the firmware's timer interrupt. A parked hart's `msip` is written by `sbi_hart_start`. `mtime` runs at 10 MHz (the device tree's `timebase-frequency`). |
| PLIC | Not touched |
| Test device (`0x100000`) | Written only by `sbi_system_reset`. S-mode can reach it too (PMP entry 1 opens everything outside the firmware region), but an OS should use SRST. |

Do not assume: SBI extensions other than base, TIME, IPI, RFENCE, HSM and SRST,
a set `stvec` (install one before the first delegated trap), secondary
harts being available, or IPIs and remote fences reaching a hart that is
not started.

### Traps

An image entered in S-mode takes the delegated exceptions and interrupts
above at its own `stvec`. Everything else, `ecall` from S-mode included,
reaches M-mode.

`mtvec` points at a firmware handler. An `ecall` from S-mode (cause `9`)
is an SBI call and returns (below). The M-mode timer interrupt is how
`sbi_set_timer` fires: the firmware raises the supervisor timer interrupt
(`mip.STIP`) and returns to where S-mode was. Any other trap that reaches M-mode is
reported instead of jumping to address 0, and does not return: the
handler prints `trap mcause=` and the cause as 16 lowercase hex digits
(no `0x`), then `boot halted`, and halts. With the delegation above, an
S-mode image only gets there through a firmware bug. An image entered in
M-mode installs its own `mtvec` before it relies on traps (an M-mode
`ecall` is cause `000000000000000b`).

### SBI

An S-mode image calls the firmware with `ecall`, following the RISC-V SBI
calling convention: `a7` is the extension ID, `a6` the function ID, and
`a0`–`a5` the arguments. The firmware returns the error in `a0` and the
value in `a1`, preserves every other register (`sp` included), and
resumes at the instruction after the `ecall`. An unknown extension or
function returns `SBI_ERR_NOT_SUPPORTED` (`-2`).

| Item | Value |
|------|-------|
| Spec version | `0x01000000` (SBI 1.0) |
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
call returns once the interrupts are sent, not taken. A mask that names
any hart that is not started returns `SBI_ERR_INVALID_PARAM` (`-3`) and
raises nothing, on that hart or any other. An empty mask succeeds and
does nothing.

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
hart behind it, or 8 and above, returns `SBI_ERR_INVALID_PARAM`.

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
IPI to the parked hart 1 fails and raises nothing, and an IPI to itself
prints `ipi=ok` from its software-interrupt handler), `riscv-ipi-smp`
(with `-smp 2`, hart 1 is started and takes IPIs by mask bit, by mask base and with base `-1`, which also reaches hart 0;
`remote_fence_i` and `remote_sfence_vma` on both harts return; then both
harts fence each other 20000 times at once without the firmware
deadlocking), `riscv-fence-smp3` (with `-smp 3`, harts 0 and 1 both
fence hart 2 20000 times at once, then hart 2 takes an IPI from each),
`riscv-rfence`
(with `-smp 2`, a fence whose mask also names hart 1 fails, and
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
(a shutdown for a system failure, status 1) and `riscv-no-kernel`. By hand:

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

Expect `SquirrelBIOS RISC-V`, then `jumping to kernel at
0x0000000080200000, next_mode=1`, then whatever the image prints. For a
Linux kernel, see "Trying Linux" in `riscv/README.md`.
`tests/riscv-kernel.S` is a minimal image.
