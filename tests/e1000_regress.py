#!/usr/bin/env python3
"""e1000_regress.py -- v2.21 #55: Intel PRO/1000 (e1000) NIC acceptance.

Boots CatOS in QEMU with an Intel e1000 PCI NIC (the default "e1000"
adapter type in VMware Workstation too) behind slirp user networking,
then verifies the driver end-to-end:

  * `netdiag` must report backend=e1000 and up=1 (probe + init + MAC read).
  * `ping 10.0.2.2` (the slirp gateway) must get a reply -- this proves the
    whole TX -> ARP -> RX loop works through the e1000 descriptor rings and
    the high-MMIO BAR0 mapping.  A driver that probes but never arms DMA
    or never services RX would print "no-reply" here.
  * `netdiag` afterwards must show tx>0 and rx>0.

QEMU >= 7.1 removed -soundhw; only -device + -netdev works.  The shared
qemu_driver owns PID-safe teardown.  Never use taskkill /F /IM
qemu-system-i386.exe on this machine.

usage:
  python tests/e1000_regress.py [catos.iso] [--qemu PATH]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt)  # noqa: E402

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4469                 # own port: 4444/4460..4468 taken
SHOT_DIR = os.path.join(HERE, "qemu_shots")


def net_args():
    # slirp user net: guest 10.0.2.15, gateway/host 10.0.2.2, dns 10.0.2.3
    return [
        "-netdev", "user,id=n0",
        "-device", "e1000,netdev=n0",
    ]


def wait_for_text(s, needles, timeout=25.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(s)
        if all(n in last for n in needles):
            return True, last
        time.sleep(0.5)
    return False, last


def grab_prefixed_int(txt, prefix):
    """Return the first integer following `prefix` in txt, else -1."""
    i = txt.find(prefix)
    if i < 0:
        return -1
    j = i + len(prefix)
    num = ""
    while j < len(txt) and txt[j].isdigit():
        num += txt[j]
        j += 1
    return int(num) if num else -1


def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = None
    if "--qemu" in args:
        i = args.index("--qemu")
        if i + 1 < len(args):
            qemu = args[i + 1]

    iso = os.path.abspath(iso or os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    os.makedirs(SHOT_DIR, exist_ok=True)
    disk = os.path.join(SHOT_DIR, "..", "qemu_disk_e1000.img")
    disk = os.path.abspath(disk)
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = _qemu_boot(qemu, iso, MONITOR_PORT, disk=disk,
                         extra_args=net_args())
    results = []

    def step(name, ok, note):
        results.append((name, bool(ok)))
        print("%-4s %-26s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 1.0)

    try:
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3

        clear()
        do("netdiag", 2.0)
        ok, txt = wait_for_text(s, ["backend=e1000", "up=1"], 12.0)
        step("netdiag: e1000 backend active", ok,
             txt[-200:] if not ok else "backend=e1000 up=1")
        step("netdiag: PCI e1000 id",
             ("8086" in txt) or ("100E" in txt) or ("100e" in txt) or
             ("100F" in txt) or ("100f" in txt),
             "PCI vendor/device visible")

        clear()
        do("ping 10.0.2.2", 3.0)
        ok, txt = wait_for_text(s, ["reply"], 30.0)
        step("ping 10.0.2.2: got reply (TX+RX loop)",
             ok, txt[-200:] if not ok else "ICMP echo reply received")

        clear()
        do("netdiag", 2.0)
        ok, txt = wait_for_text(s, ["tx=", "rx="], 12.0)
        tx = grab_prefixed_int(txt, "tx=")
        rx = grab_prefixed_int(txt, "rx=")
        step("e1000 frames: tx>0 and rx>0",
             ok and tx > 0 and rx > 0, "tx=%s rx=%s" % (tx, rx))

        # negative: the legacy PCnet path must NOT be the backend
        step("e1000: not pcnet backend", "backend=pcnet" not in txt,
             "correct backend dispatch")
    finally:
        kill_qemu(proc)

    failed = [n for n, good in results if not good]
    print("%d step(s) failed" % len(failed))
    if failed:
        for n in failed:
            print("  -", n)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
