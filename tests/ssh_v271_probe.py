#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ssh_v271_probe.py -- v2.71 kernel SSH-2 server acceptance.

Boots CatOS with an e1000 NIC behind slirp user-net with hostfwd
tcp::2222 -> guest:22, runs `sshd` in the guest, then from the HOST:

  1. exec channel via the system OpenSSH client: `echo MARKER` must
     come back with exit status 0.
  2. exec channel: `uname` returns CatOS identity text.
  3. interactive shell channel via paramiko: send `echo ...` and read
     the echoed command output plus the `> ` prompt.
  4. tamper protection: exec with a deliberately failing command
     returns non-zero exit status.

usage: python tests/ssh_v271_probe.py [catos.iso]
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MONITOR_PORT = 4499
SSH_PORT = 2222
MARKER = "CATOS_SSH_EXEC_OK_271"

SSH_OPTS = [
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "PreferredAuthentications=none,keyboard-interactive,password",
    "-o", "NumberOfPasswordPrompts=0",
    "-o", "ConnectTimeout=20",
    "-o", "KexAlgorithms=curve25519-sha256",
    "-o", "HostKeyAlgorithms=ssh-ed25519",
    "-o", "Ciphers=aes128-ctr",
    "-o", "MACs=hmac-sha2-256",
    "-o", "LogLevel=ERROR",
]


def wait_for_text(s, needles, timeout=25.0):
    from qemu_driver import screen_text
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(s)
        if all(n in last for n in needles):
            return True, last
        time.sleep(0.5)
    return False, last


def host_ssh(cmd):
    argv = ["ssh", "-p", str(SSH_PORT)] + SSH_OPTS + ["root@127.0.0.1", cmd]
    try:
        r = subprocess.run(argv, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60)
        return r.returncode, r.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def main():
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    sys.path.insert(0, HERE)
    from qemu_driver import find_qemu, kill_qemu, sendkey, type_text, \
        wait_for_prompt, _stage_image

    qemu = find_qemu(None)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    iso_i = _stage_image(iso)
    cmdline = [qemu, "-m", "256", "-no-reboot",
               "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso_i,
               "-boot", "order=d", "-display", "none",
               "-netdev", "user,id=n0,hostfwd=tcp::%d-:22" % SSH_PORT,
               "-device", "e1000,netdev=n0",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    s = None
    for _ in range(40):
        if proc.poll() is not None:
            print("FATAL: qemu exited rc=%r" % proc.returncode)
            return 3
        try:
            s = socket.create_connection(("127.0.0.1", MONITOR_PORT), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if s is None:
        print("FATAL: no monitor")
        return 3
    results = []

    def step(name, ok, note=""):
        results.append((name, bool(ok)))
        print("%-4s %-44s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    try:
        # boot default is gfx; blind-switch to text (probe convention)
        time.sleep(20)
        type_text(s, "text", 0.05); sendkey(s, "ret", 0.6)
        sendkey(s, "y", 0.8); time.sleep(1.2)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        do("sshd", 3.0)
        ok, txt = wait_for_text(s, ["listening on port 22"], 20.0)
        step("sshd: listener armed on port 22", ok, txt[-100:] if not ok else "")

        # let slirp see the listener before the client dials
        time.sleep(1.0)

        # 1. exec channel: system OpenSSH client
        rc, out = host_ssh("echo %s" % MARKER)
        step("exec: OpenSSH echo returns marker",
             rc == 0 and MARKER in out, out.strip()[:120] if not (rc == 0 and MARKER in out) else "")

        # 2. exec channel: uname identity
        rc, out = host_ssh("uname")
        step("exec: uname reports CatOS",
             rc == 0 and ("CatOS" in out or "catos" in out.lower()),
             out.strip()[:120])

        # 3. interactive shell channel via paramiko
        try:
            import paramiko
            cli = paramiko.SSHClient()
            cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            cli.connect("127.0.0.1", port=SSH_PORT, username="root",
                        password="catos", allow_agent=False,
                        look_for_keys=False, timeout=25,
                        banner_timeout=25, auth_timeout=25)
            chan = cli.invoke_shell(width=120, height=40)
            time.sleep(0.8)
            chan.send("echo SHELLCHAN_OK_271\n")
            buf = b""
            deadline = time.time() + 20
            while time.time() < deadline:
                if chan.recv_ready():
                    buf += chan.recv(4096)
                    if b"SHELLCHAN_OK_271" in buf.replace(b"\r", b""):
                        break
                time.sleep(0.2)
            ok3 = b"SHELLCHAN_OK_271" in buf.replace(b"\r", b"")
            step("shell: paramiko interactive channel", ok3,
                 buf.decode("utf-8", "replace").strip()[:120] if not ok3 else "")
            chan.close()
            cli.close()
        except Exception as e:
            step("shell: paramiko interactive channel", False,
                 "%s: %s" % (type(e).__name__, e))

        # 4. exec exit status propagation
        rc, out = host_ssh("nosuchcmd_xyz")
        step("exec: failing command -> nonzero status",
             rc != 0, "rc=%r out=%r" % (rc, out.strip()[:80]))

        ok, txt = wait_for_text(s, ["session opened"], 5.0)
        step("sshd: sessions visible on console", ok)
    finally:
        kill_qemu(proc)

    npass = sum(1 for _, ok in results if ok)
    print("========== SUMMARY ==========")
    for name, ok in results:
        print("PASS" if ok else "FAIL", name)
    print("%d/%d passed" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
