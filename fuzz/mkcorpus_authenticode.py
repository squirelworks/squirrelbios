#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny seed corpus for fuzz/authenticode_harness.c.

Original code. Writes synthetic PKCS#7 / Authenticode / X.509 /
EFI_SIGNATURE_LIST / PE-with-cert-table blobs under the given directory
(default: fuzz/corpus-authenticode). Layouts follow public specs only
(X.690 DER, RFC 2315 PKCS #7, RFC 5280 X.509, Microsoft Authenticode
PE / SpcIndirectDataContent, UEFI signature lists). No vendor firmware
blobs and no real certificates.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

# OIDs used by der.c (DER-encoded value bytes, without tag/length).
OID_RSA = bytes([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01])
OID_SIGNED_DATA = bytes([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02])
OID_MESSAGE_DIGEST = bytes([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04])
OID_SPC_INDIRECT = bytes(
    [0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04]
)
OID_SHA256 = bytes([0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01])

# UEFI EFI_SIGNATURE_LIST type GUIDs (little-endian bytes).
GUID_CERT_SHA256 = bytes.fromhex(
    "2616c4c14c509240aca941f936934328"
)
GUID_CERT_X509 = bytes.fromhex(
    "a159c0a5e494a74a87b5ab155c2bf072"
)


def der_len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    out = []
    v = n
    while v:
        out.append(v & 0xFF)
        v >>= 8
    out.reverse()
    return bytes([0x80 | len(out)]) + bytes(out)


def der_tlv(tag: int, value: bytes) -> bytes:
    return bytes([tag]) + der_len(len(value)) + value


def der_seq(*parts: bytes) -> bytes:
    return der_tlv(0x30, b"".join(parts))


def der_set(*parts: bytes) -> bytes:
    return der_tlv(0x31, b"".join(parts))


def der_oid(oid: bytes) -> bytes:
    return der_tlv(0x06, oid)


def der_null() -> bytes:
    return bytes([0x05, 0x00])


def der_int(v: int) -> bytes:
    if v == 0:
        return bytes([0x02, 0x01, 0x00])
    raw = []
    x = v
    while x:
        raw.append(x & 0xFF)
        x >>= 8
    raw.reverse()
    if raw[0] & 0x80:
        raw = [0x00] + raw
    return der_tlv(0x02, bytes(raw))


def der_octet(b: bytes) -> bytes:
    return der_tlv(0x04, b)


def der_bit_unused0(payload: bytes) -> bytes:
    return der_tlv(0x03, bytes([0x00]) + payload)


def der_ctx(n: int, value: bytes) -> bytes:
    return der_tlv(0xA0 | n, value)


def sha256_digest_alg() -> bytes:
    return der_seq(der_oid(OID_SHA256), der_null())


def fake_rsa_spki(n_bytes: int = 32) -> bytes:
    """Minimal SubjectPublicKeyInfo with a tiny modulus (not a real key)."""
    n = der_tlv(0x02, bytes([0x00]) + bytes([0xAA] * n_bytes))
    e = der_int(65537)
    ne = der_seq(n, e)
    alg = der_seq(der_oid(OID_RSA), der_null())
    return der_seq(alg, der_bit_unused0(ne))


def fake_x509() -> bytes:
    """Bare Certificate SEQUENCE: tbs with SPKI at the expected slot."""
    version = der_ctx(0, der_int(2))
    serial = der_int(1)
    sigalg = der_seq(der_oid(OID_SHA256), der_null())
    name = der_seq()  # empty Name
    validity = der_seq(der_tlv(0x17, b"250101000000Z"), der_tlv(0x17, b"350101000000Z"))
    spki = fake_rsa_spki()
    tbs = der_seq(version, serial, sigalg, name, validity, name, spki)
    sig = der_bit_unused0(bytes([0x11] * 16))
    return der_seq(tbs, sigalg, sig)


def spc_indirect_content(digest: bytes) -> bytes:
    """SpcIndirectDataContent SEQUENCE value (tag 0x30's contents)."""
    # SpcAttributeTypeAndOptionalValue: SEQ { OID, optional }
    attr = der_seq(der_oid(OID_SPC_INDIRECT[:9] + bytes([0x01])), der_seq())
    # Actually der.c wants: SpcAttributeTypeAndOptionalValue as first SEQ,
    # then DigestInfo. The Spc OID is on the ContentInfo, not inside.
    # Inside SpcIndirectDataContent: SEQ { attr SEQ, DigestInfo SEQ }.
    attr_inner = der_seq(der_oid(bytes([0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0F])))
    dinfo = der_seq(sha256_digest_alg(), der_octet(digest))
    return der_seq(attr_inner, dinfo)


