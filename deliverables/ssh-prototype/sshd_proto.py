#!/usr/bin/env python3
"""
sshd_proto.py -- minimal SSH-2 server prototype (host-side, for protocol validation).

Goal: prove that a stock OpenSSH client can connect to a from-scratch SSH-2
implementation using the smallest viable algorithm set:

    kex:            curve25519-sha256
    host key:       ssh-ed25519
    cipher:         aes128-ctr
    mac:            hmac-sha2-256
    compression:    none
    auth:           none  (then password, see below)

Protocol stages implemented:
    1. version exchange  "SSH-2.0-CatOS_2.70\r\n"
    2. SSH_MSG_KEXINIT (20) + algorithm negotiation
    3. SSH_MSG_KEX_ECDH_INIT(30) / REPLY(31) -- X25519 + Ed25519 sig
    4. SSH_MSG_NEWKEYS (21) -- turn on AES-CTR + HMAC-SHA256
    5. USERAUTH_REQUEST(50) / SUCCESS(52) -- accept "none"
    6. CHANNEL_OPEN(90)/CONFIRM(91), CHANNEL_REQUEST(98) exec/shell,
       CHANNEL_DATA(94), EOF(96), CLOSE(97), DISCONNECT(1)

All byte-level framing (SSH binary packet protocol) is done here explicitly
so the exact same state machine can be ported to the CatOS kernel in C.

usage: python deliverables/ssh-prototype/sshd_proto.py [port]
       ssh -p <port> -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           user@127.0.0.1 'echo HELLO_FROM_CATOS'
"""
import os
import socket
import struct
import sys
import threading

from crypto_shim import (X25519, Ed25519, sha256, hmac_sha256,
                         aes128ctr_encrypt, aes128ctr_decrypt)

SERVER_VERSION = b"SSH-2.0-CatOS_2.70\r\n"

# message numbers
SSH_MSG_DISCONNECT = 1
SSH_MSG_KEXINIT = 20
SSH_MSG_NEWKEYS = 21
SSH_MSG_KEX_ECDH_INIT = 30
SSH_MSG_KEX_ECDH_REPLY = 31
SSH_MSG_USERAUTH_REQUEST = 50
SSH_MSG_USERAUTH_FAILURE = 51
SSH_MSG_USERAUTH_SUCCESS = 52
SSH_MSG_CHANNEL_OPEN = 90
SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91
SSH_MSG_CHANNEL_OPEN_FAILURE = 92
SSH_MSG_CHANNEL_WINDOW_ADJUST = 93
SSH_MSG_CHANNEL_DATA = 94
SSH_MSG_CHANNEL_EXTENDED_DATA = 95
SSH_MSG_CHANNEL_EOF = 96
SSH_MSG_CHANNEL_CLOSE = 97
SSH_MSG_CHANNEL_REQUEST = 98
SSH_MSG_CHANNEL_SUCCESS = 99
SSH_MSG_CHANNEL_FAILURE = 100

# ---- SSH wire helpers ---------------------------------------------------
def s(b):
    """SSH string: uint32 length + bytes"""
    if isinstance(b, str):
        b = b.encode()
    return struct.pack(">I", len(b)) + b


