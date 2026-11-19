"""tui_probe.py -- live probe: the v2.48 TUI lab apps render and exit cleanly.

Ports of the win-demo TUI designs (E:/Tools/tui_design/catos_designs/
win-demo) run inside the guest through the monitor, same pattern as
btop_probe.py:

  0. boot reaches the shell prompt (+ guest disk formatted so /bin has
     the built-ins: ctop.ce, cfm.ce, charness.ce, ctheme.ce, cmenu.ce,
     cgreet.ce are installed by the v2.48 kernel image)
  1. cmenu    -> launcher menu renders, T cycles theme, q
  2. ctop     -> task manager repaints with the live task table, P sort, q
  3. cfm      -> file explorer: Enter into a dir, Up/Up, Backspace up, q
  4. ctheme   -> theme lab: T T cycles back, q
  5. cgreet   -> greeting page (banner + session box), Enter streams, q
  6. charness -> agent session: Enter plays a step, Space side panel, q

Each step screenshots to tests/qemu_shots_tui/ and asserts the frame
changed after the keys were sent (the app repainted).  Final step: the
shell must still answer (C5 check: the whole tour left no damage).

usage:
  python tests/tui_probe.py [--qemu PATH] [--port N]
"""
import os, sys, time, glob, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
SHOTS = os.path.join(HERE, "qemu_shots_tui")

from qemu_driver import (find_qemu, boot, kill_qemu, cmd, sendkey,
                         type_text, wait_for_prompt, screen_text)  # noqa: E402

PORT_DEFAULT = 4493          # regress = 4444, crash_attack = 4492


def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    assert data[:2] == b"P6", "screendump did not produce PPM"
    parts, pos = [], 2
    while len(parts) < 3:
        while pos < len(data) and data[pos:pos+1].isspace(): pos += 1
        if data[pos:pos+1] == b"#":
            while data[pos] not in (10, 13): pos += 1
            continue
        st = pos
        while pos < len(data) and not data[pos:pos+1].isspace(): pos += 1
        parts.append(int(data[st:pos]))
    pos += 1
    w, h, _ = parts
    raw = data[pos:pos + w*h*3]
    def ck(t, p):
        return struct.pack(">I", len(p)) + t + p + struct.pack(
            ">I", zlib.crc32(t + p) & 0xffffffff)
    scan = b"".join(b"\x00" + raw[y*w*3:(y+1)*w*3] for y in range(h))
    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + ck(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + ck(b"IDAT", zlib.compress(scan, 6))
                + ck(b"IEND", b""))
    os.remove(ppm)


class Driver:
    def __init__(self, sock):
        self.s = sock
        self.prev = None

    def snap(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        png = os.path.join(SHOTS, name + ".png")
        if os.path.exists(ppm): os.remove(ppm)
        cmd(self.s, "screendump %s" % ppm.replace("\\", "/"), 0.5)
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("screendump produced no file for %s" % name)
        ppm_to_png(ppm, png)
        return png

    def changed(self, name):
        p = self.snap(name)
        blob = open(p, "rb").read()
        ok = self.prev is not None and blob != self.prev
        self.prev = blob
        return ok, len(blob)

    def run_app(self, label, keys=()):
        type_text(self.s, label + "\n")
        time.sleep(3.5)                          # first frame renders
        ok1, n1 = self.changed(label + "_a")
        for k in keys:
            sendkey(self.s, k, 0.9)
        time.sleep(1.5)
        ok2, n2 = self.changed(label + "_b") if keys else (True, 0)
        # quit the app so the next step's command reaches the shell
        sendkey(self.s, "q"); time.sleep(2.0)
        return (ok1 and ok2), "a=%d b=%d" % (n1, n2)


def main():
    args = sys.argv[1:]
    qemu = None
    if "--qemu" in args:
        i = args.index("--qemu"); qemu = args[i+1]; args.pop(i+1); args.pop(i)
    port = PORT_DEFAULT
    if "--port" in args:
        i = args.index("--port"); port = int(args[i+1]); args.pop(i+1); args.pop(i)

    iso = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found"); return 2
    os.makedirs(SHOTS, exist_ok=True)
    for old in glob.glob(os.path.join(SHOTS, "*.png")): os.remove(old)

    # sparse guest disk; boot with -hda so /bin can be formatted into place
    disk = os.path.join(HERE, "qemu_disk_tui.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as f: f.truncate(64 * 1024 * 1024)

    proc, s = boot(qemu, iso, port, disk=disk)
    print("monitor connected")
    d = Driver(s)
    # gfx is the boot default -> the VGA text buffer is blank and the TUI
    # apps demand 60x16+ cells.  Switch to text mode BLIND like
    # btop_probe.py (keystrokes work in any video mode), then wait for
    # the prompt in the text buffer.
    time.sleep(9)
    type_text(s, "text"); sendkey(s, "ret"); time.sleep(0.6)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(1.5)
    if not wait_for_prompt(s, 30):
        raise RuntimeError("no shell prompt after text switch")
    # format the guest volume so /bin has the built-in TUI apps
    type_text(s, "format"); sendkey(s, "ret"); time.sleep(0.4)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(6)
    d.prev = open(d.snap("00_boot"), "rb").read()
    print("seed shot 00_boot")

    results = []
    def step(name, fn):
        try:
            ok, note = fn()
            results.append((name, ok, note))
            print("%-4s %-12s %s" % ("PASS" if ok else "WARN", name, note))
        except Exception as e:
            results.append((name, False, str(e)))
            print("ERR  %-12s %s" % (name, e))

    step("cmenu",   lambda: d.run_app("cmenu",  keys=("t",)))
    step("ctop",    lambda: d.run_app("ctop",   keys=("p",)))
    step("cfm",     lambda: d.run_app("cfm",
        keys=("ret", "up", "up", "bksp", "tab")))
    step("ctheme",  lambda: d.run_app("ctheme", keys=("t", "t")))
    step("cgreet",  lambda: d.run_app("cgreet", keys=("ret",)))
    step("charness",lambda: d.run_app("charness", keys=("ret", "spc")))

    def shell_alive():
        # run_app() already quit each app; the shell owns the screen now.
        type_text(s, "echo tui-alive"); sendkey(s, "ret"); time.sleep(3)
        ok, n = d.changed("99_shell_alive")
        txt = screen_text(s)
        return ok and "tui-alive" in txt, "marker=%r" % ("tui-alive" in txt)

    step("shell-alive", shell_alive)

    kill_qemu(proc)
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d  shots in %s"
          % (len(results), len(results) - len(fails), len(fails), SHOTS))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
