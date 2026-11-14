#!/usr/bin/env python3
"""regression_after_fix.py -- before/after proof for the red-team crash fixes.

WHAT THIS PROVES
    The same four attack CEs are executed twice, from a cold boot each time:

      * on  --baseline <iso>  (tests/baseline-catos.iso, the unpatched kernel)
      * on  --fixed    <iso>  (the kernel built after the patches)

    and the script asserts the *transition*:

        baseline : the console does NOT answer afterwards  (C3/C5 -- the wedge)
        fixed    : the attack prints ATK-DONE and the shell still answers

    Only the pair "broken before / working after" is evidence.  A run that
    merely prints ATK-DONE on both sides proves nothing, which is why the
    baseline side is not optional.

THE FOUR CASES  (file:line references are to the unpatched kernel)
    W1  ATKWAIT.CE 1   wait(gettid())      task.c:1618-1625 + :400-404
    W2  ATKWAIT.CE 2   wait(0) = the shell  same, but via tid 0
    H1  ATKHEAP.CE     heap metadata abuse  task.c:813-871
    K1  ATKPROC.CE     kill(0) = the shell  task.c:381-398

    K1 is the one case whose *baseline* still prints ATK-DONE: the attack
    keeps running after it destroys the shell, and only the console is gone.
    That is why the pass criterion is "does the shell answer a command
    afterwards", never "did ATK-DONE appear".  The code path makes this
    unambiguous: task_kill() reaches its `return 0` (task.c:404-414) only
    after task_cleanup() + state=T_ZOMBIE + maybe_autoreap(), and the
    "[kill: tid N (name)]" line is printed *after* all of that -- so a
    baseline screen showing `kill(0)=0` plus that line is proof the shell
    slot was destroyed, regardless of what the attacker prints next.

LIVENESS PROBE
    After the attack the driver types `faultinfo` and requires
    "fault recorded" / "last fault" on screen.  A fresh guest cannot contain
    that text before the probe, so the match cannot be stale screen content
    (the trap qemu_driver.py warns about: an 80x25 VGA dump keeps old rows
    forever).  K1 additionally runs `/bin/hello.ce` from the shell.

ISOLATION
    -net none, -display none, private monitor port 4496, one fresh copy of
    the armed disk per run (never the shared attack_disk.img), and QEMU is
    killed by PID only.

    Shared large files are READ-ONLY here: baseline-catos.iso, kernel_v25/
    catos.iso and attack_disk_base.img are only ever copied *from*.  The
    arsenal image is the red team's; if it is missing this script stops
    instead of rebuilding it (see --allow-build-arsenal).

DISK SPACE
    Every run needs a 64 MiB disk copy.  The driver refuses to start when
    the volume holding tests/ has less than 200 MB free: a guest whose disk
    writes fail mid-run looks exactly like a C5 corruption result.

Usage:
    python tests/regression_after_fix.py                    # both sides
    python tests/regression_after_fix.py --side fixed       # fixed only
    python tests/regression_after_fix.py --only W1,K1
    python tests/regression_after_fix.py --baseline tests/baseline-catos.iso \
                                         --fixed kernel_v25/catos.iso
    python tests/regression_after_fix.py --allow-build-arsenal
                                         # only when attack_disk_base.img is
                                         # gone AND no red-team run is active
"""
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu_driver import (boot, find_qemu, kill_qemu, wait_for_prompt,
                         type_text, sendkey, read_screen, screen_text,
                         cmd)                                        # noqa: E402

DEFAULT_BASELINE = os.path.join(HERE, "baseline-catos.iso")
DEFAULT_FIXED = os.path.join(ROOT, "kernel_v25", "catos.iso")
DISK_RUN = os.path.join(HERE, "regress_fix_disk.img")     # our own copy
LOGDIR = os.path.join(HERE, "attack_logs")
PORT = 4496                        # 4492 = crash_attack, 4444 = regress
FREE_MIN_MB = 200
ARMED = None                       # set by main() via resolve_armed()

FATAL = ("KERNEL PANIC", "ring0 kernel fault", "*** exception:")
REBOOT = ("GNU GRUB", "Booting `CatOS", "CatOS kernel v")

