/* SquirrelBIOS common/fw: ELF64 executable loader (either byte order).
 * Written from the System V ABI's ELF-64 object file format.
 * Original code. */
#include "fwlib.h"

#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#define ET_EXEC     2
#define ET_DYN      3
#define PT_LOAD     1

struct ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static uint16_t g16(const struct elf_image *img, uint16_t v)
{
    return img->big_endian == FW_BIG_ENDIAN ? v : fw_bswap16(v);
}
static uint32_t g32(const struct elf_image *img, uint32_t v)
{
    return img->big_endian == FW_BIG_ENDIAN ? v : fw_bswap32(v);
}
static uint64_t g64(const struct elf_image *img, uint64_t v)
{
    return img->big_endian == FW_BIG_ENDIAN ? v : fw_bswap64(v);
}

static void read_phdr(const void *file, const struct elf_image *img,
                      const struct ehdr *eh, int i, struct phdr *ph)
{
    struct phdr raw;
    memcpy(&raw, (const uint8_t *)file + g64(img, eh->phoff) +
                 (uint64_t)i * g16(img, eh->phentsize), sizeof(raw));
    ph->type = g32(img, raw.type);
    ph->flags = g32(img, raw.flags);
    ph->offset = g64(img, raw.offset);
    ph->vaddr = g64(img, raw.vaddr);
    ph->paddr = g64(img, raw.paddr);
    ph->filesz = g64(img, raw.filesz);
    ph->memsz = g64(img, raw.memsz);
    ph->align = g64(img, raw.align);
}

int elf_probe(const void *file, uint64_t size, int machine, struct elf_image *img)
{
    struct ehdr eh;
    memset(img, 0, sizeof(*img));
    memcpy(&eh, file, sizeof(eh));
    if (memcmp(eh.ident, "\177ELF", 4))
        return -1;
    if (eh.ident[EI_CLASS] != ELFCLASS64 ||
        (eh.ident[EI_DATA] != ELFDATA2LSB && eh.ident[EI_DATA] != ELFDATA2MSB))
        return -2;
    img->big_endian = eh.ident[EI_DATA] == ELFDATA2MSB;
    uint16_t type = g16(img, eh.type);
    if (type != ET_EXEC && type != ET_DYN)
        return -2;
    img->machine = g16(img, eh.machine);
    if (img->machine != machine)
        return -3;
    img->entry = g64(img, eh.entry);
    int n = g16(img, eh.phnum);
    if (size && g64(img, eh.phoff) + (uint64_t)n * g16(img, eh.phentsize) > size)
        return -5;

    int loads = 0, entry_found = 0;
    img->low = ~0ull;
    img->high = 0;
    /* Any segment with p_paddr 0 makes the image run in place (Linux's
     * vmlinux: linked at 0xc000..., p_paddr 0, relocates itself) */
    for (int i = 0; i < n; i++) {
        struct phdr ph;
        read_phdr(file, img, &eh, i, &ph);
        if (ph.type == PT_LOAD && ph.paddr == 0)
            img->in_place = 1;
    }
    for (int i = 0; i < n; i++) {
        struct phdr ph;
        read_phdr(file, img, &eh, i, &ph);
        if (ph.type != PT_LOAD)
            continue;
        if (size && ph.offset + ph.filesz > size)
            return -5;
        loads++;
        uint64_t lo, hi;
        if (img->in_place) {
            lo = (uint64_t)(uintptr_t)file + ph.offset;
            hi = lo + ph.memsz;
        } else {
            lo = ph.paddr;
            hi = ph.paddr + ph.memsz;
        }
        if (lo < img->low)
            img->low = lo;
        if (hi > img->high)
            img->high = hi;
        if (!entry_found && img->entry >= ph.vaddr && img->entry - ph.vaddr < ph.memsz) {
            entry_found = 1;
            img->entry_phys = (img->in_place ? (uint64_t)(uintptr_t)file + ph.offset : ph.paddr) +
                              (img->entry - ph.vaddr);
        }
    }
    if (!loads)
        return -4;
    if (!entry_found)  /* entry outside the segments: treat it as physical */
        img->entry_phys = img->entry;
    return 0;
}

void elf_place(const void *file, const struct elf_image *img)
{
    struct ehdr eh;
    if (img->in_place)
        return;
    memcpy(&eh, file, sizeof(eh));
    int n = g16(img, eh.phnum);
    for (int i = 0; i < n; i++) {
        struct phdr ph;
        read_phdr(file, img, &eh, i, &ph);
        if (ph.type != PT_LOAD)
            continue;
        uint8_t *dst = (uint8_t *)(uintptr_t)ph.paddr;
        memmove(dst, (const uint8_t *)file + ph.offset, ph.filesz);
        memset(dst + ph.filesz, 0, ph.memsz - ph.filesz);
    }
}

const char *elf_error(int err)
{
    switch (err) {
    case -1: return "not an ELF file";
    case -2: return "not a 64-bit ELF executable";
    case -3: return "ELF file is for another machine";
    case -4: return "ELF file has nothing to load";
    case -5: return "ELF segment runs past the end of the file";
    }
    return "bad ELF file";
}
