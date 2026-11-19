"""Live QEMU probe: CSO1 dlopen/dlsym/dlclose works at runtime (v2.47).

Boots the ISO, switches from the default gfx console to text (needs a
physical Y), formats a fresh disk so /bin is populated with the built-in
apps AND /bin/libmath.so, then runs `dltest` and checks the full output.

The test proves:
  * the new append-only ABI slots (66..68) dispatch through the vsyscall
    thunks with correct argument counts,
  * dlopen parses a CSO1 header and maps a fixed-base image,
  * dlsym resolves six named exports to callable addresses,
  * a library function can call back into the OS via the injected __ce_api,
  * unknown names / bad handles fail safely (NULL),
  * dlclose unmaps cleanly.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, cmd, sendkey,
                         type_text, wait_for_prompt, screen_text)

ISO = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
SHOTS = os.path.join(HERE, "qemu_shots_dl")
PORT = 4497
os.makedirs(SHOTS, exist_ok=True)

disk = os.path.join(SHOTS, "disk.img")
if not os.path.exists(disk):
    open(disk, "wb").truncate(64 * 1024 * 1024)

p, s = boot(find_qemu(), ISO, PORT, disk=disk,
            log=os.path.join(SHOTS, "qemu.log"))
try:
    time.sleep(9)                       # GRUB + kernel init in gfx mode
    type_text(s, "text"); sendkey(s, "ret"); time.sleep(0.6)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(1.5)
    if not wait_for_prompt(s, 30):
        raise RuntimeError("no shell prompt after text switch")

    type_text(s, "format"); sendkey(s, "ret"); time.sleep(0.4)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(5)

    # dltest itself is the real test: it dlopen()s /bin/libmath.so, which
    # only succeeds if the built-in installer embedded both artifacts.

    type_text(s, "dltest"); sendkey(s, "ret"); time.sleep(1.5)
    out = screen_text(s)
    print("=== dltest output ===\n" + out[-2400:])
    assert "dltest: CSO1 dynamic-link demo" in out, "dltest did not start"
    assert "loaded libmath.so" in out, "dlopen failed"
    assert "libmath 1.0 (CSO1 shared object)" in out, "dlsym(version) failed"
    assert "[libmath] hello from a shared object" in out, "library API callback failed"
    assert "7+5=12" in out, "add() wrong result"
    assert "7-5=2" in out, "sub() wrong result"
    assert "7*5=35" in out, "mul() wrong result"
    assert "gcd(84,36)=12" in out, "gcd() wrong result"
    assert "WARNING" not in out, "safe-failure check reported a problem"
    assert "dlclose -> 0 (0=ok)" in out, "dlclose failed"
    assert "dltest: PASS" in out, "dltest did not report PASS"

    # A second run proves the module slot is released by dlclose/task exit
    # and that a fresh task can load the same library again.
    type_text(s, "dltest"); sendkey(s, "ret"); time.sleep(1.5)
    out2 = screen_text(s)
    assert "dltest: PASS" in out2, "second dltest run failed (module leak?)"
    print("DYNAMIC LINK LIVE PROBE PASS (two clean cycles)")
finally:
    kill_qemu(p)
