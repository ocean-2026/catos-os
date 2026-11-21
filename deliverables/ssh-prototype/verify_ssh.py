#!/usr/bin/env python3
"""verify_ssh.py -- acceptance test for the SSH-2 host prototype.

Starts sshd_min.py on a free port, connects with the SYSTEM OpenSSH client,
runs `echo hi` and asserts the server's marker comes back.  Repeatable proof
that the byte-level protocol implementation is correct before porting to the
CatOS kernel.

usage: python deliverables/ssh-prototype/verify_ssh.py
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MARKER = b"HELLO_FROM_CATOS_SSH_PROTO"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    port = free_port()
    env = dict(os.environ)
    srv = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "sshd_min.py"),
                            str(port)], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, env=env)
    try:
        time.sleep(1.0)
        cmd = ["ssh", "-p", str(port),
               "-o", "StrictHostKeyChecking=no",
               "-o", "UserKnownHostsFile=/dev/null",
               "-o", "PreferredAuthentications=none",
               "-o", "ConnectTimeout=5",
               "-o", "KexAlgorithms=curve25519-sha256",
               "-o", "HostKeyAlgorithms=ssh-ed25519",
               "-o", "Ciphers=aes128-ctr",
               "-o", "MACs=hmac-sha2-256",
               "-o", "LogLevel=ERROR",
               "user@127.0.0.1", "echo hi"]
        r = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=25)
        out = r.stdout
        ok = MARKER in out
        print("SSH client stdout:", out.decode("utf-8", "replace").strip())
        print("%-4s %s" % ("PASS" if ok else "FAIL",
                           "system ssh connects + exec returns marker"))
        # also grab server log tail for the record
        srv.terminate()
        try:
            slog = srv.communicate(timeout=3)[0].decode("utf-8", "replace")
            stages = [x for x in ("KEX complete", "auth complete",
                                  "channel open", "session complete")
                      if x in slog]
            print("server stages reached:", ", ".join(stages))
            ok = ok and len(stages) == 4
            print("%-4s %s" % ("PASS" if len(stages) == 4 else "FAIL",
                               "all SSH protocol stages completed"))
        except subprocess.TimeoutExpired:
            pass
        return 0 if ok else 1
    finally:
        if srv.poll() is None:
            srv.kill()


if __name__ == "__main__":
    sys.exit(main())
