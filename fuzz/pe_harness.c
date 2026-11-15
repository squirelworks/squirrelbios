/* Host fuzz / corpus-smoke harness for common/efi/image.c (PE/COFF).
 * Original code. CONTINUANCE Phase G: feed malformed PE32+ images into
 * LoadImage / UnloadImage without QEMU. Never StartImage — that would
 * execute a fuzzed entry point.
 *
 * Build:
 *   make fuzz-pe              standalone corpus runner (gcc)
 *   make fuzz-pe-libfuzzer    clang -fsanitize=fuzzer,address (optional)
 *
 * Entry point is LLVMFuzzerTestOneInput so the same object links under
 * libFuzzer or AFL++ (persistent mode) or the standalone main below.
 *
 * Do not include <stdio.h>: the firmware declares void printf, which
 * conflicts with libc. The standalone runner uses POSIX open/read/write. */
#include "efi.h"
#include "host_stubs.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cap so a single input cannot force a multi-megabyte PE copy. */
#define FUZZ_PE_MAX (256u * 1024u)

int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    EFI_BOOT_SERVICES bs;
    EFI_HANDLE out;
    u8 *buf;
    usize cap;
    EFI_STATUS s;

    host_reset();
    if (!data || size < 2)
        return 0;

    cap = size > FUZZ_PE_MAX ? FUZZ_PE_MAX : size;
    buf = (u8 *)malloc(cap);
    if (!buf)
        return 0;
    memcpy(buf, data, cap);

    memset(&bs, 0, sizeof bs);
    efi_image_init(&bs);

    out = NULL;
    s = efi_load_image(0, efi_firmware_handle, NULL, buf, cap, &out);
    if (s == EFI_SUCCESS && out)
        efi_unload_image(out);

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
    char msg[256];
    size_t i, j;
    const char *p;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        say("fuzz-pe: cannot open ");
        say(path);
        say("\n");
        return 1;
    }
    n = lseek(fd, 0, SEEK_END);
    if (n < 0) {
        close(fd);
        say("fuzz-pe: cannot size ");
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

    i = 0;
    p = "ok ";
    while (*p && i + 1 < sizeof msg)
        msg[i++] = *p++;
    for (j = 0; path[j] && i + 1 < sizeof msg; j++)
        msg[i++] = path[j];
    p = " (";
    while (*p && i + 1 < sizeof msg)
        msg[i++] = *p++;
    {
        char num[32];
        size_t k = 0;
        usize v = (usize)got;
        if (v == 0)
            num[k++] = '0';
        else {
            char tmp[32];
            size_t t = 0;
            while (v && t < sizeof tmp) {
                tmp[t++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (t)
                num[k++] = tmp[--t];
        }
        for (j = 0; j < k && i + 1 < sizeof msg; j++)
            msg[i++] = num[j];
    }
    p = " bytes)\n";
    while (*p && i + 1 < sizeof msg)
        msg[i++] = *p++;
    msg[i] = 0;
    say(msg);
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
