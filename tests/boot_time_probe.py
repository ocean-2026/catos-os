#!/usr/bin/env python3
"""boot_time_probe.py -- boot-to-prompt wall-clock probe for CatOS.

Boots the ISO under the same monitor/PS/2 conventions as qemu_regress.py,
adds `-serial file:<log>` so kernel COM1 stage markers are captured, polls
the frame buffer for the shell prompt, and reports per-run wall time.

usage:
  python3 tests/boot_time_probe.py <catos.iso> [--qemu PATH] [--port N]
          [--disk IMG] [--runs N] [--serial FILE] [--timeout S]
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_driver import kill_qemu, read_screen  # noqa: E402

PROMPT = re.compile(r"catos:[^\n]*[\$#]")


def stage_image(path):
    if not path or not os.path.isfile(path):
        return None
    stage = os.path.join(tempfile.gettempdir(),
                         "catos_bootprobe_%d" % os.getpid())
    os.makedirs(stage, exist_ok=True)
    dst = os.path.join(stage, os.path.basename(path))
    subprocess.run(["cp", "-f", path, dst], check=True)
    return dst


def boot(qemu, iso, port, disk, serial, timeout):
    iso = stage_image(iso)
    disk = stage_image(disk)
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-serial", "file:" + serial]
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % disk]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    sock = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            kill_qemu(proc)
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=3)
            break
        except OSError:
            time.sleep(0.5)
    if sock is None:
        kill_qemu(proc)
        raise RuntimeError("no monitor on port %d" % port)
    return proc, sock


def wait_prompt(s, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if PROMPT.search(read_screen(s)):
                return True
        except Exception:
            pass
        time.sleep(0.25)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--qemu", default="/home/user/bin/qemu-system-i386")
    ap.add_argument("--port", type=int, default=4444)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--serial", default="/tmp/catos_boot_serial.log")
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    times = []
    for i in range(args.runs):
        serial = args.serial if args.runs == 1 else \
            "%s.%d" % (args.serial, i)
        if os.path.exists(serial):
            os.remove(serial)
        t0 = time.time()
        proc, sock = boot(args.qemu, args.iso, args.port + i, args.disk,
                          serial, args.timeout)

        # poll the serial file for kernel [btime] markers, recording the
        # wall-clock arrival of each line (pre-kernel gap = first arrival)
        marks = []
        prompt_at = None
        deadline = time.time() + args.timeout
        seen = set()
        while time.time() < deadline:
            if os.path.exists(serial):
                try:
                    with open(serial, "r", errors="replace") as f:
                        content = f.read()
                    for line in content.splitlines():
                        if line.startswith("[btime]") and line not in seen:
                            seen.add(line)
                            marks.append((time.time() - t0, line))
                except OSError:
                    pass
            try:
                if PROMPT.search(read_screen(sock)):
                    prompt_at = time.time() - t0
                    break
            except Exception:
                pass
            time.sleep(0.1)

        if prompt_at is None:
            kill_qemu(proc)
            print("run %d: TIMEOUT" % i)
            continue
        times.append(prompt_at)
        print("run %d: prompt in %.0f ms" % (i, prompt_at * 1000))
        if marks:
            first = marks[0][0]
            last = marks[-1][0]
            print("  pre-kernel (BIOS+GRUB+ISO load): %.0f ms" %
                  (first * 1000))
            print("  kernel markers: %.0f ms, post-last-marker: %.0f ms" %
                  ((last - first) * 1000, (prompt_at - last) * 1000))
            print("  stage table (kernel ms / wall arrival ms):")
            prev_ms = None
            for wall, line in marks:
                # [btime] <ms>ms <tag>
                parts = line.split(" ", 2)
                kms = int(parts[1][:-2]) if len(parts) > 1 else 0
                delta = "" if prev_ms is None else \
                    "  (+%dms)" % (kms - prev_ms)
                prev_ms = kms
                print("    %7.0f ms  %4d ms%s  %s" %
                      (wall * 1000, kms, delta, parts[2] if len(parts) > 2 else ""))
        kill_qemu(proc)
        time.sleep(0.5)

    if times:
        print("----")
        print("runs=%d  min=%.0f ms  avg=%.0f ms  max=%.0f ms" %
              (len(times), min(times) * 1000,
               sum(times) / len(times) * 1000, max(times) * 1000))
    return 0 if times else 1


if __name__ == "__main__":
    sys.exit(main())
