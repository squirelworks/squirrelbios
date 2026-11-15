/* Host fuzz / corpus-smoke harness for Authenticode / PKCS#7 parsers
 * (x86_64/secure/der.c, pe_authenticode_*, sb_db_walk). Original code.
 * CONTINUANCE Phase G: feed malformed PKCS#7, X.509, EFI_SIGNATURE_LIST
 * and PE certificate-table blobs without QEMU. Never verifies against a
 * real key store; never StartImage.
 *
 * Build:
 *   make fuzz-authenticode              standalone corpus runner (gcc)
 *   make fuzz-authenticode-libfuzzer    clang -fsanitize=fuzzer,address
 *
 * Entry point is LLVMFuzzerTestOneInput so the same object links under
 * libFuzzer or AFL++ or the standalone main below.
 *
 * Do not include <stdio.h>: the firmware declares void printf, which
 * conflicts with libc. The standalone runner uses POSIX open/read/write. */
#include "secure.h"
#include "host_stubs.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cap so a single input cannot force a multi-megabyte parse copy. */
#define FUZZ_AUTH_MAX (256u * 1024u)

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }

/* Sink for sb_db_walk: exercise entry_key / X.509 path; never accept. */
static int fuzz_db_entry(const EFI_GUID *type, const u8 *data, u32 size, void *ctx)
{
    struct rsa_key k;
    (void)ctx;
    (void)sb_entry_key(type, data, size, &k);
    return 0;
}

/* Walk a PE security directory the same way secure_check_image does, and
 * feed each PKCS#7 WIN_CERTIFICATE body to pkcs7_authenticode. */
static void fuzz_pe_certs(const u8 *img, usize size)
{
    u32 pe, ndirs, off, len, at;
    const u8 *opt;
    u8 h[32];
    struct authenticode a;

    if (size < 0x40 || img[0] != 'M' || img[1] != 'Z')
        return;
    pe = rd32(img + 0x3C);
    if ((u64)pe + 24 + 112 > size || memcmp(img + pe, "PE\0\0", 4))
        return;
    opt = img + pe + 24;
    if (rd16(opt) != 0x20B)
        return;
    ndirs = rd32(opt + 108);
    off = 0;
    len = 0;
    if (ndirs > 4) {
        off = rd32(opt + 112 + 4 * 8);
        len = rd32(opt + 112 + 4 * 8 + 4);
    }
    if (!len || (u64)off + len > size || off < 0x40)
        return;

    (void)pe_authenticode_hash(img, size, h);

    for (at = 0; at + 8 <= len;) {
        const u8 *w = img + off + at;
        u32 wlen = rd32(w);
        if (wlen < 8 || wlen > len - at)
            break;
        if (rd16(w + 4) == 0x0200 && rd16(w + 6) == 0x0002) {
            if (pkcs7_authenticode(w + 8, wlen - 8, &a)) {
                u8 sh[32];
                sb_signed_hash(&a, sh);
                (void)sh;
                (void)h;
            }
        }
        at += (wlen + 7) & ~7u;
    }
}

int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    struct authenticode a;
    struct rsa_key k;
    u8 *buf;
    u8 content_hash[32];
    u8 pehash[32];
    usize cap;

    host_reset();
    if (!data)
        return 0;

    cap = size > FUZZ_AUTH_MAX ? FUZZ_AUTH_MAX : size;
    buf = (u8 *)malloc(cap ? cap : 1);
    if (!buf)
        return 0;
    if (cap)
        memcpy(buf, data, cap);

    /* PKCS#7 Authenticode (SpcIndirectDataContent + signer). */
    if (cap <= 0xffffffffu)
        (void)pkcs7_authenticode(buf, (u32)cap, &a);

    /* Detached PKCS#7 (authvar path): content hash is zeros, or the
     * leading 32 bytes when present; also try the tail alone. */
    memset(content_hash, 0, sizeof content_hash);
    if (cap >= 32)
        memcpy(content_hash, buf, 32);
    if (cap <= 0xffffffffu)
        (void)pkcs7_detached(buf, (u32)cap, content_hash, &a);
    if (cap > 32 && (cap - 32) <= 0xffffffffu)
        (void)pkcs7_detached(buf + 32, (u32)(cap - 32), content_hash, &a);

    /* X.509 SubjectPublicKeyInfo / certificate. */
    if (cap <= 0xffffffffu)
        (void)x509_rsa_key(buf, (u32)cap, &k);

    /* Authenticode PE hash (headers + sections; ignores cert table). */
    (void)pe_authenticode_hash(buf, cap, pehash);

    /* PE security directory → PKCS#7 bodies. */
    fuzz_pe_certs(buf, cap);

    /* EFI_SIGNATURE_LIST database (db / dbx shape). */
    if (cap <= 0xffffffffu)
        (void)sb_db_walk(buf, (u32)cap, fuzz_db_entry, NULL);

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
        say("fuzz-authenticode: cannot open ");
        say(path);
        say("\n");
        return 1;
    }
    n = lseek(fd, 0, SEEK_END);
    if (n < 0) {
        close(fd);
        say("fuzz-authenticode: cannot size ");
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
