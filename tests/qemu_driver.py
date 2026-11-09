#!/usr/bin/env python3
"""qemu_driver.py -- shared, PID-safe QEMU monitor driver for the probe
scripts in tests/.

Why this module exists
----------------------
The first generation of probe scripts cleaned up before booting with

    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-i386.exe"])

That kills EVERY qemu-system-i386 on the machine, including guests owned
by other drivers running concurrently.  When two drivers overlap, each
one tears down the other's guest and the survivor sees its HMP monitor
socket reset (WinError 10054), which looks like a random, flaky failure
("same script, 4/7 one run and 2/7 the next").

On a shared machine the only correct cleanup is BY PID: kill exactly the
process this script started.  That is what `kill_qemu(proc)` does.

Conventions used by every driver built on this module
-----------------------------------------------------
* `boot()` returns `(proc, sock)` -- keep the proc and hand it back to
  `kill_qemu(proc)` so only our own guest dies.
* Each driver picks its OWN monitor port (never share 4444 with another
  driver running at the same time).
* The CatOS shell's screen-clear command is `clear`, NOT `cls`.  `cls`
  is an unknown command: it prints "unknown command: cls" and leaves
  the previous screen contents in place, so any assertion that greps
  the screen can match stale text from a previous command.
* The CatOS shell has NO `cat` command -- use `ls /bin` to check for the
  presence of a file.

usage:
    from qemu_driver import boot, kill_qemu, cmd, sendkey, type_text, \
        read_screen, screen_text, clear_screen
"""
import os
import re
import socket
import subprocess
import time

VGA_TEXT = 0xB8000
VGA_COLS = 80
VGA_ROWS = 25

DEFAULT_QEMU_CANDIDATES = [
    r"C:\qemu-portable\qemu-system-i386.exe",
    r"C:\Program Files\qemu\qemu-system-i386.exe",
    r"C:\Program Files\QEMU\qemu-system-i386.exe",
    "qemu-system-i386",
]


def find_qemu(explicit=None):
    """Locate the qemu-system-i386 binary."""
    if explicit:
        return explicit
    for c in DEFAULT_QEMU_CANDIDATES:
        if os.path.isfile(c):
            return c
    for d in (os.environ.get("PATH") or "").split(os.pathsep):
        p = os.path.join(d, "qemu-system-i386.exe")
        if os.path.isfile(p):
            return p
    return None


def kill_qemu(proc=None):
    """Stop the QEMU instance THIS script started.

    `taskkill /IM qemu-system-i386.exe` kills every QEMU on the machine,
    including the ones other drivers run concurrently.  Two unrelated
    drivers then tear each other's guest down (the survivor sees the
    monitor socket reset with WinError 10054).  Kill by PID instead.
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


def boot(qemu, iso, port, disk=None, extra_args=None, log=None,
         monitor_wait=40):
    """Start one QEMU guest and connect to its HMP monitor.

    Returns `(proc, sock)`.  The caller MUST keep `proc` and pass it to
    `kill_qemu(proc)` so that no other guest is disturbed.
    """
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port]
    if extra_args:
        cmdline += list(extra_args)
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % disk]
    if log:
        cmdline += ["-D", log]

    out = subprocess.DEVNULL
    err = subprocess.DEVNULL
    proc = subprocess.Popen(cmdline, stdout=out, stderr=err)
    for _ in range(monitor_wait):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port),
                                                   timeout=5)
        except OSError:
            time.sleep(2)
    kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def cmd(s, c, wait=0.5):
    """Send one HMP command and return everything it printed."""
    try:
        s.sendall((c + "\n").encode())
    except Exception as e:
        return "<<send failed: %s>>" % type(e).__name__
    time.sleep(wait)
    s.setblocking(False)
    time.sleep(0.15)
    chunks = []
    try:
        while True:
            try:
                b = s.recv(65536)
            except BlockingIOError:
                break
            if not b:
                break
            chunks.append(b)
    finally:
        s.setblocking(True)
    return b"".join(chunks).decode("latin-1", "replace")


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


def read_screen(s):
    """Read the VGA text frame buffer through the monitor's xp dump.

    The kernel's text back-end writes 0xB8000 in 80x25 cells of
    (char, attribute).  `xp /N 0xADDR` prints one line per 16 bytes,
    each line shaped

        000b8000: 0x0b200b20 0x0b5f0b20 0x0b5f0b5f 0x0b5f0b5f

    i.e. FOUR 32-bit words per line, each word holding FOUR bytes in
    LITTLE-ENDIAN order (low address first).  The monitor also echoes
    the command back into the socket, mangled ("xxpxp xp /4000 0xb8000"),
    so only lines starting with an 8-hex-digit address are memory dumps.

    A 2-hex-digit regex would capture fragments such as "0x0b" out of
    "0x0b200b20" and scramble every character -- hence the explicit
    8-digit match plus little-endian unpacking.
    """
    nbytes = VGA_COLS * VGA_ROWS * 2
    out = cmd(s, "xp /%d 0x%x" % (nbytes, VGA_TEXT), wait=2.0)
    data = bytearray()
    for line in out.splitlines():
        line = line.strip()
        if not re.match(r"^[0-9a-fA-F]{8}:", line):
            continue                      # monitor echo / prompt noise
        for word in re.findall(r"0x([0-9a-fA-F]{8})", line):
            v = int(word, 16)
            # little-endian: the lowest byte is the lowest address
            data += bytes((v & 0xFF, (v >> 8) & 0xFF,
                           (v >> 16) & 0xFF, (v >> 24) & 0xFF))
    if not data:
        return ""
    rows = []
    for r in range(VGA_ROWS):
        chars = []
        base = r * VGA_COLS * 2
        for c in range(VGA_COLS):
            off = base + c * 2
            if off < len(data):
                chars.append(chr(data[off]) if 32 <= data[off] < 127 else " ")
        rows.append("".join(chars).rstrip())
    return "\n".join(rows)


def screen_text(s):
    """The screen as one blob with whitespace collapsed, for substring
    assertions that must survive line wrapping."""
    return re.sub(r"\s+", " ", read_screen(s))


def clear_screen(s, wait=1.0):
    """Blank the screen.

    The CatOS shell's clear command is `clear`.  `cls` is NOT a command:
    it prints "unknown command: cls" and leaves the previous screen
    contents in place, so a later assertion can match stale text.
    """
    type_text(s, "clear")
    sendkey(s, "ret", wait)


def wait_for_prompt(s, timeout=90.0, poll=2.0):
    """Wait until the CatOS shell prompt is on screen; return True/False.

    A fixed sleep is not enough: the kernel boots through GRUB and then
    installs the built-in apps, and a command typed before the shell is
    listening is silently dropped.  That is not a harmless race -- the
    dropped `format` command leaves the disk image completely blank, and
    the next step then fails with a confusing "no such file" instead of
    "the guest was not ready yet".

    The match is anchored on the prompt's shape ("catos" ":" <pwd> "$")
    rather than on the bare word "catos": the GRUB boot menu also names
    the CatOS entry, so a loose match returns while the kernel is still
    booting and the first real command is dropped again.
    """
    pattern = re.compile(r"catos:[^\n]*\$")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if pattern.search(read_screen(s)):
                return True
        except Exception:
            pass                      # monitor hiccup: keep polling
        time.sleep(poll)
    return False
