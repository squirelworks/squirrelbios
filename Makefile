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
             $(TEST_DIR)/riscv-ecall.bin $(TEST_DIR)/riscv-deleg.bin \
             $(TEST_DIR)/riscv-sbi.bin $(TEST_DIR)/riscv-timer.bin \
             $(TEST_DIR)/riscv-ipi.bin $(TEST_DIR)/riscv-rfence.bin \
             $(TEST_DIR)/riscv-hsm.bin $(TEST_DIR)/riscv-hsm-stop.bin \
             $(TEST_DIR)/riscv-srst.bin $(TEST_DIR)/riscv-srst-reboot.bin \
             $(TEST_DIR)/riscv-srst-fail.bin $(TEST_DIR)/riscv-pmp.bin \
             $(TEST_DIR)/riscv-dt.bin $(TEST_DIR)/riscv-envcfg.bin \
             $(TEST_DIR)/riscv-ipi-smp.bin

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
# The image, .bss included, must fit the 2 MB region PMP entry 0 denies to
# S-mode (FW_BASE and FW_SIZE in squirrel-bios.S)
$(RISCV_BIN): riscv/squirrel-bios.elf
	@end=$$($(RISCV64)nm $< | awk '$$3 == "_end" { print $$1 }'); \
	if [ $$((0x$$end)) -gt $$((0x80200000)) ]; then \
	    echo "riscv: image ends at 0x$$end, past the PMP-protected region" >&2; exit 1; \
	fi
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

$(TEST_DIR)/riscv-sbi.o: tests/riscv-sbi.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-sbi.elf: $(TEST_DIR)/riscv-sbi.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-sbi.bin: $(TEST_DIR)/riscv-sbi.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-timer.o: tests/riscv-timer.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-timer.elf: $(TEST_DIR)/riscv-timer.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-timer.bin: $(TEST_DIR)/riscv-timer.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-ipi.o: tests/riscv-ipi.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-ipi.elf: $(TEST_DIR)/riscv-ipi.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-ipi.bin: $(TEST_DIR)/riscv-ipi.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-rfence.o: tests/riscv-rfence.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-rfence.elf: $(TEST_DIR)/riscv-rfence.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-rfence.bin: $(TEST_DIR)/riscv-rfence.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-hsm.o: tests/riscv-hsm.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-hsm.elf: $(TEST_DIR)/riscv-hsm.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-hsm.bin: $(TEST_DIR)/riscv-hsm.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-hsm-stop.o: tests/riscv-hsm-stop.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-hsm-stop.elf: $(TEST_DIR)/riscv-hsm-stop.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-hsm-stop.bin: $(TEST_DIR)/riscv-hsm-stop.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-pmp.o: tests/riscv-pmp.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-pmp.elf: $(TEST_DIR)/riscv-pmp.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-pmp.bin: $(TEST_DIR)/riscv-pmp.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-dt.o: tests/riscv-dt.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-dt.elf: $(TEST_DIR)/riscv-dt.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-dt.bin: $(TEST_DIR)/riscv-dt.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-envcfg.o: tests/riscv-envcfg.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-envcfg.elf: $(TEST_DIR)/riscv-envcfg.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-envcfg.bin: $(TEST_DIR)/riscv-envcfg.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-ipi-smp.o: tests/riscv-ipi-smp.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-ipi-smp.elf: $(TEST_DIR)/riscv-ipi-smp.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-ipi-smp.bin: $(TEST_DIR)/riscv-ipi-smp.elf
	$(RISCV64)objcopy -O binary $< $@

# One SRST image per reset: shutdown, cold reboot, shutdown for a failure
$(TEST_DIR)/riscv-srst.o: tests/riscv-srst.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-srst-reboot.o: tests/riscv-srst.S | $(TEST_DIR)
	$(RISCV64)as --defsym RESET_TYPE=1 -o $@ $<
$(TEST_DIR)/riscv-srst-fail.o: tests/riscv-srst.S | $(TEST_DIR)
	$(RISCV64)as --defsym RESET_REASON=1 -o $@ $<
SRST_IMAGES := $(TEST_DIR)/riscv-srst $(TEST_DIR)/riscv-srst-reboot $(TEST_DIR)/riscv-srst-fail
$(SRST_IMAGES:=.elf): %.elf: %.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(SRST_IMAGES:=.bin): %.bin: %.elf
	$(RISCV64)objcopy -O binary $< $@

test: all $(TEST_BINS)
	tests/smoke.sh

clean:
	rm -f $(X86_ROM) \
	      arm/squirrel-bios.o arm/squirrel-bios.elf $(ARM_BIN) \
	      riscv/squirrel-bios.o riscv/squirrel-bios.elf $(RISCV_BIN)
	rm -rf build
