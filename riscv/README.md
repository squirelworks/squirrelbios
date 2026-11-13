# RISC-V SquirrelBIOS

Minimal firmware for QEMU `virt`.

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

- One hart boots: the one `fw_dynamic_info.boot_hart` names when it is below 512, otherwise hart 0 (QEMU's `-kernel` path always names 0); other harts park in `wfi` until the OS starts them with `sbi_hart_start`; `sbi_hart_stop` sends a hart back
- Takes the NS16550A UART and the CLINT (or, with `aclint=on`, the ACLINT MSWI and MTIMER) from the device tree QEMU passes in `a1`, the lowest when there are several; the build's addresses (`0x10000000`, `0x02000000`) stay only for what the tree does not name. It prints `dt: UART at ..., CLINT msip at ..., mtime at ...`
- Initialises the UART (115200 8N1) and prints `SquirrelBIOS RISC-V`
- Finds the `-kernel` image QEMU loaded (via the `fw_dynamic_info` block in `a2`)
- Enters it in the mode QEMU's `next_mode` asks for, S-mode for `-kernel`,
  with PMP closing the firmware region (`0x80000000`–`0x801FFFFF`) to S-mode and opening the rest of memory, `a0` = hart ID, `a1` = device tree
- Adds the firmware region to the device tree's `/reserved-memory` (`no-map`), so Linux does not use it
- Lets S-mode use the cache-block instructions (Zicbom, Zicboz) and its own timer compare (Sstc) when the hart has them, as Linux expects
- In S-mode, supervisor faults and interrupts are delegated to the image; `ecall` from S-mode stays in M-mode
- Provides an SBI to S-mode through `ecall`: base, TIME, IPI, RFENCE, HSM, SRST, DBCN and SSE (spec 2.0, with SSE from 3.0, implementation ID `0x5351`); `sbi_set_timer` uses the CLINT, `sbi_send_ipi` and the remote fences reach every started hart through the CLINT software interrupt; `sbi_system_reset` powers off or resets through QEMU's test device at `0x100000`
- Any other trap that reaches M-mode prints `ras: hart=<16 hex> mcause=<16 hex> mepc=<16 hex> mtval=<16 hex> fatal`, parks every other started hart, prints `boot halted`, then shuts the machine down as an SRST system failure (QEMU exit status 1)
- Without `-kernel`, prints the reason and halts

QEMU puts the image at `0x80200000` on rv64, so link it there.

## Trying Linux

The quickest way is the scripted check from the top-level README, which
downloads Ubuntu 24.04's riscv64 kernel (6.8) and busybox and boots them
to a shell:

```bash
make linux-shell                                  # prints PASS linux-shell
LINUX_SMP=4 tests/linux-shell.sh --interactive    # 4 harts, the shell is yours
make linux-big                                    # 64 harts, 64 GB in 2 NUMA nodes, a NIC and an NVMe drive
```

To boot a kernel of your own, give QEMU a raw RISC-V `Image` (the file
with `RISCV` at offset `0x30`; an EFI `vmlinuz` with `zimg` at offset 4
is compressed and has to be unpacked first):

```bash
make riscv
qemu-system-riscv64 -M virt -smp 4 -m 1G -nographic \
    -bios riscv/squirrel-bios.bin -kernel Image \
    -append "console=ttyS0 earlycon=uart8250,mmio,0x10000000"
```

Add `-initrd initramfs.cpio.gz` (or a disk) for a root file system.
Without one, the boot ends at `Kernel panic - not syncing: VFS: Unable
to mount root fs`, which means everything up to the root mount worked.
Expect the firmware's `dt: firmware region ... reserved (no-map)` line,
then Linux reporting `SBI specification v2.0 detected`, `SBI
implementation ID=0x5351`, and the extensions below.

`probe_extension` reports these SBI extensions, and no others:

| Extension | Used by Linux for |
|---|---|
| Base (`0x10`) | spec version, implementation ID, probing |
| TIME (`0x54494D45`) | the timer, on harts without Sstc (QEMU's default hart has it, so Linux writes `stimecmp` itself) |
| IPI (`0x735049`) | inter-processor interrupts |
| RFENCE (`0x52464E43`) | remote `fence.i` and TLB shootdowns |
| HSM (`0x48534D`) | starting secondary harts (`-smp`), CPU hotplug |
| SRST (`0x53525354`) | `poweroff` and `reboot` |
| DBCN (`0x4442434E`) | `earlycon=sbi` and the `hvc` SBI console: `console_write`, `console_read`, `console_write_byte` on the UART |

Linux probes DBCN only when the spec version is 2.0 or later, which is
why the firmware reports 2.0; `earlycon=sbi` then works too. Not
provided: the legacy SBI v0.1 calls, PMU,
system suspend and the other newer extensions, which Linux probes for and
does without.

Known limits:

- A kernel that only reads `riscv,isa-extensions` from the device tree
  (Ubuntu's 7.0 kernel, for example) stops early with `this kernel does
  not parse "riscv,isa"`: QEMU 8.2 writes only `riscv,isa`. That is the
  QEMU version, not the firmware; a newer QEMU writes both.
- Only QEMU `virt` is supported: the UART and CLINT come from the device
  tree, but the firmware still assumes virt's test device and memory
  layout.

Linux is not part of `make test` or CI, and no kernel, busybox or
initramfs is kept in the repo.

See `../common/handoff.md`.

## UEFI-class firmware (`PLATFORM=riscv64-uefi`, roadmap D3)

```bash
make riscv-uefi
qemu-system-riscv64 -M virt -nographic -bios riscv/uefi/squirrel-bios.bin \
    -kernel Image -initrd initramfs.cpio.gz -append "console=ttyS0"
make linux-riscv-uefi        # Ubuntu's 6.8 kernel through its EFI stub to a shell
```

`riscv/uefi/squirrel-bios.bin` is this SBI, assembled with `UEFI=1` and
padded to 2 MB, followed by the shared UEFI core (`common/efi`) with
`riscv/uefi`, linked at `0x80200000`. The SBI runs as above, but always
enters the core in S-mode (`entering the UEFI core at ...`) with
`a0` = hart ID, `a1` = device tree and `a2` = the `-kernel` image QEMU
loaded (at the 2 MB boundary after the file, `0x80400000`) or 0. The
core keeps the SBI's first 2 MB reserved, runs with translation off,
and:

- reads the NS16550A, the timebase and the goldfish RTC from the device
  tree, and uses the SBI for `ResetSystem` (SRST);
- builds the UEFI memory map from `/memory` and `/reserved-memory`,
  keeps the `-kernel` image and the `/chosen` initrd as boot services
  data, itself as runtime services code and the RTC as runtime MMIO;
- installs `RISCV_EFI_BOOT_PROTOCOL` (the boot hart for Linux's EFI
  stub) and the device tree as a configuration table;
- starts a `-kernel` PE image as an EFI application with `/chosen`
  `bootargs` as its LoadOptions and the initrd on LoadFile2 (the Linux
  initrd media path), or an image given as
  `-fw_cfg name=opt/org.squirrelworks/boot.efi,file=...`, or
  `\EFI\BOOT\BOOTRISCV64.EFI` (a long FAT name) from a virtio-blk disk
  (`-device virtio-blk-device`, legacy or
  `-global virtio-mmio.force-legacy=false`).

Linux uses the runtime services after `SetVirtualAddressMap` (`GetTime`
from the goldfish RTC, variables, reset). Not yet: ACPI (RHCT); SSE has only the software-injected local event.
