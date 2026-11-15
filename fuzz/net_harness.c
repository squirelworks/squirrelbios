/* Host fuzz / corpus-smoke harness for DHCP / TFTP / HTTP parsers
 * (common/efi/net_parse.c). Original code. CONTINUANCE Phase G: feed
 * malformed packets and responses without QEMU or a NIC.
 *
 * Build:
 *   make fuzz-net              standalone corpus runner (gcc)
 *   make fuzz-net-libfuzzer    clang -fsanitize=fuzzer,address */
#include "net_parse.h"
#include "host_stubs.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUZZ_NET_MAX (256u * 1024u)
#define FUZZ_HTTP_BODY_MAX (64u * 1024u)

static int fuzz_alloc(void *ctx, u64 need, void **p)
{
    (void)ctx;
    if (need > FUZZ_HTTP_BODY_MAX)
        return -1;
    *p = malloc((size_t)need);
    return *p ? 0 : -1;
}

static void fuzz_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    u8 *buf;
    usize cap;
    struct net_parse_cfg cfg;
    struct net_tftp_view tv;
    struct net_http_rx rx;
    u8 msgtype;
    u32 sid;
    u32 ip;
    u16 port;
    const char *path;
    char host[64];
    char url[256];

    host_reset();
    if (!data)
        return 0;

    cap = size > FUZZ_NET_MAX ? FUZZ_NET_MAX : size;
    buf = (u8 *)malloc(cap ? cap : 1);
    if (!buf)
        return 0;
    if (cap)
        memcpy(buf, data, cap);

    msgtype = 0;
    sid = 0;
    (void)net_parse_dhcp(buf, (u32)cap, &cfg, &msgtype, &sid);
    (void)net_parse_tftp(buf, (u32)cap, &tv);

    /* URL: treat bytes as a C string (cap and NUL). */
    {
        usize ul = cap < sizeof url - 1 ? cap : sizeof url - 1;
        memcpy(url, buf, ul);
        url[ul] = 0;
        (void)net_parse_http_url(url, &ip, &port, &path, host, sizeof host);
        (void)net_parse_ipv4(url);
    }

    /* HTTP response stream (headers + body / chunked). */
    net_http_rx_init(&rx, FUZZ_HTTP_BODY_MAX, fuzz_alloc, fuzz_free, NULL);
    net_http_rx_feed(&rx, buf, (u32)cap);
    /* Second feed with a split after first half when large enough. */
    if (cap > 8) {
        net_http_rx_clear(&rx);
        net_http_rx_feed(&rx, buf, (u32)(cap / 2));
        net_http_rx_feed(&rx, buf + cap / 2, (u32)(cap - cap / 2));
    }
    net_http_rx_clear(&rx);

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
        say("fuzz-net: cannot open ");
        say(path);
        say("\n");
        return 1;
    }
    n = lseek(fd, 0, SEEK_END);
    if (n < 0) {
        close(fd);
        say("fuzz-net: cannot size ");
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
