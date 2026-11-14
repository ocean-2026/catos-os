#!/usr/bin/env python3
"""qemu_regress.py -- end-to-end kernel regression driven through the
QEMU monitor (sendkey + screendump), following the pattern of the
historical ot/qemu_*.py and tests/node48_qemu_test.py drivers.

Coverage (each step screenshots to tests/qemu_shots/):
  0. boot reaches the shell prompt          (frame != black)
  1. help                                   (frame changes)
  2. display  -- front-end status
  3. gfx      -- graphics mode switch
  4. text Y   -- consent gate accepts a physical Y
  5. text N   -- consent gate rejects N (stays in gfx)
  6. mmaptest -- v2.10 mmap/munmap regression (PASS line expected; the
                 screenshot diff proves the program ran)
  7. hello.elf spawn attempt (ELF path; binary-compat is checked on the
     host by tools/mkelf.py self-check)
  8. vdiag    -- MTRR / scroll state (for the v2.9.1 untested marker)

The script only asserts structural facts (non-black frames, frame
changes between steps); text-level verification is done by a human
looking at the PNGs, since there is no OCR dependency here.

QEMU lifecycle: the guest is torn down BY PID, never by image name.
A global `taskkill /IM qemu-system-i386.exe` also kills the guests of
other drivers running concurrently on the same machine, which makes
both drivers fail in a random-looking way (the survivor's monitor
socket is reset with WinError 10054).

usage:
  python tests/qemu_regress.py <catos.iso> [--qemu PATH] [--keep]
"""
import os, socket, subprocess, sys, time, glob, struct, zlib

HERE    = os.path.dirname(os.path.abspath(__file__))
SHOTS   = os.path.join(HERE, "qemu_shots")
MONITOR = ("127.0.0.1", 4444)

DEFAULT_QEMU_CANDIDATES = [
    r"C:\qemu-portable\qemu-system-i386.exe",
    r"C:\Program Files\qemu\qemu-system-i386.exe",
    r"C:\Program Files\QEMU\qemu-system-i386.exe",
    "qemu-system-i386",
]

def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.isfile(explicit) else explicit
    import shutil
    which = shutil.which("qemu-system-i386") or shutil.which("qemu-system-i386.exe")
    if which:
        return which
    for c in DEFAULT_QEMU_CANDIDATES:
        if os.path.isfile(c):
            return c
    return None

def kill_qemu(proc=None):
    """Stop the QEMU instance THIS script started.

    `taskkill /IM qemu-system-i386.exe` kills every QEMU on the machine,
    including the ones other drivers run concurrently.  Two unrelated
    drivers then tear each other's guest down (the survivor sees the
    monitor socket reset with WinError 10054).
    """
    if proc is None:
        return
    try:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    except Exception:
        pass

def boot(iso, qemu, disk=None, port=None):
    from qemu_driver import _stage_image
    iso = _stage_image(iso)
    if disk:
        disk = _stage_image(disk)
    kill_qemu()                 # no-op: nothing of ours is running yet
    time.sleep(1)
    mon_port = port or MONITOR[1]
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d",
               "-display", "none",
               "-monitor", "tcp:%s:%d,server,nowait" % (MONITOR[0], mon_port)]
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % disk]
    print("boot:", " ".join(cmdline))
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(20):
        try:
            return proc, socket.create_connection((MONITOR[0], mon_port),
                                                   timeout=5)
        except OSError:
            time.sleep(2)
    kill_qemu(proc)
    raise RuntimeError("could not reach the QEMU monitor")

def cmd(s, c, wait=0.5):
    s.sendall((c + "\n").encode())
    time.sleep(wait)
    try:
        s.setblocking(False)
        time.sleep(0.1)
        s.recv(65536)
    except BlockingIOError:
        pass
    s.setblocking(True)

def sendkey(s, key, wait=0.12):
    cmd(s, "sendkey " + key, wait)