CASES = [
    dict(id="W1", ce="ATKWAIT.CE", args="1", t=25,
         vector="wait(gettid()) -- self-deadlock while the shell waits for us",
         proof="WAITSELF-AFTER",
         base_expect="console wedged (attack parks on itself, shell parked on it)"),
    dict(id="W2", ce="ATKWAIT.CE", args="2", t=25,
         vector="wait(0) -- mutual deadlock with the shell",
         proof="WAIT0-AFTER",
         base_expect="console wedged"),
    dict(id="H1", ce="ATKHEAP.CE", args="", t=25,
         vector="alloc/free/brk metadata abuse (double free -> freelist cycle)",
         proof="ATK-DONE",
         base_expect="ring0 spins in sys_alloc with IF=0 (hard hang)"),
    dict(id="K1", ce="ATKPROC.CE", args="", t=25,
         vector="kill(0) -- ring3 destroys the ring0 shell task",
         proof="ATK-DONE",
         base_expect="ATK-DONE printed, then the console never returns",
         # The strongest evidence on the fixed side is NOT "the prompt came
         # back" (a wedged arena could also cost us the prompt).  It is that
         # task_kill() refused BEFORE its destructive path: task.c:414
         # returns 0 only after task_cleanup()+T_ZOMBIE+maybe_autoreap, and
         # the "[kill: tid N (name)]" print at task.c:413 comes after all of
         # them.  So on a patched kernel the attack must print kill(0)=-2
         # and the "tid 0" kill line must never appear at all.
         # (No base_require: the 80x25 screen scrolls, so on the baseline
         # side that same line can legitimately scroll out of view; there
         # the alive=False assertion is the reliable one.)
         fixed_require=["kill(0)=-2"],
         fixed_forbid=["[kill: tid 0 ("],
         extra_hello=True),
]


# ------------------------------------------------------------------ helpers
def free_mb(path):
    try:
        st = os.statvfs(path)
        return (st.f_bavail * st.f_frsize) // (1024 * 1024)
    except Exception:
        return 10 ** 6


def tail(txt, n=22):
    rows = [r for r in (txt or "").split("\n") if r.strip()]
    return "\n".join(rows[-n:])


def resolve_armed(allow_build=False):
    """Return a disk image that already carries the attack CEs.

    `attack_disk_base.img` is the RED TEAM'S ARSENAL and is read-only from
    here: if it is missing we do NOT rebuild it (crash_attack.arm_disk() is
    only idempotent when the file exists -- when it is gone it silently
    writes a 64 MiB image into the shared path, which races with whoever is
    running attacks).  Instead we build our OWN image, `regress_fix_armed.img`,
    from kernel_v25/catos_disk.img and reuse it afterwards.  Same content,
    no shared state touched.
    """
    shared = os.path.join(HERE, "attack_disk_base.img")
    if os.path.exists(shared):
        return shared                      # read-only from here on
    private = os.path.join(HERE, "regress_fix_armed.img")
    if os.path.exists(private):
        return private
    print("[arm] shared arsenal missing -> building a PRIVATE armed image")
    print("      %s" % private)
    print("      (from kernel_v25/catos_disk.img; the red team's file is "
          "never written)")
    sys.stdout.flush()
    import crash_attack as ca
    shutil.copyfile(ca.DISK_SRC, private)
    inj = os.path.join(HERE, "inject_any.py")
    for name83, fn in ca.CE_SRC.items():
        ca.sh([sys.executable, inj, private,
               os.path.join(ca.CE_BIN, fn), name83])
    return private


def wait_attack(s, p, deadline, window):
    """Poll until the attack finishes, the guest dies, or the window closes.
    Returns (state, screen, why) with state in OK/C1/C2/None."""
    last = ""
    while time.time() < deadline:
        if p.poll() is not None:
            return "C2", last, "QEMU exited (guest reset)"
        scr = read_screen(s)
        last = scr
        for f in FATAL:
            if f in scr:
                return "C1", scr, "kernel fatal string: " + f
        for f in REBOOT:
            if f in scr:
                return "C2", scr, "boot screen came back: " + f
        if "ATK-DONE" in re.sub(r"\s+", " ", scr):
            return "OK", scr, "ATK-DONE"
        time.sleep(1.0)
    return None, last, "no ATK-DONE inside %ds" % window


_TICK_RE = re.compile(r"at tick 0x([0-9a-fA-F]+)")


def probe_alive(s, elapsed=None):
    """Type a command and require its output.  A wedged guest echoes nothing
    new, and 'fault recorded' cannot be stale on a fresh guest."""
    txt = screen_text(s)
    if "fault recorded" in txt or "last fault" in txt:
        return True                      # already answered earlier
    type_text(s, "faultinfo")
    sendkey(s, "ret", 2.2)
    txt = screen_text(s)
    ok = ("fault recorded" in txt) or ("last fault" in txt)
    # SOFT sanity check on the tick the kernel reports.  intr.c:228 arms the
    # PIT at 100 Hz, so `at tick` grows by ~100 per second of *uptime*, and
    # uptime can never exceed the wall clock since we called boot().  A tick
    # far above that means we are reading a screen produced by a guest that
    # started before ours -- i.e. a previous QEMU that kill_qemu() missed.
    # Deliberately advisory only: a parse failure or an unexpected timer
    # change must not turn a valid experiment into a failure (that is how
    # you get an inconclusive report, not a safer one).
    m = _TICK_RE.search(txt)
    if ok and m and elapsed:
        tick = int(m.group(1), 16)
        limit = int(elapsed * 100 * 2) + 1000      # 2x slack on top
        if tick > limit:
            print("  ! tick 0x%x implausible for %.0fs uptime (limit 0x%x): "
                  "screen may belong to a leftover guest" % (tick, elapsed, limit))
    return ok