def authenticode_signed_data(digest: bytes, *, with_attrs: bool = True) -> bytes:
    """ContentInfo { signedData, [0] SignedData { ... SpcIndirect ... } }."""
    spc_value = spc_indirect_content(digest)
    spc_seq = der_tlv(0x30, spc_value)  # full SEQUENCE including tag+len
    content = der_seq(der_oid(OID_SPC_INDIRECT), der_ctx(0, spc_seq))
    digest_algs = der_set(sha256_digest_alg())

    # SignerInfo
    version = der_int(1)
    issuer_serial = der_seq(der_seq(), der_int(1))
    dig_alg = sha256_digest_alg()
    if with_attrs:
        # messageDigest must equal SHA-256(spc_seq.v) = sha256(spc_value)
        md = hashlib.sha256(spc_value).digest()
        md_attr = der_seq(der_oid(OID_MESSAGE_DIGEST), der_set(der_octet(md)))
        # authenticatedAttributes as [0] IMPLICIT — der.c reads attrs.raw
        # with tag 0xA0; messageDigest walk uses inside(attrs) as a SET of
        # Attribute SEQUENCEs. Encode [0] wrapping the attribute SEQ bytes
        # (not a nested SET): firmware treats the [0] value as the SET body.
        attrs = der_tlv(0xA0, md_attr)
        dig_enc_alg = der_seq(der_oid(OID_RSA), der_null())
        enc_digest = der_octet(bytes([0x22] * 16))
        signer = der_seq(
            version, issuer_serial, dig_alg, attrs, dig_enc_alg, enc_digest
        )
    else:
        dig_enc_alg = der_seq(der_oid(OID_RSA), der_null())
        enc_digest = der_octet(bytes([0x33] * 16))
        signer = der_seq(version, issuer_serial, dig_alg, dig_enc_alg, enc_digest)

    signers = der_set(signer)
    # Optional certificates [0] empty omitted
    signed = der_seq(
        der_int(1),
        digest_algs,
        content,
        signers,
    )
    return der_seq(der_oid(OID_SIGNED_DATA), der_ctx(0, signed))


def detached_signed_data(content_hash: bytes, *, with_attrs: bool = True) -> bytes:
    """Detached ContentInfo SignedData (empty content)."""
    digest_algs = der_set(sha256_digest_alg())
    empty_content = der_seq(der_oid(OID_SIGNED_DATA[:9] + bytes([0x01])))  # data OID-ish
    # Use id-data OID 1.2.840.113549.1.7.1
    oid_data = bytes([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01])
    empty_content = der_seq(der_oid(oid_data))

    version = der_int(1)
    issuer_serial = der_seq(der_seq(), der_int(1))
    dig_alg = sha256_digest_alg()
    if with_attrs:
        md_attr = der_seq(
            der_oid(OID_MESSAGE_DIGEST), der_set(der_octet(content_hash))
        )
        attrs = der_tlv(0xA0, md_attr)
        dig_enc_alg = der_seq(der_oid(OID_RSA), der_null())
        enc_digest = der_octet(bytes([0x44] * 16))
        signer = der_seq(
            version, issuer_serial, dig_alg, attrs, dig_enc_alg, enc_digest
        )
    else:
        dig_enc_alg = der_seq(der_oid(OID_RSA), der_null())
        enc_digest = der_octet(bytes([0x55] * 16))
        signer = der_seq(version, issuer_serial, dig_alg, dig_enc_alg, enc_digest)

    signed = der_seq(der_int(1), digest_algs, empty_content, der_set(signer))
    return der_seq(der_oid(OID_SIGNED_DATA), der_ctx(0, signed))


