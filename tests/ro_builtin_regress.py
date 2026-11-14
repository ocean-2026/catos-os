#!/usr/bin/env python3
"""ro_builtin_regress.py -- on-metal acceptance for the read-only built-ins.

Three dedicated shell-level cases for the H8 fix (the kernel's built-in
apps must not be replaceable) and for the LFN-first name resolution it
depends on:

  TC-1  lfn-shadow   A plain 8.3 file whose name is the TRUNCATED form of
                     a built-in ("/bin/allocpro.ce" for the built-in
                     "/bin/allocprobe.ce") must not shadow the built-in:
                     `run /bin/allocprobe.ce` still has to run the
                     built-in.  Before the lookup was fixed to prefer the
                     long name, the exact 8.3 name won -- so the user's
                     file was executed instead, which is the same
                     code-replacement primitive the read-only attribute
                     was meant to close.

  TC-2  ro-builtin   No shell path may replace or delete a built-in:
                       cp    -> "cp: permission denied"
                       edit  -> "edit: ... is read-only"
                       rm    -> "rm: permission denied (read-only file)"
                     and the built-in must still run afterwards, which
                     proves the code on disk was not replaced.  The size
                     of the target is compared before and after the
                     attempts, so a refused write that still touched the
                     file (truncation, a backup copy) is caught too.

  TC-3  passwd-writable
                     The read-only attribute must NOT bleed into
                     /etc/passwd: useradd / passwd / chcaps rewrite the
                     user database, and they have to keep working.  The
                     file must also still be the real database and not
                     the 17-byte payload the perms demo used to write
                     over it.

These are the SHELL-level cases; the CE-ABI-level probes (A18 sysprot,
A19 autostart) live in tests/crash_attack.py and are complementary.

The screen is read through the QEMU monitor's physical-memory dump of the
VGA text buffer ("xp /4000 0xb8000") instead of being OCR-ed from a
screenshot -- the frame buffer holds (char, attribute) pairs, so the
character is every second byte.

The QEMU lifecycle lives in tests/qemu_driver.py.  Two rules matter on
a shared machine:
  * the guest is torn down BY PID, never by image name;
  * the shell's screen-clear command is `clear`, not `cls`.

usage:
  python tests/ro_builtin_regress.py <catos.iso> [--qemu PATH] [--keep]
"""
import os
import re
import subprocess
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_driver import (clear_screen, cmd, find_qemu, kill_qemu,  # noqa: E402
                          read_screen, screen_text, sendkey, type_text,
                          wait_for_prompt)

HERE = os.path.dirname(os.path.abspath(__file__))
MONITOR = ("127.0.0.1", 4444)

# the perms demo used to overwrite /etc/passwd with this payload
PERMS_PAYLOAD_LEN = 17


