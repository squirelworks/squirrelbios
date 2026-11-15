/* Host fuzz / corpus-smoke harness for USB descriptor walks
 * (common/efi/usb_desc.c). Original code. CONTINUANCE Phase G.
 *
 * Build:
 *   make fuzz-usb              standalone corpus runner (gcc)
 *   make fuzz-usb-libfuzzer    clang -fsanitize=fuzzer,address */
#include "usb_desc.h"
#include "host_stubs.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUZZ_USB_MAX (64u * 1024u)

int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    u8 *buf;
    usize cap;
    struct usb_bot_info bot;
    u16 vid, pid;
    u8 mps0;

    host_reset();
    if (!data)
        return 0;

    cap = size > FUZZ_USB_MAX ? FUZZ_USB_MAX : size;
    buf = (u8 *)malloc(cap ? cap : 1);
    if (!buf)
        return 0;
    if (cap)
        memcpy(buf, data, cap);

    (void)usb_parse_device_desc(buf, (u32)cap, &vid, &pid, &mps0);
    if (cap >= 8)
        (void)usb_parse_device_desc(buf, 8, &vid, &pid, &mps0);
    (void)usb_config_find_bot(buf, (u32)cap, &bot);
    /* Also try treating the bytes as a config that starts mid-stream. */
    if (cap > 9)
        (void)usb_config_find_bot(buf + 1, (u32)(cap - 1), &bot);

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
        say("fuzz-usb: cannot open ");
        say(path);
        say("\n");
        return 1;
    }
    n = lseek(fd, 0, SEEK_END);
    if (n < 0) {
        close(fd);
        say("fuzz-usb: cannot size ");
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
