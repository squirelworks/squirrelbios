/* SquirrelBIOS UEFI: an HTTP/1.1 client for HTTP boot. Original code,
 * written from RFC 9110 and RFC 9112 (HTTP semantics and HTTP/1.1).
 *
 * GET of an http:// URL whose host is an IPv4 address or a DNS name
 * (resolved through net_dns.c with the server from DHCP option 6), over
 * net_tcp.c. The response body is taken by Content-Length, chunked
 * transfer coding, or the connection closing; 301/302/303/307/308 are
 * followed up to four times. The body grows in a pool buffer the caller
 * frees. Header / chunked / URL parsing lives in net_parse.c so the
 * host fuzz harness exercises the same code. */
#include "net.h"
#include "net_parse.h"

#define REDIRECTS   4
#define IDLE_MS     10000

static struct net_http_rx h;

static int pool_alloc(void *ctx, u64 need, void **p)
{
    (void)ctx;
    return efi_allocate_pool(EfiBootServicesData, need, p) == EFI_SUCCESS ? 0 : -1;
}

static void pool_free(void *ctx, void *p)
{
    (void)ctx;
    efi_free_pool(p);
}

static void on_data(const u8 *d, u32 n)
{
    net_http_rx_feed(&h, d, n);
}

static void append(char *dst, u32 *n, u32 max, const char *s)
{
    while (*s && *n < max - 1)
        dst[(*n)++] = *s++;
    dst[*n] = 0;
}

int net_http_get(const char *url, void **buf, u32 *size)
{
    static char cur[256];
    u32 ul = (u32)strlen(url);
    if (ul >= sizeof cur) {
        printf("net: URL too long\n");
        return 0;
    }
    memcpy(cur, url, ul + 1);
    for (int hop = 0; hop <= REDIRECTS; hop++) {
        u32 ip;
        u16 port;
        const char *path;
        char host[64];
        if (!net_parse_http_url(cur, &ip, &port, &path, host, sizeof host)) {
            printf("net: not an http:// URL: %s\n", cur);
            return 0;
        }
        if (!ip) {
            u32 dns = net_dns_server();
            if (!dns || !net_dns_lookup(dns, host, &ip)) {
                printf("net: cannot resolve %s\n", host);
                return 0;
            }
        }
        net_http_rx_init(&h, NET_FILE_MAX, pool_alloc, pool_free, NULL);
        if (!net_tcp_connect(ip, port, on_data)) {
            printf("net: no connection to %s port %u\n", host, port);
            return 0;
        }
        char req[512];
        u32 rn = 0;
        append(req, &rn, sizeof req, "GET ");
        append(req, &rn, sizeof req, path);
        append(req, &rn, sizeof req, " HTTP/1.1\r\nHost: ");
        append(req, &rn, sizeof req, host);
        append(req, &rn, sizeof req, "\r\nUser-Agent: SquirrelBIOS\r\nAccept: */*\r\nConnection: close\r\n\r\n");
        if (net_tcp_send(req, rn)) {
            printf("net: the HTTP request was not taken\n");
            net_tcp_abort();
            net_http_rx_clear(&h);
            return 0;
        }
        u64 seen = net_ms();
        u32 last = 0;
        int st = 0;
        while (!h.done) {
            st = net_tcp_poll(50);
            if (st)
                break;
            if (h.size != last || h.hlen) {
                if (h.size != last)
                    seen = net_ms();
                last = h.size;
            }
            if (net_ms() - seen > IDLE_MS)
                break;
        }
        int whole = h.done && !h.failed;
        if (!h.done && st == 1 && h.in_body && h.status == 200 && !h.chunked &&
            (h.clen == ~0u || h.size == h.clen))
            whole = 1;          /* the body ran to the connection's end */
        if (h.done)
            net_tcp_close();
        else if (st == 1)
            net_tcp_close();
        else
            net_tcp_abort();
        if (h.in_body && h.status >= 300 && h.status < 400 && h.status != 304 && h.location[0]) {
            if (h.buf)
                efi_free_pool(h.buf);
            h.buf = NULL;
            if (h.location[0] == '/') {     /* same server */
                char next[256];
                u32 nn = 0;
                append(next, &nn, sizeof next, "http://");
                append(next, &nn, sizeof next, host);
                if (port != 80) {
                    char pb[8];
                    int k = 7;
                    pb[k] = 0;
                    for (u32 v = port; v; v /= 10)
                        pb[--k] = (char)('0' + v % 10);
                    append(next, &nn, sizeof next, ":");
                    append(next, &nn, sizeof next, pb + k);
                }
                append(next, &nn, sizeof next, h.location);
                memcpy(cur, next, nn + 1);
            } else {
                u32 n = (u32)strlen(h.location);
                memcpy(cur, h.location, n + 1);
            }
            printf("net: HTTP %d, to %s\n", h.status, cur);
            continue;
        }
        if (!h.in_body || h.status != 200) {
            if (h.in_body)
                printf("net: HTTP %d for %s\n", h.status, cur);
            else
                printf("net: no HTTP answer from %s\n", host);
            if (h.buf)
                efi_free_pool(h.buf);
            return 0;
        }
        if (h.too_large) {
            printf("net: %s is larger than the %u-byte limit\n", cur, NET_FILE_MAX);
            if (h.buf)
                efi_free_pool(h.buf);
            return 0;
        }
        if (!whole) {
            printf("net: HTTP transfer of %s broke off after %u bytes\n", cur, h.size);
            if (h.buf)
                efi_free_pool(h.buf);
            return 0;
        }
        if (!h.buf)
            efi_allocate_pool(EfiBootServicesData, 1, (void **)&h.buf);
        *buf = h.buf;
        *size = h.size;
        return 1;
    }
    printf("net: too many HTTP redirects\n");
    return 0;
}
