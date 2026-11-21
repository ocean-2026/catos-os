#!/usr/bin/env python3
"""
crypto_shim.py -- SSH-2 prototype crypto via the host's OpenSSL libcrypto.

Wraps exactly the primitives a minimal SSH-2 server needs:
  * X25519    (curve25519-sha256 KEX)
  * Ed25519   (ssh-ed25519 host key signatures)
  * AES-128-CTR (aes128-ctr transport cipher)
  * HMAC-SHA256 is done with Python's hmac/hashlib (no libcrypto needed).

These are HOST-ONLY helpers to validate the protocol before porting to the
CatOS kernel.  The kernel port will reimplement each primitive in C.
"""
import ctypes
import ctypes.util
import hashlib
import hmac
import os

# Locate libcrypto-3-x64.dll (Windows) or libcrypto.so (POSIX)
_LIB = None
for cand in [
    "C:/Users/Admin/.workbuddy/binaries/python/versions/3.13.12/DLLs/libcrypto-3-x64.dll",
    ctypes.util.find_library("crypto"),
    "libcrypto.so.3", "libcrypto.so", "libcrypto-3-x64.dll",
]:
    if not cand:
        continue
    try:
        _LIB = ctypes.CDLL(cand)
        _DLLName = cand
        break
    except OSError:
        continue
if _LIB is None:
    raise RuntimeError("libcrypto not found (needed for X25519/Ed25519/AES)")

_lib = _LIB
_EVP_PKEY_new_raw_private_key = _lib.EVP_PKEY_new_raw_private_key
_EVP_PKEY_new_raw_private_key.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                          ctypes.c_void_p, ctypes.c_size_t]
_EVP_PKEY_new_raw_private_key.restype = ctypes.c_void_p
_EVP_PKEY_new_raw_public_key = _lib.EVP_PKEY_new_raw_public_key
_EVP_PKEY_new_raw_public_key.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                         ctypes.c_void_p, ctypes.c_size_t]
_EVP_PKEY_new_raw_public_key.restype = ctypes.c_void_p
_EVP_PKEY_free = _lib.EVP_PKEY_free
_EVP_PKEY_free.argtypes = [ctypes.c_void_p]
_EVP_PKEY_get_raw_public_key = _lib.EVP_PKEY_get_raw_public_key
_EVP_PKEY_get_raw_public_key.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.POINTER(ctypes.c_size_t)]
_EVP_PKEY_get_raw_public_key.restype = ctypes.c_int
_EVP_PKEY_CTX_new = _lib.EVP_PKEY_CTX_new
_EVP_PKEY_CTX_new.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_EVP_PKEY_CTX_new.restype = ctypes.c_void_p
_EVP_PKEY_CTX_new_from_pkey = getattr(_lib, "EVP_PKEY_CTX_new_from_pkey", None)
if _EVP_PKEY_CTX_new_from_pkey:
    _EVP_PKEY_CTX_new_from_pkey.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                            ctypes.c_void_p]
    _EVP_PKEY_CTX_new_from_pkey.restype = ctypes.c_void_p
_EVP_PKEY_derive_init = _lib.EVP_PKEY_derive_init
_EVP_PKEY_derive_init.argtypes = [ctypes.c_void_p]
_EVP_PKEY_derive_set_peer = _lib.EVP_PKEY_derive_set_peer
_EVP_PKEY_derive_set_peer.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_EVP_PKEY_derive = _lib.EVP_PKEY_derive
_EVP_PKEY_derive.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                             ctypes.POINTER(ctypes.c_size_t)]
_EVP_PKEY_CTX_free = _lib.EVP_PKEY_CTX_free
_EVP_PKEY_CTX_free.argtypes = [ctypes.c_void_p]
_EVP_DigestSignInit = _lib.EVP_DigestSignInit
_EVP_DigestSignInit.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_EVP_DigestSign = _lib.EVP_DigestSign
_EVP_DigestSign.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p,
                            ctypes.c_size_t]
_EVP_MD_CTX_new = _lib.EVP_MD_CTX_new
_EVP_MD_CTX_new.restype = ctypes.c_void_p
_EVP_MD_CTX_free = _lib.EVP_MD_CTX_free
_EVP_MD_CTX_free.argtypes = [ctypes.c_void_p]
_EVP_CIPHER_CTX_new = _lib.EVP_CIPHER_CTX_new
_EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
_EVP_CIPHER_CTX_free = _lib.EVP_CIPHER_CTX_free
_EVP_CIPHER_CTX_free.argtypes = [ctypes.c_void_p]

# NIDs
NID_X25519 = 1034
NID_ED25519 = 1087

# EVP_aes_128_ctr()
_lib.EVP_aes_128_ctr.restype = ctypes.c_void_p
_EVP_aes_128_ctr = _lib.EVP_aes_128_ctr
_lib.EVP_EncryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_lib.EVP_EncryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, ctypes.c_int]
_lib.EVP_DecryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
_lib.EVP_DecryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, ctypes.c_int]


def _buf(b):
    return (ctypes.c_ubyte * len(b)).from_buffer_copy(b) if b else None


def sha256(b):
    return hashlib.sha256(b).digest()


