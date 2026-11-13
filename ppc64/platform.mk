# SquirrelBIOS ppc64le-powernv (roadmap E1): build rules, included by the
# top-level Makefile. Needs a powerpc64le cross gcc (Ubuntu:
# gcc-powerpc64le-linux-gnu); without one the platform is skipped and its
# make test cases report SKIP.

all: $(if $(PLATFORM),,ppc64)

PLATFORMS += ppc64le-powernv
OUT_ppc64le-powernv := squirrel-bios.bin

PPC64      ?= powerpc64le-linux-gnu-
HAVE_PPC64 := $(shell command -v $(PPC64)gcc >/dev/null 2>&1 && echo y)
PPC64_BIN  := ppc64/squirrel-bios.bin
PPC64_SRCS := ppc64/start.S ppc64/main.c common/fw/string.c common/fw/printf.c \
              common/fw/fdt.c common/fw/fdt_print.c common/fw/elf.c common/fw/prompt.c
PPC64_DEPS := $(PPC64_SRCS) ppc64/opal.h ppc64/link.ld common/fw/fwlib.h common/fw/fdt.h ppc64/platform.mk
# Firmware: big-endian ELFv2 (the OPAL convention), no floating point or
# vector registers (their facilities may be off), linked at FW_BASE
PPC64_FW_BASE := 0x30000000
PPC64_CFLAGS := -mbig-endian -mabi=elfv2 -mcpu=power8 -msoft-float -mno-altivec -mno-vsx \
                -mcmodel=medium -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
                -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
                -fno-asynchronous-unwind-tables -nostdlib -static -Icommon/fw
PPC64_LDFLAGS := -mbig-endian -nostdlib -static -no-pie -Wl,-T,ppc64/link.ld \
                 -Wl,--defsym,FW_BASE=$(PPC64_FW_BASE) -Wl,--build-id=none \
                 -Wl,--no-warn-rwx-segments

# $(1) output directory, $(2) extra C flags
define PPC64_BUILD
	@mkdir -p $(1)
	$(PPC64)gcc $(PPC64_CFLAGS) $(2) $(PPC64_LDFLAGS) $(PPC64_SRCS) -o $(1)/squirrel-bios.elf
	$(PPC64)objcopy -O binary $(1)/squirrel-bios.elf $@
endef

ifeq ($(HAVE_PPC64),y)
.PHONY: ppc64
ppc64: $(PPC64_BIN)
$(PPC64_BIN): $(PPC64_DEPS)
	$(call PPC64_BUILD,build/ppc64)
build/ppc64le-powernv/%/squirrel-bios.bin: $(PPC64_DEPS) build/ppc64le-powernv/%/config.h
	$(call PPC64_BUILD,$(@D),-DSQ_CONFIG -I$(@D))

# Test kernel: little-endian ELFv2 like a ppc64le Linux, linked at 256 MB
PPC64_TEST_CFLAGS := -mlittle-endian -mabi=elfv2 -mcpu=power8 -msoft-float -mno-altivec \
                     -mno-vsx -mcmodel=medium -std=gnu11 -O2 -Wall -Wextra -Werror \
                     -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
                     -fno-asynchronous-unwind-tables -nostdlib -static -Icommon/fw
$(TEST_DIR)/ppc64-kernel.elf: tests/ppc64-kernel.c tests/ppc64-start.S tests/ppc64-kernel.ld \
		common/fw/string.c common/fw/printf.c common/fw/fdt.c common/fw/fwlib.h common/fw/fdt.h | $(TEST_DIR)
	$(PPC64)gcc $(PPC64_TEST_CFLAGS) -no-pie -Wl,-T,tests/ppc64-kernel.ld -Wl,--build-id=none \
	    -Wl,--no-warn-rwx-segments -Wl,--defsym,KBASE=0x10000000 tests/ppc64-start.S \
	    tests/ppc64-kernel.c common/fw/string.c common/fw/printf.c common/fw/fdt.c -lgcc -o $@
# The same kernel linked over the firmware's region, which must be refused
$(TEST_DIR)/ppc64-overlap.elf: tests/ppc64-kernel.c tests/ppc64-start.S tests/ppc64-kernel.ld \
		common/fw/string.c common/fw/printf.c common/fw/fdt.c common/fw/fwlib.h common/fw/fdt.h | $(TEST_DIR)
	$(PPC64)gcc $(PPC64_TEST_CFLAGS) -no-pie -Wl,-T,tests/ppc64-kernel.ld -Wl,--build-id=none \
	    -Wl,--no-warn-rwx-segments -Wl,--defsym,KBASE=0x30100000 tests/ppc64-start.S \
	    tests/ppc64-kernel.c common/fw/string.c common/fw/printf.c common/fw/fdt.c -lgcc -o $@
# Typed at the firmware prompt when no kernel is given
$(TEST_DIR)/ppc64-prompt.keys: | $(TEST_DIR)
	printf 'help\rinfo\rdate\rdt /chosen\rbogus\rpoweroff\r' > $@

.PHONY: ppc64-test-files config-default-ppc64
ppc64-test-files: $(PPC64_BIN) $(TEST_DIR)/ppc64-kernel.elf $(TEST_DIR)/ppc64-overlap.elf \
                  $(TEST_DIR)/ppc64-prompt.keys config-default-ppc64
config-default-ppc64: $(PPC64_BIN) build/ppc64le-powernv/default/squirrel-bios.bin
	cmp $(PPC64_BIN) build/ppc64le-powernv/default/squirrel-bios.bin
	@echo "config-default: ppc64le-powernv default.cfg build matches plain make"
test: ppc64-test-files
else
.PHONY: ppc64
ppc64:
	@echo "ppc64: no $(PPC64)gcc, skipping ppc64le-powernv"
endif

.PHONY: clean-ppc64
clean: clean-ppc64
clean-ppc64:
	rm -f $(PPC64_BIN)
