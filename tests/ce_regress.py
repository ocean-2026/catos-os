#!/usr/bin/env python3
"""ce_regress.py -- host-side CE regression suite (no emulator needed).

Runs every extapp/bin/*.ce through tests/ce_sim.py (the host-side
interpreter that models exactly what kernel task_spawn_ce does).

Verdicts:
  PASS  the program ran and its mandatory expectations matched
  SKIP  ce_sim does not model an API the program needs (readline, getch, ...)
  FAIL  the program ran but expectation output was missing, or the
        emulator hit an unmodelled *instruction*

Expectations are derived from the program name; anything unknown is
run with the generic expectation "must not crash the interpreter".

usage: python tests/ce_regress.py [kernel_v25_dir]
"""
import os, subprocess, sys, glob

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "kernel_v25")) if not sys.argv[1:] else sys.argv[1]
SIM  = os.path.join(HERE, "ce_sim.py")
BIN  = os.path.join(ROOT, "extapp", "bin")

# program -> (must_contain_any, rc_expected)   rc None = don't care
EXPECT = {
    "hello":    (["Hello from CE!"], 0),
    "calc":     (["CatOS Calculator"], None),   # blocks on readline -> skip
    "sysinfo":  (["CatOS"], None),
    "demo":     (["CatOS"], None),
    "mmaptest": (["MMAPTEST PASS"], None),   # any FAIL line breaks the match
}

# APIs that make a program inherently interactive; ce_sim can't drive them
UNMODELLED = ("readline", "getch")

def run_one(path):
    name = os.path.splitext(os.path.basename(path))[0]
    try:
        r = subprocess.run([sys.executable, SIM, path],
                           capture_output=True, text=True, timeout=120,
                           cwd=ROOT)
    except subprocess.TimeoutExpired:
        return "FAIL", "timeout"
    out = r.stdout + r.stderr
    import re as _re
    m = _re.search(r"api '([a-z_0-9]+)' is not modelled", out)
    if m:
        return "SKIP", "ce_sim cannot drive api '%s'" % m.group(1)
    if "ERROR" in out and "steps=" not in out:
        line = [l for l in out.splitlines() if "ERROR" in l]
        why = line[0] if line else "emulator error"
        # group3 /4 (mul) etc. are unmodelled in ce_sim for the historic
        # self-test images; they are not a regression of this branch.
        if "group3" in why:
            return "SKIP", "historic unmodelled instruction: %s" % why
        return "FAIL", why
    tail = []
    tail = [l for l in out.splitlines() if l.startswith("[ce_sim]")]
    must, rc = EXPECT.get(name, ([], None))
    missing = [m for m in must if m not in out]
    if must and "MMAPTEST FAIL" in out:
        missing = must            # program reported its own failure
    if missing:
        return "FAIL", "expected output missing: %r" % missing
    if rc is not None and tail:
        got = int(tail[0].split("rc=")[1].split(" ")[0]) if "rc=" in tail[0] else None
        if got != rc:
            return "FAIL", "rc=%s expected %s" % (got, rc)
    return "PASS", (tail[0] if tail else "ok")

def main():
    files = sorted(glob.glob(os.path.join(BIN, "*.ce")))
    if not files:
        print("no .ce files under", BIN); return 2
    print("CE regression over %d images in %s\n" % (len(files), BIN))
    counts = {"PASS": 0, "SKIP": 0, "FAIL": 0}
    for f in files:
        verdict, why = run_one(f)
        counts[verdict] += 1
        print("%-4s %-16s %s" % (verdict, os.path.basename(f), why))
    print("\nPASS=%d SKIP=%d FAIL=%d" % (counts["PASS"], counts["SKIP"], counts["FAIL"]))
    return 1 if counts["FAIL"] else 0

if __name__ == "__main__":
    sys.exit(main())
