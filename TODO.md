# TODO

The laundry list for [ROADMAP.md](ROADMAP.md). One checkbox is one PR,
branched from `main` and merged to `main` before the next one starts.
Stacked PRs are not used.

Work from the top. Finish a section before starting the next one. Inside
a section, do items from the top and honour any `Depends on` line. The
same PR updates `common/handoff.md` (and the architecture README, when
the visible behaviour changes) and checks this box.

Acceptance always includes `make test` passing, plus the check written on
the item. New images go in `tests/`, get a Makefile rule, and get a
`run` line in `tests/smoke.sh`. Failure text matches the style already in
the contract (`disk: ...` / `boot: ...`, then `boot halted`).

## 1. Pin the contract in `make test`

- [x] `x86-handoff-check` — The x86 stand-in checks the memory map and the flags, not only the magic.
  - Acceptance: `tests/x86-boot.asm` prints `memmap=ok` only after the magic is `'SQMM'`, the entry size is 20, the table ends at or before `0x0600`, the conventional and reserved regions from `common/handoff.md` are present, and the entries are sorted by base and do not overlap. It prints `flags=ok` only when `IF` and `DF` are clear. Do not hardcode QEMU's default RAM size. `x86-boot` expects `OS-ENTRY x86 dl=ok memmap=ok flags=ok`.

- [x] `x86-atapi` — Smoke-test the ATAPI reject. No firmware change.
  - Acceptance: a new `x86-atapi` case boots with a CD-ROM as the primary master and expects `disk: primary master is ATAPI (CD/DVD), not supported` and `boot halted`. `x86-boot` still passes.

- [x] `x86-mem-low` — Smoke-test the CMOS path used when RAM is at most 16 MB.
  - Acceptance: a new `x86-mem-low` case passes `-m 8` and expects a usable entry at base `0x00100000` of length `0x00700000`. That length is what `x86/memmap.inc` computes from CMOS `0x30`/`0x31` when `0x34`/`0x35` are zero. If this QEMU stores a different pair of bytes, match the test to those bytes and record them in the PR. `x86-boot` stays free of a hardcoded default RAM size. Depends on: `x86-handoff-check`.

- [x] `x86-mem-high` — Smoke-test the RAM-above-4 GB entry.
  - Acceptance: a new `x86-mem-high` case passes a size above 4 GB (`-m 5120`; `qemu-system-x86_64` is fine if `qemu-system-i386` refuses it) and expects a usable entry at base `0x100000000` with a non-zero length. The conventional entries are still checked. Do not hardcode how QEMU splits the rest of the RAM around the PCI hole. Depends on: `x86-handoff-check`.

- [x] `riscv-a2` — The RISC-V stand-in checks `a2`.
  - Acceptance: `tests/riscv-kernel.S` prints `a2=ok` only when `a2` is 0. `riscv-boot` expects `OS-ENTRY riscv hart=ok dtb=ok a2=ok`.

- [x] `ci-make-test` — Run `make test` on every push and pull request.
  - Acceptance: a workflow installs `nasm`, `gcc-aarch64-linux-gnu`, `gcc-riscv64-linux-gnu`, `qemu-system-x86`, `qemu-system-arm`, and `qemu-system-misc`, runs `make test`, and the README says that is the check CI runs. Depends on: `x86-handoff-check`, `riscv-a2`.

## 2. RISC-V: S-mode and an SBI

Original code only. Extension IDs, the register convention, the CLINT
addresses, and the `hart_start` argument rules are in
[ROADMAP.md](ROADMAP.md). `probe_extension` returns 0 for an extension
until that extension's box is checked.

- [x] `riscv-trap` — Install an M-mode trap handler that prints the cause and halts.
  - Acceptance: `mtvec` points at a handler that prints `trap mcause=` and 16 lowercase hex digits (no `0x`), then `boot halted`. A new stand-in executes `ecall` and the `riscv-ecall` case expects `trap mcause=000000000000000b` (environment call from M-mode). `riscv-boot` does not trap. The contract states that `mtvec` is set and that the OS installs its own vector before it relies on traps. Depends on: `riscv-a2`.

