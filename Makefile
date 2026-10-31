.PHONY: all x86 arm riscv clean

all: x86 arm riscv

x86: x86/squirrel-bios.rom

x86/squirrel-bios.rom: x86/squirrel-bios.asm x86/memmap.inc
	nasm -f bin -i x86/ $< -o $@

arm: arm/squirrel-bios.bin

arm/squirrel-bios.bin: arm/squirrel-bios.S
	aarch64-linux-gnu-as -o arm/squirrel-bios.o $<
	aarch64-linux-gnu-ld -Ttext=0x0 -o arm/squirrel-bios.elf arm/squirrel-bios.o
	aarch64-linux-gnu-objcopy -O binary arm/squirrel-bios.elf $@

riscv: riscv/squirrel-bios.bin

riscv/squirrel-bios.bin: riscv/squirrel-bios.S
	riscv64-linux-gnu-as -o riscv/squirrel-bios.o $<
	riscv64-linux-gnu-ld -Ttext=0x80000000 -o riscv/squirrel-bios.elf riscv/squirrel-bios.o
	riscv64-linux-gnu-objcopy -O binary riscv/squirrel-bios.elf $@

clean:
	rm -f x86/squirrel-bios.rom \
	      arm/squirrel-bios.o arm/squirrel-bios.elf arm/squirrel-bios.bin \
	      riscv/squirrel-bios.o riscv/squirrel-bios.elf riscv/squirrel-bios.bin