def hmac_sha256(key, b):
    return hmac.new(key, b, hashlib.sha256).digest()


# ---- X25519 -------------------------------------------------------------
class X25519:
    def __init__(self, priv=None):
        if priv is None:
            priv = os.urandom(32)
        self.priv = priv
        self.pkey = _EVP_PKEY_new_raw_private_key(NID_X25519, None, _buf(priv), 32)
        assert self.pkey, "X25519 priv key failed"
        out = (ctypes.c_ubyte * 32)()
        n = ctypes.c_size_t(32)
        assert _EVP_PKEY_get_raw_public_key(self.pkey, out, ctypes.byref(n)) == 1
        self.pub = bytes(out)[:n.value]

    def exchange(self, peer_pub):
        peer = _EVP_PKEY_new_raw_public_key(NID_X25519, None, _buf(peer_pub), 32)
        assert peer, "X25519 peer key failed"
        ctx = _EVP_PKEY_CTX_new(self.pkey, None) if not _EVP_PKEY_CTX_new_from_pkey \
            else _EVP_PKEY_CTX_new_from_pkey(None, self.pkey, None)
        assert ctx, "EVP_PKEY_CTX_new failed"
        assert _EVP_PKEY_derive_init(ctx) == 1
        assert _EVP_PKEY_derive_set_peer(ctx, peer) == 1
        n = ctypes.c_size_t(0)
        assert _EVP_PKEY_derive(ctx, None, ctypes.byref(n)) == 1
        out = (ctypes.c_ubyte * n.value)()
        assert _EVP_PKEY_derive(ctx, out, ctypes.byref(n)) == 1
        secret = bytes(out)[:n.value]
        _EVP_PKEY_CTX_free(ctx)
        _EVP_PKEY_free(peer)
        return secret

    def __del__(self):
        if getattr(self, "pkey", None):
            _EVP_PKEY_free(self.pkey)
            self.pkey = None


# ---- Ed25519 ------------------------------------------------------------
class Ed25519:
    def __init__(self, seed=None):
        if seed is None:
            seed = os.urandom(32)
        self.seed = seed
        self.pkey = _EVP_PKEY_new_raw_private_key(NID_ED25519, None, _buf(seed), 32)
        assert self.pkey, "Ed25519 priv key failed"
        out = (ctypes.c_ubyte * 32)()
        n = ctypes.c_size_t(32)
        assert _EVP_PKEY_get_raw_public_key(self.pkey, out, ctypes.byref(n)) == 1
        self.pub = bytes(out)[:n.value]

    def sign(self, data):
        ctx = _EVP_MD_CTX_new()
        assert _EVP_DigestSignInit(ctx, None, None, None, self.pkey) == 1
        n = ctypes.c_size_t(0)
        assert _EVP_DigestSign(ctx, None, ctypes.byref(n), _buf(data), len(data)) == 1
        out = (ctypes.c_ubyte * n.value)()
        assert _EVP_DigestSign(ctx, out, ctypes.byref(n), _buf(data), len(data)) == 1
        sig = bytes(out)[:n.value]
        _EVP_MD_CTX_free(ctx)
        return sig

    def __del__(self):
        if getattr(self, "pkey", None):
            _EVP_PKEY_free(self.pkey)
            self.pkey = None


# ---- AES-128-CTR --------------------------------------------------------
def _aes_ctr(key, iv, data, enc=True):
    ctx = _EVP_CIPHER_CTX_new()
    init = _lib.EVP_EncryptInit_ex if enc else _lib.EVP_DecryptInit_ex
    upd = _lib.EVP_EncryptUpdate if enc else _lib.EVP_DecryptUpdate
    assert init(ctx, _EVP_aes_128_ctr(), None, _buf(key), _buf(iv)) == 1
    out = (ctypes.c_ubyte * (len(data) + 16))()
    n = ctypes.c_int(0)
    assert upd(ctx, out, ctypes.byref(n), _buf(data), len(data)) == 1
    res = bytes(out)[:n.value]
    _EVP_CIPHER_CTX_free(ctx)
    return res


def aes128ctr_encrypt(key, iv, data):
    return _aes_ctr(key, iv, data, True)


def aes128ctr_decrypt(key, iv, data):
    return _aes_ctr(key, iv, data, False)


if __name__ == "__main__":
    print("libcrypto:", _DLLName)
    x = X25519()
    y = X25519()
    assert x.exchange(y.pub) == y.exchange(x.pub)
    print("X25519 OK  (pub=%s...)" % x.pub[:8].hex())
    e = Ed25519()
    sig = e.sign(b"hello")
    print("Ed25519 OK (pub=%s... sig=%d bytes)" % (e.pub[:8].hex(), len(sig)))
    key, iv = os.urandom(16), os.urandom(16)
    pt = b"CatOS SSH prototype\x00\xff"
    ct = aes128ctr_encrypt(key, iv, pt)
    assert aes128ctr_decrypt(key, iv, ct) == pt
    print("AES-128-CTR OK (%d bytes round-trip, binary-safe)" % len(pt))
    print("HMAC-SHA256 OK:", hmac_sha256(b"k", b"m").hex()[:16], "...")
