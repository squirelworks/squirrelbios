# SquirrelBIOS build
#
#   make            build all three firmware images
#   make test       build, then boot each one under QEMU (tests/smoke.sh)
#   make linux-shell  boot a downloaded Linux kernel on riscv to a busybox shell
#   make linux-big    the same on a large virtual node (64 harts, 64 GB)
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
TEST_BINS := $(TEST_DIR)/x86-boot.bin $(TEST_DIR)/x86-one-sector.bin $(TEST_DIR)/x86-bad-sector.bin \
             $(TEST_DIR)/x86-extra.bin $(TEST_DIR)/x86-extra-map.bin \
             $(TEST_DIR)/x86-extra-boot.bin $(TEST_DIR)/x86-extra-range.bin \
             $(TEST_DIR)/x86-osload.bin $(TEST_DIR)/x86-int13.bin \
             $(TEST_DIR)/x86-bda.bin $(TEST_DIR)/x86-a20.bin \
             $(TEST_DIR)/x86-pit.bin $(TEST_DIR)/x86-kbd.bin \
             $(TEST_DIR)/x86-time.bin \
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
             $(TEST_DIR)/riscv-ras.bin

.PHONY: all x86 arm riscv test linux-shell linux-big linux-stress os-boot clean
.DELETE_ON_ERROR:

all: x86 arm riscv

x86: $(X86_ROM)
arm: $(ARM_BIN)
riscv: $(RISCV_BIN)

# --- firmware ---------------------------------------------------------

$(X86_ROM): x86/squirrel-bios.asm x86/memmap.inc x86/vga.inc x86/font.inc
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

# INT 1Ah: tick count and RTC time (BCD)
$(TEST_DIR)/x86-time.bin: tests/x86-time.asm | $(TEST_DIR)
	$(NASM) -f bin $< -o $@

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

test: all $(TEST_BINS)
	tests/smoke.sh

# Optional, not part of make test: downloads an Ubuntu riscv64 kernel and
# busybox into build/linux (about 40 MB, once) and checks for a shell prompt
linux-shell: riscv
	tests/linux-shell.sh

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
	rm -f $(X86_ROM) \
	      arm/squirrel-bios.o arm/squirrel-bios.elf $(ARM_BIN) \
	      riscv/squirrel-bios.o riscv/squirrel-bios.elf $(RISCV_BIN)
	rm -rf build