- [x] `riscv-smode` — Honour `next_mode`, and enter S-mode with PMP allowing access.
  - Acceptance: read `fw_dynamic_info` version and, when it is at least 2, `next_mode` at offset 24. Print it once while bringing the path up and expect `1` (S-mode) for `qemu-system-riscv64 -M virt -kernel`. On S-mode, program one PMP entry that grants S-mode read, write, and execute over the whole physical address space, set `mstatus.MPP` to S, and `mret` to the image. On M-mode, keep today's jump. `riscv-boot` still prints `hart=ok dtb=ok a2=ok`, which is the proof that the UART store and the device-tree load survive the mode switch. `riscv-ecall` now expects `trap mcause=0000000000000009`. The contract's privilege line matches this. Depends on: `riscv-trap`.

- [x] `riscv-delegate` — Delegate supervisor faults, and keep supervisor `ecall` in M-mode.
  - Acceptance: `medeleg` includes illegal instruction, the access faults, and the page faults, and does not include environment call from S-mode. `mideleg` includes supervisor software, timer, and external interrupts. A new stand-in sets `stvec`, executes an illegal instruction, and prints `deleg=ok` from S-mode. Its `ecall` still reaches the M-mode handler (`mcause=0000000000000009`). Depends on: `riscv-smode`.

- [x] `riscv-sbi-base` — Implement the SBI base extension and return to the caller.
  - Acceptance: a stand-in `ecall`s `get_spec_version`, `get_impl_id`, and `probe_extension` for base. It prints `sbi=ok` only when the spec version is `0x01000000`, the impl id is the one recorded in the contract (not 0 and not 1), and probing base succeeds. Probing TIME, IPI, RFENCE, HSM, and SRST returns not present. `a0`/`a1` are the only registers changed. Depends on: `riscv-delegate`.

- [x] `riscv-sbi-time` — Implement `sbi_set_timer` on the CLINT.
  - Acceptance: the call programs `mtimecmp` for the requesting hart and enables the M-mode timer interrupt. The M-mode handler injects the supervisor timer interrupt and reprograms `mtimecmp` so the interrupt stays pending once. A stand-in sets a timer in the near future, enables S-mode timer interrupts, and prints `timer=ok` from its handler. `probe_extension` for TIME succeeds. Depends on: `riscv-sbi-base`.

- [x] `riscv-sbi-ipi` — Implement `sbi_send_ipi` for the running hart.
  - Acceptance: the hart mask is the SBI scalar bit-vector, bit N of `a0` selecting hart `a1 + N`. A stand-in that IPIs itself passes `a0 = 1` and `a1` = its hart id, and prints `ipi=ok` from its supervisor software-interrupt handler. A hart mask that names only a hart that is not started returns an SBI error and does not wake a parked hart. `probe_extension` for IPI succeeds. Depends on: `riscv-sbi-base`.

- [x] `riscv-sbi-rfence` — Implement remote fence for the running hart.
  - Acceptance: a stand-in calls `sbi_remote_sfence_vma` with the same scalar hart mask as `riscv-sbi-ipi` (only itself). The call returns success and the stand-in prints `rfence=ok`. A mask that includes a hart that has not been started returns an SBI error. `probe_extension` for RFENCE succeeds. Depends on: `riscv-sbi-base`.

- [x] `riscv-sbi-hsm` — Start a parked hart in S-mode.
  - Acceptance: `sbi_hart_start` leaves the target hart's `wfi`, enters it in S-mode at the given address with `a0` = hart id and `a1` = the opaque argument, and `sbi_hart_get_status` reports started. Parked harts currently sit in `wfi` with interrupts off, so the wake has to be something this QEMU actually resumes (a CLINT software interrupt, with `mie` set if that is what resumes it). A `riscv-hsm` case with `-smp 2` prints `hart1=ok` from hart 1 and shows the opaque value the boot hart passed. Hart 1 does not receive the device tree unless that pointer was the opaque argument. `probe_extension` for HSM succeeds. Depends on: `riscv-smode`, `riscv-sbi-base`.

