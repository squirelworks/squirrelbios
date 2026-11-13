#!/usr/bin/env python3
"""Turn a static ELF64 stand-in into a PE32+ EFI application. Original code.

    tests/mkpe.py MACHINE IN.elf OUT.efi

The ELF is linked by tests/efi-app.ld: __image_base (the PE image base)
and one loaded section from image base + 4 KB, linked with --emit-relocs
so its absolute 64-bit relocations (R_AARCH64_ABS64, R_RISCV_64) are
still there; each becomes a DIR64 base relocation. The PE has headers at
0, .text (everything) at 0x1000 and .reloc after it, file layout equal to
memory layout. MACHINE is aarch64 or riscv64.
"""
import struct
import sys

MACHINES = {"aarch64": (0xAA64, 183, 257), "riscv64": (0x5064, 243, 2)}
ALIGN = 0x1000


def align(v, a=ALIGN):
    return (v + a - 1) // a * a


def main():
    machine, src, dst = sys.argv[1:4]
    pe_machine, elf_machine, abs64 = MACHINES[machine]
    elf = open(src, "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        sys.exit("mkpe: not a little-endian ELF64 file")
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", elf, 16)
    if e_machine != elf_machine:
        sys.exit("mkpe: ELF machine %d is not %s" % (e_machine, machine))
    sections = [struct.unpack_from("<IIQQQQIIQQ", elf, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    shstr = sections[e_shstrndx]
    names = elf[shstr[4]:shstr[4] + shstr[5]]

    def name(sh):
        return names[sh[0]:names.index(b"\0", sh[0])].decode()

    symtab = next(s for s in sections if s[1] == 2)
    strtab = sections[symtab[6]]
    base = None
    for off in range(symtab[4], symtab[4] + symtab[5], 24):
        st_name, _, _, _, st_value, _ = struct.unpack_from("<IBBHQQ", elf, off)
        s = elf[strtab[4] + st_name:elf.index(b"\0", strtab[4] + st_name)]
        if s == b"__image_base":
            base = st_value
    if base is None:
        sys.exit("mkpe: no __image_base symbol")

    text = [s for s in sections if name(s) == ".text"][0]
    _, sh_type, _, addr, offset, size = text[:6]
    if addr != base + ALIGN:
        sys.exit("mkpe: .text is at %#x, not image base + 4 KB" % addr)
    body = bytearray(elf[offset:offset + size]) if sh_type != 8 else bytearray(size)
    text_size = align(len(body))
    body += bytes(text_size - len(body))

    # absolute relocations against .text, kept by --emit-relocs
    sites = []
    for s in sections:
        if s[1] == 4 and sections[s[7]] is text:        # SHT_RELA for .text
            for off in range(s[4], s[4] + s[5], 24):
                r_offset, r_info, _ = struct.unpack_from("<QQq", elf, off)
                if r_info & 0xFFFFFFFF == abs64:
                    sites.append(r_offset - base)
    sites.sort()
    reloc = bytearray()
    pages = {}
    for rva in sites:
        pages.setdefault(rva & ~0xFFF, []).append(rva & 0xFFF)
    for page in sorted(pages):
        entries = [0xA000 | o for o in pages[page]]
        if len(entries) % 2:
            entries.append(0)
        reloc += struct.pack("<II", page, 8 + 2 * len(entries))
        reloc += struct.pack("<%dH" % len(entries), *entries)
    reloc_rva = ALIGN + text_size
    reloc_size = len(reloc)
    reloc_raw = align(max(reloc_size, 1))
    reloc += bytes(reloc_raw - reloc_size)
    image_size = reloc_rva + reloc_raw

    hdr = bytearray(ALIGN)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x40)
    pe = 0x40
    hdr[pe:pe + 4] = b"PE\0\0"
    opt_size = 112 + 16 * 8
    struct.pack_into("<HHIIIHH", hdr, pe + 4, pe_machine, 2, 0, 0, 0, opt_size, 0x0022)
    opt = pe + 24
    struct.pack_into("<HBBIIIII", hdr, opt, 0x20B, 0, 0, text_size, reloc_raw, 0,
                     e_entry - base, ALIGN)
    struct.pack_into("<QIIHHHHHHIIIIHHQQQQII", hdr, opt + 24, base, ALIGN, ALIGN,
                     0, 0, 0, 0, 0, 0, 0, image_size, ALIGN, 0, 10, 0,
                     0x10000, 0x1000, 0x10000, 0x1000, 0, 16)
    struct.pack_into("<II", hdr, opt + 112 + 5 * 8, reloc_rva, reloc_size)
    sect = opt + opt_size
    struct.pack_into("<8sIIIIIIHHI", hdr, sect, b".text", text_size, ALIGN, text_size, ALIGN,
                     0, 0, 0, 0, 0xE0000020)
    struct.pack_into("<8sIIIIIIHHI", hdr, sect + 40, b".reloc", reloc_size, reloc_rva, reloc_raw,
                     reloc_rva, 0, 0, 0, 0, 0x42000040)
    with open(dst, "wb") as f:
        f.write(hdr + body + reloc)


if __name__ == "__main__":
    main()