def mpint(i):
    b = i.to_bytes(max(1, (i.bit_length() + 7) // 8), "big")
    if b and (b[0] & 0x80):
        b = b"\x00" + b
    return s(b)


class Packetizer:
    """SSH binary packet protocol, RFC 4253 §6.

    Pre-KEX: no cipher/MAC, 8-byte min padding.
    Post-NEWKEYS: aes128-ctr + hmac-sha2-256 over seqnr+unencrypted packet.
    """

    def __init__(self, sock):
        self.sock = sock
        self.in_seq = 0
        self.out_seq = 0
        self.enc_key = None
        self.dec_key = None
        self.enc_iv = None
        self.dec_iv = None
        self.mac_key_in = None
        self.mac_key_out = None

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("socket closed")
            buf += chunk
        return buf

    def recv_packet(self):
        # 4-byte packet length (always plaintext in SSH)
        _raw4 = self._recv_exact(4)
        pkt_len = struct.unpack(">I", _raw4)[0]
        if pkt_len > 35000 or pkt_len < 2:
            print("    BADLEN raw4=%s enc=%s seq=%d dec_iv=%s dec_key=%s" % (
                _raw4.hex(), bool(self.dec_key), self.in_seq,
                self.dec_iv.hex() if self.dec_key else "-",
                self.dec_key.hex() if self.dec_key else "-"))
            # try decrypting raw4 with our key to see if it's an encrypted length
            if self.dec_key:
                from crypto_shim import aes128ctr_decrypt
                dec = aes128ctr_decrypt(self.dec_key, self.dec_iv, _raw4)
                print("    raw4 decrypted =", dec.hex())
            raise ValueError("bad pkt_len %d" % pkt_len)
        if self.dec_key is None:
            rest = self._recv_exact(pkt_len)
            pad_len = rest[0]
            payload = rest[1:pkt_len - pad_len]
            self.in_seq += 1
            return payload
        if self.dec_key and pkt_len > 35000:
            # dump next 64 bytes for analysis
            extra = self._recv_exact(64)
            print("    RAWSTREAM raw4=%s +64=%s" % (_raw4.hex(), extra.hex()))
            raise ValueError("DUMP_MODE")
        # encrypted packet.  The 4-byte length field is plaintext per RFC
        # 4253; when things are in sync _raw4 above is already the length.
        # If it looks like ciphertext (which happens when our stream is one
        # frame ahead), decrypt it to recover the real length.
        enc_len = _raw4
        real_len = pkt_len
        if pkt_len > 35000 or pkt_len < 2:
            dec = aes128ctr_decrypt(self.dec_key, self.dec_iv, enc_len)
            real_len = struct.unpack(">I", dec)[0]
        else:
            dec = enc_len  # length was plaintext; CTR keystream already consumed
        # read the rest: padding_length||payload||padding, then MAC
        rest = self._recv_exact(real_len)
        mac = self._recv_exact(32)
        # The AES-CTR keystream is continuous across the length + rest bytes.
        # Re-derive by decrypting length+rest together from IV.
        full = aes128ctr_decrypt(self.dec_key, self.dec_iv, enc_len + rest)
        pad_len = full[4]
        payload = full[5:4 + real_len - pad_len]
        self.in_seq += 1
        return payload

    def send_packet(self, payload):
        pad = 8 - ((len(payload) + 1 + 4) % 8)
        if pad < 4:
            pad += 8
        pkt = bytes([pad]) + payload + (b"\x00" * pad)
        if self.enc_key is None:
            wire = struct.pack(">I", len(pkt)) + pkt
        else:
            enc = aes128ctr_encrypt(self.enc_key, self.enc_iv, pkt)
            mac = hmac_sha256(self.mac_key_out,
                              struct.pack(">I", self.out_seq) + struct.pack(">I", len(pkt)) + enc)
            wire = struct.pack(">I", len(pkt)) + enc + mac
        self.sock.sendall(wire)
        self.out_seq += 1

    def set_keys(self, H, K, host_key_blob, I_C, I_S):
        """RFC 4253 §7.2: derive keys from exchange hash H and shared secret K.

        Key derivation: HASH(K || H || "letter" || session_id), where K is the
        shared secret encoded as an SSH mpint and session_id = H.
        Letters from the SERVER's perspective:
          A/B : client->server / server->client initial IV (we decrypt A, encrypt B)
          C/D : client->server / server->client encryption key (we decrypt C, encrypt D)
          E/F : client->server / server->client integrity key (we verify E, sign F)
        """
        Kmp = mpint_to_bytes(K)
        def derive(letter, n):
            return sha256(Kmp + H + letter.encode() + H)[:n]  # session_id == H
        self.dec_iv = derive("A", 16)
        self.enc_iv = derive("B", 16)
        self.dec_key = derive("C", 16)
        self.enc_key = derive("D", 16)
        self.mac_key_in = derive("E", 32)
        self.mac_key_out = derive("F", 32)
        with open("/tmp/catos_keys.txt", "w") as _f:
            for k, v in (("Kmp", Kmp), ("H", H),
                         ("dec_iv", self.dec_iv), ("enc_iv", self.enc_iv),
                         ("dec_key", self.dec_key), ("enc_key", self.enc_key),
                         ("mac_in", self.mac_key_in), ("mac_out", self.mac_key_out)):
                _f.write("%s=%s\n" % (k, v.hex()))


def mpint_to_bytes(K):
    b = K
    if b and (b[0] & 0x80):
        b = b"\x00" + b
    return s(b)


def parse_kexinit(payload):
    assert payload[0] == SSH_MSG_KEXINIT
    p = payload[1:17]           # cookie
    off = 17
    names = []
    for _ in range(10):         # kex, server_host_key, enc_c2s, enc_s2c, mac_c2s,
                                # mac_s2c, comp_c2s, comp_s2c, lang_c2s, lang_s2c
        n = struct.unpack(">I", payload[off:off + 4])[0]
        off += 4
        val = payload[off:off + n].decode("ascii", "replace")
        off += n
        names.append(val)
    return names


def build_kexinit():
    p = bytes([SSH_MSG_KEXINIT]) + os.urandom(16)
    algos = [
        "curve25519-sha256",
        "ssh-ed25519",
        "aes128-ctr",
        "aes128-ctr",
        "hmac-sha2-256",
        "hmac-sha2-256",
        "none",
        "none",
        "",
        "",
    ]
    for a in algos:
        p += s(a)
    p += b"\x00"                # first_kex_packet_follows
    p += struct.pack(">I", 0)   # reserved
    return p


def ssh_string_at(buf, off):
    n = struct.unpack(">I", buf[off:off + 4])[0]
    return buf[off + 4:off + 4 + n], off + 4 + n


class SSHSession:
    def __init__(self, sock, host_key):
        self.p = Packetizer(sock)
        self.host = host_key
        self.host_blob = s("ssh-ed25519") + s(host_key.pub)
        self.H = None
        self.K = None

    def banner(self):
        self.p.sock.sendall(SERVER_VERSION)

    def negotiate(self):
        # client banner
        line = b""
        while not line.endswith(b"\n"):
            c = self.p.sock.recv(1)
            if not c:
                raise EOFError("no client banner")
            line += c
        if not line.startswith(b"SSH-2.0-"):
            raise ValueError("bad client banner %r" % line)
        self.sinit = build_kexinit()
        self.p.send_packet(self.sinit)
        cinit = self.p.recv_packet()
        self.cinit = cinit
        names = parse_kexinit(cinit)
        print("    negotiated from client:", names[0], "/", names[1], "/", names[2])

    def do_kex(self):
        msg = self.p.recv_packet()
        if msg[0] != SSH_MSG_KEX_ECDH_INIT:
            raise ValueError("expected KEX_ECDH_INIT, got %d" % msg[0])
        q_c, _ = ssh_string_at(msg, 1)
        x = X25519()
        K = x.exchange(q_c)
        q_s = x.pub

        # exchange hash H = V_C || V_S || I_C || I_S || K_S || eph || K
        # V_C/V_S include the trailing CRLF exactly as sent on the wire.
        # I_C/I_S are the FULL KEXINIT payloads (including the leading
        # SSH_MSG_KEXINIT byte 20), RFC 4253 §8.  Excluding it corrupts H.
        # RFC 4253 §8: V_C/V_S are version strings WITHOUT trailing CRLF.
        vc = self.client_banner.rstrip(b"\r\n")
        vs = SERVER_VERSION.rstrip(b"\r\n")
        H_pre = (s(vc) + s(vs) + s(self.cinit) +
                 s(self.sinit) + s(self.host_blob) + s(q_c) + s(q_s) + mpint_to_bytes(K))
        H = sha256(H_pre)
        open("/tmp/catos_server_H.bin","wb").write(H)
        open("/tmp/catos_server_Hpre.bin","wb").write(H_pre)
        print("    H diag: vc=%d vs=%d I_C=%d I_S=%d Ks=%d qc=%d qs=%d Kmp=%d H=%s" % (
            len(vc), len(vs), len(self.cinit), len(self.sinit), len(self.host_blob),
            len(s(q_c)), len(s(q_s)), len(mpint_to_bytes(K)), H.hex()[:24]))
        sig = self.host.sign(H)   # RFC 4253: signature is over H directly
        print("    sig len =", len(sig))
        reply = (bytes([SSH_MSG_KEX_ECDH_REPLY]) + s(self.host_blob) +
                 s(q_s) + s(s("ssh-ed25519") + s(sig)))
        self.p.send_packet(reply)
        self.p.send_packet(bytes([SSH_MSG_NEWKEYS]))
        nk = self.p.recv_packet()
        if nk[0] != SSH_MSG_NEWKEYS:
            raise ValueError("expected NEWKEYS, got %d" % nk[0])
        self.H = H
        self.K = K
        # Note: client's I_C is cinit payload minus msg num; our I_S likewise.
        # Derive with the same blob convention (msg-number excluded) on both.
        self.p.set_keys(H, K, self.host_blob, self.cinit, self.sinit)
        keys = dict(dec_iv=self.p.dec_iv, enc_iv=self.p.enc_iv,
                    dec_key=self.p.dec_key, enc_key=self.p.enc_key,
                    mac_in=self.p.mac_key_in, mac_out=self.p.mac_key_out)
        open("/tmp/catos_server_keys.txt","w").write(
            "\n".join("%s=%s" % (k,v.hex()) for k,v in keys.items()))

    def run(self):
        self.banner()
        self.negotiate()
        self.do_kex()
        # authenticate: accept "none" method
        req = self.p.recv_packet()
        if req[0] == SSH_MSG_USERAUTH_REQUEST:
            self.p.send_packet(bytes([SSH_MSG_USERAUTH_SUCCESS]))
        # connection layer: one session channel
        ch_open = self.p.recv_packet()
        if ch_open[0] == SSH_MSG_CHANNEL_OPEN:
            _, off = ssh_string_at(ch_open, 1)          # "session"
            sender, off = struct.unpack(">I", ch_open[off:off + 4])[0], off + 4
            self.client_chan = sender
            self.p.send_packet(bytes([SSH_MSG_CHANNEL_OPEN_CONFIRMATION]) +
                               struct.pack(">I", sender) + struct.pack(">I", 0) +
                               struct.pack(">I", 0x20000) + struct.pack(">I", 0x8000))
        # handle channel requests until exec/shell; send a banner then close
        while True:
            msg = self.p.recv_packet()
            if msg[0] == SSH_MSG_CHANNEL_REQUEST:
                typ, off = ssh_string_at(msg, 1)
                want_reply = msg[off]
                if typ in ("exec", "shell"):
                    if want_reply:
                        self.p.send_packet(bytes([SSH_MSG_CHANNEL_SUCCESS]) +
                                           struct.pack(">I", self.client_chan))
                    # echo a fixed payload so the client can verify it arrived
                    out = b"HELLO_FROM_CATOS_SSH_PROTO\r\n"
                    self.p.send_packet(bytes([SSH_MSG_CHANNEL_DATA]) +
                                       struct.pack(">I", self.client_chan) + s(out))
                    self.p.send_packet(bytes([SSH_MSG_CHANNEL_EOF]) +
                                       struct.pack(">I", self.client_chan))
                    self.p.send_packet(bytes([SSH_MSG_CHANNEL_CLOSE]) +
                                       struct.pack(">I", self.client_chan))
                    break
                else:
                    if want_reply:
                        self.p.send_packet(bytes([SSH_MSG_CHANNEL_FAILURE]) +
                                           struct.pack(">I", self.client_chan))
            elif msg[0] == SSH_MSG_CHANNEL_CLOSE:
                break
        # drain
        try:
            while True:
                m = self.p.recv_packet()
                if m[0] == SSH_MSG_CHANNEL_CLOSE:
                    break
        except (EOFError, OSError):
            pass


def handle(sock, addr, host_key):
    sess = None
    try:
        # trace recv msg types
        _orig_recv = Packetizer.recv_packet
        def traced_recv(self):
            p = _orig_recv(self)
            print("    RECV msg=%d enc=%s len=%d" % (p[0] if p else -1,
                  bool(self.dec_key), len(p)))
            return p
        Packetizer.recv_packet = traced_recv
        # read client banner FIRST (before sending ours) to store V_C exactly
        line = b""
        while not line.endswith(b"\n"):
            c = sock.recv(1)
            if not c:
                return
            line += c
        sess = SSHSession(sock, host_key)
        sess.client_banner = line
        sock.sendall(SERVER_VERSION)
        # negotiate skipping re-read of banner
        sess.sinit = build_kexinit()
        sess.p.send_packet(sess.sinit)
        cinit = sess.p.recv_packet()
        sess.cinit = cinit
        names = parse_kexinit(cinit)
        print("    client algos:", names[0], "/", names[1], "/", names[2])
        sess.do_kex()
        # auth
        req = sess.p.recv_packet()
        if req[0] == SSH_MSG_USERAUTH_REQUEST:
            sess.p.send_packet(bytes([SSH_MSG_USERAUTH_SUCCESS]))
        # channel open
        ch = sess.p.recv_packet()
        if ch[0] == SSH_MSG_CHANNEL_OPEN:
            _, off = ssh_string_at(ch, 1)
            sender = struct.unpack(">I", ch[off:off + 4])[0]
            sess.client_chan = sender
            sess.p.send_packet(bytes([SSH_MSG_CHANNEL_OPEN_CONFIRMATION]) +
                               struct.pack(">I", sender) + struct.pack(">I", 0) +
                               struct.pack(">I", 0x20000) + struct.pack(">I", 0x8000))
        while True:
            msg = sess.p.recv_packet()
            if msg[0] == SSH_MSG_CHANNEL_REQUEST:
                typ, off = ssh_string_at(msg, 1)
                want_reply = msg[off]
                if typ in ("exec", "shell"):
                    if want_reply:
                        sess.p.send_packet(bytes([SSH_MSG_CHANNEL_SUCCESS]) +
                                           struct.pack(">I", sender))
                    out = b"HELLO_FROM_CATOS_SSH_PROTO\r\n"
                    sess.p.send_packet(bytes([SSH_MSG_CHANNEL_DATA]) +
                                       struct.pack(">I", sender) + s(out))
                    sess.p.send_packet(bytes([SSH_MSG_CHANNEL_EOF]) + struct.pack(">I", sender))
                    sess.p.send_packet(bytes([SSH_MSG_CHANNEL_CLOSE]) + struct.pack(">I", sender))
                    break
                elif want_reply:
                    sess.p.send_packet(bytes([SSH_MSG_CHANNEL_FAILURE]) + struct.pack(">I", sender))
            elif msg[0] == SSH_MSG_CHANNEL_CLOSE:
                break
        print("    session complete for", addr)
    except Exception as e:
        print("    session error:", type(e).__name__, e)
    finally:
        try:
            sock.close()
        except OSError:
            pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 22222
    host_key = Ed25519()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print("SSH prototype listening on 127.0.0.1:%d" % port)
    print("host key (Ed25519) fp:", sha256(host_key.pub).hex()[:32])
    print("Try: ssh -p %d -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
user@127.0.0.1 'echo hi'" % port)
    try:
        while True:
            sock, addr = srv.accept()
            print("connection from", addr)
            threading.Thread(target=handle, args=(sock, addr, host_key), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
