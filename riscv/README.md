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

See `../common/handoff.md`.
