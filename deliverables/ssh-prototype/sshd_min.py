#!/usr/bin/env python3
"""
sshd_min.py -- clean minimal SSH-2 server (host prototype).

Validated facts carried over from sshd_proto.py debugging:
  * exchange hash H = SHA256(s(V_C)||s(V_S)||s(I_C)||s(I_S)||s(K_S)||
                            s(q_c)||s(q_s)||mpint(K)), V without CRLF,
                            I = full KEXINIT payload incl msg byte.
  * Ed25519 signature is over H directly; blob = s("ssh-ed25519")||s(sig).
  * key derivation SHA256(mpint(K)||H||"A".."F"||session_id=H) -- byte-exact
    match with paramiko verified for all six keys.
  * kex curve25519-sha256, host ssh-ed25519, cipher aes128-ctr, mac hmac-sha2-256.

Binary packet protocol (RFC 4253 §6):
    uint32 packet_length          <- PLAINTEXT
    byte   padding_length         \
    byte[n] payload                > encrypted with AES-128-CTR
    byte[padding]                  /
    byte[mac_len] MAC              <- HMAC-SHA256(seq||packet||macfields)
CTR keystream: IV is a 128-bit BIG-ENDIAN counter starting at the derived IV;
OpenSSL EVP treats it little-endian, but for one packet < 16 blocks we implement
the counter manually so it matches OpenSSH exactly.
"""
import hashlib
import hmac
import os
import socket
import struct
import sys
import threading

from crypto_shim import (X25519, Ed25519, sha256, hmac_sha256,
                         aes128ctr_encrypt, aes128ctr_decrypt)

class CTRStream:
    """Persistent AES-128-CTR stream: keystream is CONTINUOUS across packets.
    OpenSSH/paramiko's aes128-ctr encrypts the WHOLE binary packet (including
    the 4-byte length field) with one long-lived cipher object.  The counter
    is a 128-bit big-endian value starting at the derived IV and incremented
    per 16-byte block (RFC 4344)."""
    def __init__(self, key, iv):
        from crypto_shim import aes128ctr_encrypt
        self.key = key
        self.iv = int.from_bytes(iv, "big")
        self.buf = b""
        self.block = 0

    def _more(self):
        iv_bytes = ((self.iv + self.block) & ((1 << 128) - 1)).to_bytes(16, "big")
        # one block of keystream = AES(iv_counter) with zero plaintext
        self.buf += aes128ctr_encrypt(self.key, iv_bytes, b"\x00" * 16)
        self.block += 1

    def crypt(self, data):
        while len(self.buf) < len(data):
            self._more()
        ks = self.buf[:len(data)]
        self.buf = self.buf[len(data):]
        return bytes(a ^ b for a, b in zip(data, ks))


SERVER_VERSION = b"SSH-2.0-CatOS_2.70"

MSG_DISCONNECT = 1
MSG_SERVICE_REQUEST = 5
MSG_SERVICE_ACCEPT = 6
MSG_KEXINIT = 20
MSG_NEWKEYS = 21
MSG_KEX_ECDH_INIT = 30
MSG_KEX_ECDH_REPLY = 31
MSG_USERAUTH_REQUEST = 50
MSG_USERAUTH_SUCCESS = 52
MSG_CHANNEL_OPEN = 90
MSG_CHANNEL_OPEN_CONFIRMATION = 91
MSG_CHANNEL_DATA = 94
MSG_CHANNEL_EOF = 96
MSG_CHANNEL_CLOSE = 97
MSG_CHANNEL_REQUEST = 98
MSG_CHANNEL_SUCCESS = 99
MSG_CHANNEL_FAILURE = 100
MSG_EXT_INFO = 7


def s(b):
    if isinstance(b, str):
        b = b.encode()
    return struct.pack(">I", len(b)) + b


def mpint(K):
    b = K
    # strip leading zero bytes (unsigned integer minimal encoding), keep one
    # leading zero if high bit set (two's complement for mpint)
    b = b.lstrip(b"\x00") or b"\x00"
    if b[0] & 0x80:
        b = b"\x00" + b
    return s(b)


def ssh_str_at(buf, off):
    n = struct.unpack(">I", buf[off:off + 4])[0]
    return buf[off + 4:off + 4 + n], off + 4 + n


