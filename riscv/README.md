# RISC-V SquirrelBIOS

Minimal firmware for QEMU `virt`.

```bash
make riscv
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

- Hart 0 boots; other harts park in `wfi` until the OS starts them with `sbi_hart_start`; `sbi_hart_stop` sends a hart back
- Initialises the NS16550A UART (`0x10000000`) and prints `SquirrelBIOS RISC-V`
- Finds the `-kernel` image QEMU loaded (via the `fw_dynamic_info` block in `a2`)
- Enters it in the mode QEMU's `next_mode` asks for, S-mode for `-kernel`,
  with PMP closing the firmware region (`0x80000000`–`0x801FFFFF`) to S-mode and opening the rest of memory, `a0` = hart ID, `a1` = device tree
- Adds the firmware region to the device tree's `/reserved-memory` (`no-map`), so Linux does not use it
- Lets S-mode use the cache-block instructions (Zicbom, Zicboz) and its own timer compare (Sstc) when the hart has them, as Linux expects
- In S-mode, supervisor faults and interrupts are delegated to the image; `ecall` from S-mode stays in M-mode
- Provides an SBI to S-mode through `ecall`: base, TIME, IPI, RFENCE, HSM and SRST (spec 1.0, implementation ID `0x5351`); `sbi_set_timer` uses the CLINT, `sbi_send_ipi` and the remote fences reach every started hart through the CLINT software interrupt; `sbi_system_reset` powers off or resets through QEMU's test device at `0x100000`
- Any other trap that reaches M-mode prints `trap mcause=<16 hex digits>`, then `boot halted`, and halts
- Without `-kernel`, prints the reason and halts

QEMU puts the image at `0x80200000` on rv64, so link it there.

## Trying Linux

The quickest way is the scripted check from the top-level README, which
downloads Ubuntu 24.04's riscv64 kernel (6.8) and busybox and boots them
to a shell:

```bash
make linux-shell                                  # prints PASS linux-shell
LINUX_SMP=4 tests/linux-shell.sh --interactive    # 4 harts, the shell is yours
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
then Linux reporting `SBI specification v1.0 detected`, `SBI
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

Not provided: the legacy SBI v0.1 calls and the debug console (DBCN), so
`earlycon=sbi` fails (`Malformed early option 'earlycon'`) and early
messages only appear once `ttyS0` is up; use the `uart8250` earlycon
above. Also not provided: PMU,
system suspend and the other newer extensions, which Linux probes for and
does without.

Known limits:

- A kernel that only reads `riscv,isa-extensions` from the device tree
  (Ubuntu's 7.0 kernel, for example) stops early with `this kernel does
  not parse "riscv,isa"`: QEMU 8.2 writes only `riscv,isa`. That is the
  QEMU version, not the firmware; a newer QEMU writes both.
- Only QEMU `virt` is supported: the firmware assumes its CLINT, UART,
  test device and memory layout.

Linux is not part of `make test` or CI, and no kernel, busybox or
initramfs is kept in the repo.

See `../common/handoff.md`.
