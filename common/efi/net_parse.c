/* SquirrelBIOS UEFI: DHCP / TFTP / HTTP parse helpers (see net_parse.h).
 * Original code. Linked into the firmware network stack and the host
 * fuzz harness. */
#include "net_parse.h"

void *memcpy(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int memcmp(const void *a, const void *b, usize n);

static u32 ipget(const u8 *p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static u16 get16(const u8 *p)
{
    return (u16)(p[0] << 8 | p[1]);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int prefix_ieq(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (lower(*s) != lower(*p))
            return 0;
    return 1;
}

u32 net_parse_ipv4(const char *s)
{
    u32 a = 0, b = 0, c = 0, d = 0, n = 0;
    const char *p = s;

    if (!s || !*s)
        return 0;
    while (*p >= '0' && *p <= '9')
        a = a * 10 + (u32)(*p++ - '0'), n = 1;
    if (!n || *p++ != '.' || a > 255)
        return 0;
    n = 0;
    while (*p >= '0' && *p <= '9')
        b = b * 10 + (u32)(*p++ - '0'), n = 1;
    if (!n || *p++ != '.' || b > 255)
        return 0;
    n = 0;
    while (*p >= '0' && *p <= '9')
        c = c * 10 + (u32)(*p++ - '0'), n = 1;
    if (!n || *p++ != '.' || c > 255)
        return 0;
    n = 0;
    while (*p >= '0' && *p <= '9')
        d = d * 10 + (u32)(*p++ - '0'), n = 1;
    if (!n || *p || d > 255)
        return 0;
    {
        u8 bytes[4] = { (u8)a, (u8)b, (u8)c, (u8)d };
        u32 v;
        memcpy(&v, bytes, 4);
        return v;
    }
}

int net_parse_dhcp(const u8 *d, u32 len, struct net_parse_cfg *c,
                   u8 *msgtype, u32 *server_id)
{
    u32 i;
    int overload = 0;
    u8 type = 0;
    u32 sid = 0;

    if (!d || !c || len < 240 || d[0] != 2)
        return 0;
    if (d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
        return 0;

    memset(c, 0, sizeof *c);
    c->ip = ipget(d + 16);
    c->server = ipget(d + 20);

    i = 240;
    while (i < len && d[i] != 255) {
        if (d[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + d[i + 1] > len)
            break;
        {
            u8 code = d[i], n = d[i + 1];
            const u8 *v = d + i + 2;
            if (code == 53 && n >= 1)
                type = v[0];
            else if (code == 54 && n >= 4)
                sid = ipget(v);
            else if (code == 1 && n >= 4)
                c->mask = ipget(v);
            else if (code == 3 && n >= 4)
                c->gw = ipget(v);
            else if (code == 6 && n >= 4)
                c->dns = ipget(v);
            else if (code == 52 && n >= 1)
                overload = v[0];
            else if (code == 66 && n) {
                u32 m = n < sizeof c->server_name - 1 ? n : sizeof c->server_name - 1;
                memcpy(c->server_name, v, m);
                c->server_name[m] = 0;
            } else if (code == 67 && n) {
                u32 m = n < sizeof c->file - 1 ? n : sizeof c->file - 1;
                memcpy(c->file, v, m);
                c->file[m] = 0;
            }
            i += 2u + n;
        }
    }
    if (!c->file[0] && !(overload & 1) && d[108]) {
        memcpy(c->file, d + 108, 127);
        c->file[127] = 0;
    }
    if (!c->server_name[0] && !(overload & 2) && d[44]) {
        memcpy(c->server_name, d + 44, 63);
        c->server_name[63] = 0;
    }
    if (msgtype)
        *msgtype = type;
    if (server_id)
        *server_id = sid;
    return 1;
}

static u32 tf_num(const u8 *s, u32 n)
{
    u32 v = 0;
    for (u32 i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (u32)(s[i] - '0');
    return v;
}

static int tf_ieq(const u8 *a, const char *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        u8 c = a[i];
        if (c >= 'A' && c <= 'Z')
            c = (u8)(c + 32);
        if (c != (u8)b[i])
            return 0;
    }
    return !b[n];
}

int net_parse_tftp(const u8 *d, u32 len, struct net_tftp_view *out)
{
    u16 op;

    if (!d || !out || len < 4)
        return 0;
    memset(out, 0, sizeof *out);
    op = get16(d);
    out->opcode = op;
    if (op == 5) {
        u32 m = len - 4 < sizeof out->err - 1 ? len - 4 : sizeof out->err - 1;
        memcpy(out->err, d + 4, m);
        out->err[m] = 0;
        out->payload = d + 4;
        out->payload_len = len - 4;
        return 1;
    }
    if (op == 6) {
        u32 i = 2;
        while (i < len) {
            u32 k = i;
            while (i < len && d[i])
                i++;
            {
                u32 kl = i - k, v = ++i;
                while (i < len && d[i])
                    i++;
                if (tf_ieq(d + k, "blksize", kl))
                    out->blksize = tf_num(d + v, i - v);
                else if (tf_ieq(d + k, "tsize", kl))
                    out->tsize = tf_num(d + v, i - v);
                i++;
            }
        }
        return 1;
    }
    if (op == 3) {
        out->block = get16(d + 2);
        out->payload = d + 4;
        out->payload_len = len - 4;
        return 1;
    }
    return 0;
}

int net_parse_http_url(const char *url, u32 *ip, u16 *port,
                       const char **path, char *host, u32 host_max)
{
    const char *p;
    u32 n, pt;

    if (!url || !ip || !port || !path || !host || host_max < 2)
        return 0;
    if (!prefix_ieq(url, "http://"))
        return 0;
    p = url + 7;
    n = 0;
    while (p[n] && p[n] != ':' && p[n] != '/')
        n++;
    if (!n || n >= host_max)
        return 0;
    memcpy(host, p, n);
    host[n] = 0;
    *ip = net_parse_ipv4(host);
    p += n;
    pt = 80;
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

static int http_grow(struct net_http_rx *h, u64 need)
{
    void *nb;

    if (need <= h->cap)
        return 1;
    if (!h->alloc || !h->body_max || need > h->body_max)
        return 0;
    {
        u64 cap = h->cap ? h->cap : 65536;
        while (cap < need)
            cap *= 2;
        if (cap > h->body_max)
            cap = h->body_max;
        if (h->alloc(h->alloc_ctx, cap, &nb) != 0)
            return 0;
        if (h->buf) {
            memcpy(nb, h->buf, h->size);
            if (h->free)
                h->free(h->alloc_ctx, h->buf);
        }
        h->buf = nb;
        h->cap = (u32)cap;
        return 1;
    }
}

static void http_body(struct net_http_rx *h, const u8 *d, u32 n)
{
    if (h->done || !n)
        return;
    if (!h->alloc || !h->body_max) {
        if ((u64)h->size + n > 0xffffffffu)
            h->failed = h->done = 1;
        else
            h->size += n;
        return;
    }
    if (!http_grow(h, (u64)h->size + n)) {
        h->too_large = (u64)h->size + n > h->body_max;
        h->failed = 1;
        h->done = 1;
        return;
    }
    memcpy(h->buf + h->size, d, n);
    h->size += n;
}

static void http_chunked(struct net_http_rx *h, const u8 *d, u32 n)
{
    for (u32 i = 0; i < n && !h->done;) {
        u8 c = d[i];
        switch (h->cstate) {
        case 0:
            if (c == '\n') {
                if (!h->cdigits) {
                    h->failed = h->done = 1;
                    return;
                }
                h->cstate = h->cleft ? 1 : 3;
                h->cdigits = 0;
            } else if (h->cdigits >= 0) {
                int v = c >= '0' && c <= '9' ? c - '0'
                      : lower(c) >= 'a' && lower(c) <= 'f' ? lower(c) - 'a' + 10
                      : -1;
                if (v >= 0 && h->cdigits < 8) {
                    h->cleft = h->cleft << 4 | (u32)v;
                    h->cdigits++;
                } else if (v >= 0) {
                    h->failed = h->done = 1;
                    return;
                } else {
                    h->cdigits = h->cdigits ? -h->cdigits : 0;
                }
            }
            i++;
            break;
        case 1: {
            u32 take = n - i < h->cleft ? n - i : h->cleft;
            http_body(h, d + i, take);
            h->cleft -= take;
            i += take;
            if (!h->cleft)
                h->cstate = 2;
            break;
        }
        case 2:
            if (c == '\n')
                h->cstate = 0;
            i++;
            break;
        case 3:
            h->done = 1;
            break;
        }
    }
}

static void http_parse_headers(struct net_http_rx *h)
{
    const char *p = h->hdr;

    h->clen = ~0u;
    h->status = 0;
    if (!prefix_ieq(p, "HTTP/1.")) {
        h->failed = h->done = 1;
        return;
    }
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; i++, p++)
        h->status = h->status * 10 + (*p - '0');
    for (;;) {
        while (*p && *p != '\n')
            p++;
        if (!*p)
            break;
        p++;
        if (prefix_ieq(p, "content-length:")) {
            const char *v = p + 15;
            u32 n = 0;
            while (*v == ' ' || *v == '\t')
                v++;
            while (*v >= '0' && *v <= '9')
                n = n * 10 + (u32)(*v++ - '0');
            h->clen = n;
        } else if (prefix_ieq(p, "transfer-encoding:")) {
            for (const char *v = p + 18; *v && *v != '\r' && *v != '\n'; v++)
                if (prefix_ieq(v, "chunked"))
                    h->chunked = 1;
        } else if (prefix_ieq(p, "location:")) {
            const char *v = p + 9;
            u32 n = 0;
            while (*v == ' ' || *v == '\t')
                v++;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < sizeof h->location - 1)
                n++;
            memcpy(h->location, v, n);
            h->location[n] = 0;
        }
    }
    if (h->chunked)
        h->clen = ~0u;
}

void net_http_rx_init(struct net_http_rx *rx, u32 body_max,
                      int (*alloc)(void *ctx, u64 need, void **p),
                      void (*free)(void *ctx, void *p), void *alloc_ctx)
{
    memset(rx, 0, sizeof *rx);
    rx->body_max = body_max;
    rx->alloc = alloc;
    rx->free = free;
    rx->alloc_ctx = alloc_ctx;
}

void net_http_rx_clear(struct net_http_rx *rx)
{
    if (rx->buf && rx->free)
        rx->free(rx->alloc_ctx, rx->buf);
    {
        int (*a)(void *, u64, void **) = rx->alloc;
        void (*f)(void *, void *) = rx->free;
        void *c = rx->alloc_ctx;
        u32 m = rx->body_max;
        memset(rx, 0, sizeof *rx);
        rx->alloc = a;
        rx->free = f;
        rx->alloc_ctx = c;
        rx->body_max = m;
    }
}

void net_http_rx_feed(struct net_http_rx *h, const u8 *d, u32 n)
{
    while (n && !h->in_body && !h->done) {
        if (h->hlen >= sizeof h->hdr - 1) {
            h->failed = h->done = 1;
            return;
        }
        h->hdr[h->hlen++] = (char)*d++;
        n--;
        if (h->hlen >= 4 && !memcmp(h->hdr + h->hlen - 4, "\r\n\r\n", 4)) {
            h->hdr[h->hlen] = 0;
            http_parse_headers(h);
            h->in_body = 1;
            if (h->status != 200 || h->clen == 0)
                h->done = 1;
            else if (h->clen != ~0u && h->alloc && h->body_max &&
                     !http_grow(h, h->clen)) {
                h->failed = h->done = 1;
                h->too_large = h->clen > h->body_max;
            }
        }
    }
    if (!n || h->done)
        return;
    if (h->chunked) {
        http_chunked(h, d, n);
    } else {
        if (h->clen != ~0u && h->size + n > h->clen)
            n = h->clen - h->size;
        http_body(h, d, n);
        if (h->clen != ~0u && h->size >= h->clen)
            h->done = 1;
    }
}
