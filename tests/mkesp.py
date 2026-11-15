#!/usr/bin/env python3
"""Build a GPT disk with a FAT32 EFI System Partition for the UEFI tests.
Original code.

  tests/mkesp.py BOOTX64.EFI OUT.img [NAME]

NAME is the file's name in \\EFI\\BOOT (BOOTX64.EFI when left out). A name
that is not 8.3, such as BOOTRISCV64.EFI, gets long-name entries and a
BOOTRI~1.EFI alias.

The image is 64 MiB: protective MBR, GPT header and entries, then a FAT32
ESP starting at LBA 2048 holding the boot file.
FAT32 needs at least 65525 clusters; 64 MiB with 512-byte clusters covers that.
"""
import math
import struct
import sys
import uuid

SECTOR = 512
ESP_LBA = 2048
DISK_BYTES = 64 * 1024 * 1024
DISK_SECS = DISK_BYTES // SECTOR
ESP_SECS = DISK_SECS - ESP_LBA - 33          # leave room for backup GPT
ESP_GUID = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
PART_GUID = uuid.UUID('A1B2C3D4-E5F6-4789-ABCD-EF0123456789')
DISK_GUID = uuid.UUID('01234567-89AB-CDEF-0123-456789ABCDEF')


def u16(x):
    return struct.pack('<H', x)


def u32(x):
    return struct.pack('<I', x)


def u64(x):
    return struct.pack('<Q', x)


def crc32(data: bytes) -> int:
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF


def guid_bytes(u: uuid.UUID) -> bytes:
    """UEFI mixed-endian GUID encoding."""
    b = u.bytes_le[:4] + u.bytes_le[4:6] + u.bytes_le[6:8] + u.bytes[8:]
    return b


def short_name(name: str) -> bytes:
    """The 8.3 directory name for name, or a ~1 alias when it is not 8.3."""
    base, _, ext = name.upper().rpartition('.')
    if not base:
        base, ext = ext, ''
    if len(base) <= 8 and len(ext) <= 3:
        return (base.ljust(8) + ext.ljust(3)).encode()
    return (base[:6] + '~1').ljust(8).encode() + ext[:3].ljust(3).encode()


def lfn_entries(name: str, short: bytes) -> list:
    """Long-name entries for name, in on-disk order (last part first)."""
    csum = 0
    for b in short:
        csum = (((csum & 1) << 7) + (csum >> 1) + b) & 0xFF
    chars = [ord(c) for c in name] + [0]
    chars += [0xFFFF] * (-len(chars) % 13)
    parts = len(chars) // 13
    out = []
    for n in range(parts, 0, -1):
        c = chars[(n - 1) * 13:n * 13]
        e = bytearray(32)
        e[0] = n | (0x40 if n == parts else 0)
        e[1:11] = b''.join(u16(x) for x in c[0:5])
        e[11] = 0x0F
        e[13] = csum
        e[14:26] = b''.join(u16(x) for x in c[5:11])
        e[28:32] = b''.join(u16(x) for x in c[11:13])
        out.append(bytes(e))
    return out