def build_kexinit():
    p = bytes([MSG_KEXINIT]) + os.urandom(16)
    algos = [os.environ.get("SSH_KEX", "curve25519-sha256"), "ssh-ed25519", "aes128-ctr", "aes128-ctr",
             "hmac-sha2-256", "hmac-sha2-256", "none", "none", "", ""]
    for a in algos:
        p += s(a)
    return p + b"\x00" + struct.pack(">I", 0)


def parse_kexinit_names(payload):
    off = 17
    names = []
    for _ in range(10):
        n = struct.unpack(">I", payload[off:off + 4])[0]
        off += 4
        names.append(payload[off:off + n].decode("ascii", "replace"))
        off += n
    return names


class Conn:
    def __init__(self, sock, host):
        self.sock = sock
        self.host = host
        self.host_blob = s("ssh-ed25519") + s(host.pub)
        self.in_seq = 0
        self.out_seq = 0
        self.enc_key = self.dec_key = None
        self.enc_iv = self.dec_iv = None
        self.mac_in = self.mac_out = None
        self.enc_stream = None
        self.dec_stream = None

    def raw(self, n):
        buf = b""
        while len(buf) < n:
            c = self.sock.recv(n - len(buf))
            if not c:
                raise EOFError("closed")
            buf += c
        return buf

    def send(self, payload):
        # RFC 4253 §6: packet_length field (4 bytes) + padding_length (1) +
        # payload + padding must be a multiple of 8 (cipher block size),
        # and padding must be >= 4.
        pad = 8 - ((4 + 1 + len(payload)) % 8)
        if pad < 4:
            pad += 8
        pkt = bytes([pad]) + payload + b"\x00" * pad
        plain_len = struct.pack(">I", len(pkt))
        if self.enc_key is None:
            wire = plain_len + pkt
        else:
            # MAC covers seq + PLAINTEXT packet (non-EtM), then whole
            # packet is encrypted with the persistent CTR stream.
            mac = hmac_sha256(self.mac_out,
                              struct.pack(">I", self.out_seq) + plain_len + pkt)
            allenc = self.enc_stream.crypt(plain_len + pkt)
            wire = allenc + mac
        self.sock.sendall(wire)
        self.out_seq += 1

    def recv(self):
        if self.dec_key is None:
            plain_len = self.raw(4)
            n = struct.unpack(">I", plain_len)[0]
            if n < 2 or n > 35000:
                raise ValueError("bad packet_length %d" % n)
            body = self.raw(n)
            pad = body[0]
            payload = body[1:n - pad]
            self.in_seq += 1
            return payload[0], payload[1:]
        # aes128-ctr (RFC 4344 SDCTR): the ENTIRE packet including the
        # 4-byte length is encrypted with a persistent CTR stream whose
        # keystream continues across packets.  Read one block (16 bytes),
        # decrypt to recover length, then read the rest.
        enc_len = self.raw(4)
        n = struct.unpack(">I", self.dec_stream.crypt(enc_len))[0]
        if n < 2 or n > 35000:
            raise ValueError("bad packet_length %d" % n)
        enc_body = self.raw(n)
        body = self.dec_stream.crypt(enc_body)  # padding_length||payload||padding
        mac = self.raw(32)
        pad = body[0]
        payload = body[1:n - pad]
        self.in_seq += 1
        return payload[0], payload[1:]

    def set_keys(self, H, K):
        Kmp = mpint(K)
        def d(letter, count):
            out = b""
            prev = b""
            while len(out) < count:
                prev = sha256(Kmp + H + letter.encode() + H + prev)
                out += prev
            return out[:count]
        self.dec_iv = d("A", 16)
        self.enc_iv = d("B", 16)
        self.dec_key = d("C", 16)
        self.enc_key = d("D", 16)
        self.mac_in = d("E", 32)
        self.mac_out = d("F", 32)
        self.enc_stream = CTRStream(self.enc_key, self.enc_iv)
        self.dec_stream = CTRStream(self.dec_key, self.dec_iv)


