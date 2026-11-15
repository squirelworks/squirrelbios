/* A Simple Network Protocol test app for the UEFI firmware (x86_64,
 * aarch64, riscv64), started by the firmware's network boot. Original
 * code; the protocol is declared here from the UEFI specification, not
 * from the firmware's headers, so a layout mistake on either side shows.
 *
 * It finds the SNP, walks it through Start, Initialize, ReceiveFilters,
 * MCastIpToMac, then sends an ARP request for 10.0.2.2 (QEMU's user
 * network gateway) with Transmit filling the Ethernet header, gets the
 * buffer back from GetStatus, waits on WaitForPacket, checks that a
 * small buffer gets EFI_BUFFER_TOO_SMALL with the size, receives the
 * ARP reply, finds the MAC address node on the handle's device path and
 * shuts the NIC down. One line, each check ok or bad:
 *
 *   SNP mode=ok start=ok init=ok filter=ok mcast=ok tx=ok txbuf=ok wait=ok small=ok rx=ok arp=ok path=ok stop=ok
 *
 * then it returns 0x1234. Built like tests/efi-app.c (tests/mkpe.py). */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long uintn;
typedef u64 status;
typedef void *handle;
typedef u16 char16;

#if defined(__x86_64__)
#define API __attribute__((ms_abi))
#else
#define API
#endif

#define ERR(n) (0x8000000000000000ULL | (n))
#define BUFFER_TOO_SMALL ERR(5)
#define NOT_READY ERR(6)
#define ALREADY_STARTED ERR(20)

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } guid;
typedef struct { u64 signature; u32 revision, header_size, crc32, reserved; } table_header;
typedef void (*nofn)(void);

typedef struct {
    table_header hdr;
    nofn raise_tpl, restore_tpl, allocate_pages, free_pages, get_memory_map, allocate_pool;
    status (API *free_pool)(void *);
    nofn create_event, set_timer, wait_for_event, signal_event, close_event;
    status (API *check_event)(void *);
    nofn install_protocol, reinstall_protocol, uninstall_protocol;
    status (API *handle_protocol)(handle, const guid *, void **);
    nofn reserved, register_protocol_notify, locate_handle, locate_device_path,
         install_configuration_table, load_image, start_image, exit, unload_image,
         exit_boot_services, get_next_monotonic_count;
    status (API *stall)(uintn);
    nofn set_watchdog_timer, connect_controller, disconnect_controller, open_protocol,
         close_protocol, open_protocol_information, protocols_per_handle;
    status (API *locate_handle_buffer)(u32, const guid *, void *, uintn *, handle **);
    nofn locate_protocol;
} boot_services;

typedef struct text_out {
    nofn reset;
    status (API *output_string)(struct text_out *, char16 *);
} text_out;

typedef struct {
    table_header hdr;
    char16 *firmware_vendor;
    u32 firmware_revision;
    handle con_in_handle;
    void *con_in;
    handle con_out_handle;
    text_out *con_out;
    handle stderr_handle;
    void *std_err;
    void *rt;
    boot_services *bs;
} system_table;

typedef struct { u8 addr[32]; } mac_addr;
typedef union { u32 v4; u8 addr[16]; } ip_addr;

typedef struct {
    u32 state, hw_address_size, media_header_size, max_packet_size, nvram_size,
        nvram_access_size, receive_filter_mask, receive_filter_setting,
        max_mcast_filter_count, mcast_filter_count;
    mac_addr mcast_filter[16];
    mac_addr current_address, broadcast_address, permanent_address;
    u8 if_type, mac_address_changeable, multiple_tx_supported, media_present_supported,
       media_present;
} snp_mode;

typedef struct snp {
    u64 revision;
    status (API *start)(struct snp *);
    status (API *stop)(struct snp *);
    status (API *initialize)(struct snp *, uintn, uintn);
    status (API *reset)(struct snp *, u8);
    status (API *shutdown)(struct snp *);
    status (API *receive_filters)(struct snp *, u32, u32, u8, uintn, mac_addr *);
    status (API *station_address)(struct snp *, u8, mac_addr *);
    status (API *statistics)(struct snp *, u8, uintn *, void *);
    status (API *mcast_ip_to_mac)(struct snp *, u8, ip_addr *, mac_addr *);
    status (API *nvdata)(struct snp *, u8, uintn, uintn, void *);
    status (API *get_status)(struct snp *, u32 *, void **);
    status (API *transmit)(struct snp *, uintn, uintn, void *, mac_addr *, mac_addr *, u16 *);
    status (API *receive)(struct snp *, uintn *, uintn *, void *, mac_addr *, mac_addr *, u16 *);
    void *wait_for_packet;
    snp_mode *mode;
} snp;

