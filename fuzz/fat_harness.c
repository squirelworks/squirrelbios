/* Host fuzz / corpus-smoke harness for common/efi/fat.c.
 * Original code. CONTINUANCE Phase G: feed malformed disk images into
 * the firmware FAT12/16/32 Simple File System without QEMU.
 *
 * Build:
 *   make fuzz-fat              standalone corpus runner (gcc)
 *   make fuzz-fat-libfuzzer    clang -fsanitize=fuzzer,address (optional)
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

/* Cap so a single input cannot allocate a multi-gigabyte working copy. */
#define FUZZ_IMG_MAX (512u * 1024u)

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

struct fuzz_disk {
    EFI_BLOCK_IO_PROTOCOL bio;
    EFI_BLOCK_IO_MEDIA media;
    u8 *img;
    usize len;
};

static EFI_STATUS EFIAPI bio_reset(EFI_BLOCK_IO_PROTOCOL *this, BOOLEAN ext)
{
    (void)this;
    (void)ext;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bio_read(EFI_BLOCK_IO_PROTOCOL *this, u64 media_id,
                                  u64 lba, UINTN bytes, void *buf)
{
    struct fuzz_disk *d = (struct fuzz_disk *)this;
    u64 off, end;
    if (!d || !buf || media_id != d->media.media_id)
        return EFI_MEDIA_CHANGED;
    if (bytes == 0)
        return EFI_SUCCESS;
    if (bytes % d->media.block_size)
        return EFI_BAD_BUFFER_SIZE;
    off = lba * d->media.block_size;
    end = off + bytes;
    if (end < off || end > d->len)
        return EFI_DEVICE_ERROR;
    memcpy(buf, d->img + (usize)off, (usize)bytes);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bio_write(EFI_BLOCK_IO_PROTOCOL *this, u64 media_id,
                                   u64 lba, UINTN bytes, void *buf)
{
    struct fuzz_disk *d = (struct fuzz_disk *)this;
    u64 off, end;
    if (!d || !buf || media_id != d->media.media_id)
        return EFI_MEDIA_CHANGED;
    if (d->media.read_only)
        return EFI_WRITE_PROTECTED;
    if (bytes == 0)
        return EFI_SUCCESS;
    if (bytes % d->media.block_size)
        return EFI_BAD_BUFFER_SIZE;
    off = lba * d->media.block_size;
    end = off + bytes;
    if (end < off || end > d->len)
        return EFI_DEVICE_ERROR;
    memcpy(d->img + (usize)off, buf, (usize)bytes);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bio_flush(EFI_BLOCK_IO_PROTOCOL *this)
{
    (void)this;
    return EFI_SUCCESS;
}

static u32 pick_block_size(const u8 *data, usize size)
{
    u16 bps;
    if (size < 13)
        return 512;
    bps = rd16(data + 11);
    if (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096)
        return bps;
    return 512;
}

/* Walk a few directory entries and try a short list of opens/reads so
 * the cluster / LFN / FAT-chain paths see the input, not only the BPB. */
static void poke_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs)
{
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS s;
    u8 info[512];
    UINTN n;
    static const CHAR16 *const names[] = {
        u"\\",
        u"\\EFI",
        u"\\EFI\\BOOT",
        u"\\EFI\\BOOT\\BOOTX64.EFI",
        u"\\README.TXT",
        u"\\A.TXT",
        u"\\VERYLONGNAMETHATNEEDSLFN.TXT",
    };
    usize i;

    if (!sfs || !sfs->open_volume)
        return;
    s = sfs->open_volume(sfs, &root);
    if (s != EFI_SUCCESS || !root)
        return;

    for (i = 0; i < 8; i++) {
        n = sizeof info;
        memset(info, 0, sizeof info);
        if (root->read(root, &n, info) != EFI_SUCCESS || n == 0)
            break;
    }
    root->set_position(root, 0);

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        f = NULL;
        s = root->open(root, &f, (CHAR16 *)names[i], EFI_FILE_MODE_READ, 0);
        if (s != EFI_SUCCESS || !f)
            continue;
        n = sizeof info;
        f->get_info(f, (EFI_GUID *)&efi_file_info_guid, &n, info);
        n = 64;
        f->read(f, &n, info);
        f->close(f);
    }

    /* One write-create attempt: exercises allocation / LFN naming when
     * the volume still has free clusters; failures are fine. */
    f = NULL;
    if (root->open(root, &f, (CHAR16 *)u"\\FUZZ.TMP",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0) == EFI_SUCCESS && f) {
        n = 4;
        f->write(f, &n, (void *)"fuzz");
        f->flush(f);
        f->close(f);
    }

    root->close(root);
}

int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    struct fuzz_disk disk;
    u8 *img;
    usize cap, rounded;
    u32 bsz;

    host_reset();
    if (!data || size < 32)
        return 0;

    cap = size > FUZZ_IMG_MAX ? FUZZ_IMG_MAX : size;
    bsz = pick_block_size(data, cap);
    rounded = (cap + bsz - 1) / bsz * bsz;
    if (rounded < bsz)
        return 0;
    if (rounded > FUZZ_IMG_MAX)
        rounded = FUZZ_IMG_MAX / bsz * bsz;

    img = (u8 *)malloc(rounded);
    if (!img)
        return 0;
    memset(img, 0, rounded);
    memcpy(img, data, cap);

    memset(&disk, 0, sizeof disk);
    disk.img = img;
    disk.len = rounded;
    disk.media.media_id = 1;
    disk.media.removable_media = 0;
    disk.media.media_present = 1;
    disk.media.logical_partition = 1;
    disk.media.read_only = 0;
    disk.media.write_caching = 0;
    disk.media.block_size = bsz;
    disk.media.io_align = 1;
    disk.media.last_block = rounded / bsz - 1;
    disk.bio.revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    disk.bio.media = &disk.media;
    disk.bio.reset = bio_reset;
    disk.bio.read_blocks = bio_read;
    disk.bio.write_blocks = bio_write;
    disk.bio.flush_blocks = bio_flush;

    if (fat_install((EFI_HANDLE)(uptr)0x1, &disk.bio))
        poke_volume(host_last_sfs());

    free(img);
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
        say("fuzz-fat: cannot open ");
        say(path);
        say("\n");
        return 1;
    }
    n = lseek(fd, 0, SEEK_END);
    if (n < 0) {
        close(fd);
        say("fuzz-fat: cannot size ");
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

    /* "ok <path> (<n> bytes)\n" without libc printf */
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