def type_text(s, text, wait=0.12):
    special = {" ": "spc", "-": "minus", "=": "equal", ".": "dot",
               "/": "slash", "\n": "ret"}
    for ch in text:
        k = special.get(ch)
        if k is None:
            k = "shift-%s" % ch.lower() if ch.isupper() else ch
        sendkey(s, k, wait)

KEYMAP_RETRY = {"y": "y", "n": "n"}

def ppm_to_png(ppm, png):
    """HMP screendump emits PPM (P6) regardless of the file extension;
    convert to a minimal PNG so the existing PNG checks (and human
    viewers) keep working.  Zero dependencies: hand-built PNG chunks."""
    with open(ppm, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError("screendump did not produce a PPM (got %r)" % data[:2])
    parts, pos = [], 2
    while len(parts) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":          # comment line
            while pos < len(data) and data[pos] not in (10, 13):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(int(data[start:pos]))
    pos += 1                                    # single ws after maxval
    w, h, _ = parts
    raw = data[pos:pos + w * h * 3]
    if len(raw) < w * h * 3:
        raise ValueError("PPM pixel data truncated")

    def chunk(typ, payload):
        c = struct.pack(">I", len(payload)) + typ + payload
        return c + struct.pack(">I", zlib.crc32(typ + payload) & 0xffffffff)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8-bit RGB
    stride = w * 3
    scan = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride]
                    for y in range(h))
    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(scan, 6)))
        f.write(chunk(b"IEND", b""))
    os.remove(ppm)

def shot(s, name):
    """Take a screenshot and return the PNG path.

    HMP screendump is asynchronous: the monitor acknowledges the command
    before the guest has finished writing the file, so the file may not
    be there yet when we look.  Wait for it instead of assuming 1.2 s was
    enough -- on a slow (or freshly created) disk image it is not, and
    the failure then looks like "no PNG" rather than "not ready".
    """
    png = os.path.join(SHOTS, name + ".png")
    ppm = os.path.join(SHOTS, name + ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 0.5)
    for _ in range(40):                 # up to ~20 s
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("screendump produced no file for %s" % name)
    ppm_to_png(ppm, png)
    return png