- [x] `riscv-sbi-hsm-stop` — Implement `sbi_hart_stop` for a hart that was started.
  - Acceptance: a started secondary hart calls `sbi_hart_stop` and the boot hart's later `hart_get_status` reports stopped. The stopped hart does not keep printing. Depends on: `riscv-sbi-hsm`.

- [x] `riscv-sbi-srst` — Implement `sbi_system_reset`.
  - Acceptance: a stand-in calls shutdown and the log contains `reset=ok` before the machine halts or QEMU exits. `probe_extension` for SRST succeeds. Depends on: `riscv-sbi-base`.

- [x] `riscv-pmp-firmware` — Stop S-mode from writing the firmware image.
  - Acceptance: PMP entry 0 covers the firmware image with write clear, and it sits at a lower index than the allow-all entry from `riscv-smode`. Leave it unlocked, so M-mode can still execute the SBI handler (a locked deny would apply to M-mode too). A stand-in store into the firmware range faults, S-mode reports it, and the log contains `pmp=ok`. Fetches of the stand-in at `0x80200000` still work. Depends on: `riscv-delegate`, `riscv-smode`.

- [x] `riscv-dt-reserve` — Tell the OS the firmware region is reserved.
  - Acceptance: before an S-mode entry the firmware adds `/reserved-memory` with a `no-map` child over `0x80000000`–`0x801FFFFF` to the device tree, so Linux does not use the memory PMP closes to it. A stand-in walks the tree, finds the node, and prints `dt=ok`; the rest of the tree still parses. Depends on: `riscv-pmp-firmware`.

- [x] `riscv-menvcfg` — Turn on what Linux uses from S-mode on QEMU's default hart.
  - Acceptance: the firmware sets `menvcfg` `CBIE`/`CBCFE`/`CBZE` and `STCE` where the hart has them, and `set_timer` writes `stimecmp` when Sstc is on. A stand-in runs `cbo.zero` and the other `cbo` instructions and gets a timer interrupt from a direct `stimecmp` write, and prints `envcfg=ok`; `riscv-timer` still passes. Without this, Linux 6.8 stops with an illegal instruction in `clear_page`. Depends on: `riscv-sbi-time`.

- [x] `riscv-ipi-smp` — Make IPIs and remote fences reach other started harts.
  - Acceptance: `send_ipi` raises `SSIP` on every started hart the mask names, and the RFENCE functions run the fence on each and return when all have. A `-smp 2` stand-in gets IPIs on hart 1 by mask bit, base and base `-1`, fences both harts, and has both harts fence each other many times at once without a deadlock, then prints `ipi-smp=ok`. A fence returns only after a pass that began after the request finished on each target; a `-smp 3` stand-in has two harts fence a third at once. Without this, Linux with `-smp 2` stalls after bringing up its CPUs. Depends on: `riscv-sbi-ipi`, `riscv-sbi-rfence`, `riscv-sbi-hsm`.

- [x] `riscv-linux-doc` — Document the QEMU command that tries a Linux `Image` on this SBI.
  - Acceptance: `riscv/README.md` gives the command line, lists the extensions `probe_extension` now reports, and says a Linux image is not part of `make test`. No kernel binary is added to the repo. Depends on: `riscv-sbi-time`, `riscv-sbi-ipi`, `riscv-sbi-rfence`, `riscv-sbi-hsm`, `riscv-sbi-srst`, `riscv-dt-reserve`, `riscv-menvcfg`, `riscv-ipi-smp`.

- [x] `riscv-linux-shell` — Check that a Linux kernel boots on this SBI all the way to a shell.
  - Acceptance: `make linux-shell` (not part of `make test`) downloads pinned Ubuntu 24.04 riscv64 `linux-image` and `busybox-static` packages, checks their SHA-256, builds a busybox initramfs in `build/linux/`, and boots it. It prints `PASS linux-shell` when the prompt appears, `echo linux-shell=$((40+2))` prints `linux-shell=42`, and `poweroff` makes QEMU exit with status 0. No kernel, busybox or initramfs is added to the repo.

## 3. x86: load more than one sector

