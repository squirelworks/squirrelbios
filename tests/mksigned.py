#!/usr/bin/env python3
"""Secure Boot test material for the x86_64-uefi firmware (roadmap C4).
Original code: an Authenticode signer and EFI signature lists in plain
Python (no OpenSSL), with two RSA-2048 test keys made from fixed seeds so
every build gives the same bytes. The keys are for tests only.

    tests/mksigned.py APP.efi OUTDIR
    tests/mksigned.py --sign IN.efi OUT.efi     (IN signed with key A)

writes to OUTDIR:
    sb-signed.efi     APP signed with key A (its certificate included)
    sb-wrongkey.efi   APP signed with key B
    sb-tampered.efi   sb-signed.efi with one byte of .text changed
    sb-db-x509.esl    db: key A's certificate
    sb-db-rsa.esl     db: key A as an EFI_CERT_RSA2048 entry
    sb-db-mixed.esl   db: a SHA-256 list (one unrelated hash) then key A's certificate
    sb-db-hash.esl    db: the Authenticode SHA-256 of the unsigned APP
    sb-dbx-hash.esl   dbx: the Authenticode SHA-256 of APP (signed or not, the same)
    sb-dbx-x509.esl   dbx: key A's certificate

and, for enrolled keys, authenticated variable writes (an
EFI_VARIABLE_AUTHENTICATION_2 then the data, as given to SetVariable).
Key A is the PK, key B the KEK, key C neither:
    auth-PK.auth            PK = key A's certificate, signed by A
    auth-KEK.auth           KEK = key B's certificate, signed by A (ContentInfo form)
    auth-db.auth            db = key A's certificate, signed by B
    auth-db-badsigner.auth  db = APP's SHA-256, signed by C
    auth-db-old.auth        db = APP's SHA-256, signed by B, dated before auth-db.auth
    auth-db-append.auth     append APP's SHA-256 to db, signed by B
    auth-PK-delete.auth     an empty PK (back to setup mode), signed by A
"""
import hashlib
import os
import random
import struct
import sys


# ---------------------------------------------------------------- RSA keys

def is_prime(n, rnd):
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(40):
        a = rnd.randrange(2, n - 2)
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def prime(bits, rnd):
    while True:
        c = rnd.getrandbits(bits) | (3 << (bits - 2)) | 1
        if c % 65537 != 1 and is_prime(c, rnd):
            return c


def rsa_key(seed):
    rnd = random.Random(seed)
    while True:
        p, q = prime(1024, rnd), prime(1024, rnd)
        n = p * q
        if p != q and n.bit_length() == 2048:
            break
    d = pow(65537, -1, (p - 1) * (q - 1))
    return n, d


def sign(key, data):
    n, d = key
    digest_info = bytes.fromhex("3031300d060960864801650304020105000420") + hashlib.sha256(data).digest()
    em = b"\x00\x01" + b"\xff" * (256 - 3 - len(digest_info)) + b"\x00" + digest_info
    return pow(int.from_bytes(em, "big"), d, n).to_bytes(256, "big")


# ---------------------------------------------------------------- DER