def png_size_and_nonblack(path):
    """minimal PNG scan: returns (w, h, nonblack_estimate)"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w = h = bpp = ctype = 0
    idat = b""
    pos = 8
    while pos + 8 <= len(data):
        ln, typ = struct.unpack_from(">I4s", data, pos)
        chunk = data[pos + 8: pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack_from(">IIBB", chunk, 0)
            if depth != 8 or ctype not in (2, 6):
                return (w, h, 0.0)      # palette/greyscale: can't judge
            bpp = 3 if ctype == 2 else 4
        elif typ == b"IDAT":
            idat += chunk
        pos += 12 + ln
    try:
        raw = zlib.decompress(idat)
    except zlib.error:
        return (w, h, 0.0)
    stride = w * bpp + 1            # +1 filter byte per scanline
    bright = 0
    total = 0
    step = max(3 * 16, 1)
    for row in range(0, len(raw) - 1, max(stride, 1)):
        line = raw[row + 1: row + stride]
        for px in range(0, len(line) - (bpp - 1), step):
            v = line[px] + line[px + 1] + line[px + 2]
            total += 1
            if v > 24:
                bright += 1
    return (w, h, bright / max(total, 1))

def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = None
    if "--qemu" in args:
        i = args.index("--qemu")
        if i + 1 < len(args):
            qemu = args.pop(i + 1)
            args.pop(i)
    # --port / --disk let two drivers run concurrently on one host
    # without fighting over the monitor port or the FAT volume image.
    port = MONITOR[1]
    if "--port" in args:
        i = args.index("--port")
        if i + 1 < len(args):
            port = int(args.pop(i + 1))
            args.pop(i)
    disk_override = None
    if "--disk" in args:
        i = args.index("--disk")
        if i + 1 < len(args):
            disk_override = args.pop(i + 1)
            args.pop(i)
    if iso is None:
        iso = os.path.join(ROOT_DIR(), "kernel_v25", "catos.iso")
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found; install QEMU first "
              "(winget install SoftwareFreedomConservacy.QEMU)")
        return 2
    os.makedirs(SHOTS, exist_ok=True)
    for old in glob.glob(os.path.join(SHOTS, "*.png")):
        os.remove(old)

    disk = os.path.abspath(disk_override or
                           os.path.join(SHOTS, "..", "qemu_disk.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)      # sparse 64 MiB IDE disk
    proc, s = boot(iso, qemu, disk, port=port)
    print("monitor connected")
    time.sleep(25)                     # boot to shell (TCG is slow; GRUB alone takes ~10s)
    results = []

    def step(name, fn):
        try:
            ok, note = fn()
            results.append((name, ok, note))
            print("%-4s %-18s %s" % ("PASS" if ok else "WARN", name, note))
        except Exception as e:
            results.append((name, False, str(e)))
            print("%-4s %-18s %s" % ("ERR ", name, e))

    def snap_and_check(name, min_bright=0.01):
        p = shot(s, name)
        info = png_size_and_nonblack(p)
        if not info:
            return False, "no PNG"
        w, h, b = info
        return b >= min_bright, "%dx%d bright=%.2f" % (w, h, b)

    prev = [None]
    def frame_changed(name):
        p = shot(s, name)
        info = png_size_and_nonblack(p)
        if not info:
            return False, "no PNG"
        with open(p, "rb") as f:
            blob = f.read()
        changed = prev[0] is not None and blob != prev[0]
        prev[0] = blob
        return changed, "size=%d" % len(blob)

    def boot_seed():
        ok, note = snap_and_check("00_boot")
        with open(os.path.join(SHOTS, "00_boot.png"), "rb") as f:
            prev[0] = f.read()          # seed: format is the first diff vs boot
        return ok, note
    step("boot-shell", boot_seed)

    # blank disk -> format the FAT32 volume so /bin has the built-ins
    # (the boot banner warns "No FAT32 volume on IDE drive" otherwise).
    # The kernel's confirm reads a full LINE (kb_readline), so the 'y'
    # MUST be followed by Enter -- a bare y would swallow the next
    # command's characters into the confirmation line.
    def do_type(cmdline):
        type_text(s, cmdline)
        sendkey(s, "ret", 1.2)
    def do(cmdline):
        type_text(s, cmdline)
        sendkey(s, "ret", 1.2)
    def do_format():
        do_type("format")
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 8.0)        # fs_format(64 MiB) + builtin install
        return frame_changed("00b_format")
    step("format", do_format)
    step("help", lambda: (do("help"), frame_changed("01_help"))[1])
    step("display", lambda: (do("display"), frame_changed("02_display"))[1])
    step("gfx", lambda: (do("gfx"), frame_changed("03_gfx"))[1])
    step("text-reject-N", lambda: (do("text"), sendkey(s, "n", 1.0),
                                   frame_changed("04_text_n"))[2])
    step("text-accept-Y", lambda: (do("text"), sendkey(s, "y", 1.0),
                                   frame_changed("05_text_y"))[2])
    step("ls-bin", lambda: (do("ls /bin"), frame_changed("05b_lsbin"))[1])
    step("mmaptest", lambda: (do("mmaptest"), frame_changed("06_mmaptest"))[1])
    step("hello-elf", lambda: (do("hello.elf"), frame_changed("07_hello_elf"))[1])
    step("vdiag", lambda: (do("vdiag"), frame_changed("08_vdiag"))[1])
    step("final-frame", lambda: snap_and_check("09_final"))

    kill_qemu(proc)              # by PID: never touch another driver's guest
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d warn=%d  shots in %s"
          % (len(results), len(results) - len(fails), len(fails), SHOTS))
    return 0 if not fails else 1

def ROOT_DIR():
    return os.path.dirname(HERE)

if __name__ == "__main__":
    sys.exit(main())
