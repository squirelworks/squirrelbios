# SquirrelBIOS sparc64-sun4u (roadmap E2): build rules, included by the
# top-level Makefile. Needs a sparc64 cross gcc (Ubuntu:
# gcc-sparc64-linux-gnu); without one the platform is skipped and its
# make test cases report SKIP.

all: $(if $(PLATFORM),,sparc64)

PLATFORMS += sparc64-sun4u
OUT_sparc64-sun4u := squirrel-bios.bin

SPARC64      ?= sparc64-linux-gnu-
HAVE_SPARC64 := $(shell command -v $(SPARC64)gcc >/dev/null 2>&1 && echo y)
SPARC64_BIN  := sparc64/squirrel-bios.bin
SPARC64_SRCS := sparc64/start.S sparc64/main.c common/fw/string.c common/fw/printf.c \
                common/fw/fdt.c common/fw/fdt_print.c common/fw/prompt.c common/fw/elf.c
SPARC64_DEPS := $(SPARC64_SRCS) sparc64/link.ld common/fw/fwlib.h common/fw/fdt.h sparc64/platform.mk
# Medium-low code model (everything below 4 GB: the firmware runs at
# 0xf0000000), no FPU (the OS owns it), static, no PIC
SPARC64_CFLAGS := -m64 -mcpu=ultrasparc -mcmodel=medlow -mno-fpu -std=gnu11 -O2 -Wall \
                  -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-pie \
                  -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib -static \
                  -Icommon/fw
SPARC64_LDFLAGS := -no-pie -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
                   -Wl,-z,noexecstack

define SPARC64_BUILD
	@mkdir -p $(1)
	$(SPARC64)gcc $(SPARC64_CFLAGS) $(2) $(SPARC64_LDFLAGS) -Wl,-T,sparc64/link.ld \
	    $(SPARC64_SRCS) -lgcc -o $(1)/squirrel-bios.elf
	$(SPARC64)objcopy -O binary $(1)/squirrel-bios.elf $@
endef

ifeq ($(HAVE_SPARC64),y)
.PHONY: sparc64
sparc64: $(SPARC64_BIN)
$(SPARC64_BIN): $(SPARC64_DEPS)
	$(call SPARC64_BUILD,build/sparc64)
build/sparc64-sun4u/%/squirrel-bios.bin: $(SPARC64_DEPS) build/sparc64-sun4u/%/config.h
	$(call SPARC64_BUILD,$(@D),-DSQ_CONFIG -I$(@D))

# Test kernel: an ELF linked at 0x404000 like a sparc64 Linux, run with
# the firmware's register-window traps and client interface
$(TEST_DIR)/sparc64-kernel.elf: tests/sparc64-kernel.c tests/sparc64-start.S \
		common/fw/string.c common/fw/printf.c common/fw/fwlib.h | $(TEST_DIR)
	$(SPARC64)gcc $(SPARC64_CFLAGS) $(SPARC64_LDFLAGS) -Wl,-Ttext=0x404000 -Wl,-e,_start \
	    tests/sparc64-start.S tests/sparc64-kernel.c common/fw/string.c common/fw/printf.c -o $@
# The test kernel on an IDE disk image (from sector 0, whole sectors)
$(TEST_DIR)/sparc64-disk.img: $(TEST_DIR)/sparc64-kernel.elf
	cp $< $@
	truncate -s %512 $@
$(TEST_DIR)/sparc64-ide.keys: sparc64/platform.mk | $(TEST_DIR)
	printf '\rboot squirrel disk\r' > $@
$(TEST_DIR)/sparc64-prompt.keys: sparc64/platform.mk | $(TEST_DIR)
	printf 'help\rinfo\rdt /chosen\rpci\rbogus\r' > $@

.PHONY: sparc64-test-files config-default-sparc64
sparc64-test-files: $(SPARC64_BIN) $(TEST_DIR)/sparc64-kernel.elf \
                    $(TEST_DIR)/sparc64-prompt.keys config-default-sparc64 \
                    $(TEST_DIR)/sparc64-disk.img $(TEST_DIR)/sparc64-ide.keys
config-default-sparc64: $(SPARC64_BIN) build/sparc64-sun4u/default/squirrel-bios.bin
	cmp $(SPARC64_BIN) build/sparc64-sun4u/default/squirrel-bios.bin
	@echo "config-default: sparc64-sun4u default.cfg build matches plain make"
test: sparc64-test-files
else
.PHONY: sparc64
sparc64:
	@echo "sparc64: no $(SPARC64)gcc, skipping sparc64-sun4u"
endif

.PHONY: clean-sparc64
clean: clean-sparc64
clean-sparc64:
	rm -f $(SPARC64_BIN)