def tlv(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + body


def seq(*parts):
    return tlv(0x30, b"".join(parts))


def set_(*parts):
    return tlv(0x31, b"".join(parts))


def integer(v):
    b = v.to_bytes(v.bit_length() // 8 + 1, "big")
    return tlv(0x02, b)


def oid(dotted):
    nums = [int(x) for x in dotted.split(".")]
    out = bytes([40 * nums[0] + nums[1]])
    for v in nums[2:]:
        enc = [v & 0x7F]
        v >>= 7
        while v:
            enc.append(0x80 | (v & 0x7F))
            v >>= 7
        out += bytes(reversed(enc))
    return tlv(0x06, out)


NULL = tlv(0x05, b"")
SHA256 = seq(oid("2.16.840.1.101.3.4.2.1"), NULL)
RSA_ENC = seq(oid("1.2.840.113549.1.1.1"), NULL)
SHA256_RSA = seq(oid("1.2.840.113549.1.1.11"), NULL)
SPC_INDIRECT = "1.3.6.1.4.1.311.2.1.4"


def name_der(cn):
    return name(cn)


def name(cn):
    return seq(set_(seq(oid("2.5.4.3"), tlv(0x0C, cn.encode()))))


def certificate(key, cn, serial):
    n, _ = key
    spki = seq(RSA_ENC, tlv(0x03, b"\x00" + seq(integer(n), integer(65537))))
    validity = seq(tlv(0x17, b"260101000000Z"), tlv(0x17, b"360101000000Z"))
    tbs = seq(tlv(0xA0, integer(2)), integer(serial), SHA256_RSA, name(cn), validity, name(cn), spki)
    return seq(tbs, SHA256_RSA, tlv(0x03, b"\x00" + sign(key, tbs)))


# ---------------------------------------------------------------- PE

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def pe_layout(img):
    pe = u32(img, 0x3C)
    opt = pe + 24
    nsect = u16(img, pe + 6)
    optsize = u16(img, pe + 20)
    sect = opt + optsize
    return opt, sect, nsect


def authenticode_hash(img):
    opt, sect, nsect = pe_layout(img)
    cksum = opt + 64
    secdir = opt + 112 + 4 * 8
    hdr = u32(img, opt + 60)
    cert_size = u32(img, secdir + 4)
    h = hashlib.sha256()
    h.update(img[:cksum])
    h.update(img[cksum + 4:secdir])
    h.update(img[secdir + 8:hdr])
    sections = sorted((u32(img, sect + 40 * i + 20), u32(img, sect + 40 * i + 16)) for i in range(nsect))
    done = hdr
    for ptr, raw in sections:
        if raw:
            h.update(img[ptr:ptr + raw])
            done += raw
    if len(img) > done + cert_size:
        h.update(img[done:len(img) - cert_size])
    return h.digest()


def authenticode_sign(img, key, cert, issuer, serial):
    img = bytearray(img)
    img += b"\x00" * (-len(img) % 8)
    digest = authenticode_hash(bytes(img))
    spc = seq(seq(oid("1.3.6.1.4.1.311.2.1.15"), seq(tlv(0x03, b"\x00"), tlv(0xA0, tlv(0xA2, tlv(0x80, b""))))),
              seq(SHA256, tlv(0x04, digest)))
    spc_content = spc[2:] if spc[1] < 0x80 else spc[2 + (spc[1] & 0x7F):]
    attrs = (seq(oid("1.2.840.113549.1.9.3"), set_(oid(SPC_INDIRECT))) +
             seq(oid("1.2.840.113549.1.9.4"), set_(tlv(0x04, hashlib.sha256(spc_content).digest()))))
    signature = sign(key, set_(attrs))
    signer = seq(integer(1), seq(name(issuer), integer(serial)), SHA256, tlv(0xA0, attrs), RSA_ENC,
                 tlv(0x04, signature))
    signed = seq(integer(1), set_(SHA256), seq(oid(SPC_INDIRECT), tlv(0xA0, spc)), tlv(0xA0, cert),
                 set_(signer))
    pkcs7 = seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, signed))
    win = struct.pack("<IHH", 8 + len(pkcs7), 0x0200, 0x0002) + pkcs7
    win += b"\x00" * (-len(win) % 8)
    opt, _, _ = pe_layout(img)
    struct.pack_into("<II", img, opt + 112 + 4 * 8, len(img), len(win))
    return bytes(img + win)


# ---------------------------------------------------------------- lists

def guid(s):
    a, b, c, d, e = s.split("-")
    return struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16)) + bytes.fromhex(d + e)


CERT_SHA256 = guid("c1c41626-504c-4092-aca9-41f936934328")
CERT_RSA2048 = guid("3c5766e8-269c-4e34-aa14-ed776e85b3b6")
CERT_X509 = guid("a5c059a1-94e4-4aa7-87b5-ab155c2bf072")
OWNER = guid("5a5a5153-4952-5245-4c42-494f53000001")    # "SQRIRREL BIOS" test owner


def esl(kind, entries):
    size = 16 + len(entries[0])
    body = b"".join(OWNER + e for e in entries)
    return kind + struct.pack("<III", 28 + len(body), 0, size) + body


# ---------------------------------------------------------------- authenticated variables

PKCS7_GUID = guid("4aafd29d-68df-49ee-8aa9-347d375665a7")
GLOBAL = guid("8be4df61-93ca-11d2-aa0d-00e098032b8c")
IMAGE_SECURITY = guid("d719b2cb-3d3a-4596-a3bc-dad00e67656f")


def efi_time(year, month, day, hour=0, minute=0, second=0):
    return struct.pack("<HBBBBBBIhBB", year, month, day, hour, minute, second, 0, 0, 0, 0, 0)