- [x] `x86-boot-sig` — Refuse to jump when the boot sector has no `0xAA55` signature.
  - Acceptance: after a clean read, a last word other than `0xAA55` prints `disk: boot sector has no 0xAA55 signature` and `boot halted`, and is not retried. A new `x86-bad-sector` case uses a 512-byte image without the signature and expects that text. `x86-boot` (which has the signature) still passes. Depends on: `x86-handoff-check`.

- [x] `x86-extra-sectors` — Load the sectors a header in sector 0 asks for.
  - Acceptance: entry stays `0x0000:0x7C00` with `DL` and the memory map unchanged. When sector 0 contains the header below, read that many following sectors to the given real-mode address and only then jump. With the magic absent, behaviour is today's one-sector hand-off (`x86-boot` still passes). A new `x86-extra` case whose second sector prints `payload=ok` passes. A header that lands on the memory map or on the boot sector prints `disk: load overlaps firmware data` and `boot halted`.
  - Header, little-endian, immediately before the signature:

    | Offset | Size | Field |
    |--------|------|-------|
    | `0x1F0` | 4 | Magic bytes `53 51 42 4C` (`SQBL`). Absent means one sector. |
    | `0x1F4` | 2 | Extra sector count, 1–128. |
    | `0x1F6` | 2 | Load segment. |
    | `0x1F8` | 2 | Load offset. |
    | `0x1FA` | 4 | Reserved, zero. |
    | `0x1FE` | 2 | `0xAA55`, as in `x86-boot-sig`. |

  - The linear address `segment * 16 + offset` stays below `0xA0000`, and the extra sectors must not cross `0xA0000` or overlap `0x0500`–`0x05FF` or `0x7C00`–`0x7DFF`. Anything else prints the overlap message, or `disk: load address is out of range`, and halts. Depends on: `x86-boot-sig`.

## 4. AArch64: finish the Linux entry

- [x] `arm-sctlr` — Enter the image with the MMU and D-cache off, on purpose.
  - Acceptance: before the jump, at the EL QEMU started the firmware in, clear `SCTLR.M` and `SCTLR.C`, clear the endianness bits so the CPU is little-endian, keep `DAIF` masked, and invalidate the instruction cache (`ic iallu`, then a barrier). The stand-in reads the `SCTLR` of its current exception level and prints `sctlr=ok` only when `M` and `C` are clear and `DAIF` is fully masked. `arm-boot` expects `OS-ENTRY arm dtb=ok regs=ok sctlr=ok`. The contract describes this as firmware state.

- [x] `arm-dma-timeout` — Give up if an fw_cfg DMA never completes.
  - Acceptance: the poll in `fwcfg_dma_read` stops after a finite number of iterations, prints `boot: fw_cfg DMA read of the kernel failed` (the existing line) or a new line documented next to it, then `boot halted`. `arm-boot` and `arm-no-kernel` still pass, so a completed transfer is unchanged. Depends on: `arm-sctlr`.

- [x] `arm-ram-size` — Refuse a kernel that does not fit in RAM.
  - Acceptance: read fw_cfg key `0x03` (RAM size, 8 little-endian bytes). If `0x40200000 + kernel_size` extends past `0x40000000 + ram_size`, or the image would overlap the device tree below `0x40200000`, print `boot: kernel does not fit in RAM` and `boot halted`. `arm-boot` still passes. A new `arm-too-big` case builds a large image under `build/tests/` (do not commit it) and expects that message. Depends on: `arm-dma-timeout`.

- [x] `arm-image-header` — Honour the ARM64 `Image` header when its magic is present.
  - Acceptance: the header is the Linux arm64 one. Magic is the bytes `41 52 4D 64` at file offset `0x38`. `text_offset` is the u64 at offset 8, `image_size` the u64 at offset 16, `flags` the u64 at offset 24. No magic means today's raw load at `0x40200000` (`arm-boot` unchanged). Magic with bit 3 of `flags` clear (kernel physical placement fixed) loads at the 2 MB-aligned RAM base plus `text_offset` and jumps there. Magic with bit 3 set keeps `0x40200000`. Apply the `arm-ram-size` fit check at the address actually used. A header whose `image_size` is larger than the bytes fw_cfg delivered prints a documented message and halts. *(As built: the reverse. Linux's `image_size` includes BSS, so it is always larger than the file; the firmware uses it for the fit check and refuses a non-zero `image_size` that is smaller than the file.)* New cases: `arm-image-fixed` (linked at RAM base + `text_offset`, expects `OS-ENTRY arm`) and `arm-image-reloc` (bit 3 set, linked at `0x40200000`, expects `OS-ENTRY arm`). Depends on: `arm-ram-size`.

