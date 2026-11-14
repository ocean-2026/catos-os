"""Read g_fault_last from the halted guest right after A10 (ATKGFX.CE).

Why: V-03's exception vector is the single fact that decides whether the
`be_vbe24` clip() overflow is the *only* root cause (see
tests/isolation_logs/SEV-FRAMEWORK.md 8.4.2).  A10 halts the kernel
(cli + for(;;) hlt) and, because the console was switched to 800x600x24,
the text screen is blank -- so the screen tells us nothing.  But
fault_record() wrote the record into .bss *before* halting, and HMP `xp`
still reads physical memory after halt.

SELF-CHECK: five fields must all match, otherwise the address is wrong and
the intno MUST NOT be trusted:
    [0]==1        valid
    [1] in 0..31  vector        <-- the answer
    [2]==0        ring0
    [9] ~= eip    cross-check against `info registers`
    [10] == cr2   cross-check against `info registers`

usage: python tests/a10_intno.py [--keep-disk]
"""
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_driver as q

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The fault only exists in the PRE-FIX kernel: 65b326f6 carries Hunk 5 (clip()
# int64) and no longer faults, so reading g_fault_last there yields all-zero.
# Use --baseline to run the frozen baseline ISO (4dc87877).
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
if "--baseline" in sys.argv:
    ISO = os.path.join(ROOT, "tests", "baseline-catos.iso")
# pristine source: the attacks' CEs are NOT baked into it (tests/attack_disk.img
# only carries whichever ones red team injected last), so inject our own copy.
SRC = os.path.join(ROOT, "kernel_v25", "catos_disk.img")
CE = os.path.join(ROOT, "kernel_v25", "extapp", "bin", "atk_gfx.ce")
INJECT = os.path.join(ROOT, "tests", "inject_any.py")
DISK = os.path.join(ROOT, "tests", "a10_intno_disk.img")
PORT = 4494                      # this script's only port
# _g_fault_last: .bss layout unchanged between 4dc87877 and 65b326f6,
# confirmed by code-reviewer with `objdump -t build/kernel.pe`.
ADDR = "0x15fb780"
KEEP = "--keep-disk" in sys.argv

FIELDS = ["valid", "intno", "ring", "tid", "task0", "task1", "task2", "task3",
          "err", "eip", "cr2", "eflags", "ticks"]


def hexword(s):
    m = re.findall(r"0x([0-9a-fA-F]{8})", s)
    return [int(x, 16) for x in m]


def reg_value(regs, name):
    m = re.search(name + r"=([0-9a-fA-F]{8})", regs)
    return int(m.group(1), 16) if m else None


def main():
    if not os.path.exists(SRC):
        print("missing source disk: %s" % SRC)
        return 2
    if os.path.exists(DISK):
        os.remove(DISK)
    shutil.copyfile(SRC, DISK)        # SRC itself is never written
    # inject the attack CE into OUR copy (same tool red team uses)
    import subprocess
    r = subprocess.run([sys.executable, INJECT, DISK, CE, "ATKGFX.CE"],
                       capture_output=True)
    if r.returncode != 0:
        print("inject failed rc=%d: %s" % (r.returncode,
              (r.stdout + r.stderr).decode("utf-8", "replace")[-400:]))
        return 2
    proc = s = None
    rc = 1
    try:
        proc, s = q.boot(q.find_qemu(), ISO, PORT, disk=DISK)
        q.wait_for_prompt(s, timeout=90)
        # diagnostic: what does /bin actually hold in this image?
        q.type_text(s, "ls /bin")
        q.sendkey(s, "ret", 2.0)
        print("--- screen: ls /bin ---\n%s" % q.screen_text(s).strip()[-1200:])
        # type_text() only types characters -- the shell needs an explicit
        # Return before it runs anything (crash_attack.py does the same).
        q.type_text(s, "run /bin/ATKGFX.CE")
        q.sendkey(s, "ret", 1.0)
        time.sleep(2.0)
        print("--- screen right after run ---\n%s"
              % q.screen_text(s).strip()[-700:])
        regs = ""
        for _ in range(30):           # attack switches to graphics mode:
            time.sleep(2)             # text screen goes blank, poll registers
            regs = q.cmd(s, "info registers", wait=0.7)
            if "HLT=1" in regs:
                break
        halted = "HLT=1" in regs
        print("halted        : %s" % halted)
        eip_reg = reg_value(regs, "EIP")
        cr2_reg = reg_value(regs, "CR2")
        iflag = "IF=False" in regs or "IF=0" in regs
        print("EIP=%s CR2=%s IF_disabled=%s" % (
            "0x%08x" % eip_reg if eip_reg is not None else "?",
            "0x%08x" % cr2_reg if cr2_reg is not None else "?", iflag))

        raw = q.cmd(s, "xp /13xw %s" % ADDR, wait=0.8)
        print("\n--- xp /13xw %s ---\n%s" % (ADDR, raw.strip()))
        words = hexword(raw)
        if len(words) < 13:
            print("\nRESULT: unreadable (got %d words) -- address likely wrong"
                  % len(words))
            return 1
        rec = dict(zip(FIELDS, words[:13]))
        print("\n--- decoded ---")
        for k in FIELDS:
            print("  %-8s = 0x%08x (%d)" % (k, rec[k], rec[k]))

        checks = [
            ("[0] valid == 1", rec["valid"] == 1),
            ("[1] intno in 0..31", 0 <= rec["intno"] <= 31),
            ("[2] ring == 0", rec["ring"] == 0),
            # NOTE: [9] is the *faulting instruction* address, NOT the address
            # the CPU halted at (0x0101cd61 = hlt loop).  Comparing them was a
            # wrong expectation in the first version of this check.
            ("[9] eip is in kernel text (0x0100_0000..0x010f_ffff)",
             0x01000000 <= rec["eip"] <= 0x010fffff),
            ("[9] eip != halt-loop address",
             rec["eip"] != (eip_reg or 0)),
            ("[10] cr2 matches info registers",
             cr2_reg is not None and rec["cr2"] == cr2_reg),
            ("task name decodes to the attacker",
             "ATK" in "".join(chr((rec["task%d" % i] >> (8 * b)) & 0xff)
                              for i in range(4) for b in range(4)
                              if 32 <= (rec["task%d" % i] >> (8 * b)) & 0xff < 127)),
        ]
        print("\n--- self-check (all must pass before trusting intno) ---")
        ok = True
        for label, good in checks:
            print("  [%s] %s" % ("PASS" if good else "FAIL", label))
            ok = ok and good
        if ok:
            print("\nRESULT: intno = %d (0x%x), err = 0x%x, ring = %d"
                  % (rec["intno"], rec["intno"], rec["err"], rec["ring"]))
            print("VERDICT: %s" % (
                "#PF (vector 14)" if rec["intno"] == 14 else
                "#GP (13) -- clip() store is NOT the faulting instruction"
                if rec["intno"] == 13 else
                "#UD (6) -- suspect ud2 from __builtin_unreachable()"
                if rec["intno"] == 6 else
                "vector %d -- needs a new hypothesis" % rec["intno"]))
            rc = 0
        else:
            print("\nRESULT: address wrong, intno NOT to be trusted")
        return rc
    finally:
        try:
            if s:
                q.cmd(s, "quit", wait=0.5)
        except Exception:
            pass
        if proc is not None:
            q.kill_qemu(proc)
        if not KEEP and os.path.exists(DISK):
            os.remove(DISK)


if __name__ == "__main__":
    sys.exit(main())
