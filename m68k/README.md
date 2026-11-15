# m68k SquirrelBIOS (`m68k-virt`, roadmap E3)

Firmware for QEMU's m68k `virt` machine (68040), in C with a small
assembly entry.

```bash
make m68k             # needs m68k-linux-gnu-gcc (gcc-m68k-linux-gnu)
qemu-system-m68k -M virt -nographic -kernel m68k/squirrel-bios.elf \
    -initrd os.elf -append "console=ttyGF0"
```

QEMU's `virt` machine has no ROM: the firmware is the `-kernel` ELF,
which QEMU loads at its link address (`0x03c00000`, so at least 64 MB of
RAM) and starts in supervisor mode with the MMU off. The OS image comes as
`-initrd`.

## What it does

- Reads QEMU's bootinfo records (right after the firmware's `_end`):
  memory, CPU, the command line, and where the Goldfish TTY, RTC and
  interrupt controller, the virtio-mmio window and the `virt ctrl` power
  device are.
- Console: the Goldfish TTY (`PUT_CHAR`; input with `READ_BUFFER`).
  Prints `SquirrelBIOS m68k`.
- Its own exception vectors (VBR): anything unexpected prints
  `trap: vector ... at pc ...` and halts.
- Boots the `-initrd` image if it is an ELF32 m68k executable: its
  `PT_LOAD` segments go to their physical addresses (they must not cover
  the firmware or the image file).
- Without an image (or if it is refused), the `sq>` prompt: `info`,
  `bootinfo`, `date` (Goldfish RTC), `boot`, `poweroff` and `reset`
  (`virt ctrl`), `help`.

## Hand-off

The way an m68k Linux is started:

| | |
|---|---|
| Entry | the ELF entry, supervisor mode, interrupts masked (`SR` = `0x2700`), MMU off, `VBR` = 0 |
| Bootinfo | right after the image's end (rounded up to an even address): QEMU's records without `BI_RAMDISK` (that was the OS image itself) and with the command line, ending with `BI_LAST` |
| `%sp` | just below the firmware (set your own) |

## Linux

Linux 6.8 (`virt_defconfig`, built from Ubuntu's `linux-source-6.8.0`)
boots through the firmware to "VFS: Unable to mount root fs" (there is no
room left for an initramfs, see below):

    qemu-system-m68k -M virt -nographic -kernel m68k/squirrel-bios.elf \
        -initrd vmlinux -append console=ttyGF0

## Limits (next steps)

- The OS image takes the `-initrd` slot, so there is no separate initrd
  for it.
- No virtio drivers in the firmware: the OS image must come from QEMU.

## Test

`make test` runs `tests/m68k.smoke` (skipped without `qemu-system-m68k`
or the cross compiler): `tests/m68k-kernel.c`, linked at `0x1000`, checks
the CPU state and the bootinfo it gets, with 128 MB and 64 MB of RAM, and
powers off; the prompt and a refused non-ELF image are checked too.

Original code, written from the m68k Linux boot protocol (bootinfo
records), the Goldfish device register lists and the 68040 manual, not
from another firmware's source.
