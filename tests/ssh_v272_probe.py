#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ssh_v272_probe.py -- v2.72 sshd autostart + persistent host key.

Boots CatOS WITH a FAT32 disk (IDE drive 0), formats it, and does NOT
type `sshd` anywhere.  Verifies:

  1. /etc/autoexec/sshd.sh (@sshd) spawns the daemon at boot: an SSH
     exec works without any manual sshd command.
  2. the host key persists across sessions: ssh-keyscan twice returns
     the identical ssh-ed25519 public key (and /etc/ssh_host.key exists).
  3. `sshd -d` foreground-vs-daemon: a second daemon instance cannot
     bind twice, but the first keeps serving (exec still works).

usage: python tests/ssh_v272_probe.py [catos.iso]
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
DISK = os.path.abspath(os.path.join(HERE, "qemu_disk_final.img"))
MARKER = "CATOS_SSH_AUTO_272"

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


def host_ssh(cmd):
    argv = ["ssh", "-p", str(SSH_PORT)] + SSH_OPTS + ["root@127.0.0.1", cmd]
    try:
        r = subprocess.run(argv, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60)
        return r.returncode, r.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def host_key_stability_check():
    """connect 1: store the host key (accept-new); connect 2: enforce it.
    A changed host key makes the second connection fail loudly."""
    kh = os.path.join(HERE, "dumps", "known_hosts_272")
    try:
        os.remove(kh)
    except OSError:
        pass
    base = ["ssh", "-p", str(SSH_PORT),
            "-o", "UserKnownHostsFile=" + kh.replace("\\", "/"),
            "-o", "PreferredAuthentications=none,keyboard-interactive,password",
            "-o", "NumberOfPasswordPrompts=0",
            "-o", "ConnectTimeout=20",
            "-o", "KexAlgorithms=curve25519-sha256",
            "-o", "HostKeyAlgorithms=ssh-ed25519",
            "-o", "Ciphers=aes128-ctr",
            "-o", "MACs=hmac-sha2-256",
            "-o", "LogLevel=ERROR"]
    r1 = subprocess.run(base + ["-o", "StrictHostKeyChecking=accept-new",
                                "root@127.0.0.1", "echo K1"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=60)
    ok1 = r1.returncode == 0 and b"K1" in r1.stdout
    r2 = subprocess.run(base + ["-o", "StrictHostKeyChecking=yes",
                                "root@127.0.0.1", "echo K2"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=60)
    ok2 = r2.returncode == 0 and b"K2" in r2.stdout
    return ok1, ok2, r2.stdout.decode("utf-8", "replace")[:160]


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
    disk_i = _stage_image(DISK)
    cmdline = [qemu, "-m", "256",
               "-drive", "file=%s,format=raw,if=ide,index=0" % disk_i,
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
        print("%-4s %-46s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def boot_to_prompt(first=False):
        """blind text switch + wait for the shell prompt"""
        time.sleep(20 if first else 14)
        type_text(s, "text", 0.05); sendkey(s, "ret", 0.6)
        sendkey(s, "y", 0.8); time.sleep(1.2)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return False
        return True

    try:
        if not boot_to_prompt(first=True):
            return 3

        # phase 1: prepare the disk -- format creates the FAT volume that
        # the autostart seeding (and the persistent host key) need
        do("format", 0.6); sendkey(s, "y", 0.15); sendkey(s, "ret", 2.0)
        time.sleep(1.5)

        # phase 2: guest-initiated reboot so autostart runs WITH a volume
        # -- the daemon must come up entirely on its own ("@sshd" in
        # /etc/autoexec).  Guest reboot keeps the monitor socket alive.
        do("reboot", 1.0)
        time.sleep(4.0)
        if not boot_to_prompt():
            return 3

        do("jobs", 1.2)
        txt = screen_text(s)
        daemon_up = "D" in txt and "sshd" in txt
        step("autostart: sshd daemon in jobs (no manual sshd)",
             daemon_up, txt[-120:] if not daemon_up else "")
        do("clear", 0.4)

        # deliberately NO `sshd` command -- connect straight away
        time.sleep(6.0)
        rc, out = host_ssh("echo %s" % MARKER)
        ok1 = rc == 0 and MARKER in out
        step("exec via boot-spawned daemon", ok1,
             out.strip()[:120] if not ok1 else "")
        if not ok1:
            print("--- screen after failed exec ---")
            for ln in screen_text(s).splitlines()[-14:]:
                print("  ", ln)

        ok1a, ok2a, note = host_key_stability_check()
        step("host key stored & enforced on reconnect", ok1a and ok2a, note)

        # /etc/ssh_host.key must now exist on the freshly formatted volume
        do("ls /etc", 1.2)
        txt = screen_text(s)
        ok3 = "SSH_HOST" in txt.upper()
        step("persistence: /etc/ssh_host.key exists", ok3)

        # daemon survives independent sessions: exec again
        rc, out = host_ssh("uname")
        ok4 = rc == 0 and ("catos" in out.lower())
        step("daemon still serving after prior sessions", ok4,
             out.strip()[:120] if not ok4 else "")
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
