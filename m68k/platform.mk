# SquirrelBIOS m68k-virt (roadmap E3): build rules, included by the
# top-level Makefile. Needs an m68k cross gcc (Ubuntu: gcc-m68k-linux-gnu);
# without one the platform is skipped and its make test cases report SKIP.

all: $(if $(PLATFORM),,m68k)

M68K       ?= m68k-linux-gnu-
HAVE_M68K  := $(shell command -v $(M68K)gcc >/dev/null 2>&1 && echo y)
M68K_ELF   := m68k/squirrel-bios.elf
M68K_SRCS  := m68k/start.S m68k/main.c common/fw/string.c common/fw/printf.c \
              common/fw/prompt.c
M68K_DEPS  := $(M68K_SRCS) m68k/link.ld common/fw/fwlib.h m68k/platform.mk
# 68040, no FPU code (the OS owns the FPU), static, no PIC; an OS image
# may start at address 0 (Linux's vmlinux does), so no null-pointer games. QEMU's virt
# machine has no ROM: the firmware is an ELF that QEMU loads at FW_BASE.
M68K_CFLAGS := -mcpu=68040 -msoft-float -std=gnu11 -O2 -Wall -Wextra -Werror \
               -ffreestanding -fno-builtin -fno-delete-null-pointer-checks -fno-pic -fno-pie -fno-stack-protector \
               -fno-asynchronous-unwind-tables -nostdlib -static -Icommon/fw
M68K_LDFLAGS := -no-pie -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-z,noexecstack
M68K_FW_BASE := 0x03c00000

ifeq ($(HAVE_M68K),y)
.PHONY: m68k
m68k: $(M68K_ELF)
$(M68K_ELF): $(M68K_DEPS)
	$(M68K)gcc $(M68K_CFLAGS) $(M68K_LDFLAGS) -Wl,-T,m68k/link.ld \
	    -Wl,--defsym,FW_BASE=$(M68K_FW_BASE) $(M68K_SRCS) -lgcc -o $@

# Test kernel: an ELF linked at 0x1000 like an m68k Linux, given to the
# firmware as -initrd
$(TEST_DIR)/m68k-kernel.elf: tests/m68k-kernel.c tests/m68k-start.S \
		common/fw/string.c common/fw/printf.c common/fw/fwlib.h | $(TEST_DIR)
	$(M68K)gcc $(M68K_CFLAGS) $(M68K_LDFLAGS) -Wl,-n -Wl,-Ttext=0x1000 -Wl,-e,_start \
	    tests/m68k-start.S tests/m68k-kernel.c common/fw/string.c common/fw/printf.c \
	    -lgcc -o $@
$(TEST_DIR)/m68k-prompt.keys: | $(TEST_DIR)
	printf 'help\rinfo\rbootinfo\rdate\rbogus\rpoweroff\r' > $@

.PHONY: m68k-test-files
m68k-test-files: $(M68K_ELF) $(TEST_DIR)/m68k-kernel.elf $(TEST_DIR)/m68k-prompt.keys
test: m68k-test-files
else
.PHONY: m68k
m68k:
	@echo "m68k: no $(M68K)gcc, skipping m68k-virt"
endif
