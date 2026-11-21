#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ssh_step2_probe.py -- focused diagnostics after KEX fix.

Boots CatOS + sshd, then:
  1. OpenSSH exec `echo MARKER`   -> print exact rc + full out
  2. OpenSSH exec `uname`         -> print exact rc + full out
  3. paramiko exec `echo P_OK`    -> print full channel stream
  4. paramiko invoke_shell        -> dump received bytes + guest console
usage: python tests/ssh_step2_probe.py
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
]


def wait_for_text(s, needle, timeout=20.0):
    from qemu_driver import screen_text
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(s)
        if needle in last:
            return True, last
        time.sleep(0.5)
    return False, last


def host_ssh(cmd, extra=None):
    argv = ["ssh", "-p", str(SSH_PORT)] + SSH_OPTS + (extra or []) + \
           ["root@127.0.0.1", cmd]
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
        wait_for_prompt, _stage_image, screen_text

    qemu = find_qemu(None)
    iso_i = _stage_image(iso)
    cmdline = [qemu, "-m", "256", "-no-reboot",
               "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso_i,
               "-boot", "order=d", "-display", "none",
               "-netdev", "user,id=n0,hostfwd=tcp::%d-:22" % SSH_PORT,
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
    try:
        time.sleep(20)
        type_text(msoc, "text", 0.05); sendkey(msoc, "ret", 0.6)
        sendkey(msoc, "y", 0.8); time.sleep(1.2)
        if not wait_for_prompt(msoc, timeout=120.0):
            print("FATAL: no shell prompt")
            return 3
        type_text(msoc, "sshd", 0.05); sendkey(msoc, "ret", 3.0)
        ok, _ = wait_for_text(msoc, "listening on port 22")
        print("[probe] sshd armed:", ok)
        time.sleep(1.0)

        print("=" * 70)
        rc, out = host_ssh("echo MARKER_A")
        print("1. OpenSSH exec echo: rc=%r out=%r" % (rc, out))
        rc, out = host_ssh("uname")
        print("2. OpenSSH exec uname: rc=%r out=%r" % (rc, out))

        import paramiko
        cli = paramiko.SSHClient()
        cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            cli.connect("127.0.0.1", port=SSH_PORT, username="root",
                        password="catos", allow_agent=False,
                        look_for_keys=False, timeout=25, banner_timeout=25,
                        auth_timeout=25)
            print("paramiko CONNECTED")
        except Exception as e:
            print("paramiko connect FAILED: %s: %s"
                  % (type(e).__name__, e))
        try:
            si, so, se = cli.exec_command("echo P_OK", timeout=20)
            print("3. paramiko exec: out=%r err=%r rc=%r"
                  % (so.read(), se.read(), so.channel.recv_exit_status()))
        except Exception as e:
            print("3. paramiko exec FAILED: %s: %s" % (type(e).__name__, e))
        try:
            chan = cli.invoke_shell(width=120, height=40)
            buf = b""
            t0 = time.time()
            while time.time() - t0 < 8:
                if chan.recv_ready():
                    buf += chan.recv(4096)
                else:
                    time.sleep(0.2)
            print("4. paramiko shell stream: %r" % buf)
            if chan.open:
                chan.send("echo SHELL_OK\n")
                t0 = time.time()
                while time.time() - t0 < 8:
                    if chan.recv_ready():
                        buf += chan.recv(4096)
                        if b"SHELL_OK" in buf:
                            break
                    time.sleep(0.2)
                print("   after send: %r" % buf)
            chan.close()
        except Exception as e:
            print("4. paramiko shell FAILED: %s: %s" % (type(e).__name__, e))
        try:
            cli.close()
        except Exception:
            pass

        time.sleep(1.0)
        gtxt = screen_text(msoc)
        print("=" * 70)
        print("GUEST CONSOLE:")
        for line in gtxt.splitlines():
            print("  " + line.rstrip())
        print("=" * 70)
    finally:
        kill_qemu(proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
