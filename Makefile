# SquirrelBIOS build
#
#   make            build all three firmware images
#   make test       build, then boot each one under QEMU (tests/smoke.sh)
#   make clean      remove everything the build and tests produce
#
# Cross tools can be overridden, e.g. make AARCH64=aarch64-elf-

NASM    ?= nasm
AARCH64 ?= aarch64-linux-gnu-
RISCV64 ?= riscv64-linux-gnu-

X86_ROM   := x86/squirrel-bios.rom
ARM_BIN   := arm/squirrel-bios.bin
RISCV_BIN := riscv/squirrel-bios.bin

TEST_DIR  := build/tests
TEST_BINS := $(TEST_DIR)/x86-boot.bin $(TEST_DIR)/arm-kernel.bin $(TEST_DIR)/riscv-kernel.bin \
             $(TEST_DIR)/riscv-ecall.bin $(TEST_DIR)/riscv-deleg.bin

.PHONY: all x86 arm riscv test clean
.DELETE_ON_ERROR:

all: x86 arm riscv

x86: $(X86_ROM)
arm: $(ARM_BIN)
riscv: $(RISCV_BIN)

# --- firmware ---------------------------------------------------------

$(X86_ROM): x86/squirrel-bios.asm x86/memmap.inc
	$(NASM) -f bin -i x86/ $< -o $@

arm/squirrel-bios.o: arm/squirrel-bios.S
	$(AARCH64)as -o $@ $<
arm/squirrel-bios.elf: arm/squirrel-bios.o
	$(AARCH64)ld -Ttext=0x0 -o $@ $<
$(ARM_BIN): arm/squirrel-bios.elf
	$(AARCH64)objcopy -O binary $< $@

riscv/squirrel-bios.o: riscv/squirrel-bios.S
	$(RISCV64)as -o $@ $<
riscv/squirrel-bios.elf: riscv/squirrel-bios.o
	$(RISCV64)ld -Ttext=0x80000000 -o $@ $<
$(RISCV_BIN): riscv/squirrel-bios.elf
	$(RISCV64)objcopy -O binary $< $@

# --- test payloads (stand-ins for the OS) ------------------------------

$(TEST_DIR):
	mkdir -p $@

$(TEST_DIR)/x86-boot.bin: tests/x86-boot.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

$(TEST_DIR)/arm-kernel.o: tests/arm-kernel.S | $(TEST_DIR)
	$(AARCH64)as -o $@ $<
$(TEST_DIR)/arm-kernel.elf: $(TEST_DIR)/arm-kernel.o
	$(AARCH64)ld -Ttext=0x40200000 -o $@ $<
$(TEST_DIR)/arm-kernel.bin: $(TEST_DIR)/arm-kernel.elf
	$(AARCH64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-kernel.o: tests/riscv-kernel.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-kernel.elf: $(TEST_DIR)/riscv-kernel.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-kernel.bin: $(TEST_DIR)/riscv-kernel.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-ecall.o: tests/riscv-ecall.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-ecall.elf: $(TEST_DIR)/riscv-ecall.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-ecall.bin: $(TEST_DIR)/riscv-ecall.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-deleg.o: tests/riscv-deleg.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-deleg.elf: $(TEST_DIR)/riscv-deleg.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-deleg.bin: $(TEST_DIR)/riscv-deleg.elf
	$(RISCV64)objcopy -O binary $< $@

test: all $(TEST_BINS)
	tests/smoke.sh

clean:
	rm -f $(X86_ROM) \
	      arm/squirrel-bios.o arm/squirrel-bios.elf $(ARM_BIN) \
	      riscv/squirrel-bios.o riscv/squirrel-bios.elf $(RISCV_BIN)
	rm -rf build