def boot(iso, qemu, disk, port=None):
    """Start the guest and connect to its monitor.

    Returns (proc, sock): the caller must keep `proc` and hand it to
    kill_qemu() so that no OTHER qemu on this machine is disturbed.
    """
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
        iso = os.path.join(os.path.dirname(HERE), "kernel_v25", "catos.iso")
    iso = os.path.abspath(iso)
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    disk = os.path.abspath(disk_override or
                           os.path.join(HERE, "qemu_disk.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = boot(iso, qemu, disk, port=port)
    print("monitor connected")
    # Wait for the shell instead of sleeping a fixed 25 s: a command
    # typed before the shell is listening is dropped, and a dropped
    # `format` leaves the disk image blank with no visible error.
    if not wait_for_prompt(s):
        print("FATAL: shell prompt never appeared")
        kill_qemu(proc)
        return 3

    results = []

    def step(name, fn):
        try:
            ok, note = fn()
            results.append((name, ok, note))
            print("%-4s %-20s %s" % ("PASS" if ok else "FAIL", name, note))
        except Exception as e:
            results.append((name, False, str(e)))
            print("%-4s %-20s %s" % ("ERR ", name, e))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        # `clear`, not `cls`: the CatOS shell has no `cls` command, so
        # using it leaves the previous screen contents in place and a
        # later assertion can match stale text.
        clear_screen(s, 1.0)

    def fsize_of(path):
        """Size the shell reports for `path`, or None if it has none.

        One short line per file, so it cannot be thrown off by the
        screen layout the way a directory listing can.
        """
        clear()
        do("fsize " + path, 1.5)
        t = screen_text(s)
        m = re.search(r"fsize:\s*(\d+)\s*bytes", t)
        return int(m.group(1)) if m else None

    def step_format():
        clear()
        do("format", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 10.0)          # fs_format(64 MiB) + install
        return True, "formatted"

    def step_lfn_shadow():
        """TC-1: a plain 8.3 file must not shadow a built-in app."""
        clear()
        # "allocprobe.ce" is stored as the alias "ALLOCPRO~1.CE" plus a
        # long-name entry on a kernel-formatted volume, so the plain 8.3
        # name "allocpro.ce" is a DIFFERENT entry and can be created.
        # (On a host-built disk image the built-in IS that 8.3 entry, so
        # the cp is refused instead -- both outcomes must leave the
        # built-in runnable.)
        do("cp /bin/hello.ce /bin/allocpro.ce", 2.0)
        clear()
        do("run /bin/allocprobe.ce", 6.0)
        txt = screen_text(s)
        ran_builtin = "ALLOCPROBE" in txt
        ran_shadow = "hello from ce!" in txt.lower()
        ok = ran_builtin and not ran_shadow
        return ok, "builtin=%s shadow=%s" % (ran_builtin, ran_shadow)

    def step_ro_builtin():
        """TC-2: no shell path may replace or delete a built-in."""
        detail = []
        ok = True

        size_before = fsize_of("/bin/hello.ce")
        detail.append("size0=%s" % size_before)

        clear()
        do("cp /bin/hello.ce /tmp/hello.copy", 2.0)
        clear()
        do("cp /tmp/hello.copy /bin/hello.ce", 2.0)
        t = screen_text(s).lower()
        cp_refused = "permission denied" in t
        detail.append("cp=%s" % cp_refused)
        ok = ok and cp_refused

        clear()
        do("edit /bin/hello.ce", 2.0)
        # if the guard is missing the editor opens and blocks the shell;
        # ^X leaves it again so the run can continue
        sendkey(s, "ctrl-x", 1.0)
        t = screen_text(s).lower()
        # the editor's title row is "CatOS nano ..."; the refusal is
        # "edit: <path> is read-only"
        ed_refused = "read-only" in t and "nano" not in t
        detail.append("edit=%s" % ed_refused)
        ok = ok and ed_refused

        clear()
        do("rm /bin/hello.ce", 2.0)
        t = screen_text(s).lower()
        rm_refused = "read-only" in t
        detail.append("rm=%s" % rm_refused)
        ok = ok and rm_refused

        clear()
        do("run /bin/hello.ce", 6.0)
        t = screen_text(s).lower()
        still_runs = "hello from ce!" in t
        detail.append("runs=%s" % still_runs)
        ok = ok and still_runs

        # a refused write that still touched the file (truncation, a
        # backup copy) would leave the size changed
        size_after = fsize_of("/bin/hello.ce")
        unchanged = (size_before is not None and size_after == size_before)
        detail.append("size=%s" % size_after)
        ok = ok and unchanged

        return ok, " ".join(detail)

    def step_passwd_writable():
        """TC-3: the user database must stay writable."""
        detail = []
        ok = True

        clear()
        do("useradd bob 102 102 0", 2.0)
        t = screen_text(s)
        added = "user added" in t
        detail.append("useradd=%s" % added)
        ok = ok and added

        clear()
        do("passwd bob secret1", 2.0)
        t = screen_text(s)
        pw = "password set" in t
        detail.append("passwd=%s" % pw)
        ok = ok and pw

        clear()
        do("chcaps 2 bob", 2.0)
        t = screen_text(s)
        caps = "capabilities updated" in t
        detail.append("chcaps=%s" % caps)
        ok = ok and caps

        clear()
        do("lsuser", 2.0)
        t = screen_text(s)
        listed = "bob" in t
        detail.append("lsuser=%s" % listed)
        ok = ok and listed

        # the real database, not the 17-byte payload the perms demo used
        # to write over /etc/passwd
        size = fsize_of("/etc/passwd")
        intact = size is not None and size > PERMS_PAYLOAD_LEN
        detail.append("passwd_size=%s" % size)
        ok = ok and intact

        return ok, " ".join(detail)

    step("format", step_format)
    step("lfn-shadow", step_lfn_shadow)
    step("ro-builtin", step_ro_builtin)
    step("passwd-writable", step_passwd_writable)

    if "--keep" not in args:
        kill_qemu(proc)
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