def make_fat32(esp_secs: int, efi_file: bytes, name: str = 'BOOTX64.EFI') -> bytearray:
    """A FAT32 volume of esp_secs sectors containing EFI/BOOT/<name>."""
    bps = 512
    spc = 1                                  # 512-byte clusters: enough for FAT32 in 64 MiB
    reserved = 32
    fats = 2
    fat_secs = 1
    for _ in range(8):
        data_secs = esp_secs - reserved - fats * fat_secs
        clusters = data_secs // spc
        fat_secs = max(1, math.ceil((clusters + 2) * 4 / bps))
    data_secs = esp_secs - reserved - fats * fat_secs
    clusters = data_secs // spc
    # The FAT must have an entry for every cluster: if the last round
    # left it one short, the volume ends where the FAT does
    clusters = min(clusters, fat_secs * bps // 4 - 2)
    vol_secs = reserved + fats * fat_secs + clusters * spc
    assert clusters >= 65525, f'volume too small for FAT32 ({clusters} clusters)'

    vol = bytearray(esp_secs * bps)

    # Boot sector
    bs = bytearray(bps)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSDOS5.0'
    bs[11:13] = u16(bps)
    bs[13] = spc
    bs[14:16] = u16(reserved)
    bs[16] = fats
    bs[17:19] = u16(0)                       # root entries
    bs[19:21] = u16(0)
    bs[21] = 0xF8
    bs[22:24] = u16(0)                       # fat16 size
    bs[24:26] = u16(32)
    bs[26:28] = u16(1)
    bs[28:32] = u32(0)                       # hidden
    bs[32:36] = u32(vol_secs)
    bs[36:40] = u32(fat_secs)
    bs[40:42] = u16(0)
    bs[42:44] = u16(0)
    bs[44:48] = u32(2)                       # root cluster
    bs[48:50] = u16(1)                       # FSInfo
    bs[50:52] = u16(6)                       # backup boot
    bs[66] = 0x29
    bs[67:71] = u32(0x1234ABCD)
    bs[71:82] = b'NO NAME    '            # no label entry in the root
    bs[82:90] = b'FAT32   '
    bs[510:512] = b'\x55\xAA'
    vol[0:512] = bs
    vol[6 * bps:7 * bps] = bs                # backup

    # FSInfo
    fsi = bytearray(bps)
    fsi[0:4] = b'RRaA'
    fsi[484:488] = b'rrAa'
    fsi[488:492] = u32(0xFFFFFFFF)
    fsi[492:496] = u32(0xFFFFFFFF)
    fsi[510:512] = b'\x55\xAA'              # trail signature 0xAA550000
    vol[1 * bps:2 * bps] = fsi

    def fat_set(clus: int, val: int):
        for f in range(fats):
            off = (reserved + f * fat_secs) * bps + clus * 4
            vol[off:off + 4] = u32(val & 0x0FFFFFFF)

    fat_set(0, 0x0FFFFFF8)
    fat_set(1, 0x0FFFFFFF)
    fat_set(2, 0x0FFFFFFF)                   # root dir, one cluster

    data_start = reserved + fats * fat_secs

    def write_dir_entry(dir_clus: int, index: int, name: bytes, attr: int, first: int, size: int):
        assert len(name) == 11
        lba = data_start + (dir_clus - 2) * spc
        off = lba * bps + index * 32
        ent = bytearray(32)
        ent[0:11] = name
        ent[11] = attr
        ent[20:22] = u16(first >> 16)
        ent[26:28] = u16(first & 0xFFFF)
        ent[28:32] = u32(size)
        vol[off:off + 32] = ent

    # Allocate clusters for EFI, BOOT dirs and the file
    next_clus = 3
    efi_clus = next_clus
    fat_set(efi_clus, 0x0FFFFFFF)
    next_clus += 1
    boot_clus = next_clus
    fat_set(boot_clus, 0x0FFFFFFF)
    next_clus += 1

    file_clusters = max(1, math.ceil(len(efi_file) / (spc * bps)))
    file_first = next_clus
    for i in range(file_clusters):
        cl = file_first + i
        nxt = 0x0FFFFFFF if i + 1 == file_clusters else cl + 1
        fat_set(cl, nxt)
    next_clus += file_clusters

    # Root: EFI
    write_dir_entry(2, 0, b'EFI        ', 0x10, efi_clus, 0)
    # EFI: BOOT
    write_dir_entry(efi_clus, 0, b'.          ', 0x10, efi_clus, 0)
    write_dir_entry(efi_clus, 1, b'..         ', 0x10, 0, 0)    # 0: the root
    write_dir_entry(efi_clus, 2, b'BOOT       ', 0x10, boot_clus, 0)
    # BOOT: the boot file, with long-name entries when it is not 8.3
    write_dir_entry(boot_clus, 0, b'.          ', 0x10, boot_clus, 0)
    write_dir_entry(boot_clus, 1, b'..         ', 0x10, efi_clus, 0)
    short = short_name(name)
    index = 2
    if b'~' in short:
        for e in lfn_entries(name, short):
            off = (data_start + (boot_clus - 2) * spc) * bps + index * 32
            vol[off:off + 32] = e
            index += 1
    write_dir_entry(boot_clus, index, short, 0x20, file_first, len(efi_file))

    # File payload
    payload_off = (data_start + (file_first - 2) * spc) * bps
    end = payload_off + len(efi_file)
    assert end <= len(vol), f'EFI file past end of ESP ({end} > {len(vol)})'
    vol[payload_off:end] = efi_file
    if len(vol) != esp_secs * bps:
        # Defensive: keep the ESP exactly esp_secs sectors for GPT placement
        vol = vol[:esp_secs * bps] + bytes(max(0, esp_secs * bps - len(vol)))
    assert len(vol) == esp_secs * bps
    return vol


def make_gpt(esp: bytes) -> bytearray:
    img = bytearray(DISK_BYTES)
    # Protective MBR
    mbr = bytearray(SECTOR)
    mbr[446:462] = bytes([
        0x00, 0x00, 0x01, 0x00, 0xEE, 0xFE, 0xFF, 0xFF,
        0x01, 0x00, 0x00, 0x00,
    ]) + u32(DISK_SECS - 1)
    mbr[510:512] = b'\x55\xAA'
    img[0:SECTOR] = mbr

    # One partition entry
    ent = bytearray(128)
    ent[0:16] = guid_bytes(ESP_GUID)
    ent[16:32] = guid_bytes(PART_GUID)
    ent[32:40] = u64(ESP_LBA)
    ent[40:48] = u64(ESP_LBA + ESP_SECS - 1)
    entries = bytearray(128 * 128)
    entries[0:128] = ent
    ent_crc = crc32(entries)

    def gpt_header(alt_lba: int, my_lba: int, ent_lba: int) -> bytes:
        h = bytearray(SECTOR)
        h[0:8] = b'EFI PART'
        h[8:12] = u32(0x00010000)
        h[12:16] = u32(92)
        h[24:32] = u64(my_lba)
        h[32:40] = u64(alt_lba)
        h[40:48] = u64(34)                   # first usable (after entries at LBA 2..33)
        h[48:56] = u64(DISK_SECS - 34)
        h[56:72] = guid_bytes(DISK_GUID)
        h[72:80] = u64(ent_lba)
        h[80:84] = u32(128)
        h[84:88] = u32(128)
        h[88:92] = u32(ent_crc)
        h[16:20] = u32(0)                    # header CRC placeholder
        h[16:20] = u32(crc32(h[:92]))
        return h

    img[1 * SECTOR:2 * SECTOR] = gpt_header(DISK_SECS - 1, 1, 2)
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    # Backup entries and header at the end
    backup_ent_lba = DISK_SECS - 33
    img[backup_ent_lba * SECTOR:backup_ent_lba * SECTOR + len(entries)] = entries
    img[(DISK_SECS - 1) * SECTOR:DISK_SECS * SECTOR] = gpt_header(1, DISK_SECS - 1, backup_ent_lba)

    assert len(esp) == ESP_SECS * SECTOR
    img[ESP_LBA * SECTOR:ESP_LBA * SECTOR + len(esp)] = esp
    return img


def main():
    efi = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    esp = make_fat32(ESP_SECS, efi, sys.argv[3] if len(sys.argv) > 3 else 'BOOTX64.EFI')
    img = make_gpt(esp)
    open(out, 'wb').write(img)
    print(f'mkesp: wrote {out} ({len(img)} bytes), ESP {ESP_SECS} sectors, file {len(efi)} bytes')


if __name__ == '__main__':
    main()
