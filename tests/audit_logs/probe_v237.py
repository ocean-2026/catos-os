import os, sys, time, socket
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from qemu_driver import boot, kill_qemu, wait_for_prompt, type_text, sendkey, cmd

QEMU = "/home/user/bin/qemu-system-i386"
ISO = "/home/user/Doubao/chats/38453665424926722/CatOs/kernel_v25/catos.iso"
OUT = "/home/user/Doubao/chats/38453665424926722/CatOs/tests/audit_logs"
PORT = 4573

proc, s = boot(QEMU, ISO, PORT, extra_args=["-vga", "std"])
try:
    if not wait_for_prompt(s):
        print("FAIL prompt"); sys.exit(1)
    print("PASS prompt")
    type_text(s, "help\n", 0.8)
    time.sleep(1.0)
    cmd(s, "screendump %s/help_v237.ppm" % OUT, 1.0)
    type_text(s, "gfx24\n", 0.6)
    time.sleep(3.0)
    cmd(s, "screendump %s/demo_v237.ppm" % OUT, 1.0)
    print("dumped")
finally:
    kill_qemu(proc)