def wr16(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<H", buf, off, val & 0xFFFF)


def wr32(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)


def pe_with_cert_table(pkcs7: bytes) -> bytes:
    """Minimal PE32+ with a security data directory pointing at a WIN_CERTIFICATE."""
    pe_off = 0x40
    optsize = 112 + 6 * 8  # enough for 6 data dirs (security is index 4)
    coff_off = pe_off + 4
    opt_off = coff_off + 20
    sect_off = opt_off + optsize
    hdr_size = 0x200

    # WIN_CERTIFICATE: dwLength, wRevision=0x0200, wCertificateType=0x0002
    wlen = 8 + len(pkcs7)
    wlen_aligned = (wlen + 7) & ~7
    win_cert = bytearray(wlen_aligned)
    wr32(win_cert, 0, wlen)
    wr16(win_cert, 4, 0x0200)
    wr16(win_cert, 6, 0x0002)
    win_cert[8 : 8 + len(pkcs7)] = pkcs7

    cert_off = hdr_size
    total = cert_off + len(win_cert)
    img = bytearray(total)
    img[0] = ord("M")
    img[1] = ord("Z")
    wr32(img, 0x3C, pe_off)
    img[pe_off : pe_off + 4] = b"PE\0\0"
    wr16(img, coff_off + 0, 0x8664)
    wr16(img, coff_off + 2, 0)  # nsect
    wr16(img, coff_off + 16, optsize)
    wr16(img, coff_off + 18, 0x0022)
    wr16(img, opt_off + 0, 0x20B)
    wr32(img, opt_off + 56, total)  # SizeOfImage
    wr32(img, opt_off + 60, hdr_size)  # SizeOfHeaders
    wr16(img, opt_off + 68, 10)  # EFI app
    wr32(img, opt_off + 108, 6)  # NumberOfRvaAndSizes
    # DataDirectory[4] = security (file offset, size) — PE uses file offset here
    wr32(img, opt_off + 112 + 4 * 8, cert_off)
    wr32(img, opt_off + 112 + 4 * 8 + 4, len(win_cert))
    img[cert_off : cert_off + len(win_cert)] = win_cert
    (void_sect := sect_off)  # silence; no sections
    del void_sect
    return bytes(img)


def efi_sig_list_sha256(digest: bytes) -> bytes:
    """One EFI_SIGNATURE_LIST of type EFI_CERT_SHA256_GUID."""
    # SignatureOwner GUID (nil) + 32-byte hash
    owner = bytes(16)
    sig = owner + digest
    sig_size = len(sig)
    hdr_size = 0
    list_size = 28 + hdr_size + sig_size
    out = bytearray(list_size)
    out[0:16] = GUID_CERT_SHA256
    wr32(out, 16, list_size)
    wr32(out, 20, hdr_size)
    wr32(out, 24, sig_size)
    out[28:] = sig
    return bytes(out)


def efi_sig_list_x509(cert: bytes) -> bytes:
    owner = bytes(16)
    sig = owner + cert
    sig_size = len(sig)
    list_size = 28 + sig_size
    out = bytearray(list_size)
    out[0:16] = GUID_CERT_X509
    wr32(out, 16, list_size)
    wr32(out, 20, 0)
    wr32(out, 24, sig_size)
    out[28:] = sig
    return bytes(out)


def truncated_der() -> bytes:
    return bytes([0x30, 0x82, 0x01, 0x00, 0x06])  # claims long length, short body


def wrong_oid_contentinfo() -> bytes:
    bad_oid = bytes([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x99])
    return der_seq(der_oid(bad_oid), der_ctx(0, der_seq(der_int(1))))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "-o",
        "--out",
        type=Path,
        default=Path("fuzz/corpus-authenticode"),
        help="output directory",
    )
    args = ap.parse_args()
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    digest = hashlib.sha256(b"squirrelbios-fuzz-seed").digest()
    pkcs7 = authenticode_signed_data(digest, with_attrs=True)
    pkcs7_noattr = authenticode_signed_data(digest, with_attrs=False)
    detached = detached_signed_data(digest, with_attrs=True)
    x509 = fake_x509()

    seeds = {
        "emptyish.bin": b"\x00" * 64,
        "truncated_der.bin": truncated_der(),
        "wrong_oid_pkcs7.bin": wrong_oid_contentinfo(),
        "pkcs7_authenticode.bin": pkcs7,
        "pkcs7_authenticode_noattr.bin": pkcs7_noattr,
        "pkcs7_detached.bin": detached,
        "x509_tiny.der": x509,
        "pe_with_pkcs7.efi": pe_with_cert_table(pkcs7),
        "siglist_sha256.bin": efi_sig_list_sha256(digest),
        "siglist_x509.bin": efi_sig_list_x509(x509),
        "siglist_bad_size.bin": bytes(GUID_CERT_SHA256)
        + struct.pack("<III", 8, 0, 48),  # list too small
        "randomish.bin": bytes(range(256)),
    }
    for name, data in seeds.items():
        path = out / name
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
