#!/usr/bin/env python3
"""es1371_regress.py -- v2.21 #56: VMware/ES1370 PCI audio acceptance.

Boots CatOS in QEMU with an Ensoniq ES1370 PCI card (the same register
family as VMware's ES1371 1274:1371) and a `wav` audio backend, then checks
both sides of the contract:

  guest side : `sndinfo` must report the PCI backend (NOT SB16), `sndtest`
               and the ring3 `run /bin/sndtest.ce` must report PASS.
  host side  : QEMU's captured WAV must contain non-silent samples.  A
               driver that probes but never arms DAC1 DMA yields a silent
               WAV, so screen assertions alone are not enough.

QEMU >= 7.1 removed -soundhw; only -device + -audiodev works.  The shared
qemu_driver owns PID-safe teardown and the VGA text decoder.  Never use
taskkill /F /IM qemu-system-i386.exe on this machine.

usage:
  python tests/es1371_regress.py [catos.iso] [--qemu PATH]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt)
from snd_regress import wav_check, win_path  # noqa: E402

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4468                 # own port: 4444/4460..4467/4476 taken
SHOT_DIR = os.path.join(HERE, "qemu_shots")


def audio_args(wav):
    return [
        "-audiodev", "wav,id=snd0,path=%s" % wav,
        "-device", "es1370,audiodev=snd0",
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
    wav = win_path(os.path.join(SHOT_DIR, "es1371.wav"))
    if os.path.exists(wav):
        os.remove(wav)

    disk = win_path(os.path.join(HERE, "qemu_disk_es1371.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = _qemu_boot(qemu, iso, MONITOR_PORT, disk=disk,
                         extra_args=audio_args(wav))
    results = []

    def step(name, ok, note):
        results.append((name, bool(ok)))
        print("%-4s %-24s %s" % ("PASS" if ok else "FAIL", name, note))

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
        do("sndinfo", 2.0)
        ok, txt = wait_for_text(s, ["ES1370/1371 (PCI)"], 12.0)
        step("sndinfo: PCI backend", ok, txt[-200:] if not ok else "selected ES1370/1371")

        ok2, txt2 = wait_for_text(s, ["pci dev="], 6.0)
        step("sndinfo: device id", ok2 and "5000" in txt2,
             txt2[-120:] if not (ok2 and "5000" in txt2) else "0x5000 (QEMU ES1370)")
        step("sndinfo: no SB16 claim", "SB16 ready" not in txt, "backend dispatch correct")

        clear()
        do("sndtest 440 300", 2.0)
        ok, txt = wait_for_text(s, ["SNDTEST PASS"], 25.0)
        step("sndtest: kernel tone", ok, txt[-220:] if not ok else "PASS")

        clear()
        do("run /bin/sndtest.ce", 2.0)
        ok, txt = wait_for_text(s, ["SND CE PASS"], 25.0)
        step("sndtest.ce: ring3 ABI", ok, txt[-220:] if not ok else "PASS")

        # Let QEMU flush the wav backend before killing the guest.
        time.sleep(2.0)
    finally:
        kill_qemu(proc)

    ok, note = wav_check(wav)
    step("host WAV non-silent", ok, note)

    failed = [n for n, good in results if not good]
    print("%d step(s) failed" % len(failed))
    if failed:
        for n in failed:
            print("  -", n)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
