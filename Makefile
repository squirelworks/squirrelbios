# SquirrelBIOS build
#
#   make            build all four firmware images
#   make PLATFORM=x86-pc CONFIG=default
#                   build one platform from configs/<platform>/<config>.cfg
#                   into build/<platform>/<config>/ (see configs/README.md)
#   make test       build, then boot each one under QEMU (tests/smoke.sh)
#   make linux-shell  boot a downloaded Linux kernel on riscv to a busybox shell
#   make linux-big    the same on a large virtual node (64 harts, 64 GB)
#   make clean      remove everything the build and tests produce
#
# Cross tools can be overridden, e.g. make AARCH64=aarch64-elf-

NASM    ?= nasm
CC64    ?= gcc
AARCH64 ?= aarch64-linux-gnu-
RISCV64 ?= riscv64-linux-gnu-

X86_ROM   := x86/squirrel-bios.rom
# The x86 modules squirrel-bios.asm includes (core/, boot/, drivers/, tables/)
X86_SRCS  := $(wildcard x86/*/*.inc x86/*/*/*.inc)
ARM_BIN   := arm/squirrel-bios.bin
RISCV_BIN := riscv/squirrel-bios.bin
# x86-64 UEFI-class firmware (roadmap C1): a 1 MB flash image, its body
# (C and NASM) linked to run from RAM at X64_FW_BASE, and the 4 KB reset
# stub (x86_64/reset/reset.asm) at its top
X64_FD      := x86_64/squirrel-bios.fd
X64_C       := $(wildcard x86_64/*/*.c)
X64_H       := $(wildcard x86_64/include/*.h)
X64_FLASH   := 0x100000
X64_FW_BASE := 0x200000
X64_CFLAGS  := -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
               -fno-pic -fno-pie -no-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
               -mno-red-zone -mgeneral-regs-only -nostdlib -static -Ix86_64/include

# AArch64 UEFI-class firmware (roadmap D1): the shared UEFI core in
# common/efi plus arm/uefi, linked to run from RAM at ARM_FW_BASE (start.S
# copies it there from flash at 0)
ARM_FD      := arm/uefi/squirrel-bios.fd
ARM_FW_BASE := 0x40100000
AARCH64_CC  ?= $(AARCH64)gcc
EFI_C       := $(wildcard common/efi/*.c)
EFI_H       := $(wildcard common/efi/include/*.h)
EFI_CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
               -fno-pic -fno-pie -no-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
               -nostdlib -static -Icommon/efi/include
ARM_EFI_CFLAGS := $(EFI_CFLAGS) -mgeneral-regs-only -mstrict-align -mno-outline-atomics -Iarm/uefi
# RISC-V UEFI-class firmware (roadmap D3): the SBI (riscv/squirrel-bios.S
# with UEFI=1) padded to 2 MB, then the shared UEFI core plus riscv/uefi,
# linked at RV_FW_BASE and run in S-mode
RV_UEFI_BIN := riscv/uefi/squirrel-bios.bin
RV_FW_BASE  := 0x80200000
RISCV64_CC  ?= $(RISCV64)gcc
RV_EFI_CFLAGS := $(EFI_CFLAGS) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
                 -mno-relax -mstrict-align -Iriscv/uefi

TEST_DIR  := build/tests
TEST_BINS := $(TEST_DIR)/x86-boot.bin $(TEST_DIR)/x86-one-sector.bin $(TEST_DIR)/x86-bad-sector.bin \
             $(TEST_DIR)/x86-extra.bin $(TEST_DIR)/x86-extra-map.bin \
             $(TEST_DIR)/x86-extra-boot.bin $(TEST_DIR)/x86-extra-range.bin \
             $(TEST_DIR)/x86-osload.bin $(TEST_DIR)/x86-int13.bin \
             $(TEST_DIR)/x86-bda.bin $(TEST_DIR)/x86-a20.bin \
             $(TEST_DIR)/x86-pit.bin $(TEST_DIR)/x86-kbd.bin $(TEST_DIR)/x86-kbd-many.bin $(TEST_DIR)/x86-mouse.bin \
             $(TEST_DIR)/x86-vbe.bin $(TEST_DIR)/x86-vbe-text.bin \
             $(TEST_DIR)/x86-time.bin $(TEST_DIR)/x86-vid.bin \
             $(TEST_DIR)/x86-write.bin $(TEST_DIR)/x86-int15a20.bin \
             $(TEST_DIR)/x86-slave.bin $(TEST_DIR)/x86-acpi.bin \
             $(TEST_DIR)/x86-smbios.bin $(TEST_DIR)/x86-bzimage.bin \
             $(TEST_DIR)/x86-optrom.bin \
             $(TEST_DIR)/x86-bzimage-big.bin $(TEST_DIR)/x86-bzimage-toobig.bin \
             $(TEST_DIR)/x86-params.bin $(TEST_DIR)/x86-params-slave.bin \
             $(TEST_DIR)/x86-floppy.img $(TEST_DIR)/x86-floppy-nosig.img \
             $(TEST_DIR)/x86-pci.bin $(TEST_DIR)/x86-edd.img \
             $(TEST_DIR)/x86-ahci.img $(TEST_DIR)/x86-nvme.img \
             $(TEST_DIR)/x86-usbdisk.img \
             $(TEST_DIR)/x86-virtio.img \
             $(TEST_DIR)/tftp/x86-netboot.bin $(TEST_DIR)/tftp/x86-netboot-2k.bin \
             $(TEST_DIR)/tftp/x86-netboot-big.bin $(TEST_DIR)/tftp/x86-netboot-huge.bin \
             $(TEST_DIR)/x86-cd.iso $(TEST_DIR)/x86-cd-slave.iso \
             $(TEST_DIR)/arm-kernel.bin $(TEST_DIR)/arm-too-big.bin \
             $(TEST_DIR)/arm-image-fixed.bin $(TEST_DIR)/arm-image-reloc.bin \
             $(TEST_DIR)/arm-image-short.bin $(TEST_DIR)/arm-initrd.img \
             $(TEST_DIR)/riscv-kernel.bin \
             $(TEST_DIR)/riscv-ecall.bin $(TEST_DIR)/riscv-deleg.bin \
             $(TEST_DIR)/riscv-sbi.bin $(TEST_DIR)/riscv-timer.bin \
             $(TEST_DIR)/riscv-ipi.bin $(TEST_DIR)/riscv-rfence.bin \
             $(TEST_DIR)/riscv-hsm.bin $(TEST_DIR)/riscv-hsm-stop.bin \
             $(TEST_DIR)/riscv-srst.bin $(TEST_DIR)/riscv-srst-reboot.bin \
             $(TEST_DIR)/riscv-srst-fail.bin $(TEST_DIR)/riscv-pmp.bin \
             $(TEST_DIR)/riscv-dt.bin $(TEST_DIR)/riscv-envcfg.bin \
             $(TEST_DIR)/riscv-ipi-smp.bin $(TEST_DIR)/riscv-fence-smp3.bin \
             $(TEST_DIR)/riscv-harts.bin \
             $(TEST_DIR)/riscv-boot-hart.bin $(TEST_DIR)/riscv-kernel-hart2.bin \
             $(TEST_DIR)/riscv-numa.bin $(TEST_DIR)/riscv-aia.bin \
             $(TEST_DIR)/riscv-ras.bin \
             $(TEST_DIR)/x86_64-app.efi $(TEST_DIR)/x86_64-app-exit.efi \
             $(TEST_DIR)/x86_64-con.efi $(TEST_DIR)/x86_64-con.keys \
             $(TEST_DIR)/x86_64-esp.img $(TEST_DIR)/x86_64-gop.efi \
             $(TEST_DIR)/arm-efi-app.efi $(TEST_DIR)/arm-efi-app-exit.efi \
             $(TEST_DIR)/arm-efi-app-ebs.efi $(TEST_DIR)/arm-efi-initrd.img \
             $(TEST_DIR)/riscv-efi-app.efi $(TEST_DIR)/riscv-efi-app-exit.efi \
             $(TEST_DIR)/riscv-efi-app-ebs.efi

.PHONY: all x86 x86_64 arm arm-uefi riscv riscv-uefi test linux-shell linux-arm linux-riscv-uefi linux-x86 linux-big linux-stress os-boot clean
.DELETE_ON_ERROR:

PLATFORMS := x86-pc x86_64-uefi aarch64-virt riscv64-virt aarch64-uefi riscv64-uefi
PLATFORM  ?=
CONFIG    ?= default
OUT_x86-pc       := squirrel-bios.rom
OUT_x86_64-uefi  := squirrel-bios.fd
OUT_aarch64-virt := squirrel-bios.bin
OUT_riscv64-virt := squirrel-bios.bin
OUT_aarch64-uefi := squirrel-bios.fd
OUT_riscv64-uefi := squirrel-bios.bin

ifeq ($(PLATFORM),)
all: x86 x86_64 arm riscv arm-uefi riscv-uefi
else
ifeq ($(filter $(PLATFORM),$(PLATFORMS)),)
$(error PLATFORM must be one of: $(PLATFORMS))
endif
ifeq ($(wildcard configs/$(PLATFORM)/$(CONFIG).cfg),)
$(error configs/$(PLATFORM)/$(CONFIG).cfg does not exist)
endif
all: build/$(PLATFORM)/$(CONFIG)/$(OUT_$(PLATFORM))
endif

x86: $(X86_ROM)
x86_64: $(X64_FD)
arm: $(ARM_BIN)
arm-uefi: $(ARM_FD)
riscv-uefi: $(RV_UEFI_BIN)
riscv: $(RISCV_BIN)

# --- firmware ---------------------------------------------------------

$(X86_ROM): x86/squirrel-bios.asm $(X86_SRCS)
	$(NASM) -f bin -i x86/ $< -o $@

# x86_64-uefi: $(1) is the directory for the pieces, $(2) extra C flags.
# The body must leave room for the 4 KB stub.
define X64_BUILD
	@mkdir -p $(1)
	$(NASM) -f elf64 x86_64/core/start.asm -o $(1)/start.o
	$(CC64) $(X64_CFLAGS) $(2) -Wl,-T,x86_64/link.ld -Wl,--defsym,FW_BASE=$(X64_FW_BASE) \
	    -Wl,--build-id=none -Wl,--no-warn-rwx-segments $(1)/start.o $(X64_C) -o $(1)/body.elf
	objcopy -O binary $(1)/body.elf $(1)/body.bin
	@size=$$(stat -c %s $(1)/body.bin); \
	if [ $$size -gt $$(($(X64_FLASH) - 0x1000)) ]; then \
	    echo "x86_64: body is $$size bytes, past the flash image" >&2; exit 1; \
	fi
	$(NASM) -f bin -DFLASH_SIZE=$(X64_FLASH) -DFW_BASE=$(X64_FW_BASE) x86_64/reset/reset.asm -o $(1)/reset.bin
	cp $(1)/body.bin $@.tmp
	truncate -s $$(($(X64_FLASH) - 0x1000)) $@.tmp
	cat $(1)/reset.bin >> $@.tmp
	mv $@.tmp $@
endef

$(X64_FD): x86_64/core/start.asm x86_64/reset/reset.asm x86_64/link.ld $(X64_C) $(X64_H)
	$(call X64_BUILD,build/x86_64,)

arm/squirrel-bios.o: arm/squirrel-bios.S
	$(AARCH64)as -o $@ $<
arm/squirrel-bios.elf: arm/squirrel-bios.o
	$(AARCH64)ld -Ttext=0x0 -o $@ $<
$(ARM_BIN): arm/squirrel-bios.elf
	$(AARCH64)objcopy -O binary $< $@

# aarch64-uefi: $(1) is the directory for the pieces, $(2) extra C flags
define ARM_EFI_BUILD
	@mkdir -p $(1)
	$(AARCH64_CC) $(ARM_EFI_CFLAGS) $(2) -c arm/uefi/start.S -o $(1)/start.o
	$(AARCH64_CC) $(ARM_EFI_CFLAGS) $(2) -Wl,-T,arm/uefi/link.ld -Wl,--defsym,FW_BASE=$(ARM_FW_BASE) \
	    -Wl,--build-id=none -Wl,--no-warn-rwx-segments $(1)/start.o arm/uefi/arch.c $(EFI_C) -o $(1)/body.elf
	$(AARCH64)objcopy -O binary $(1)/body.elf $@
endef

$(ARM_FD): arm/uefi/start.S arm/uefi/arch.c arm/uefi/arch.h arm/uefi/link.ld $(EFI_C) $(EFI_H)
	$(call ARM_EFI_BUILD,build/aarch64-uefi,)

riscv/squirrel-bios.o: riscv/squirrel-bios.S
	$(RISCV64)as -o $@ $<
riscv/squirrel-bios.elf: riscv/squirrel-bios.o
	$(RISCV64)ld -Ttext=0x80000000 -o $@ $<
# riscv64-uefi: $(1) is the directory for the pieces, $(2) extra C flags,
# $(3) extra assembler flags for the SBI. The SBI (with .bss) must end
# below RV_FW_BASE, and the core (with .bss) 2 MB later, where QEMU puts
# -kernel (the next 2 MB boundary after the image file)
define RV_EFI_BUILD
	@mkdir -p $(1)
	$(RISCV64)as --defsym UEFI=1 $(3) -o $(1)/sbi.o riscv/squirrel-bios.S
	$(RISCV64)ld -Ttext=0x80000000 -o $(1)/sbi.elf $(1)/sbi.o
	@end=$$($(RISCV64)nm $(1)/sbi.elf | awk '$$3 == "_end" { print $$1 }'); \
	if [ $$((0x$$end)) -gt $$(($(RV_FW_BASE))) ]; then \
	    echo "riscv-uefi: the SBI ends at 0x$$end, past $(RV_FW_BASE)" >&2; exit 1; \
	fi
	$(RISCV64)objcopy -O binary $(1)/sbi.elf $(1)/sbi.bin
	$(RISCV64_CC) $(RV_EFI_CFLAGS) $(2) -c riscv/uefi/start.S -o $(1)/start.o
	$(RISCV64_CC) $(RV_EFI_CFLAGS) $(2) -Wl,-T,riscv/uefi/link.ld -Wl,--defsym,FW_BASE=$(RV_FW_BASE) \
	    -Wl,--build-id=none -Wl,--no-warn-rwx-segments $(1)/start.o riscv/uefi/arch.c $(EFI_C) -o $(1)/body.elf
	@end=$$($(RISCV64)nm $(1)/body.elf | awk '$$3 == "__fw_end" { print $$1 }'); \
	if [ $$((0x$$end)) -gt $$(($(RV_FW_BASE) + 0x200000)) ]; then \
	    echo "riscv-uefi: the UEFI core ends at 0x$$end, past 2 MB" >&2; exit 1; \
	fi
	$(RISCV64)objcopy -O binary $(1)/body.elf $(1)/body.bin
	cp $(1)/sbi.bin $@.tmp
	truncate -s $$(($(RV_FW_BASE) - 0x80000000)) $@.tmp
	cat $(1)/body.bin >> $@.tmp
	mv $@.tmp $@
endef

$(RV_UEFI_BIN): riscv/squirrel-bios.S riscv/uefi/start.S riscv/uefi/arch.c riscv/uefi/arch.h \
		riscv/uefi/link.ld $(EFI_C) $(EFI_H)
	$(call RV_EFI_BUILD,build/riscv64-uefi,,)

# The image, .bss included, must fit the 2 MB region PMP entry 0 denies to
# S-mode (FW_BASE and FW_SIZE in squirrel-bios.S)
$(RISCV_BIN): riscv/squirrel-bios.elf
	@end=$$($(RISCV64)nm $< | awk '$$3 == "_end" { print $$1 }'); \
	if [ $$((0x$$end)) -gt $$((0x80200000)) ]; then \
	    echo "riscv: image ends at 0x$$end, past the PMP-protected region" >&2; exit 1; \
	fi
	$(RISCV64)objcopy -O binary $< $@

# --- configured builds (make PLATFORM=... CONFIG=...) ------------------
# Same sources as above, plus the config.inc tools/mkconfig.sh makes from
# configs/<platform>/<config>.cfg. Every key defaults in the source to
# what default.cfg says, so the default build matches plain make.

.SECONDARY:

build/x86-pc/%/config.inc: configs/x86-pc/%.cfg tools/mkconfig.sh
	@mkdir -p $(@D)
	tools/mkconfig.sh nasm $< > $@
build/aarch64-virt/%/config.inc: configs/aarch64-virt/%.cfg tools/mkconfig.sh
	@mkdir -p $(@D)
	tools/mkconfig.sh gas $< > $@
build/riscv64-virt/%/config.inc: configs/riscv64-virt/%.cfg tools/mkconfig.sh
	@mkdir -p $(@D)
	tools/mkconfig.sh gas $< > $@
build/%/config.h: configs/%.cfg tools/mkconfig.sh
	@mkdir -p $(@D)
	tools/mkconfig.sh c $< > $@

build/x86-pc/%/squirrel-bios.rom: x86/squirrel-bios.asm $(X86_SRCS) \
		build/x86-pc/%/config.inc build/x86-pc/%/config.h
	$(NASM) -f bin -DSQ_CONFIG -i $(@D)/ -i x86/ $< -o $@

build/x86_64-uefi/%/squirrel-bios.fd: x86_64/core/start.asm x86_64/reset/reset.asm x86_64/link.ld \
		$(X64_C) $(X64_H) build/x86_64-uefi/%/config.h
	$(call X64_BUILD,$(@D)/obj,-DSQ_CONFIG -I$(@D))

build/aarch64-virt/%/squirrel-bios.o: arm/squirrel-bios.S build/aarch64-virt/%/config.inc build/aarch64-virt/%/config.h
	$(AARCH64)as --defsym SQ_CONFIG=1 -I $(@D) -o $@ $<
build/aarch64-virt/%/squirrel-bios.elf: build/aarch64-virt/%/squirrel-bios.o
	$(AARCH64)ld -Ttext=0x0 -o $@ $<
build/aarch64-virt/%/squirrel-bios.bin: build/aarch64-virt/%/squirrel-bios.elf
	$(AARCH64)objcopy -O binary $< $@

build/aarch64-uefi/%/squirrel-bios.fd: arm/uefi/start.S arm/uefi/arch.c arm/uefi/arch.h \
		arm/uefi/link.ld $(EFI_C) $(EFI_H) build/aarch64-uefi/%/config.h
	$(call ARM_EFI_BUILD,$(@D)/obj,-DSQ_CONFIG -I$(@D))

build/riscv64-uefi/%/config.inc: configs/riscv64-uefi/%.cfg tools/mkconfig.sh
	@mkdir -p $(@D)
	tools/mkconfig.sh gas $< > $@
build/riscv64-uefi/%/squirrel-bios.bin: riscv/squirrel-bios.S riscv/uefi/start.S riscv/uefi/arch.c \
		riscv/uefi/arch.h riscv/uefi/link.ld $(EFI_C) $(EFI_H) \
		build/riscv64-uefi/%/config.inc build/riscv64-uefi/%/config.h
	$(call RV_EFI_BUILD,$(@D)/obj,-DSQ_CONFIG -I$(@D),--defsym SQ_CONFIG=1 -I $(@D))

build/riscv64-virt/%/squirrel-bios.o: riscv/squirrel-bios.S build/riscv64-virt/%/config.inc build/riscv64-virt/%/config.h
	$(RISCV64)as --defsym SQ_CONFIG=1 -I $(@D) -o $@ $<
build/riscv64-virt/%/squirrel-bios.elf: build/riscv64-virt/%/squirrel-bios.o
	$(RISCV64)ld -Ttext=0x80000000 -o $@ $<
build/riscv64-virt/%/squirrel-bios.bin: build/riscv64-virt/%/squirrel-bios.elf
	@end=$$($(RISCV64)nm $< | awk '$$3 == "_end" { print $$1 }'); \
	if [ $$((0x$$end)) -gt $$((0x80200000)) ]; then \
	    echo "riscv: image ends at 0x$$end, past the PMP-protected region" >&2; exit 1; \
	fi
	$(RISCV64)objcopy -O binary $< $@

# Each platform's default.cfg must build the same bytes as plain make
.PHONY: config-default
config-default: x86 x86_64 arm riscv arm-uefi riscv-uefi build/x86-pc/default/squirrel-bios.rom \
		build/aarch64-uefi/default/squirrel-bios.fd build/riscv64-uefi/default/squirrel-bios.bin \
		build/x86_64-uefi/default/squirrel-bios.fd \
		build/aarch64-virt/default/squirrel-bios.bin build/riscv64-virt/default/squirrel-bios.bin \
		build/riscv64-virt/small/squirrel-bios.bin build/x86-pc/minimal/squirrel-bios.rom
	cmp $(X86_ROM) build/x86-pc/default/squirrel-bios.rom
	cmp $(X64_FD) build/x86_64-uefi/default/squirrel-bios.fd
	cmp $(ARM_BIN) build/aarch64-virt/default/squirrel-bios.bin
	cmp $(RISCV_BIN) build/riscv64-virt/default/squirrel-bios.bin
	cmp $(ARM_FD) build/aarch64-uefi/default/squirrel-bios.fd
	cmp $(RV_UEFI_BIN) build/riscv64-uefi/default/squirrel-bios.bin
	@echo "config-default: default.cfg builds match plain make"

# --- test payloads (stand-ins for the OS) ------------------------------

$(TEST_DIR):
	mkdir -p $@

$(TEST_DIR)/x86-boot.bin: tests/x86-boot.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# Sector 1 alone, without the SQBL header or the VGA check
$(TEST_DIR)/x86-one-sector.bin: tests/x86-boot.asm | $(TEST_DIR)
	$(NASM) -f bin -DONE_SECTOR $< -o $@

# The same sector with its last word zeroed instead of 0xAA55
$(TEST_DIR)/x86-bad-sector.bin: tests/x86-boot.asm | $(TEST_DIR)
	$(NASM) -f bin -DNO_SIG $< -o $@

# SQBL header: a good load, then one per way the firmware must refuse it
$(TEST_DIR)/x86-extra.bin: tests/x86-extra.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
$(TEST_DIR)/x86-extra-map.bin: tests/x86-extra.asm | $(TEST_DIR)
	$(NASM) -f bin -DOVERLAP_MAP $< -o $@
$(TEST_DIR)/x86-extra-boot.bin: tests/x86-extra.asm | $(TEST_DIR)
	$(NASM) -f bin -DOVERLAP_BOOT $< -o $@
$(TEST_DIR)/x86-extra-range.bin: tests/x86-extra.asm | $(TEST_DIR)
	$(NASM) -f bin -DOUT_OF_RANGE $< -o $@

# x86_64-uefi: a hand-written PE32+ EFI application, returning and calling Exit
$(TEST_DIR)/x86_64-app.efi: tests/x86_64-app.asm tests/x86_64-pe.inc | $(TEST_DIR)
	$(NASM) -f bin -i tests/ $< -o $@
$(TEST_DIR)/x86_64-app-exit.efi: tests/x86_64-app.asm tests/x86_64-pe.inc | $(TEST_DIR)
	$(NASM) -f bin -i tests/ -DEXIT $< -o $@
# The console app and what is typed to it: a, b, up, delete, q
$(TEST_DIR)/x86_64-con.efi: tests/x86_64-con.asm tests/x86_64-pe.inc | $(TEST_DIR)
	$(NASM) -f bin -i tests/ $< -o $@
$(TEST_DIR)/x86_64-con.keys: | $(TEST_DIR)
	printf 'ab\033[A\033[3~q' > $@
$(TEST_DIR)/x86_64-gop.efi: tests/x86_64-gop.asm tests/x86_64-pe.inc | $(TEST_DIR)
	$(NASM) -f bin -i tests/ $< -o $@

# aarch64-uefi: the stand-in EFI application in C (tests/efi-app.c),
# linked at 256 MB + 4 KB and wrapped as PE32+ by tests/mkpe.py; it
# returns, calls Exit, or calls ExitBootServices and powers off
EFI_APP_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic \
                  -fno-pie -no-pie -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib -static \
                  -Wl,-T,tests/efi-app.ld -Wl,-q -Wl,--build-id=none
ARM_APP_CFLAGS := $(EFI_APP_CFLAGS) -mgeneral-regs-only -mstrict-align
$(TEST_DIR)/arm-efi-app.efi: tests/efi-app.c tests/efi-app.ld tests/mkpe.py | $(TEST_DIR)
	$(AARCH64_CC) $(ARM_APP_CFLAGS) $< -o $(@:.efi=.elf)
	python3 tests/mkpe.py aarch64 $(@:.efi=.elf) $@
$(TEST_DIR)/arm-efi-app-exit.efi: tests/efi-app.c tests/efi-app.ld tests/mkpe.py | $(TEST_DIR)
	$(AARCH64_CC) $(ARM_APP_CFLAGS) -DEXIT $< -o $(@:.efi=.elf)
	python3 tests/mkpe.py aarch64 $(@:.efi=.elf) $@
$(TEST_DIR)/arm-efi-app-ebs.efi: tests/efi-app.c tests/efi-app.ld tests/mkpe.py | $(TEST_DIR)
	$(AARCH64_CC) $(ARM_APP_CFLAGS) -DEBS $< -o $(@:.efi=.elf)
	python3 tests/mkpe.py aarch64 $(@:.efi=.elf) $@
# riscv64-uefi: the same app for RISC-V
RV_APP_CFLAGS := $(EFI_APP_CFLAGS) -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -mno-relax -mstrict-align
$(TEST_DIR)/riscv-efi-app.efi: tests/efi-app.c tests/efi-app.ld tests/mkpe.py | $(TEST_DIR)
	$(RISCV64_CC) $(RV_APP_CFLAGS) $< -o $(@:.efi=.elf)
	python3 tests/mkpe.py riscv64 $(@:.efi=.elf) $@
$(TEST_DIR)/riscv-efi-app-exit.efi: tests/efi-app.c tests/efi-app.ld tests/mkpe.py | $(TEST_DIR)
	$(RISCV64_CC) $(RV_APP_CFLAGS) -DEXIT $< -o $(@:.efi=.elf)
	python3 tests/mkpe.py riscv64 $(@:.efi=.elf) $@
$(TEST_DIR)/riscv-efi-app-ebs.efi: tests/efi-app.c tests/efi-app.ld tests/mkpe.py | $(TEST_DIR)
	$(RISCV64_CC) $(RV_APP_CFLAGS) -DEBS $< -o $(@:.efi=.elf)
	python3 tests/mkpe.py riscv64 $(@:.efi=.elf) $@
# An -initrd for the app started as -kernel: 3000 bytes starting "Sq"
$(TEST_DIR)/arm-efi-initrd.img: | $(TEST_DIR)
	python3 -c 'import sys; sys.stdout.buffer.write(b"Sq" + bytes(2998))' > $@

# GPT disk with a FAT32 ESP holding \\EFI\\BOOT\\BOOTX64.EFI (the app)
$(TEST_DIR)/x86_64-esp.img: tests/mkesp.py $(TEST_DIR)/x86_64-app.efi | $(TEST_DIR)
	python3 tests/mkesp.py $(TEST_DIR)/x86_64-app.efi $@

# Boot sector plus a 96 KB 'S2OK' image, the shape of Squirrel-OS/2's disk
$(TEST_DIR)/x86-osload.bin: tests/x86-osload.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# Boot sector that loads 192 sectors with INT 13h, the calls Squirrel-OS/2 makes
$(TEST_DIR)/x86-int13.bin: tests/x86-int13.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# BIOS data area: equipment list, conventional KB, cursor in 80x25
$(TEST_DIR)/x86-bda.bin: tests/x86-bda.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# A20 gate open: high write at 0x500000 and 0x100500 vs 0x000500
$(TEST_DIR)/x86-a20.bin: tests/x86-a20.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# PIT ~18.2 Hz and PIC IRQ0: BDA tick at 0x046C advances
$(TEST_DIR)/x86-pit.bin: tests/x86-pit.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# INT 16h keyboard; tests/x86-kbd-run.sh injects a key via the monitor
$(TEST_DIR)/x86-kbd.bin: tests/x86-kbd.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# INT 15h C2xx with a USB mouse; tests/x86-kbd-run.sh moves it
# VBE: a 640x480x32 frame buffer, then (-DTEXT) back to text mode
$(TEST_DIR)/x86-vbe.bin: tests/x86-vbe.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
$(TEST_DIR)/x86-vbe-text.bin: tests/x86-vbe.asm | $(TEST_DIR)
	$(NASM) -f bin -DTEXT $< -o $@
$(TEST_DIR)/x86-mouse.bin: tests/x86-mouse.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# The same, waiting for 20 keys (a USB keyboard's ring wraps)
$(TEST_DIR)/x86-kbd-many.bin: tests/x86-kbd.asm | $(TEST_DIR)
	$(NASM) -f bin -DKEYS=20 $< -o $@

# INT 1Ah: tick count and RTC time (BCD)
$(TEST_DIR)/x86-time.bin: tests/x86-time.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# INT 10h text: cursor, scroll, get mode
$(TEST_DIR)/x86-vid.bin: tests/x86-vid.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# INT 13h AH=03/43 disk writes
$(TEST_DIR)/x86-write.bin: tests/x86-write.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# INT 15h AX=2400/2401/2402 A20 control
$(TEST_DIR)/x86-int15a20.bin: tests/x86-int15a20.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# One-sector hand-off expecting DL=0x81 (image on primary slave only)
$(TEST_DIR)/x86-slave.bin: tests/x86-boot.asm | $(TEST_DIR)
	$(NASM) -f bin -DONE_SECTOR -DEXPECT_DL=0x81 $< -o $@

# El Torito: a 64-block no-emulation CD built around the x86-cd image
$(TEST_DIR)/x86-cd.bin: tests/x86-cd.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
$(TEST_DIR)/x86-cd.iso: $(TEST_DIR)/x86-cd.bin tests/mkiso.py
	tests/mkiso.py $< $@
# The same, expecting to be the primary slave
$(TEST_DIR)/x86-cd-slave.bin: tests/x86-cd.asm | $(TEST_DIR)
	$(NASM) -f bin -DCD_CTRL=0 -DCD_DEV=1 $< -o $@
$(TEST_DIR)/x86-cd-slave.iso: $(TEST_DIR)/x86-cd-slave.bin tests/mkiso.py
	tests/mkiso.py $< $@

# EDD 3.0 / LBA48: a sparse 200 GiB disk (a few KB on disk) with tags
# either side of 2^28 sectors and at 143 GiB
$(TEST_DIR)/x86-edd.bin: tests/x86-edd.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
$(TEST_DIR)/x86-edd.img: $(TEST_DIR)/x86-edd.bin tests/mkbigdisk.py
	tests/mkbigdisk.py $< $@ 200 0x0FFFFFFF 0x10000000 300000000
# The same on q35's AHCI controller, port 4
$(TEST_DIR)/x86-ahci.bin: tests/x86-edd.asm | $(TEST_DIR)
	$(NASM) -f bin -DAHCI=4 $< -o $@
$(TEST_DIR)/x86-ahci.img: $(TEST_DIR)/x86-ahci.bin tests/mkbigdisk.py
	tests/mkbigdisk.py $< $@ 200 0x0FFFFFFF 0x10000000 300000000
# The same as namespace 1 of an NVMe controller in PCI slot 5
$(TEST_DIR)/x86-nvme.bin: tests/x86-edd.asm | $(TEST_DIR)
	$(NASM) -f bin -DNVME=5 $< -o $@
$(TEST_DIR)/x86-nvme.img: $(TEST_DIR)/x86-nvme.bin tests/mkbigdisk.py
	tests/mkbigdisk.py $< $@ 200 0x0FFFFFFF 0x10000000 300000000
# The same as a USB disk on an xHCI in PCI slot 5
$(TEST_DIR)/x86-usbdisk.bin: tests/x86-edd.asm | $(TEST_DIR)
	$(NASM) -f bin -DUSB=5 $< -o $@
$(TEST_DIR)/x86-usbdisk.img: $(TEST_DIR)/x86-usbdisk.bin tests/mkbigdisk.py
	tests/mkbigdisk.py $< $@ 200 0x0FFFFFFF 0x10000000 300000000

# Network boot files, served by QEMU's TFTP server from $(TEST_DIR)/tftp:
# 5000 bytes (a short last block), 2048 (an empty last block), 200000
# (past block 255), and 240000 (more than fits below 0x40000)
$(TEST_DIR)/tftp:
	mkdir -p $@
$(TEST_DIR)/tftp/x86-netboot.bin: tests/x86-netboot.asm | $(TEST_DIR)/tftp
	$(NASM) -f bin -DSIZE=5000 $< -o $@
$(TEST_DIR)/tftp/x86-netboot-2k.bin: tests/x86-netboot.asm | $(TEST_DIR)/tftp
	$(NASM) -f bin -DSIZE=2048 $< -o $@
$(TEST_DIR)/tftp/x86-netboot-big.bin: tests/x86-netboot.asm | $(TEST_DIR)/tftp
	$(NASM) -f bin -DSIZE=200000 $< -o $@
$(TEST_DIR)/tftp/x86-netboot-huge.bin: tests/x86-netboot.asm | $(TEST_DIR)/tftp
	$(NASM) -f bin -DSIZE=240000 $< -o $@

# The same as a virtio-blk device in PCI slot 5
$(TEST_DIR)/x86-virtio.bin: tests/x86-edd.asm | $(TEST_DIR)
	$(NASM) -f bin -DVIRTIO=5 $< -o $@
$(TEST_DIR)/x86-virtio.img: $(TEST_DIR)/x86-virtio.bin tests/mkbigdisk.py
	tests/mkbigdisk.py $< $@ 200 0x0FFFFFFF 0x10000000 300000000

# PCI BIOS (INT 1Ah B1xx), BAR placement, IRQ routing and $PIR
$(TEST_DIR)/x86-pci.bin: tests/x86-pci.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# ACPI RSDP in the ROM area: checksum and RSDT→FACP
$(TEST_DIR)/x86-acpi.bin: tests/x86-acpi.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# SMBIOS entry point in the ROM area: _SM_ checksums and Type 0
$(TEST_DIR)/x86-smbios.bin: tests/x86-smbios.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# Minimal bzImage (HdrS) with a 32-bit payload at 1 MB
$(TEST_DIR)/x86-bzimage.bin: tests/x86-bzimage.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

# The same with a 300 KB payload (past the old 64 KB limit), and one
# whose syssize claims 64 MB so the firmware must refuse it
$(TEST_DIR)/x86-bzimage-big.bin: tests/x86-bzimage.asm | $(TEST_DIR)
	$(NASM) -f bin -DBIG $< -o $@
$(TEST_DIR)/x86-bzimage-toobig.bin: tests/x86-bzimage.asm | $(TEST_DIR)
	$(NASM) -f bin -DTOOBIG $< -o $@

# INT 13h AH=08h with the slave absent, and with a slave present
$(TEST_DIR)/x86-params.bin: tests/x86-params.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
$(TEST_DIR)/x86-params-slave.bin: tests/x86-params.asm | $(TEST_DIR)
	$(NASM) -f bin -DSLAVE $< -o $@

# 1.44 MB diskettes: the floppy stand-in in C0 H0 S1, every other sector
# tagged with its LBA; the -nosig one has no 0xAA55 (the hard disk boots)
$(TEST_DIR)/x86-floppy.bin: tests/x86-floppy.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
$(TEST_DIR)/x86-floppy.img: $(TEST_DIR)/x86-floppy.bin tests/mkfloppy.py
	tests/mkfloppy.py $< $@
$(TEST_DIR)/x86-floppy-nosig.img: $(TEST_DIR)/x86-floppy.bin tests/mkfloppy.py
	tests/mkfloppy.py $< $@ --nosig

# Legacy option ROM (55 AA); last byte patched so the 512-byte sum is 0
$(TEST_DIR)/x86-optrom.bin: tests/x86-optrom.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@
	@python3 -c "p='$@'; d=bytearray(open(p,'rb').read()); assert len(d)==512; d[511]=0; d[511]=(-sum(d))&0xff; open(p,'wb').write(d)"

$(TEST_DIR)/arm-kernel.o: tests/arm-kernel.S | $(TEST_DIR)
	$(AARCH64)as -o $@ $<
$(TEST_DIR)/arm-kernel.elf: $(TEST_DIR)/arm-kernel.o
	$(AARCH64)ld -Ttext=0x40200000 -o $@ $<
$(TEST_DIR)/arm-kernel.bin: $(TEST_DIR)/arm-kernel.elf
	$(AARCH64)objcopy -O binary $< $@

# 31 MB of zeros (sparse): with -m 32 it runs 1 MB past the end of RAM
# when loaded at 0x40200000. Built here, never committed.
$(TEST_DIR)/arm-too-big.bin: | $(TEST_DIR)
	truncate -s 31M $@

# Image header: fixed placement (RAM base + text_offset), relocatable, and
# an image_size smaller than the file
$(TEST_DIR)/arm-image-fixed.o: tests/arm-image.S | $(TEST_DIR)
	$(AARCH64)as -o $@ $<
$(TEST_DIR)/arm-image-reloc.o: tests/arm-image.S | $(TEST_DIR)
	$(AARCH64)as --defsym RELOC=1 -o $@ $<
$(TEST_DIR)/arm-image-short.o: tests/arm-image.S | $(TEST_DIR)
	$(AARCH64)as --defsym SHORT=1 -o $@ $<
$(TEST_DIR)/arm-image-fixed.elf: $(TEST_DIR)/arm-image-fixed.o
	$(AARCH64)ld -Ttext=0x40400000 -o $@ $<
$(TEST_DIR)/arm-image-reloc.elf $(TEST_DIR)/arm-image-short.elf: %.elf: %.o
	$(AARCH64)ld -Ttext=0x40200000 -o $@ $<
# A small initrd for arm-initrd; its contents do not matter to the firmware
$(TEST_DIR)/arm-initrd.img: | $(TEST_DIR)
	printf 'SquirrelBIOS test initrd\n' > $@

ARM_IMAGES := $(TEST_DIR)/arm-image-fixed $(TEST_DIR)/arm-image-reloc $(TEST_DIR)/arm-image-short
$(ARM_IMAGES:=.bin): %.bin: %.elf
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

$(TEST_DIR)/riscv-fence-smp3.o: tests/riscv-fence-smp3.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-fence-smp3.elf: $(TEST_DIR)/riscv-fence-smp3.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-fence-smp3.bin: $(TEST_DIR)/riscv-fence-smp3.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-harts.o: tests/riscv-harts.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-harts.elf: $(TEST_DIR)/riscv-harts.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-harts.bin: $(TEST_DIR)/riscv-harts.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-aia.o: tests/riscv-aia.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-aia.elf: $(TEST_DIR)/riscv-aia.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-aia.bin: $(TEST_DIR)/riscv-aia.elf
	$(RISCV64)objcopy -O binary $< $@

# riscv-numa: riscv-harts for 4 harts
$(TEST_DIR)/riscv-numa.o: tests/riscv-harts.S | $(TEST_DIR)
	$(RISCV64)as --defsym HARTS=4 -o $@ $<
$(TEST_DIR)/riscv-numa.elf: $(TEST_DIR)/riscv-numa.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-numa.bin: $(TEST_DIR)/riscv-numa.elf
	$(RISCV64)objcopy -O binary $< $@

# riscv-boot-hart: a shim that hands the firmware fw_dynamic_info with
# boot_hart = 2, and the stand-in built to expect hart 2
$(TEST_DIR)/riscv-boot-hart.o: tests/riscv-boot-hart.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-boot-hart.elf: $(TEST_DIR)/riscv-boot-hart.o
	$(RISCV64)ld -Ttext=0x81000000 -o $@ $<
$(TEST_DIR)/riscv-boot-hart.bin: $(TEST_DIR)/riscv-boot-hart.elf
	$(RISCV64)objcopy -O binary $< $@
$(TEST_DIR)/riscv-kernel-hart2.o: tests/riscv-kernel.S | $(TEST_DIR)
	$(RISCV64)as --defsym BOOT_HART=2 -o $@ $<
$(TEST_DIR)/riscv-kernel-hart2.elf: $(TEST_DIR)/riscv-kernel-hart2.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-kernel-hart2.bin: $(TEST_DIR)/riscv-kernel-hart2.elf
	$(RISCV64)objcopy -O binary $< $@

$(TEST_DIR)/riscv-ras.o: tests/riscv-ras.S | $(TEST_DIR)
	$(RISCV64)as -o $@ $<
$(TEST_DIR)/riscv-ras.elf: $(TEST_DIR)/riscv-ras.o
	$(RISCV64)ld -Ttext=0x80200000 -o $@ $<
$(TEST_DIR)/riscv-ras.bin: $(TEST_DIR)/riscv-ras.elf
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

test: x86 x86_64 arm riscv arm-uefi riscv-uefi config-default $(TEST_BINS)
	tests/smoke.sh

# Optional, not part of make test: downloads Ubuntu 24.04's arm64 kernel
# into build/linux-arm (about 20 MB, once) and boots it through its EFI
# stub on the aarch64-uefi firmware to a busybox shell
linux-arm: arm-uefi
	tests/uefi-linux.sh aarch64

# Optional, not part of make test: the same with Ubuntu 24.04's riscv64
# kernel (the package make linux-shell uses) on the riscv64-uefi firmware
linux-riscv-uefi: riscv-uefi
	tests/uefi-linux.sh riscv64

# Optional, not part of make test: downloads an Ubuntu riscv64 kernel and
# busybox into build/linux (about 40 MB, once) and checks for a shell prompt
linux-shell: riscv
	tests/linux-shell.sh

# Optional, not part of make test: downloads Ubuntu 24.04's amd64 kernel
# into build/linux-x86 (about 15 MB, once), boots its bzImage from an IDE
# disk and checks that it starts with the firmware's E820 map
linux-x86: x86
	tests/linux-x86.sh

# Optional, not part of make test. OS_IMG is a Squirrel-OS/2 hard-disk
# image (that repo's build/os.img). The OS banner is on the VGA, so the
# script reads the text buffer; serial carries the firmware line.
os-boot: x86
	tests/os-boot.sh $(OS_IMG)

# Optional, not part of make test: the first step of the large-node
# milestone in ROADMAP.md. The linux-shell boot, with Ubuntu's 6.14
# kernel (built for more harts than 6.8's 32), on a QEMU virt machine
# sized like one server node. QEMU only backs the RAM
# Linux touches, so the host needs far less than 64 GB. Passes only when
# every hart comes online; BIG_* can be lowered to try a smaller node.
# Needs zstd to unpack the 6.14 kernel.
# BIG_NUMA splits it into that many NUMA nodes (sockets, each with its own
# CLINT), the way a two-socket server is built.
# The node also gets a network card (virtio-net-pci) and an NVMe drive
# (backed by QEMU's null block driver, so no disk image is needed), and
# both must show up under /sys/bus/pci/devices by vendor:device ID.
BIG_SMP ?= 64
BIG_MEM ?= 64G
BIG_NUMA ?= 2
BIG_DEVICES := -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
               -blockdev null-co,node-name=nvme0,size=67108864 -device nvme,serial=squirrel0,drive=nvme0
BIG_WORKLOAD := for d in /sys/bus/pci/devices/*; do echo pcidev $$(cat $$d/vendor):$$(cat $$d/device); done
# virtio-net (Red Hat/virtio 1af4:1000) and NVMe (QEMU 1b36:0010)
BIG_EXPECT := pcidev 0x1af4:0x1000|pcidev 0x1b36:0x0010
linux-big: riscv
	LINUX_KERNEL=6.14 LINUX_SMP=$(BIG_SMP) LINUX_MEM=$(BIG_MEM) LINUX_EXPECT_HARTS=$(BIG_SMP) \
	    LINUX_NUMA=$(BIG_NUMA) LINUX_DEVICES='$(BIG_DEVICES)' \
	    LINUX_WORKLOAD='$(BIG_WORKLOAD)' LINUX_EXPECT='$(BIG_EXPECT)' \
	    LINUX_TIMEOUT=$${LINUX_TIMEOUT:-900} tests/linux-shell.sh

# Optional, not part of make test: repeated Linux boots on 1 to 8 harts
# with tests/smpstress.c keeping the harts fencing and interrupting each
# other (needs riscv64-linux-gnu-gcc). STRESS_HOTPLUG=1 adds CPU hotplug.
# See tests/linux-stress.sh for the knobs.
linux-stress: riscv
	tests/linux-stress.sh

clean:
	rm -f $(X86_ROM) $(X64_FD) $(ARM_FD) $(RV_UEFI_BIN) \
	      arm/squirrel-bios.o arm/squirrel-bios.elf $(ARM_BIN) \
	      riscv/squirrel-bios.o riscv/squirrel-bios.elf $(RISCV_BIN)
	rm -rf build
