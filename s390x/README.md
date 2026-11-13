# s390x SquirrelBIOS (`s390x`, roadmap E3, IPL only)

Firmware for QEMU's `s390-ccw-virtio` machine, in C with a small
assembly entry. It talks to the SCLP console, finds a virtio-blk disk on
the channel subsystem and IPLs an ELF image from it.

```bash
make s390x            # needs s390x-linux-gnu-gcc (gcc-s390x-linux-gnu)
qemu-system-s390x -M s390-ccw-virtio -nographic -bios s390x/squirrel-bios.elf \
    -drive file=disk.img,if=none,format=raw,id=d0 -device virtio-blk-ccw,drive=d0
```

## What it does

- Is a position-independent ELF linked at 0. QEMU loads a `-bios` ELF
  about 3 MB below the top of RAM (or of the first 2 GB) and starts it in
  64-bit mode, DAT and interrupts off; `start.S` applies its own
  `R_390_RELATIVE` relocations, clears `.bss` and catches program checks
  (`trap: program check ...`).
- Console: the SCLP VT220 event (`-serial`, `-nographic`). The firmware
  sets the event mask, sends whole lines as write-event-data requests and
  polls read-event-data for input. Each request waits for the service
  signal with only that external interrupt enabled.
- RAM size from the SCLP's read SCP info.
- Scans the channel subsystem (`stsch`), enables each valid subchannel
  (`msch`) and identifies it with SENSE ID: virtio devices have control
  unit type `0x3832` and the virtio device id as the model.
- Starts the first virtio-blk disk through the virtio over channel I/O
  transport (legacy interface: reset, status, features, one queue with
  `SET_VQ`, config) and reads it with `diag 0x500` notifications.
- IPL: the disk holds an s390x ELF executable from sector 0 (padded to
  whole sectors). The firmware reads its headers, stages the file just
  below itself, places the `PT_LOAD` segments (at `0x10000` or above) and
  enters it.
- Without a disk or a bootable image, the `sq>` prompt: `info`,
  `devices`, `boot`, `poweroff`, `help`.

## Hand-off

| | |
|---|---|
| Entry | the ELF entry, 64-bit addressing, DAT off, I/O, external and machine-check interrupts off, key 0, supervisor state |
| `%r2` | the RAM size in bytes |
| `%r3` | 0 |
| Lowcore `0xb8` | the IPL device's subchannel id (`0x0001xxxx`) |
| SCLP | the VT220 event mask is set for sending and receiving |

`poweroff` (and a firmware error) end in a disabled wait at `0xfff`,
which QEMU treats as a shutdown.

## Limits (next steps)

- One CPU; the others stay stopped.
- Only an ELF image on a virtio-blk disk. No Linux raw image (`image`
  with its parameter area at `0x10400`), no zipl boot records, no
  `-kernel`/`-initrd`/`-append` hand-over, no network or SCSI IPL.
- No interrupts after the hand-off are set up for the OS: it brings its
  own new PSWs.

## Test

`make test` runs `tests/s390x.smoke` (skipped without
`qemu-system-s390x` or the cross compiler): `tests/s390x-kernel.c`,
written to a disk image, checks the PSW, `%r2`, the subchannel id and its
own data and BSS, at 128 MB and 1 GB (with the disk at another device
number); the prompt and a disk that holds no s390x ELF are checked too.

Original code, written from the z/Architecture Principles of Operation,
the virtio specification (virtio over channel I/O) and public SCLP
descriptions, not from another firmware's source.
