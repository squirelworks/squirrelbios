/* SquirrelBIOS UEFI: pure DHCP / TFTP / HTTP parsers for the network
 * stack and host fuzz (CONTINUANCE Phase G). Original code from
 * RFC 2131/2132 (DHCP), RFC 1350/2347/2348/2349 (TFTP), RFC 9110/9112
 * (HTTP) and RFC 3986 (URLs). No sockets, no DMA. */
#ifndef NET_PARSE_H
#define NET_PARSE_H

#include "sq.h"

/* Filled by net_parse_dhcp from a BOOTP/DHCP reply. Same layout as the
 * network stack's net_cfg (duplicated here so this header does not pull
 * efi.h). */
struct net_parse_cfg {
    u32 ip, mask, gw, server, dns;
    char file[128];
    char server_name[64];
};

/* DHCP/BOOTP reply: requires op==BOOTREPLY, magic cookie, len >= 240.
 * Fills cfg (yiaddr/siaddr + options 1/3/6/52/53/54/66/67 and file /
 * sname when not overloaded). 1 when the packet was accepted. */
int net_parse_dhcp(const u8 *pkt, u32 len, struct net_parse_cfg *cfg,
                   u8 *msgtype, u32 *server_id);

/* TFTP packet view (opcodes 3 DATA, 5 ERROR, 6 OACK). */
struct net_tftp_view {
    u16 opcode;
    u16 block;          /* DATA */
    u32 blksize;        /* OACK; 0 if absent */
    u32 tsize;          /* OACK; 0 if absent */
    const u8 *payload;  /* DATA bytes after the 4-byte header, or ERROR text */
    u32 payload_len;
    char err[64];       /* ERROR message, NUL-terminated when opcode 5 */
};

/* Parse one TFTP message. 1 when opcode is recognised and lengths ok. */
int net_parse_tftp(const u8 *pkt, u32 len, struct net_tftp_view *out);

/* http://host[:port][/path]: 1 with pieces. *ip set when host is dotted
 * IPv4; otherwise 0 for the caller to resolve. */
int net_parse_http_url(const char *url, u32 *ip, u16 *port,
                       const char **path, char *host, u32 host_max);

/* Streaming HTTP/1.x response parser (headers + Content-Length or
 * chunked body). Cap body growth at body_max (0 = no body stored). */
struct net_http_rx {
    char hdr[4096];
    u32 hlen;
    int in_body, done, failed, too_large;
    int status;
    int chunked;
    u32 clen;
    u8 *buf;
    u32 size, cap;
    u32 body_max;
    int cstate;
    u32 cleft;
    int cdigits;
    char location[256];
    /* Optional allocator; when NULL, body bytes are counted but not kept. */
    int (*alloc)(void *ctx, u64 need, void **p);
    void (*free)(void *ctx, void *p);
    void *alloc_ctx;
};

void net_http_rx_init(struct net_http_rx *rx, u32 body_max,
                      int (*alloc)(void *ctx, u64 need, void **p),
                      void (*free)(void *ctx, void *p), void *alloc_ctx);
void net_http_rx_feed(struct net_http_rx *rx, const u8 *d, u32 n);
void net_http_rx_clear(struct net_http_rx *rx); /* free body; keep callbacks */

/* Dotted IPv4 a.b.c.d → host-order-as-bytes u32, or 0. */
u32 net_parse_ipv4(const char *s);

#endif