- [x] `arm-initrd` — Load QEMU's initrd and record it in the device tree.
  - Acceptance: fw_cfg key `0x0b` is the initrd size and key `0x12` is the bytes. When the size is non-zero, DMA the bytes to a place that does not overlap the device tree or the kernel, and set `/chosen` `linux,initrd-start` and `linux,initrd-end`. If the device tree has no spare room, print a documented message and halt instead of writing past it. A new `arm-initrd` case passes a small `-initrd` and expects `initrd at ` in the log. With no initrd, `arm-boot` still passes and the device tree is left as QEMU provided it. Depends on: `arm-image-header`.

## 5. Consoles and small hardening

- [x] `riscv-uart-baud` — Program the RISC-V UART to 115200 8N1 and say so in the contract.
  - Acceptance: the NS16550A divisor is set, `common/handoff.md` lists 115200 the way it does for COM1, and `riscv-boot` still sees the banner.

- [x] `x86-cmos-nmi` — Disable NMI for the duration of each CMOS read.
  - Acceptance: `cmos_read` sets bit 7 of port `0x70` while it indexes the register. `x86-boot`, `x86-mem-low`, and `x86-mem-high` still pass. Depends on: `x86-mem-low`, `x86-mem-high`.

- [x] `x86-vga` — Put the VGA into 80x25 text mode and load an original font.
  - Acceptance: the font is written in this repo (not taken from a VGA BIOS). The stand-in reads the mode registers, prints `vga=ok` only when they show 80x25 text, and `x86-boot` expects that string. The contract stops saying the VGA was left uninitialised. COM1 output is unchanged. Depends on: `x86-handoff-check`.

## 6. A large server node (future)

Not scheduled; see [ROADMAP.md](ROADMAP.md) milestone 6 for why and for
what `make linux-big` showed on its first run. Start after sections 1–5.
These items are not part of `make test`'s acceptance unless they say so.

- [x] `big-node-scaffold` — Add `make linux-big`: the `linux-shell` boot on `-smp 64 -m 64G`.
  - Acceptance: `tests/linux-shell.sh` takes `LINUX_MEM` and `LINUX_EXPECT_HARTS`, maps guest RAM with `reserve=off` so a 64 GB guest starts on a smaller host, and shows QEMU's own error when it fails to start. `make linux-big` passes only when the shell banner reports every hart online. `make linux-big BIG_SMP=8` passes today; the default (64) fails with `8 of 64 harts online`, which is recorded in the roadmap. `make linux-shell` and `make test` are unchanged.

- [ ] `riscv-many-harts` — Start every hart QEMU `virt` can give the firmware, not just 8.
  - Acceptance: `MAX_HARTS` covers `-smp 64` at least (QEMU 8.2 allows up to 512 on `virt`). The trap stacks, `hart_table` and `.bss` still fit under `0x80200000` (the Makefile check). A `-smp 64` stand-in starts every secondary with `sbi_hart_start` and prints `harts=64`. `make linux-big BIG_SMP=32` passes. `riscv-ipi-smp` and `riscv-fence-smp3` still pass.