static const guid snp_guid = { 0xA19832B9, 0xAC25, 0x11D3, { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
static const guid dp_guid = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static system_table *st;
static boot_services *bs;

static void puts(const char *s)
{
    char16 b[128];
    int n = 0;
    while (*s && n < 126) {
        if (*s == '\n')
            b[n++] = '\r';
        b[n++] = (u8)*s++;
    }
    b[n] = 0;
    st->con_out->output_string(st->con_out, b);
}

static void check(const char *name, int ok)
{
    puts(" ");
    puts(name);
    puts(ok ? "=ok" : "=bad");
}

static int same(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static u8 txf[64];
static u8 rxf[1600];

API status efi_main(handle image, system_table *systab)
{
    (void)image;
    st = systab;
    bs = st->bs;
    uintn count = 0;
    handle *hs = 0;
    snp *n = 0;
    puts("SNP");
    if (bs->locate_handle_buffer(2, &snp_guid, 0, &count, &hs) || !count ||
        bs->handle_protocol(hs[0], &snp_guid, (void **)&n) || !n) {
        puts(" none=bad\n");
        return 0x1234;
    }
    snp_mode *m = n->mode;
    u8 *mac = m->current_address.addr;
    check("mode", m->state == 0 && m->hw_address_size == 6 && m->media_header_size == 14 &&
                  m->max_packet_size >= 1500 && m->if_type == 1 && m->media_present &&
                  same(mac, m->permanent_address.addr, 6) && m->broadcast_address.addr[0] == 0xFF);
    check("start", n->start(n) == 0 && m->state == 1 && n->start(n) == ALREADY_STARTED);
    check("init", n->initialize(n, 0, 0) == 0 && m->state == 2);
    check("filter", n->receive_filters(n, 0x01 | 0x04, 0, 0, 0, 0) == 0 &&
                    m->receive_filter_setting == 0x05);
    ip_addr mip = { .addr = { 224, 0, 0, 251 } };
    mac_addr mm;
    static const u8 want_mm[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB };
    check("mcast", n->mcast_ip_to_mac(n, 0, &mip, &mm) == 0 && same(mm.addr, want_mm, 6));

    /* ARP: who has 10.0.2.2? tell 10.0.2.15 */
    u8 *a = txf + 14;
    static const u8 arp_head[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };
    for (int i = 0; i < 8; i++)
        a[i] = arp_head[i];
    for (int i = 0; i < 6; i++) {
        a[8 + i] = mac[i];
        a[18 + i] = 0;
    }
    a[14] = 10; a[15] = 0; a[16] = 2; a[17] = 15;
    a[24] = 10; a[25] = 0; a[26] = 2; a[27] = 2;
    for (int i = 42; i < 60; i++)
        txf[i] = 0;
    mac_addr bc;
    for (int i = 0; i < 32; i++)
        bc.addr[i] = i < 6 ? 0xFF : 0;
    u16 proto = 0x0806;
    check("tx", n->transmit(n, 14, 60, txf, 0, &bc, &proto) == 0 && txf[12] == 8 && txf[13] == 6 &&
                same(txf, bc.addr, 6) && same(txf + 6, mac, 6));
    void *back = 0;
    u32 ints = 0;
    check("txbuf", n->get_status(n, &ints, &back) == 0 && back == txf && (ints & 2));

    int signalled = 0, got = 0, small = 0, arp = 0;
    for (int t = 0; t < 2000 && !arp; t++) {
        if (!signalled && n->wait_for_packet && bs->check_event(n->wait_for_packet) == 0)
            signalled = 1;
        uintn hsz = 0, size = 10;
        mac_addr src, dst;
        u16 p = 0;
        status s = n->receive(n, &hsz, &size, rxf, &src, &dst, &p);
        if (s == BUFFER_TOO_SMALL) {
            small = size >= 42;
            size = sizeof rxf;
            s = n->receive(n, &hsz, &size, rxf, &src, &dst, &p);
        }
        if (s == NOT_READY) {
            bs->stall(1000);
            continue;
        }
        if (s)
            break;
        got = 1;
        const u8 *r = rxf + 14;
        if (p == 0x0806 && hsz == 14 && same(dst.addr, mac, 6) && same(src.addr, rxf + 6, 6) &&
            r[6] == 0 && r[7] == 2 && r[14] == 10 && r[15] == 0 && r[16] == 2 && r[17] == 2)
            arp = 1;
    }
    check("wait", signalled);
    check("small", small);
    check("rx", got);
    check("arp", arp);

    /* The handle's device path ends in a MAC address node with this MAC */
    u8 *dp = 0;
    int path = 0;
    if (bs->handle_protocol(hs[0], &dp_guid, (void **)&dp) == 0 && dp) {
        for (int i = 0; i < 16 && dp[0] != 0x7F; i++) {
            u16 len = (u16)(dp[2] | dp[3] << 8);
            if (dp[0] == 3 && dp[1] == 11 && len == 37 && same(dp + 4, mac, 6) && dp[36] == 1)
                path = 1;
            if (len < 4)
                break;
            dp += len;
        }
    }
    check("path", path);
    check("stop", n->shutdown(n) == 0 && m->state == 1 && n->stop(n) == 0 && m->state == 0);
    puts("\n");
    bs->free_pool(hs);
    return 0x1234;
}

/* What the compiler may call for the loops above */
void *memset(void *d, int c, unsigned long n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
