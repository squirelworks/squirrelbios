/* A 3 MB BOOTX64.EFI for the disk read path (roadmap D: the OS/2 UEFI
 * loader). Original code. Built like tests/x86_64-einj.c, then
 * tests/fillblob.py fills blob[] after its marker with the xorshift32
 * stream from seed 1. Read from the ESP in 512-byte clusters, it is
 * some 6000 clusters, so the firmware must both wait for every disk
 * request (not give up after a fixed number of polls) and read runs of
 * clusters at once. Reports on COM1:
 *
 *   APP bigload blob=ok
 *
 * or blob=bad if a byte of the stream is wrong. */

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define BLOB_SIZE (3u << 20)

/* Marker, then BLOB_SIZE - 8 bytes of stream; nonzero so it is file data */
__attribute__((used, aligned(16))) static volatile u8 blob[BLOB_SIZE] = { 'S', 'Q', 'B', 'L', 'O', 'B', 'M', 'K', 1 };

static void outb(unsigned short p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static u8 inb(unsigned short p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static void puts(const char *s)
{
    while (*s) {
        while (!(inb(0x3FD) & 0x20))
            ;
        outb(0x3F8, (u8)*s++);
    }
}

__attribute__((ms_abi)) u64 efi_main(void *image, void *st)
{
    (void)image;
    (void)st;
    u32 x = 1;
    int ok = 1;
    for (u32 i = 8; i < BLOB_SIZE; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if (blob[i] != (u8)x) {
            ok = 0;
            break;
        }
    }
    puts(ok ? "APP bigload blob=ok\r\n" : "APP bigload blob=bad\r\n");
    return 0;
}
