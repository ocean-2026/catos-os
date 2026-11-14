#!/usr/bin/env python3
"""qemu_setup.py -- install QEMU (from a prepared installer) and run the
kernel regression, so the on-machine test is one command end to end.

usage:
  python tests/qemu_setup.py --check          # report state only
  python tests/qemu_setup.py                  # install if needed + regress

The installer is the official qemu-w64-setup-*.exe (NSIS).  This script
looks for it at %TEMP%/qemu-setup.exe (where the download attempts left
it) and installs silently with /S.  If the shell lacks admin rights the
install will fail -- run this script from an elevated prompt once.
"""
import os, subprocess, sys, glob

TEMP    = os.environ.get("TEMP", r"C:\Users\Admin\AppData\Local\Temp")
INSTALL = os.path.join(TEMP, "qemu-setup.exe")
CANDIDATE_DIRS = [
    r"C:\Program Files\qemu",
    r"C:\Program Files\QEMU",
]

def find_qemu():
    for d in CANDIDATE_DIRS:
        p = os.path.join(d, "qemu-system-i386.exe")
        if os.path.isfile(p):
            return p
        import shutil
        which = shutil.which("qemu-system-i386") or shutil.which("qemu-system-i386.exe")
        if which:
            return which
        return None

def installer_ready():
    """server Content-Length for qemu-w64-setup-20260811.exe"""
    return os.path.isfile(INSTALL) and os.path.getsize(INSTALL) == 206615928

def install():
    if not installer_ready():
        print("installer not complete: %s (%s bytes, need 206615928)"
              % (INSTALL, os.path.getsize(INSTALL) if os.path.isfile(INSTALL) else 0))
        print("finish the download first, e.g.:")
        print("  curl -L -C - -o %s https://qemu.weilnetz.de/w64/qemu-w64-setup-20260811.exe"
              % INSTALL)
        return False
    print("installing silently (needs admin) ...")
    r = subprocess.run([INSTALL, "/S"], timeout=600)
    if r.returncode != 0 or not find_qemu():
        print("silent install failed (rc=%s).  Run %s manually as admin."
              % (r.returncode, INSTALL))
        return False
    print("installed:", find_qemu())
    return True

def main():
    if "--check" in sys.argv:
        q = find_qemu()
        print("qemu-system-i386 :", q or "NOT INSTALLED")
        print("installer        :", INSTALL,
              "complete" if installer_ready() else
              ("%d bytes (incomplete)" % os.path.getsize(INSTALL) if os.path.isfile(INSTALL) else "absent"))
        return 0
    if not find_qemu() and not install():
        return 2
    here   = os.path.dirname(os.path.abspath(__file__))
    iso    = os.path.join(here, "..", "kernel_v25", "catos.iso")
    os.environ["PATH"] = os.path.dirname(find_qemu()) + os.pathsep + os.environ["PATH"]
    r = subprocess.run([sys.executable, os.path.join(here, "qemu_regress.py"), iso])
    return r.returncode

if __name__ == "__main__":
    sys.exit(main())