- [ ] `linux-big-kernel` — Let `make linux-big` check more than 32 harts.
  - Acceptance: the script takes a pinned kernel built with `NR_CPUS` of at least 64 (downloaded and checked by SHA-256 like today's, or a documented `LINUX_IMAGE=` override), and `make linux-big` with the default 64 harts prints `PASS linux-shell`. No kernel is added to the repo. Depends on: `riscv-many-harts`.

- [ ] `riscv-boot-hart` — Boot the hart `fw_dynamic_info.boot_hart` names. *(Moved up from Speculative.)*
  - Acceptance: as in the Speculative entry: `riscv-boot` still passes, and a case whose boot hart is not 0 prints `hart=ok` from that hart. Depends on: `riscv-many-harts`.

- [ ] `riscv-sparse-harts` — Do not assume hart IDs are 0 to N-1.
  - Acceptance: the per-hart tables are indexed by a slot the firmware assigns, not by the raw hart ID, and a hart ID above `MAX_HARTS` gets a slot if one is free. A stand-in whose device tree names non-contiguous hart IDs starts each of them. Depends on: `riscv-many-harts`.

- [ ] `riscv-numa` — Keep NUMA information intact through the firmware's device-tree edits.
  - Acceptance: with `-numa node,...` for two nodes, the tree handed to the OS still has every `/memory` node with its `numa-node-id`, each `cpu` keeps its `numa-node-id`, and `/distance-map` is unchanged. The `/reserved-memory` entry from `riscv-dt-reserve` is added only once. A Linux run under `make linux-big` with two nodes reports `2 nodes` in its log. Depends on: `riscv-many-harts`.

- [ ] `riscv-aia` — Keep IPIs and the timer working on `virt,aia=aplic-imsic`.
  - Acceptance: a `riscv-ipi-smp` run with `-M virt,aia=aplic-imsic` passes, and `make linux-big` on that machine reaches the shell. Depends on: `riscv-many-harts`.

- [ ] `big-node-pcie` — Check that PCIe devices enumerate on a large node.
  - Acceptance: `make linux-big` with a `virtio-net-pci` and an `nvme` device sees both under `/sys/bus/pci/devices`. No firmware change unless the run shows one is needed.

- [ ] `big-node-netboot` — Decide and document how a diskless node gets its kernel.
  - Acceptance: a note in `common/handoff.md` saying whether the firmware loads a kernel over virtio-net or leaves network boot to the OS, with the reason. If the firmware does it, that is a separate item with its own stand-in.

- [ ] `big-node-errors` — Report hardware faults instead of only halting.
  - Acceptance: a written plan in this file for where machine faults go (the console line format, and what the OS is told), before any code.

UEFI and ACPI hand-off for servers stays out of scope; see the roadmap.

## Speculative

Not scheduled. These are ideas, not the laundry list above. Open a PR for
one only when the OS needs it and the milestones above are done. It would
still be a single PR from `main`, with a `make test` check and a contract
update.

- `x86-a20` — Turn on A20 so a later load address can sit above 1 MB. A stand-in would read a byte above 1 MB and print `a20=ok`, and the extra-sector header could then name such an address.
- `x86-bzimage` — Load a Linux `bzImage` (setup header, protected-mode entry) instead of the `SQBL` sector header. A tiny protected-mode stand-in would print `bz=ok` on COM1.
- `x86-acpi` — Build ACPI tables the OS can find. A stand-in would check the RSDP checksum and print `acpi=ok`.
- `x86-slave` — Boot from the primary slave when the master is absent. A case with the image on the slave would print `dl=ok`.
- `arm-psci` — Implement PSCI in the firmware. QEMU virt already advertises PSCI and handles it. A stand-in `CPU_ON` of a second CPU would print `cpu1=ok` with QEMU's PSCI disabled.
- `arm-gic` — Initialise the GIC and the generic timer. The OS already drives both from the device tree. A stand-in would receive a timer interrupt and print `irq=ok`.
- `riscv-dbcn` — Add the SBI debug console extension. The device tree already names the NS16550A, which Linux can use. A stand-in `sbi_debug_console_write` would print `dbcn=ok`.
- `riscv-dtb-devices` — Take the UART and CLINT addresses from the device tree instead of the QEMU virt constants. `riscv-boot` would still pass, and the addresses would no longer be immediate constants.
- `riscv-boot-hart` — Now scheduled in section 6.
- `linux-image-in-tree` — Keep a built Linux image in the repo and boot it from `make test`. The stand-in images are the suite; a distro kernel is large and is not original code.
