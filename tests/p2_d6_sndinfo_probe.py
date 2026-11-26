#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""p2-d6 sndinfo diagnostics probe (v2.82).

Acceptance for the ES1370/1371 PCM diagnostics exposed by `sndinfo`:

  * es1370  : QEMU -device es1370 -> PCI backend; `sndinfo -p` must show real
              period / avail decoded from SCOUNT/FRAMECNT, underrun n/a.
  * sb16    : only a QEMU SB16 ISA card -> PCI fields stay on the legacy
              "not exported" branch; underrun n/a.  No invented PCI numbers.
  * none    : no audio device at all -> sndinfo exits early with the
              "not initialized / no sound card" line; no PCM block printed.

Usage:
  python tests/p2_d6_sndinfo_probe.py [es1370|sb16|none]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu, screen_text,  # noqa: E402
                         sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
ISO = os.path.abspath(os.path.join(REPO, "kernel_v25", "catos.iso"))
SHOT_DIR = os.path.join(REPO, "artifacts", "v282", "p2-d6")

SCENARIOS = {
    "es1370": {"port": 4471, "shot": "01-sndinfo-es1370-device.png"},
    "sb16":   {"port": 4472, "shot": "02-sndinfo-sb16-pci-absent.png"},
    "none":   {"port": 4473, "shot": "03-sndinfo-no-device.png"},
}


def audio_args(scn, wav):
    if scn == "es1370":
        return ["-audiodev", "wav,id=snd0,path=%s" % wav,
                "-device", "es1370,audiodev=snd0"]
    if scn == "sb16":
        return ["-audiodev", "wav,id=snd0,path=%s" % wav,
                "-device", "sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0"]
    return []


def do(s, line, wait=1.2):
    type_text(s, line)
    sendkey(s, "ret", wait)


def wait_text(s, needle, timeout=12.0):
    deadline = time.time() + timeout
    best = ""
    while time.time() < deadline:
        best = screen_text(s)
        if needle in best:
            return best
        time.sleep(0.5)
    return best


def shoot(s, png):
    os.makedirs(SHOT_DIR, exist_ok=True)
    ppm = os.path.join(SHOT_DIR, os.path.basename(png)[:-4] + ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    s.sendall(("screendump %s\n" % ppm.replace("\\", "/")).encode())
    time.sleep(1.4)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        subprocess.run(["python3", "-c",
                         "from PIL import Image; Image.open(%r).save(%r)" % (ppm, png)],
                        check=False)
        os.remove(ppm)
    return png


def run(scn):
    cfg = SCENARIOS[scn]
    qemu = find_qemu(None)
    wav = os.path.join(HERE, "qemu_shots", "d6_%s.wav" % scn)
    os.makedirs(os.path.dirname(wav), exist_ok=True)
    if os.path.exists(wav):
        os.remove(wav)
    disk = os.path.join(HERE, "qemu_disk_d6.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = boot(qemu, ISO, cfg["port"], disk=disk,
                   extra_args=audio_args(scn, wav))
    force_text(s)
    try:
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL [%s]: prompt never appeared" % scn)
            return 3
        do(s, "clear", 0.8)

        if scn == "none":
            do(s, "sndinfo -p", 2.0)
            txt = wait_text(s, "sound card", 10.0)
            print("=== [%s] sndinfo -p (no device) ===" % scn)
            for r in [x.rstrip() for x in txt.splitlines() if x.strip()][:10]:
                print("  ", r)
        else:
            do(s, "sndinfo -p", 2.0)
            txt = wait_text(s, "Buffer size:", 10.0)
            print("=== [%s] sndinfo -p (idle) ===" % scn)
            for r in [x.rstrip() for x in txt.splitlines() if x.strip()][:12]:
                print("  ", r)

            # Arm one real DMA block, then re-read so SCOUNT/FRAMECNT carry
            # the live period/progress instead of the parked zeros.
            do(s, "clear", 0.8)
            do(s, "sndtest", 2.0)
            ttxt = wait_text(s, "PASS", 20.0)
            print("=== [%s] sndtest screen ===" % scn)
            for r in [x.rstrip() for x in ttxt.splitlines() if x.strip()][:10]:
                print("  ", r)
            do(s, "clear", 0.8)
            do(s, "sndinfo -p", 2.0)
            txt = wait_text(s, "Buffer size:", 10.0)
            print("=== [%s] sndinfo -p (after one tone) ===" % scn)
            for r in [x.rstrip() for x in txt.splitlines() if x.strip()][:12]:
                print("  ", r)

        png = shoot(s, os.path.join(SHOT_DIR, cfg["shot"]))
        print("  shot: %s" % png)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    scn = sys.argv[1] if len(sys.argv) > 1 else "es1370"
    if scn not in SCENARIOS:
        print("unknown scenario %r" % scn)
        sys.exit(2)
    sys.exit(run(scn) or 0)
