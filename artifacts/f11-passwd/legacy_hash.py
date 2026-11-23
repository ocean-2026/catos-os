#!/usr/bin/env python3
"""Host-side transcription of the kernel's legacy password schemes, used to
craft old-format /etc/passwd records for the F11 migration test.

Transcribed from kernel_v25/src/user.c:
  * djb2  -- user_hash_pw (pre-v2.22)
  * v1    -- pw_hash_salted32, "$S$" + 8hex salt + '$' + 8hex digest
  * v2    -- pw_hash128,       "$S$" + 16hex salt + '$' + 32hex digest
"""

FNV = 16777619
ROUNDS = 8192


def avalanche32(h):
    h &= 0xFFFFFFFF
    h ^= h >> 16; h = (h * 0x85ebca6b) & 0xFFFFFFFF
    h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xFFFFFFFF
    h ^= h >> 16
    return h & 0xFFFFFFFF


def djb2(pw):
    h = 5381
    for c in pw.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def v1_hash(pw, salt):
    h = 2166136261 ^ salt
    for r in range(ROUNDS):
        for c in pw.encode():
            h ^= c; h = (h * FNV) & 0xFFFFFFFF
        h ^= r & 0xFF;       h = (h * FNV) & 0xFFFFFFFF
        h ^= h >> 13;        h = (h * FNV) & 0xFFFFFFFF
        h ^= (salt + r) & 0xFFFFFFFF   # kernel: no FNV multiply after this xor
    return avalanche32(h)


def v2_hash(pw, s0, s1):
    h0 = 2166136261 ^ s0
    h1 = 2166136261 ^ s1 ^ 0x9E3779B9
    h2 = 2166136261 ^ s0 ^ 0x85EBCA6B
    h3 = 2166136261 ^ s1 ^ 0xC2B2AE35
    for r in range(ROUNDS):
        for c in pw.encode():
            h0 ^= c; h0 = (h0 * FNV) & 0xFFFFFFFF
            h1 ^= c; h1 = (h1 * FNV) & 0xFFFFFFFF
            h2 ^= c; h2 = (h2 * FNV) & 0xFFFFFFFF
            h3 ^= c; h3 = (h3 * FNV) & 0xFFFFFFFF
        h0 ^= r & 0xFF;       h0 = (h0 * FNV) & 0xFFFFFFFF
        h1 ^= (r >> 8) & 0xFF;  h1 = (h1 * FNV) & 0xFFFFFFFF
        h2 ^= (r >> 16) & 0xFF; h2 = (h2 * FNV) & 0xFFFFFFFF
        h3 ^= (r >> 24) & 0xFF; h3 = (h3 * FNV) & 0xFFFFFFFF
        h0 ^= h1; h1 = (h1 * FNV) & 0xFFFFFFFF
        h1 ^= h2; h2 = (h2 * FNV) & 0xFFFFFFFF
        h2 ^= h3; h3 = (h3 * FNV) & 0xFFFFFFFF
        h3 ^= h0; h0 = (h0 * FNV) & 0xFFFFFFFF
    return [avalanche32(x) for x in (h0, h1, h2, h3)]


def emit32(v):
    return "%08x" % v


def v1_record(pw, salt):
    return "$S$%s$%s" % (emit32(salt), emit32(v1_hash(pw, salt)))


def v2_record(pw, lo, hi):
    d = v2_hash(pw, lo, hi)
    return "$S$%s%s$%s" % (emit32(lo), emit32(hi),
                           "".join(emit32(w) for w in d))


if __name__ == "__main__":
    print("djb2 ErinPass1 = %08x" % djb2("ErinPass1"))
    print("v1 dave =", v1_record("DavePass1", 0x0a0b0c0d))
    print("v2 carol =", v2_record("CarolPass1", 0x11223344, 0x55667788))
