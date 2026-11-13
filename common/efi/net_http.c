/* SquirrelBIOS UEFI: an HTTP/1.1 client for HTTP boot. Original code,
 * written from RFC 9110 and RFC 9112 (HTTP semantics and HTTP/1.1).
 *
 * GET of an http:// URL whose host is an IPv4 address (there is no DNS
 * client yet), over net_tcp.c. The response body is taken by
 * Content-Length, chunked transfer coding, or the connection closing;
 * 301/302/303/307/308 are followed up to four times. The body grows in a
 * pool buffer the caller frees. */
#include "net.h"

#define HDR_MAX     4096
#define REDIRECTS   4
#define IDLE_MS     10000

static struct {
    char hdr[HDR_MAX];
    u32 hlen;
    int in_body, done, failed;
    int status;
    int chunked;
    u32 clen;                   /* Content-Length, or ~0 */
    u8 *buf;
    u32 size, cap;
    /* chunked: 0 size line, 1 data, 2 CRLF after data, 3 trailers */
    int cstate;
    u32 cleft;
    int cdigits;
    char location[256];
} h;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int prefix_ieq(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (lower(*s) != lower(*p))
            return 0;
    return 1;
}

static int grow(u32 need)
{
    if (need <= h.cap)
        return 1;
    u32 cap = h.cap ? h.cap : 65536;
    while (cap < need)
        cap *= 2;
    void *nb;
    if (efi_allocate_pool(EfiBootServicesData, cap, &nb) != EFI_SUCCESS)
        return 0;
    if (h.buf) {
        memcpy(nb, h.buf, h.size);
        efi_free_pool(h.buf);
    }
    h.buf = nb;
    h.cap = cap;
    return 1;
}

static void body_bytes(const u8 *d, u32 n)
{
    if (h.done || !n)
        return;
    if (!grow(h.size + n)) {
        h.failed = 1;
        h.done = 1;
        return;
    }
    memcpy(h.buf + h.size, d, n);
    h.size += n;
}

static void chunked(const u8 *d, u32 n)
{
    for (u32 i = 0; i < n && !h.done;) {
        u8 c = d[i];
        switch (h.cstate) {
        case 0:                 /* hex size, extensions, CRLF */
            if (c == '\n') {
                if (!h.cdigits) {
                    h.failed = h.done = 1;
                    return;
                }
                h.cstate = h.cleft ? 1 : 3;
                h.cdigits = 0;
            } else if (h.cdigits >= 0) {
                int v = c >= '0' && c <= '9' ? c - '0' : lower(c) >= 'a' && lower(c) <= 'f' ? lower(c) - 'a' + 10 : -1;
                if (v >= 0 && h.cdigits < 8) {
                    h.cleft = h.cleft << 4 | (u32)v;
                    h.cdigits++;
                } else if (v >= 0) {
                    h.failed = h.done = 1;
                    return;
                } else {
                    h.cdigits = h.cdigits ? -h.cdigits : 0;  /* the rest of the line */
                }
            }
            i++;
            break;
        case 1: {
            u32 take = n - i < h.cleft ? n - i : h.cleft;
            body_bytes(d + i, take);
            h.cleft -= take;
            i += take;
            if (!h.cleft)
                h.cstate = 2;
            break;
        }
        case 2:                 /* the CRLF after a chunk */
            if (c == '\n')
                h.cstate = 0;
            i++;
            break;
        case 3:                 /* trailers: done at the empty line */
            h.done = 1;
            break;
        }
    }
}

static void parse_headers(void)
{
    h.clen = ~0u;
    h.status = 0;
    const char *p = h.hdr;
    if (!prefix_ieq(p, "HTTP/1.")) {
        h.failed = h.done = 1;
        return;
    }
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; i++, p++)
        h.status = h.status * 10 + (*p - '0');
    for (;;) {
        while (*p && *p != '\n')
            p++;
        if (!*p)
            break;
        p++;
        if (prefix_ieq(p, "content-length:")) {
            const char *v = p + 15;
            while (*v == ' ' || *v == '\t')
                v++;
            u32 n = 0;
            while (*v >= '0' && *v <= '9')
                n = n * 10 + (u32)(*v++ - '0');
            h.clen = n;
        } else if (prefix_ieq(p, "transfer-encoding:")) {
            for (const char *v = p + 18; *v && *v != '\r' && *v != '\n'; v++)
                if (prefix_ieq(v, "chunked"))
                    h.chunked = 1;
        } else if (prefix_ieq(p, "location:")) {
            const char *v = p + 9;
            while (*v == ' ' || *v == '\t')
                v++;
            u32 n = 0;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < sizeof h.location - 1)
                n++;
            memcpy(h.location, v, n);
            h.location[n] = 0;
        }
    }
    if (h.chunked)
        h.clen = ~0u;
}

static void on_data(const u8 *d, u32 n)
{
    while (n && !h.in_body && !h.done) {
        if (h.hlen >= HDR_MAX - 1) {
            h.failed = h.done = 1;
            return;
        }
        h.hdr[h.hlen++] = (char)*d++;
        n--;
        if (h.hlen >= 4 && !memcmp(h.hdr + h.hlen - 4, "\r\n\r\n", 4)) {
            h.hdr[h.hlen] = 0;
            parse_headers();
            h.in_body = 1;
            if (h.status != 200 || h.clen == 0)
                h.done = 1;     /* nothing more wanted */
            else if (h.clen != ~0u && !grow(h.clen))
                h.failed = h.done = 1;
        }
    }
    if (!n || h.done)
        return;
    if (h.chunked) {
        chunked(d, n);
    } else {
        if (h.clen != ~0u && h.size + n > h.clen)
            n = h.clen - h.size;
        body_bytes(d, n);
        if (h.clen != ~0u && h.size >= h.clen)
            h.done = 1;
    }
}

/* http://a.b.c.d[:port][/path]: 1 with the pieces */
static int parse_url(const char *url, u32 *ip, u16 *port, const char **path, char *host, u32 hmax)
{
    if (!prefix_ieq(url, "http://"))
        return 0;
    const char *p = url + 7;
    u32 n = 0;
    while (p[n] && p[n] != ':' && p[n] != '/')
        n++;
    if (!n || n >= hmax)
        return 0;
    memcpy(host, p, n);
    host[n] = 0;
    *ip = net_ip_parse(host);
    p += n;
    u32 pt = 80;
    if (*p == ':') {
        pt = 0;
        for (p++; *p >= '0' && *p <= '9'; p++)
            pt = pt * 10 + (u32)(*p - '0');
        if (!pt || pt > 65535)
            return 0;
    }
    if (*p && *p != '/')
        return 0;
    *port = (u16)pt;
    *path = *p ? p : "/";
    return 1;
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
        if (!parse_url(cur, &ip, &port, &path, host, sizeof host)) {
            printf("net: not an http:// URL: %s\n", cur);
            return 0;
        }
        if (!ip) {
            printf("net: %s is not an IPv4 address (no DNS client)\n", host);
            return 0;
        }
        memset(&h, 0, sizeof h);
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
