#!/usr/bin/env python3
"""snd_regress.py -- on-metal acceptance for the SB16 audio driver (v2.11).

Boots CatOS in QEMU with an ISA Sound Blaster 16 and a `wav` audio backend,
then checks BOTH sides of the contract:

  guest side : `sndinfo`, `sndtest` and the ring3 `run /bin/sndtest.ce`
               must report a real DSP, a DMA-completion IRQ and PASS.
  host side  : QEMU's captured WAV must contain NON-SILENT samples.  A
               driver that probes successfully but never arms DMA produces
               a perfectly valid but silent WAV, so the screen assertions
               alone are not enough.

QEMU >= 7.1 removed -soundhw; the only working form is -device + -audiodev.
The shared qemu_driver owns PID-safe teardown, the VGA text decoder and the
`clear` (not `cls`) convention.

usage:
  python tests/snd_regress.py [catos.iso] [--qemu PATH] [--keep]
"""
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4460                 # this driver's own port; never 4444
SHOT_DIR = os.path.join(HERE, "qemu_shots")


def win_path(p):
    """QEMU is a native Windows binary and does not understand MSYS /tmp."""
    return os.path.abspath(p).replace("\\", "/")


def audio_args(wav):
    # Keep -audiodev minimal: `path` is the only wav-specific property and
    # is accepted on every QEMU version that still ships the wav backend.
    # Guest format (22050 Hz, U8 mono) is negotiated by the SB16 voice.
    return [
        "-audiodev", "wav,id=snd0,path=%s" % wav,
        "-device", "sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0",
    ]


def wav_check(path):
    """Return (ok, note): the WAV must exist and contain non-silent PCM.

    QEMU's wav backend (audio/wavaudio.c) writes a 44-byte header with the
    RIFF and data sizes set to ZERO, then patches them in wav_fini_out()
    when the voice is closed.  A `quit` from the monitor does not always
    run that fini path, so a perfectly good capture can look empty:
    never trust the size fields.  Locate `data`, take everything up to
    EOF, and patch the two sizes ourselves so the file is also a valid,
    playable WAV (useful evidence, not just a test artefact).
    """
    if not os.path.isfile(path):
        return False, "no WAV file (did QEMU accept -audiodev/-device?)"
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return False, "not a RIFF/WAVE (size=%d)" % len(data)

    # Walk the chunks.  `fmt ` has a real size; `data` may still be 0.
    i, data_at, declared = 12, None, 0
    while i + 8 <= len(data):
        cid = data[i:i + 4]
        sz = struct.unpack_from("<I", data, i + 4)[0]
        if cid == b"data":
            data_at, declared = i + 8, sz
            break
        if sz == 0 or i + 8 + sz > len(data):
            break
        i += 8 + sz + (sz & 1)
    if data_at is None:
        return False, "no data chunk marker"

    avail = len(data) - data_at
    n = declared if 0 < declared <= avail else avail
    if n < 1024:
        return False, "data payload too small (%d bytes)" % n
    payload = data[data_at:data_at + n]

    # Finalise the header in place (RIFF size @4, data size @40).
    with open(path, "r+b") as fh:
        fh.seek(4)
        fh.write(struct.pack("<I", n + 36))
        fh.seek(40)
        fh.write(struct.pack("<I", n))

    distinct = len(set(payload))
    lo, hi = min(payload), max(payload)
    # Silence is all 0x00 (s16) or all 0x80 (u8): one distinct byte and a
    # near-zero range.  A real tone spans the amplitude range.
    ok = distinct >= 8 and (hi - lo) >= 16
    rate = struct.unpack_from("<I", data, 24)[0] if len(data) >= 28 else 0
    channels = struct.unpack_from("<H", data, 22)[0] if len(data) >= 24 else 0
    bits = struct.unpack_from("<H", data, 34)[0] if len(data) >= 36 else 0

    # Cross-check with the standard library: proves the patched header is
    # a real WAV, not merely bytes we decided to accept.
    import wave
    try:
        with wave.open(path, "rb") as w:
            frames = w.getnframes()
    except Exception as e:
        return False, "patched header invalid: %s" % e

    note = ("data=%d bytes frames=%d %dHz/%dch/%dbit distinct=%d "
            "min=%d max=%d (header %s)"
            % (n, frames, rate, channels, bits, distinct, lo, hi,
               "finalized by QEMU" if declared else "patched by driver"))
    return ok, note


def wait_for_text(s, needles, timeout=25.0):
    """Poll the VGA text buffer until every needle is on screen."""
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
    wav = win_path(os.path.join(SHOT_DIR, "snd.wav"))
    if os.path.exists(wav):
        os.remove(wav)                 # old capture would mask a failure

    disk = win_path(os.path.join(HERE, "qemu_disk_snd.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = _qemu_boot(qemu, iso, MONITOR_PORT, disk=disk,
                         extra_args=audio_args(wav))
    results = []

    def step(name, fn):
        try:
            ok, note = fn()
        except Exception as e:
            ok, note = False, "ERR %s" % e
        results.append((name, ok))
        print("%-4s %-18s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 1.0)

    try:
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        # The FAT volume has to be formatted before /bin gets the built-in
        # apps (including sndtest.ce).  A fresh 64 MiB image is blank.
        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3

        def step_bin():
            clear()
            do("ls /bin", 2.5)
            txt = screen_text(s).lower()
            return ("sndtest.ce" in txt,
                    "sndtest.ce=%s" % ("sndtest.ce" in txt))

        def step_info():
            clear()
            do("sndinfo", 2.0)
            ok, txt = wait_for_text(s, ["SB16 ready"], 12.0)
            return (ok, txt[-160:] if not ok else "DSP/IRQ/DMA readback ok")

        def step_tone():
            clear()
            do("sndtest 440 800", 2.0)
            ok, txt = wait_for_text(s, ["SNDTEST PASS"], 20.0)
            return (ok, txt[-200:])

        def step_ce():
            clear()
            do("run /bin/sndtest.ce", 2.0)
            ok, txt = wait_for_text(s, ["SND CE PASS"], 25.0)
            return (ok, txt[-200:])

        step("format", lambda: (True, "FAT32 volume ready"))
        step("bin-has-sndtest", step_bin)
        step("sndinfo", step_info)
        step("sndtest-tone", step_tone)
        step("ring3-sndtest", step_ce)

        # A graceful `quit` lets QEMU close the audio voice (and, on some
        # builds, finalise the WAV header itself).  wav_check() does not
        # depend on that -- it patches the sizes if QEMU did not.
        try:
            s.sendall(b"quit\n")
        except Exception:
            pass
        try:
            proc.wait(timeout=8)
        except Exception:
            pass
    finally:
        kill_qemu(proc)

    step("wav-non-silent", lambda: wav_check(wav))

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    if "--keep" in args:
        print("capture kept at %s" % wav)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