def serve(sock, host):
    c = Conn(sock, host)
    try:
        # banner
        line = b""
        while not line.endswith(b"\n"):
            b = sock.recv(1)
            if not b:
                return
            line += b
        v_c = line.rstrip(b"\r\n")
        sock.sendall(SERVER_VERSION + b"\r\n")

        # KEXINIT
        sinit = build_kexinit()
        c.send(sinit)
        t, cinit = c.recv()
        if t != MSG_KEXINIT:
            raise ValueError("expected KEXINIT, got %d" % t)
        names = parse_kexinit_names(bytes([MSG_KEXINIT]) + cinit)
        print("    kex=%s host=%s cipher=%s" % (names[0], names[1], names[2]))

        # KEX
        t, body = c.recv()
        if t != MSG_KEX_ECDH_INIT:
            raise ValueError("expected ECDH_INIT, got %d" % t)
        q_c, _ = ssh_str_at(body, 0)
        x = X25519()
        K = x.exchange(q_c)
        q_s = x.pub
        H = sha256(s(v_c) + s(SERVER_VERSION) +
                   s(bytes([MSG_KEXINIT]) + cinit) + s(sinit) +
                   s(c.host_blob) + s(q_c) + s(q_s) + mpint(K))
        sig = host.sign(H)
        reply = bytes([MSG_KEX_ECDH_REPLY]) + s(c.host_blob) + s(q_s) + \
                s(s("ssh-ed25519") + s(sig))
        c.send(reply)
        c.send(bytes([MSG_NEWKEYS]))
        t, _ = c.recv()
        if t != MSG_NEWKEYS:
            raise ValueError("expected NEWKEYS, got %d" % t)
        c.set_keys(H, K)
        print("    KEX complete, cipher active")
        # service request -> accept (RFC 4253 §10 / RFC 4252)
        t, body = c.recv()
        if t == MSG_SERVICE_REQUEST:
            svc, _ = ssh_str_at(body, 0)
            print("    service request:", svc.decode("ascii", "replace"))
            c.send(bytes([MSG_SERVICE_ACCEPT]) + s(svc))
        # userauth (accept none / password)
        while True:
            t, body = c.recv()
            if t == MSG_USERAUTH_REQUEST:
                c.send(bytes([MSG_USERAUTH_SUCCESS]))
                break
        print("    auth complete")

        # channel
        t, body = c.recv()
        if t != MSG_CHANNEL_OPEN:
            raise ValueError("expected CHANNEL_OPEN, got %d" % t)
        typ, off = ssh_str_at(body, 0)
        sender = struct.unpack(">I", body[off:off + 4])[0]
        c.send(bytes([MSG_CHANNEL_OPEN_CONFIRMATION]) +
               struct.pack(">I", sender) + struct.pack(">I", 0) +
               struct.pack(">I", 0x20000) + struct.pack(">I", 0x8000))
        print("    channel open, client_chan=%d" % sender)

        while True:
            t, body = c.recv()
            if t == MSG_CHANNEL_REQUEST:
                # body: uint32 recipient_channel, string request_type, bool want_reply
                rtype, off = ssh_str_at(body, 4)
                want = body[off] if off < len(body) else -1
                print("    channel request: %r want=%d" % (rtype, want))
                if rtype in (b"exec", b"shell"):
                    if want:
                        c.send(bytes([MSG_CHANNEL_SUCCESS]) + struct.pack(">I", sender))
                    out = b"HELLO_FROM_CATOS_SSH_PROTO\r\n"
                    c.send(bytes([MSG_CHANNEL_DATA]) + struct.pack(">I", sender) + s(out))
                    c.send(bytes([MSG_CHANNEL_EOF]) + struct.pack(">I", sender))
                    c.send(bytes([MSG_CHANNEL_CLOSE]) + struct.pack(">I", sender))
                    print("    session complete")
                    break
                elif want:
                    c.send(bytes([MSG_CHANNEL_FAILURE]) + struct.pack(">I", sender))
            elif t in (MSG_CHANNEL_CLOSE, MSG_DISCONNECT):
                break
    except Exception as e:
        print("    error:", type(e).__name__, e)
    finally:
        try:
            sock.close()
        except OSError:
            pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 22250
    host = Ed25519()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print("SSH min server on 127.0.0.1:%d" % port)
    print("Try: ssh -p %d -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o PreferredAuthentications=none user@127.0.0.1 'echo hi'" % port)
    try:
        while True:
            sock, addr = srv.accept()
            print("connection from", addr)
            threading.Thread(target=serve, args=(sock, host), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
