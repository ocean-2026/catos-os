#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ssh_hdiff_probe.py -- v2.71 SSH exchange-hash (H) differential probe.

Boots CatOS with hostfwd tcp::2222 -> guest:22, runs `sshd`, then connects
through a recording MITM proxy with paramiko.  paramiko's kex callback is
patched to dump every H input field; the proxy records the raw wire bytes
of the plaintext KEX phase.  Offline we:

  1. rebuild H from WIRE-TRUE fields (V_C, V_S, I_C, I_S, K_S, q_c, q_s,
     and the shared K reported by paramiko) and compare against the H
     paramiko computed -- any mismatch isolates the differing field;
  2. print guest-side console (hblen / per-field lengths / H digest)
     captured from the QEMU monitor screen for the same session.

usage: python tests/ssh_hdiff_probe.py [catos.iso]
"""
import hashlib
import os
import socket
import struct
import subprocess
import sys
import threading
import time

from cryptography.hazmat.primitives import serialization

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MONITOR_PORT = 4499
SSH_FWD = 2222          # qemu hostfwd -> guest 22
MITM_PORT = 2231        # paramiko dials here

MSG_KEXINIT, MSG_NEWKEYS = 20, 21
MSG_KEX_ECDH_INIT, MSG_KEX_ECDH_REPLY = 30, 31


def s(b):
    if isinstance(b, str):
        b = b.encode()
    return struct.pack(">I", len(b)) + b


def mpint_int(K):
    b = K.to_bytes((K.bit_length() + 7) // 8 or 1, "big")
    b = b.lstrip(b"\x00") or b"\x00"
    if b[0] & 0x80:
        b = b"\x00" + b
    return b


class Recorder:
    """bidirectional byte recorder for one connection"""

    def __init__(self):
        self.c2s = b""
        self.s2c = b""
        self.lock = threading.Lock()

    def add_c2s(self, d):
        with self.lock:
            self.c2s += d

    def add_s2c(self, d):
        with self.lock:
            self.s2c += d


def mitm_proxy(listen_port, target_port, rec):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port))
    srv.listen(1)
    while True:
        csock, _ = srv.accept()
        try:
            tsock = socket.create_connection(("127.0.0.1", target_port),
                                             timeout=10)
        except OSError:
            csock.close()
            continue

        def pump(src, dst, add):
            try:
                while True:
                    d = src.recv(65536)
                    if not d:
                        break
                    add(d)
                    dst.sendall(d)
            except OSError:
                pass
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass

        t1 = threading.Thread(target=pump, args=(csock, tsock,
                                                 rec.add_c2s), daemon=True)
        t2 = threading.Thread(target=pump, args=(tsock, csock,
                                                 rec.add_s2c), daemon=True)
        t1.start(); t2.start(); t1.join(); t2.join()
        csock.close(); tsock.close()


def parse_wire(rec):
    """extract plaintext-phase fields from the recorded streams"""
    def first_banner(data):
        i = data.find(b"\n")
        return data[:i].rstrip(b"\r"), data[i + 1:]

    def parse_packets(data):
        pkts = []
        off = 0
        while off + 4 <= len(data):
            n = struct.unpack(">I", data[off:off + 4])[0]
            if n < 2 or off + 4 + n > len(data):
                break
            body = data[off + 4:off + 4 + n]
            pad = body[0]
            pkts.append(body[1:n - pad])
            off += 4 + n
        return pkts, off

    v_c, rest_c = first_banner(rec.c2s)
    v_s, rest_s = first_banner(rec.s2c)
    pkts_c, _ = parse_packets(rest_c)
    pkts_s, _ = parse_packets(rest_s)
    out = {"V_C": v_c, "V_S": v_s}
    for p in pkts_c:
        if p[0] == MSG_KEXINIT:
            out["I_C"] = p
        if p[0] == MSG_KEX_ECDH_INIT:
            qlen = struct.unpack(">I", p[1:5])[0]
            out["q_c"] = p[5:5 + qlen]
    for p in pkts_s:
        if p[0] == MSG_KEXINIT:
            out["I_S"] = p
        if p[0] == MSG_KEX_ECDH_REPLY:
            o = 1
            klen = struct.unpack(">I", p[o:o + 4])[0]
            out["K_S"] = p[o + 4:o + 4 + klen]
            o += 4 + klen
            flen = struct.unpack(">I", p[o:o + 4])[0]
            out["q_s"] = p[o + 4:o + 4 + flen]
    return out


def hexs(b, n=48):
    h = b.hex().upper()
    return h[:n] + ("..." if len(h) > n else "")


def main():
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    sys.path.insert(0, HERE)
    from qemu_driver import find_qemu, kill_qemu, sendkey, type_text, \
        wait_for_prompt, _stage_image, screen_text

    qemu = find_qemu(None)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    iso_i = _stage_image(iso)
    cmdline = [qemu, "-m", "256", "-no-reboot",
               "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso_i,
               "-boot", "order=d", "-display", "none",
               "-netdev", "user,id=n0,hostfwd=tcp::%d-:22" % SSH_FWD,
               "-device", "e1000,netdev=n0",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    msoc = None
    for _ in range(40):
        if proc.poll() is not None:
            print("FATAL: qemu exited rc=%r" % proc.returncode)
            return 3
        try:
            msoc = socket.create_connection(("127.0.0.1", MONITOR_PORT),
                                            timeout=5)
            break
        except OSError:
            time.sleep(2)
    if msoc is None:
        print("FATAL: no monitor")
        return 3

    rec = Recorder()
    threading.Thread(target=mitm_proxy,
                     args=(MITM_PORT, SSH_FWD, rec), daemon=True).start()

    dumped = {}

    def dump_kex_reply(self, m):
        # capture every H input field paramiko uses (same as upstream code)
        import paramiko.kex_curve25519 as kx
        peer_host_key_bytes = m.get_string()
        peer_key_bytes = m.get_string()
        sig = m.get_binary()
        peer_key = kx.X25519PublicKey.from_public_bytes(peer_key_bytes)
        K = self._perform_exchange(peer_key)
        K = int(__import__("binascii").hexlify(K), 16)
        hm = kx.Message()
        hm.add(self.transport.local_version,
               self.transport.remote_version,
               self.transport.local_kex_init,
               self.transport.remote_kex_init)
        hm.add_string(peer_host_key_bytes)
        hm.add_string(self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))
        hm.add_string(peer_key_bytes)
        hm.add_mpint(K)
        H = self.hash_algo(hm.asbytes()).digest()
        dumped["local_version"] = self.transport.local_version
        dumped["remote_version"] = self.transport.remote_version
        dumped["local_kex_init"] = self.transport.local_kex_init
        dumped["remote_kex_init"] = self.transport.remote_kex_init
        dumped["K_S"] = peer_host_key_bytes
        dumped["e"] = self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        dumped["f"] = peer_key_bytes
        dumped["K"] = K
        dumped["H"] = H
        dumped["sig"] = sig
        dumped["hm_bytes"] = hm.asbytes()
        dumped["my_bytes"] = (s(dumped["local_version"]) + s(dumped["remote_version"])
                              + s(dumped["local_kex_init"]) + s(dumped["remote_kex_init"])
                              + s(peer_host_key_bytes)
                              + s(dumped["e"]) + s(dumped["f"]) + mpint_int(K))
        dumped["H"] = H
        # now replicate upstream behaviour so the session can continue
        self.transport._set_K_H(K, H)
        try:
            self.transport._verify_key(peer_host_key_bytes, sig)
            self.transport._activate_outbound()
        except Exception as e:
            print("[paramiko] verify failed: %s: %s"
                  % (type(e).__name__, e))
            raise

    try:
        time.sleep(20)
        type_text(msoc, "text", 0.05); sendkey(msoc, "ret", 0.6)
        sendkey(msoc, "y", 0.8); time.sleep(1.2)
        if not wait_for_prompt(msoc, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(msoc, "sshd", 0.05); sendkey(msoc, "ret", 3.0)
        deadline = time.time() + 20
        ok = False
        while time.time() < deadline:
            if "listening on port 22" in screen_text(msoc):
                ok = True
                break
            time.sleep(0.5)
        print("[probe] sshd armed:", ok)
        time.sleep(1.0)

        import paramiko
        import paramiko.kex_curve25519 as kx
        kx.KexCurve25519._parse_kexecdh_reply = dump_kex_reply

        cli = paramiko.SSHClient()
        cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            cli.connect("127.0.0.1", port=MITM_PORT, username="root",
                        password="catos", allow_agent=False,
                        look_for_keys=False, timeout=25,
                        banner_timeout=25, auth_timeout=25)
            print("[probe] paramiko CONNECTED (verify passed)")
        except Exception as e:
            print("[probe] paramiko failed: %s: %s"
                  % (type(e).__name__, e))
        time.sleep(1.0)
        try:
            cli.close()
        except Exception:
            pass

        # ---- guest console dump ----
        gtxt = screen_text(msoc)
        print("=" * 70)
        print("GUEST CONSOLE (tail):")
        for line in gtxt.splitlines():
            if any(k in line for k in ("ssh:", "hb ", "sshd", "L vc=")):
                print("  " + line.rstrip())
        print("=" * 70)

        # ---- offline reconstruction from wire ----
        wire = parse_wire(rec)
        os.makedirs(os.path.join(HERE, "dumps"), exist_ok=True)
        with open(os.path.join(HERE, "dumps", "ssh_hdiff.bin"), "wb") as f:
            f.write(b"C2S:" + struct.pack(">I", len(rec.c2s)) + rec.c2s)
            f.write(b"S2C:" + struct.pack(">I", len(rec.s2c)) + rec.s2c)
            f.write(b"HM:" + struct.pack(">I", len(dumped.get("hm_bytes", b"")))
                    + dumped.get("hm_bytes", b""))
            f.write(b"MYB:" + struct.pack(">I", len(dumped.get("my_bytes", b"")))
                    + dumped.get("my_bytes", b""))
            f.write(b"PUB:" + dumped.get("K_S", b""))
            f.write(b"SIG:" + dumped.get("sig", b""))
            f.write(b"HXX:" + dumped.get("H", b""))
        if "I_C" not in wire or "I_S" not in wire or "K_S" not in wire:
            print("[probe] wire parse incomplete: keys=%s" % sorted(wire))
            return 4
        K = dumped["K"]
        H_wire = hashlib.sha256(
            s(wire["V_C"]) + s(wire["V_S"]) + s(wire["I_C"]) + s(wire["I_S"])
            + s(wire["K_S"]) + s(wire["q_c"]) + s(wire["q_s"])
            + mpint_int(K)).digest()
        print("V_C        wire=%r py=%r same=%s"
              % (wire["V_C"], dumped.get("local_version"),
                 wire["V_C"] == dumped.get("local_version")))
        print("V_S        wire=%r py=%r same=%s"
              % (wire["V_S"], dumped.get("remote_version"),
                 wire["V_S"] == dumped.get("remote_version")))
        print("I_C        wire %4d py %4d same=%s"
              % (len(wire["I_C"]), len(dumped.get("local_kex_init", b"")),
                 wire["I_C"] == dumped.get("local_kex_init")))
        print("I_S        wire %4d py %4d same=%s"
              % (len(wire["I_S"]), len(dumped.get("remote_kex_init", b"")),
                 wire["I_S"] == dumped.get("remote_kex_init")))
        print("K_S        wire %4d py %4d same=%s"
              % (len(wire["K_S"]), len(dumped.get("K_S", b"")),
                 wire["K_S"] == dumped.get("K_S")))
        print("q_c        wire %4d py %4d same=%s"
              % (len(wire["q_c"]), len(dumped.get("e", b"")),
                 wire["q_c"] == dumped.get("e")))
        print("q_s        wire %4d py %4d same=%s"
              % (len(wire["q_s"]), len(dumped.get("f", b"")),
                 wire["q_s"] == dumped.get("f")))
        print("H_wire(from wire fields + py K) = %s" % H_wire.hex().upper())
        print("H_paramiko                      = %s"
              % dumped["H"].hex().upper())
        if H_wire == dumped["H"]:
            print(">>> paramiko H consistent with wire; guest H must differ")
        else:
            print(">>> paramiko H != wire H -- paramiko uses different input")
        # byte-level diff: paramiko's hm.asbytes() vs our reconstruction
        a = dumped.get("hm_bytes", b"")
        b = dumped.get("my_bytes", b"")
        print("hm(asbytes) len=%d  my recon len=%d  equal=%s"
              % (len(a), len(b), a == b))
        if a != b:
            n = min(len(a), len(b))
            i = 0
            while i < n and a[i] == b[i]:
                i += 1
            print("first diff at offset %d" % i)
            lo = max(0, i - 16)
            print("  paramiko: %s" % a[lo:i + 32].hex())
            print("  my recon: %s" % b[lo:i + 32].hex())
    finally:
        kill_qemu(proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