def auth2(name, vendor, attrs, when, data, key, cert, issuer, serial, content_info=False):
    """EFI_VARIABLE_AUTHENTICATION_2 then the data: a detached PKCS#7 over
    the name (UTF-16, no NUL), vendor, attributes, time and data"""
    signed_over = name.encode("utf-16-le") + vendor + struct.pack("<I", attrs) + when + data
    attrs_der = (seq(oid("1.2.840.113549.1.9.3"), set_(oid("1.2.840.113549.1.7.1"))) +
                 seq(oid("1.2.840.113549.1.9.4"), set_(tlv(0x04, hashlib.sha256(signed_over).digest()))))
    signer = seq(integer(1), seq(name_der(issuer), integer(serial)), SHA256, tlv(0xA0, attrs_der), RSA_ENC,
                 tlv(0x04, sign(key, set_(attrs_der))))
    sd = seq(integer(1), set_(SHA256), seq(oid("1.2.840.113549.1.7.1")), tlv(0xA0, cert), set_(signer))
    if content_info:
        sd = seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, sd))
    win = struct.pack("<IHH", 24 + len(sd), 0x0200, 0x0EF1) + PKCS7_GUID + sd
    return when + win + data


def main():
    if sys.argv[1] == "--sign":
        key_a = rsa_key(0x5153_0001)
        cert_a = certificate(key_a, "SquirrelBIOS test key A", 0xA1)
        img = open(sys.argv[2], "rb").read()
        with open(sys.argv[3], "wb") as f:
            f.write(authenticode_sign(img, key_a, cert_a, "SquirrelBIOS test key A", 0xA1))
        return
    app, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    img = open(app, "rb").read()
    key_a, key_b = rsa_key(0x5153_0001), rsa_key(0x5153_0002)
    cert_a = certificate(key_a, "SquirrelBIOS test key A", 0xA1)
    cert_b = certificate(key_b, "SquirrelBIOS test key B", 0xB2)

    files = {}
    files["sb-signed.efi"] = signed = authenticode_sign(img, key_a, cert_a, "SquirrelBIOS test key A", 0xA1)
    files["sb-wrongkey.efi"] = authenticode_sign(img, key_b, cert_b, "SquirrelBIOS test key B", 0xB2)
    tampered = bytearray(signed)
    tampered[0x1000 + 0x40] ^= 0x01          # inside .text
    files["sb-tampered.efi"] = bytes(tampered)
    files["sb-db-x509.esl"] = esl(CERT_X509, [cert_a])
    files["sb-db-rsa.esl"] = esl(CERT_RSA2048, [key_a[0].to_bytes(256, "big")])
    files["sb-db-mixed.esl"] = esl(CERT_SHA256, [hashlib.sha256(b"unrelated").digest()]) + esl(CERT_X509, [cert_a])
    files["sb-db-hash.esl"] = esl(CERT_SHA256, [authenticode_hash(img + b"\x00" * (-len(img) % 8))])
    files["sb-dbx-hash.esl"] = esl(CERT_SHA256, [authenticode_hash(signed)])
    files["sb-dbx-x509.esl"] = esl(CERT_X509, [cert_a])
    # Enrolled keys: A is the PK, B the KEK; C is in neither
    key_c = rsa_key(0x5153_0003)
    cert_c = certificate(key_c, "SquirrelBIOS test key C", 0xC3)
    a_ = (key_a, cert_a, "SquirrelBIOS test key A", 0xA1)
    b_ = (key_b, cert_b, "SquirrelBIOS test key B", 0xB2)
    c_ = (key_c, cert_c, "SquirrelBIOS test key C", 0xC3)
    keys = 0x27
    t1, t2, t3 = efi_time(2026, 1, 1), efi_time(2026, 6, 1), efi_time(2026, 9, 1)
    app_hash = authenticode_hash(img + b"\x00" * (-len(img) % 8))
    files["auth-PK.auth"] = auth2("PK", GLOBAL, keys, t1, esl(CERT_X509, [cert_a]), *a_)
    files["auth-KEK.auth"] = auth2("KEK", GLOBAL, keys, t1, esl(CERT_X509, [cert_b]), *a_, content_info=True)
    files["auth-db.auth"] = auth2("db", IMAGE_SECURITY, keys, t1, esl(CERT_X509, [cert_a]), *b_)
    files["auth-db-badsigner.auth"] = auth2("db", IMAGE_SECURITY, keys, t2, esl(CERT_SHA256, [app_hash]), *c_)
    files["auth-db-old.auth"] = auth2("db", IMAGE_SECURITY, keys, efi_time(2025, 1, 1),
                                      esl(CERT_SHA256, [app_hash]), *b_)
    files["auth-db-append.auth"] = auth2("db", IMAGE_SECURITY, keys | 0x40, t2, esl(CERT_SHA256, [app_hash]), *b_)
    files["auth-PK-delete.auth"] = auth2("PK", GLOBAL, keys, t3, b"", *a_)
    for fn, data in files.items():
        with open(os.path.join(out, fn), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