def run_one(qemu, iso, case, side, log):
    """Boot `iso`, run the attack, probe liveness.  Returns a record dict."""
    rec = dict(case=case["id"], side=side, iso=os.path.basename(iso),
               vector=case["vector"], state="?", alive=False, done=False,
               why="")
    shutil.copyfile(ARMED, DISK_RUN)
    t_boot = time.time()
    p, s = boot(qemu, iso, PORT, disk=DISK_RUN)
    try:
        if not wait_for_prompt(s, timeout=120):
            rec["state"], rec["why"] = "SETUP-FAIL", "no shell prompt in 120 s"
            log.write("SETUP FAILED: no prompt\n%s\n" % read_screen(s))
            return rec

        cmd(s, "clear", wait=0.8)
        before = read_screen(s)
        line = "run /bin/%s%s" % (case["ce"],
                                  (" " + case["args"]) if case["args"] else "")
        type_text(s, line)
        sendkey(s, "ret", 1.0)
        state, scr, why = wait_attack(s, p, time.time() + case["t"], case["t"])
        rec["state"] = state or "NO-MARKER"
        rec["why"] = why
        rec["done"] = "ATK-DONE" in re.sub(r"\s+", " ", scr)
        rec["proof_seen"] = case["proof"] in re.sub(r"\s+", " ", scr)
        # Per-case hard assertions on the attack's own output (see K1): the
        # fixed side must show the refusal code and must NOT show the
        # destruction line.  Literal substrings, not regex.
        flat = re.sub(r"\s+", " ", scr)
        rec["req_missing"] = [p for p in case.get("fixed_require", [])
                              if p not in flat]
        rec["forb_hit"] = [p for p in case.get("fixed_forbid", [])
                           if p in flat]

        # liveness: this is the assertion that actually matters
        if p.poll() is None:
            rec["alive"] = probe_alive(s, elapsed=time.time() - t_boot)
        if rec["alive"] and case.get("extra_hello"):
            type_text(s, "run /bin/hello.ce")
            sendkey(s, "ret", 1.0)
            time.sleep(4.0)
            rec["alive"] = "hello" in screen_text(s).lower() or rec["alive"]
        after = read_screen(s)

        log.write("=== %s / %s ===\n" % (case["id"], side))
        log.write("iso     : %s\n" % iso)
        log.write("run     : %s\n" % line)
        log.write("vector  : %s\n" % case["vector"])
        log.write("state   : %s (%s)\n" % (rec["state"], rec["why"]))
        log.write("ATK-DONE: %s   proof marker: %s\n" %
                  (rec["done"], rec.get("proof_seen")))
        if rec.get("req_missing") or rec.get("forb_hit"):
            log.write("assertions: missing=%s  forbidden-but-present=%s\n"
                      % (rec.get("req_missing"), rec.get("forb_hit")))
        log.write("shell alive after the attack: %s\n" % rec["alive"])
        log.write("\n--- screen BEFORE ---\n%s\n" % tail(before, 6))
        log.write("\n--- screen AFTER ---\n%s\n" % tail(after))
        log.write("\n")
        return rec
    finally:
        try:
            os.unlink(DISK_RUN)
        except OSError:
            pass
        kill_qemu(p)


