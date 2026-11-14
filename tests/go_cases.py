#!/usr/bin/env python3
"""go_cases.py -- run the Go subset interpreter against the whole .GO corpus
under the CE simulator.

This is the simulator-side twin of go_regress.py.  go_regress.py drives
the real machine over the QEMU HMP monitor and proves the interpreter
works end to end (boot -> inject -> run -> read the VGA text buffer);
this driver proves the same corpus runs without a machine, which is
what makes it cheap enough to run after every change to the interpreter
or to ce_sim's instruction model.

Every case is run as the interpreter's argv[1], because that is how a
user invokes it ("run /bin/go.ce /BIN/TEST.GO").

Expected results:
  TEST.GO     rc=0, must print "ALL PASS"  -- the self-checking corpus
  SYN.GO      rc=1, must report a syntax error (a deliberate error case)
  DEMO.GO     rc=0
  MINI_GO.GO  rc=0
  MINI_IN.GO  rc=0

usage:  python tests/go_cases.py [--verbose]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import ce_sim  # noqa: E402

GO_IMAGE = os.path.join(REPO, "kernel_v25", "extapp", "bin", "go.ce")
CASEDIR = os.path.join(REPO, "kernel_v25", "extapp", "cases")

# (case file, path as seen by the guest, expected rc, required substring)
CASES = [
    ("TEST.GO", "/BIN/TEST.GO", 0, "ALL PASS"),
    ("SYN.GO", "/BIN/SYN.GO", 1, "syntax error at line 3"),
    ("DEMO.GO", "/BIN/DEMO.GO", 0, None),
    ("MINI_GO.GO", "/BIN/MINI_GO.GO", 0, None),
    ("MINI_IN.GO", "/BIN/MINI_IN.GO", 0, None),
]

MAX_STEPS = 50_000_000


def run_case(host, vpath):
    """Run one .GO case; returns (rc, output_text, steps, error_or_None)."""
    blob = open(os.path.join(CASEDIR, host), "rb").read()
    image, bss, api_off, entry = ce_sim.load_ce(GO_IMAGE)
    m = ce_sim.Machine(image, bss, api_off, {vpath.upper(): blob},
                        max_steps=MAX_STEPS)
    m.entry = entry
    m.progname = "GO.CE"
    m.install_api()
    try:
        m.run_program([vpath])
    except SystemExit as exc:
        rc = exc.args[0] if exc.args else 0
        return rc, "\n".join(m.out), m.steps, None
    except Exception as exc:                # noqa: BLE001
        return None, "\n".join(m.out), m.steps, exc
    return m.regs["eax"], "\n".join(m.out), m.steps, None


def main():
    verbose = "--verbose" in sys.argv or "-v" in sys.argv
    if not os.path.isfile(GO_IMAGE):
        print("missing %s -- build the CE apps first" % GO_IMAGE)
        return 2

    failures = []
    for host, vpath, want_rc, want_text in CASES:
        if not os.path.isfile(os.path.join(CASEDIR, host)):
            print("SKIP %-12s (no case file)" % host)
            continue
        rc, out, steps, err = run_case(host, vpath)
        problems = []
        if err is not None:
            problems.append("crashed: %s: %s" % (type(err).__name__, err))
        elif rc != want_rc:
            problems.append("rc=%r want %r" % (rc, want_rc))
        if want_text:
            # The interpreter prints through a character-at-a-time API, so
            # a message like "syntax error at line 3" arrives split across
            # several lines.  Collapse whitespace before matching, the same
            # way the on-metal driver matches against the VGA text buffer.
            flat = re.sub(r"\s+", " ", out)
            if want_text not in flat:
                problems.append("missing %r" % want_text)
        status = "PASS" if not problems else "FAIL"
        print("%-4s %-12s rc=%-4s steps=%-8d %s"
              % (status, host, rc, steps,
                 "; ".join(problems) if problems else ""))
        if problems:
            failures.append((host, problems))
        if verbose or problems:
            for line in out.splitlines():
                print("     | %s" % line)

    print("\ncases=%d ok=%d fail=%d"
          % (len(CASES), len(CASES) - len(failures), len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
