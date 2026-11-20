"""v2.45 merge acceptance: 4 GiB RAM + CE spawn still works.

Boots the freshly built ISO with -m 3840, runs `mem`, `hello`, `mmaptest`,
screenshots each step and dumps the visible text for assertion.
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, cmd, sendkey,
                         type_text, screen_text, wait_for_prompt)

TEXT_CONSENT_NOTE = ("graphics is the boot default (v2.46), so the VGA text "
                     "buffer is blank until the console is switched to text.")

ISO = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
SHOTS = os.path.join(HERE, "qemu_shots_v245")
PORT = 4488

os.makedirs(SHOTS, exist_ok=True)
qemu = find_qemu()
proc, sock = boot(qemu, ISO, PORT,
                  disk=os.path.join(HERE, "qemu_shots_v245", "disk.img"), extra_args=["-m", "3840"],
                  log=os.path.join(SHOTS, "qemu.log"))
try:
    # v2.46 made gfx the boot default -> screen_text()/wait_for_prompt()
    # cannot see anything until we switch to text.  Keystrokes work in any
    # video mode, so do it blind and answer the required physical 'Y'.
    time.sleep(9)
    type_text(sock, "text"); sendkey(sock, "ret"); time.sleep(0.6)
    sendkey(sock, "y"); sendkey(sock, "ret"); time.sleep(1.5)
    if not wait_for_prompt(sock, timeout=90):
        raise RuntimeError("shell prompt never appeared with -m 3840")
    frames = []
    # A blank -hda is normal for this probe; format so /bin/*.ce appears.
    type_text(sock, "format"); sendkey(sock, "ret"); time.sleep(0.5)
    sendkey(sock, "y"); sendkey(sock, "ret"); time.sleep(4)
    for name, line in [("00_boot", None), ("01_mem", "mem"),
                       ("02_hello", "hello"), ("03_mmaptest", "mmaptest"),
                       ("04_hwinfo", "hwinfo")]:
        if line:
            type_text(sock, line)
            sendkey(sock, "ret")
            time.sleep(1.5)
        txt = screen_text(sock)
        frames.append(txt)
        with open(os.path.join(SHOTS, name + ".txt"), "w", encoding="utf-8") as f:
            f.write(txt)
        print("=== %s ===" % name)
        print("\n".join(txt.splitlines()[:16]))
    ppm = os.path.join(SHOTS, "final.ppm")
    cmd(sock, "screendump %s" % ppm.replace("\\", "/"), 1.0)
    print("4GB probe done; logs in", SHOTS)
finally:
    kill_qemu(proc)
