# SquirrelBIOS s390x (QEMU s390-ccw-virtio, roadmap E3): build rules,
# included by the top-level Makefile. Needs an s390x cross gcc (Ubuntu:
# gcc-s390x-linux-gnu); without one the platform is skipped and its make
# test cases report SKIP.

all: $(if $(PLATFORM),,s390x)

S390X      ?= s390x-linux-gnu-
HAVE_S390X := $(shell command -v $(S390X)gcc >/dev/null 2>&1 && echo y)
S390X_ELF  := s390x/squirrel-bios.elf
S390X_SRCS := s390x/start.S s390x/main.c common/fw/string.c common/fw/printf.c \
              common/fw/elf.c common/fw/prompt.c
S390X_DEPS := $(S390X_SRCS) s390x/link.ld common/fw/fwlib.h s390x/platform.mk
# z/Architecture, no floating point or vector code (the OS owns those
# registers), static. The firmware is a position-independent ELF linked
# at 0: QEMU puts the -bios image near the top of the first 2 GB.
S390X_CFLAGS := -m64 -march=z900 -msoft-float -std=gnu11 -O2 -Wall -Wextra -Werror \
                -ffreestanding -fno-builtin -fno-delete-null-pointer-checks -fno-stack-protector \
                -fno-asynchronous-unwind-tables -nostdlib -Icommon/fw
S390X_LDFLAGS := -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-z,noexecstack

ifeq ($(HAVE_S390X),y)
.PHONY: s390x
s390x: $(S390X_ELF)
$(S390X_ELF): $(S390X_DEPS)
	$(S390X)gcc $(S390X_CFLAGS) -fPIE $(S390X_LDFLAGS) -static-pie -Wl,--no-dynamic-linker \
	    -Wl,-T,s390x/link.ld $(S390X_SRCS) -o $@

# Test kernel: an ELF linked at 0x10000 like an s390x Linux, written to a
# raw disk image (from sector 0, padded to whole sectors) that the
# firmware IPLs through virtio-blk-ccw
$(TEST_DIR)/s390x-kernel.elf: tests/s390x-kernel.c tests/s390x-start.S \
		common/fw/string.c common/fw/printf.c common/fw/fwlib.h | $(TEST_DIR)
	$(S390X)gcc $(S390X_CFLAGS) -fno-pic -fno-pie -static -no-pie $(S390X_LDFLAGS) \
	    -Wl,-n -Wl,-Ttext=0x10000 -Wl,-e,_start \
	    tests/s390x-start.S tests/s390x-kernel.c common/fw/string.c common/fw/printf.c -o $@
$(TEST_DIR)/s390x-disk.img: $(TEST_DIR)/s390x-kernel.elf
	cp $< $@
	truncate -s %512 $@
# The same kernel as a Linux-style boot image (64 KB of zeros, then the
# binary linked at 0x10000), and an initrd disk: "SQRD", zeros, and 0x5a
# in its last byte
$(TEST_DIR)/s390x-image.elf: tests/s390x-image.S tests/s390x-kernel.c tests/s390x-start.S \
		common/fw/string.c common/fw/printf.c common/fw/fwlib.h | $(TEST_DIR)
	$(S390X)gcc $(S390X_CFLAGS) -DLINUX_IMAGE -fno-pic -fno-pie -static -no-pie $(S390X_LDFLAGS) \
	    -Wl,-n -Wl,-Ttext=0x10000 -Wl,-e,_image tests/s390x-image.S \
	    tests/s390x-start.S tests/s390x-kernel.c common/fw/string.c common/fw/printf.c -o $@
$(TEST_DIR)/s390x-image.img: $(TEST_DIR)/s390x-image.elf
	$(S390X)objcopy -O binary $< $@.bin
	head -c 65536 /dev/zero > $@
	cat $@.bin >> $@
	rm -f $@.bin
	truncate -s %512 $@
$(TEST_DIR)/s390x-initrd.img: | $(TEST_DIR)
	printf 'SQRD' > $@
	truncate -s 4095 $@
	printf 'Z' >> $@
$(TEST_DIR)/s390x-boot.keys: | $(TEST_DIR)
	printf '\rboot console=ttysclp0 squirrel\r' > $@
$(TEST_DIR)/s390x-prompt.keys: | $(TEST_DIR)
	printf 'help\rinfo\rdevices\rbogus\rpoweroff\r' > $@

.PHONY: s390x-test-files
s390x-test-files: $(S390X_ELF) $(TEST_DIR)/s390x-disk.img $(TEST_DIR)/s390x-prompt.keys \
                  $(TEST_DIR)/s390x-image.img $(TEST_DIR)/s390x-initrd.img $(TEST_DIR)/s390x-boot.keys
test: s390x-test-files
else
.PHONY: s390x
s390x:
	@echo "s390x: no $(S390X)gcc, skipping s390x"
endif

.PHONY: clean-s390x
clean: clean-s390x
clean-s390x:
	rm -f $(S390X_ELF)
