#!/usr/bin/env python3
"""jobctl_regress.py -- v2.52 shell job-control minimal closed-loop regression.

Covers:
  jc-1  background job appears in `jobs` with a live state column
  jc-2  `stop TID` (SIGTSTP) moves the job to stopped state
  jc-3  `bg TID` (SIGCONT) resumes the stopped job in background
  jc-4  `kill TID` reaps the resumed job; `jobs` no longer lists it
  jc-5  `fg TID` brings a short background job to foreground, waits for
        exit, and the shell returns to the prompt
  jc-6  `fg` with no live job -> friendly error
  jc-7  `stop` with a garbage argument -> friendly error
  jc-8  no fatal fault after the round-trip

usage:
  python tests/jobctl_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)

PORT = 4463


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-34s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def run(s, c, wait=1.5):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    return screen_text(s)


def announce_tid(t):
    m = re.search(r"bg tid (\d+)", t)
    return int(m.group(1)) if m else 0


def prompt_back(t):
    lines = [ln for ln in t.splitlines() if ln.strip()]
    return bool(lines) and lines[-1].rstrip().endswith(("#", "$"))


def main():
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    iso = os.path.abspath(sys.argv[1])
    port = PORT
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    proc, sock = boot(qemu, iso, port,
                      disk=os.path.abspath(os.path.join(
                          os.path.dirname(HERE),
                          "kernel_v25", "catos_disk.img")))
    r = R()
    try:
        wait_for_prompt(sock)
        # PS/2 ghost-keystroke settle (docs/history.md): discard-type once and
        # wait for the controller to settle, or stray digits pollute argv and
        # the assertion window of the first real command.
        run(sock, "clear", 2.5)

        # jc-1: long spinner in background; jobs lists it alive
        t = run(sock, "spinner 180 &", 2.0)
        tid = announce_tid(t)
        r.check("bg-spawn-announce", tid > 0, t)
        t = run(sock, "jobs", 1.5)
        r.check("jobs-lists-running",
                "spinner" in t and ("run" in t or "sleep" in t), t)

        # jc-2: suspend (clear first: full-screen scans must not see stale
        # lines from earlier checks, nor PS/2 ghost keystroke noise)
        run(sock, "stop %d" % tid, 1.5)
        run(sock, "clear", 0.8)
        t = run(sock, "jobs", 1.5)
        r.check("jobs-shows-stopped", "stopped" in t and "spinner" in t, t)

        # jc-3: resume in background
        t = run(sock, "bg %d" % tid, 1.5)
        r.check("bg-resumes", "[bg tid" in t, t)
        run(sock, "clear", 0.8)
        t = run(sock, "jobs", 1.5)
        r.check("jobs-running-again",
                "stopped" not in t and "spinner" in t, t)

        # jc-4: reap via kill
        run(sock, "kill %d" % tid, 1.5)
        run(sock, "clear", 0.8)
        t = run(sock, "jobs", 1.5)
        r.check("kill-reaps-job", "spinner" not in t, t)

        # jc-5: fg a short background job and wait for exit (20 ticks = 10s:
        # a 4-tick task is reaped before fg can attach)
        t = run(sock, "spinner 20 &", 2.0)
        tid2 = announce_tid(t)
        r.check("fg-target-spawned", tid2 > 0, t)
        run(sock, "fg %d" % tid2, 1.0)
        done = ""
        for _ in range(30):
            done = screen_text(sock)
            if "[spinner done]" in done:
                break
            time.sleep(1.0)
        r.check("fg-runs-to-exit", "[spinner done]" in done, done)
        time.sleep(1.5)
        t = screen_text(sock)
        r.check("shell-returns-to-prompt", prompt_back(t), t)

        # jc-6: fg with no live job
        t = run(sock, "fg", 1.5)
        r.check("fg-no-jobs-msg",
                "no such job" in t or "no current job" in t, t)

        # jc-7: stop garbage argument
        t = run(sock, "stop xyz", 1.5)
        r.check("stop-garbage-arg", "no current job" in t, t)

        # jc-8: no fatal fault
        t = run(sock, "faultinfo", 1.5)
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)

    print("\n%d checks, %d failed" % (r.n, len(r.fails)))
    if r.fails:
        print("FAILED: " + ", ".join(r.fails))
        sys.exit(1)
    print("jobctl_regress: ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