def verdict(case, base, fixed):
    """Turn the two observations into PASS/FAIL for the pair.

    INVARIANT: a forbidden marker (case["fixed_forbid"]) may never be the
    only liveness criterion.  The fixed side is judged by four things ANDed
    together -- alive, ATK-DONE, no required marker missing, no forbidden
    marker present -- so "the kill line scrolled off" can never be mistaken
    for pass on its own.  Any future forbid string must be added on top of
    this, never instead of it.
    """
    def fixed_ok(f):
        """ATK-DONE + console alive + the per-case refusal assertions."""
        return (f["alive"] is True and f["done"] is True
                and not f.get("req_missing") and not f.get("forb_hit"))

    def fixed_why(f):
        return ("done=%s alive=%s missing=%s forbidden-present=%s"
                % (f["done"], f["alive"], f.get("req_missing"),
                   f.get("forb_hit")))

    if base is not None and fixed is None:
        return ("BASE-ONLY", base["alive"] is False,
                "baseline must wedge (alive=False); got alive=%s" % base["alive"])
    if fixed is not None and base is None:
        return ("FIXED-ONLY", fixed_ok(fixed),
                "fixed must print ATK-DONE, stay alive and satisfy the "
                "per-case assertions; got " + fixed_why(fixed))
    b_ok = base["alive"] is False
    f_ok = fixed_ok(fixed)
    if b_ok and f_ok:
        return "PASS", True, "wedged before (%s), works after (ATK-DONE + %s)" % (
            base["state"], "/".join(case.get("fixed_require", ["no refusal code"])))
    if not b_ok and f_ok:
        return "NOT-A-BUG", True, ("baseline did NOT wedge (%s) -- vector is "
                                   "not reproducible; drop it from the report"
                                   % base["state"])
    return "FAIL", False, ("before: alive=%s (%s); after: %s"
                           % (base["alive"], base["state"], fixed_why(fixed)))


def main():
    args = sys.argv[1:]

    def opt(name, default):
        return args[args.index(name) + 1] if name in args else default

    baseline = opt("--baseline", DEFAULT_BASELINE)
    fixed = opt("--fixed", DEFAULT_FIXED)
    side = opt("--side", "both")
    todo = CASES
    if "--only" in args:
        want = [x.strip().upper()
                for x in args[args.index("--only") + 1].split(",")]
        todo = [c for c in CASES if c["id"] in want]

    if "--list" in args:
        for c in CASES:
            print("%s  %-11s %-4s %s" % (c["id"], c["ce"], c["args"], c["vector"]))
        return 0

    need = [p for p, s in ((baseline, side in ("both", "baseline")),
                           (fixed, side in ("both", "fixed"))) if s]
    for p in need:
        if not os.path.exists(p):
            print("MISSING ISO: %s" % p)
            if p == baseline:
                print("  -> snapshot the unpatched image first:"
                      "\n     cp kernel_v25/catos.iso tests/baseline-catos.iso"
                      "\n     (do this BEFORE any `sh build.sh iso` in the "
                      "shared tree)")
            return 2

    free = free_mb(HERE)
    if free < FREE_MIN_MB:
        print("REFUSING TO START: only %d MB free where tests/ lives "
              "(need >= %d MB)." % (free, FREE_MIN_MB))
        print("  Every run copies a 64 MiB disk image; a guest whose disk "
              "writes fail is indistinguishable from a C5 corruption result.")
        return 3

    qemu = find_qemu()
    if not qemu:
        print("no qemu-system-i386 found")
        return 2

    global ARMED
    ARMED = resolve_armed(allow_build="--allow-build-arsenal" in args)
    if not ARMED:
        return 4
    os.makedirs(LOGDIR, exist_ok=True)
    print("baseline : %s" % baseline)
    print("fixed    : %s" % fixed)
    print("armed    : %s" % ARMED)

    results, t0 = [], time.time()
    for c in todo:
        base = fixed_rec = None
        lp = os.path.join(LOGDIR, "fix-%s.txt" % c["id"])
        with open(lp, "w", encoding="utf-8", errors="replace") as log:
            if side in ("both", "baseline"):
                base = run_one(qemu, baseline, c, "baseline", log)
                print("%s baseline -> state=%-9s alive=%-5s %s"
                      % (c["id"], base["state"], base["alive"], base["why"]))
                sys.stdout.flush()
            if side in ("both", "fixed"):
                fixed_rec = run_one(qemu, fixed, c, "fixed", log)
                print("%s fixed    -> state=%-9s done=%-5s alive=%-5s %s"
                      % (c["id"], fixed_rec["state"], fixed_rec["done"],
                         fixed_rec["alive"], fixed_rec["why"]))
                sys.stdout.flush()
        v, ok, note = verdict(c, base, fixed_rec)
        results.append(dict(case=c["id"], vector=c["vector"], verdict=v,
                            ok=ok, note=note, baseline=base, fixed=fixed_rec))
        print("   -> %-10s %s\n" % (v, note))
        time.sleep(1.0)

    summ = os.path.join(LOGDIR, "regression_after_fix.json")
    with open(summ, "w", encoding="utf-8") as f:
        json.dump(dict(elapsed_s=round(time.time() - t0, 1),
                       baseline=baseline, fixed=fixed,
                       results=results), f, indent=2)

    bad = [r for r in results if not r["ok"]]
    print("%d cases, %d not-passed, %.1f s -> %s"
          % (len(results), len(bad), time.time() - t0, summ))
    for r in results:
        print("  %-3s %-10s %s" % (r["case"], r["verdict"], r["note"]))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
