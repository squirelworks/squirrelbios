# Real boards (research)

Notes on taking the QEMU firmware to real boards. They are research:
nothing here is built, nothing in `make test` depends on it, and
real-hardware checks are never a merge blocker (ROADMAP D2, D3, C5).

| Directory | Board | Firmware it would build on |
|---|---|---|
| [`rpi4/`](rpi4/README.md) | Raspberry Pi 4 (BCM2711), notes on Pi 5 (BCM2712) | `aarch64-uefi` (`arm/uefi`, `common/efi`) |
| [`visionfive2/`](visionfive2/README.md) | StarFive VisionFive 2 (JH7110) | `riscv64-uefi` (the SBI in `riscv/` plus `common/efi`) |
| [`hifive-unmatched/`](hifive-unmatched/README.md) | SiFive HiFive Unmatched (FU740) | the same |
| [`ga-g41m-es2l/`](ga-g41m-es2l/README.md) | Gigabyte GA-G41M-ES2L (Intel G41 + ICH7) | `x86_64-uefi` (first as a coreboot payload) |

Each note says how the board boots, what of ours could run where, and
the gaps in our code, file by file. Addresses are the ones the boards'
own device trees and public manuals give; anything marked *(verify)*
still has to be checked against the board's device tree before code
relies on it. The firmware already reads most devices from the device
tree, so the board's tree (which its boot loader passes in) is the
first thing to dump: `dtc -I fs /proc/device-tree` under Linux.

Rules carried over from the project: original code only. Reading the
SoC manuals, the boards' published device trees and the vendors'
boot-flow documentation is fine; their boot loaders (U-Boot, TF-A,
OpenSBI, EDK2) are not a source to copy from.
