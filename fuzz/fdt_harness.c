/* Host fuzz / corpus-smoke harness for the flattened device tree reader
 * (common/fw/fdt.c via common/efi/fdt.c). Original code. CONTINUANCE
 * Phase G: feed malformed DTBs without QEMU.
 *
 * Build:
 *   make fuzz-fdt              standalone corpus runner (gcc)
 *   make fuzz-fdt-libfuzzer    clang -fsanitize=fuzzer,address
 *
 * Do not include <stdio.h>: the firmware declares void printf. */
#include "sq.h"
#include "host_stubs.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUZZ_FDT_MAX (512u * 1024u)

int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    u8 *buf;
    usize cap;
    int node, depth, n;
    u64 base, reg_size;
    u32 plen;

    host_reset();
    if (!data)
        return 0;

    cap = size > FUZZ_FDT_MAX ? FUZZ_FDT_MAX : size;
    buf = (u8 *)malloc(cap ? cap : 1);
    if (!buf)
        return 0;
    if (cap)
        memcpy(buf, data, cap);

    if (fdt_init(buf)) {
        depth = 0;
        node = -1;
        n = 0;
        while ((node = fdt_next_node(node, &depth)) >= 0 && n++ < 256) {
            const char *name = fdt_node_name(node);
            (void)name;
            (void)fdt_prop(node, "compatible", &plen);
            (void)fdt_prop(node, "reg", &plen);
            (void)fdt_prop(node, "status", &plen);
            (void)fdt_reg(node, 0, &base, &reg_size);
        }
        (void)fdt_path("/");
        (void)fdt_path("/cpus");
        (void)fdt_path("/memory");
        (void)fdt_find_compatible("virtio,mmio", -1);
        (void)fdt_find_enabled("arm,pl011", -1);
        (void)fdt_find_compatible("pci-host-ecam-generic", -1);
        (void)fdt_blob();
        (void)fdt_size();
    }

    free(buf);
    host_reset();
    return 0;
}

#ifndef FUZZ_NO_MAIN
static void say(const char *s)
{
    size_t n = 0;
    ssize_t w;
    while (s[n])
        n++;
    w = write(2, s, n);
    if (w < 0)
        return;
}

static int run_file(const char *path)
{
    int fd;
    off_t n;
    u8 *buf;
    ssize_t got;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        say("fuzz-fdt: cannot open ");
        say(path);
        say("\n");
        return 1;
    }
    n = lseek(fd, 0, SEEK_END);
    if (n < 0) {
        close(fd);
        say("fuzz-fdt: cannot size ");
        say(path);
        say("\n");
        return 1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return 1;
    }
    buf = (u8 *)malloc((size_t)n + 1);
    if (!buf) {
        close(fd);
        return 1;
    }
    got = read(fd, buf, (size_t)n);
    close(fd);
    if (got < 0)
        got = 0;
    LLVMFuzzerTestOneInput(buf, (usize)got);
    free(buf);
    say("ok ");
    say(path);
    say("\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0;
    u8 *buf;
    ssize_t got;

    if (argc <= 1) {
        buf = (u8 *)malloc(1u << 20);
        if (!buf)
            return 1;
        got = read(0, buf, 1u << 20);
        if (got < 0)
            got = 0;
        LLVMFuzzerTestOneInput(buf, (usize)got);
        free(buf);
        say("ok stdin\n");
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (run_file(argv[i]) != 0)
            rc = 1;
    }
    return rc;
}
#endif
