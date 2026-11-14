#!/usr/bin/env python3
"""crash_attack.py -- red-team crash/DoS driver for CatOS.

Every attack CE from kernel_v25/extapp/attacks/ runs in its own fresh QEMU
guest: armed disk copy, `-net none`, `-display none`, private HMP monitor
port (4492).  Outcomes are classified as

    C1 PANIC      KERNEL PANIC, or the ring0 exception path halted the box
                  ("ring0 kernel fault - system halted"), confirmed by the
                  CPU being stuck in a cli/hlt loop (identical EIP on two
                  samples AND EFL.IF = 0)
    C2 REBOOT     the guest reset (QEMU exits because boot() passes
                  -no-reboot), or the GRUB screen came back
    C3 HANG       nothing answers: no ATK-DONE and `faultinfo` produces no
                  output while the monitor still reads memory
    C4 FAULT-R0   `faultinfo` reports a fault taken in ring0 that the
                  kernel survived
    C5 CORRUPT    the attack finished (or died) but the console/system no
                  longer works as before
    OK            the kernel kept serving: the attack returned, or the
                  attacker task was killed by its own ring3 fault

Note on ATKGFX: the attack switches the console to 800x600x24, after which
the VGA text buffer at 0xB8000 is no longer updated, so the *text* screen
proves nothing.  That entry is judged from the monitor only (register
sampling), and says so in its detail line.

Usage:
    python tests/crash_attack.py
    python tests/crash_attack.py --list
    python tests/crash_attack.py --only A10,A06
    python tests/crash_attack.py --keep          # keep the guest disk

Evidence: tests/attack_logs/<id>-<name>.txt (screen dumps, faultinfo probe,
`info registers`) and tests/attack_logs/summary.json.
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
                         type_text, sendkey, read_screen, cmd)  # noqa: E402

ISO_BASELINE = os.path.join(HERE, "baseline-catos.iso")
ISO = ISO_BASELINE if os.path.exists(ISO_BASELINE) \
    else os.path.join(ROOT, "kernel_v25", "catos.iso")
DISK_SRC = os.path.join(ROOT, "kernel_v25", "catos_disk.img")
DISK_ARMED = os.path.join(HERE, "attack_disk_base.img")
DISK_RUN = os.path.join(HERE, "attack_disk.img")
CE_BIN = os.path.join(ROOT, "kernel_v25", "extapp", "bin")
BAD_DIR = os.path.join(HERE, "bad_ce")
LOGDIR = os.path.join(HERE, "attack_logs")
PORT = 4492                       # driver-private monitor port (4444 = regress)

# 8.3 injection name -> built .ce file name
CE_SRC = {
    "ATKSYSNR.CE": "atk_sysnr.ce", "ATKARGS.CE": "atk_args.ce",
    "ATKLEN.CE":   "atk_len.ce",   "ATKHEAP.CE": "atk_heap.ce",
    "ATKMMAP.CE":  "atk_mmap.ce",  "ATKPROC.CE": "atk_proc.ce",
    "ATKWAIT.CE":  "atk_wait.ce",  "ATKGFX.CE":  "atk_gfx.ce",
    "ATKPIPE.CE":  "atk_pipe.ce",  "ATKNET.CE":  "atk_net.ce",
    "ATKFS.CE":    "atk_fs.ce",    "ATKSTACK.CE": "atk_stack.ce",
    "ATKSYSP.CE":  "atk_sysprot.ce", "ATKAUTOS.CE": "atk_autostart.ce",
}

PROMPT_RE = re.compile(r"catos:[^\n]*\$")
FATAL = ("KERNEL PANIC", "ring0 kernel fault", "*** exception:")
REBOOT = ("GNU GRUB", "Booting `CatOS", "CatOS kernel v")

ATTACKS = [
    dict(id="A01", name="syscall-nr",   ce="ATKSYSNR.CE", args="", t=22,
         vector="illegal syscall numbers 65..0xFFFFFFFF + illegal int vectors",
         expect="OK: numbers rejected with -8; task dies on the 1st illegal vector"),
    dict(id="A02", name="arg-fuzz",     ce="ATKARGS.CE",  args="", t=28,
         vector="all 65 ABI slots x poison pointer/value tuples",
         expect="OK: every slot validates, error codes only"),
    dict(id="A03", name="len-abuse",    ce="ATKLEN.CE",   args="", t=32,
         vector="length/offset overflow: cat/save/file_rw/hw_str/strlen/path",
         expect="OK: clamped, negative lengths rejected"),
    dict(id="A04", name="heap-forge",   ce="ATKHEAP.CE",  args="", t=28,
         vector="alloc/free/brk: forged heap_hdr in stack, double free, brk top",
         expect="suspicious: sys_free accepts stack/image pointers"),
    dict(id="A05", name="mmap-wrap",    ce="ATKMMAP.CE",  args="", t=28,
         vector="munmap addr+len 32-bit wrap -> VMA start rewritten to 0",
         expect="suspicious: vma_munmap has no overflow check"),
    dict(id="A06", name="kill-shell",   ce="ATKPROC.CE",  args="", t=26,
         vector="kill(0)=shell task destroy; spawn/wait/kill fuzz",
         expect="C3/C5: ring0 shell destroyed, console gone for good"),
    dict(id="A07", name="wait-child",   ce="ATKWAIT.CE",  args="", t=20,
         vector="wait() on bogus/unused tids (survivable pass)",
         expect="OK"),
    dict(id="A08", name="wait-self",    ce="ATKWAIT.CE",  args="1", t=24,
         vector="wait(self): self-deadlock while the shell waits for us",
         expect="C3: task-level deadlock, console blocked"),
    dict(id="A09", name="wait-shell",   ce="ATKWAIT.CE",  args="2", t=24,
         vector="wait(shell tid 0): mutual deadlock",
         expect="C3: console dead"),
    dict(id="A10", name="gfx-overflow", ce="ATKGFX.CE",   args="", t=22,
         vector="display_mode(3) + gfx_rect x/y=0x7FFFFFFF (clip overflow)",
         expect="C1: signed overflow -> OOB LFB write -> ring0 #PF -> halt",
         gfx=True),
    dict(id="A11", name="pipe-abuse",   ce="ATKPIPE.CE",  args="", t=24,
         vector="pipe handle/length abuse, double close, write-after-close",
         expect="OK"),
    dict(id="A12", name="net-abuse",    ce="ATKNET.CE",   args="", t=40,
         vector="http_get/dns/udp/ping malformed input (guest has -net none)",
         expect="OK: error codes only (negative control: no NIC)"),
    dict(id="A13", name="fs-abuse",     ce="ATKFS.CE",    args="", t=32,
         vector="path/dir/8.3 abuse, 300-char names, delete dirs",
         expect="OK"),
    dict(id="A14", name="stack-rec",    ce="ATKSTACK.CE", args="", t=20,
         vector="deep ring3 recursion (600 x 512B) past the 128 KiB stack",
         expect="OK: attacker killed by its own ring3 page fault"),
    dict(id="A15", name="stack-vla",    ce="ATKSTACK.CE", args="vla", t=20,
         vector="8 MiB VLA on the ring3 stack",
         expect="OK: attacker killed by its own ring3 page fault"),
    dict(id="A16", name="ce-loader",    ce="",            args="", t=34,
         vector="10 crafted malformed CE01 headers (entry/img/bss/api/magic)",
         expect="OK: loader answers -5/-3 for every one"),
    dict(id="A17", name="gfx-blit-h",   ce="ATKGFX.CE",   args="h", t=22,
         vector="gfx_blit with h=0x40000000, w=1 (w*h*3 overflow probe)",
         expect="decision payload for Hunk 7: must stay OK on both ISOs",
         gfx=True),
    dict(id="A18", name="sysprot",      ce="ATKSYSP.CE", args="", t=26,
         vector="system file write surface: overwrite / delete a built-in app, 8.3 shadow name",
         expect="OK: built-in writes/deletes refused (-13), long name still resolves to the built-in"),
    dict(id="A19", name="autostart",    ce="ATKAUTOS.CE", args="", t=26,
         vector="boot-time auto-execution: drop a script into /etc/autoexec and reboot",
         expect="OK: file may be written, but the boot screen must NOT contain the marker"),
]


# ---------------------------------------------------------------- helpers
def sh(argv):
    """Run a helper command (list form: no shell quoting games on Windows)."""
    import subprocess
    r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("failed (%d): %r\n%s" %
                           (r.returncode, argv,
                            r.stdout.decode("utf-8", "replace")[-800:]))
    return r.stdout.decode("utf-8", "replace")


def arm_disk():
    """Build once an image that already carries every attack CE.  Each
    attack still gets its OWN copy of it, which is what keeps attacks from
    contaminating each other (the base is never booted)."""
    if os.path.exists(DISK_ARMED):
        return
    print("[arm] building armed disk image (injecting all CEs)")
    shutil.copyfile(DISK_SRC, DISK_ARMED)
    inj = os.path.join(HERE, "inject_any.py")
    for a in ATTACKS:
        if a["ce"]:
            sh([sys.executable, inj, DISK_ARMED,
                os.path.join(CE_BIN, CE_SRC[a["ce"]]), a["ce"]])
    for f in sorted(os.listdir(BAD_DIR)):
        sh([sys.executable, inj, DISK_ARMED, os.path.join(BAD_DIR, f), f])


def regs(s):
    return cmd(s, "info registers", wait=0.7)


def parse_regs(dump):
    eip = re.search(r"EIP=([0-9a-fA-F]{8})", dump)
    efl = re.search(r"EFL=([0-9a-fA-F]{8})", dump)
    return (eip.group(1) if eip else "",
            int(efl.group(1), 16) if efl else 0)


def tail(txt, n=26):
    rows = [r for r in (txt or "").split("\n") if r.strip()]
    return "\n".join(rows[-n:])


def sha16(path):
    """First 16 hex digits of a file's sha256 (disk-image identity)."""
    import hashlib
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()[:16]
    except OSError as e:
        return "ERR:%s" % e.errno


def host_free_mb():
    """Free space on the host volume -- a full host disk produces fake C5
    'filesystem corruption' symptoms, so every log records it."""
    try:
        return int(shutil.disk_usage(HERE).free / (1 << 20))
    except Exception:
        return -1


def guests():
    """How many qemu guests are alive on the host right now (read-only).
    TCG is CPU bound: a C3 verdict taken with 3-4 guests running is softer
    than one taken alone, so the number goes into every log."""
    try:
        import subprocess
        r = subprocess.run(["tasklist", "/FI",
                            "IMAGENAME eq qemu-system-i386.exe"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return r.stdout.decode("latin-1", "replace").lower().count(
            "qemu-system-i386")
    except Exception:
        return -1


def wait_done(s, p, deadline, fatal_out):
    """Poll the guest until it dies, prints a final marker, or times out.
    Returns ('C1'|'C2'|'OK'|None, last_screen_text, fatal_reason)."""
    last = ""
    while time.time() < deadline:
        if p.poll() is not None:
            return "C2", last, "QEMU process exited (guest reset, -no-reboot)"
        scr = read_screen(s)
        flat = re.sub(r"\s+", " ", scr)
        last = scr
        for f in FATAL:
            if f in scr:
                fatal_out.append(f)
                return "C1", scr, "kernel fatal string: " + f
        for f in REBOOT:
            if f in scr:
                return "C2", scr, "boot screen came back: " + f
        if "ATK-DONE" in flat:
            return "OK", scr, "attack completed (ATK-DONE)"
        time.sleep(1.2)
    return None, last, ""


def prepare_disk(a):
    """Copy kernel_v25/catos_disk.img to tests/attack_disk.img and inject
    ONLY this attack's CEs (the pristine source is never booted, so attacks
    can never contaminate each other).  A guest killed a moment ago can
    still hold the working image open on Windows -> retry with back-off.
    The host disk here is nearly full, so exactly one 64 MiB copy exists at
    a time and it is deleted afterwards."""
    inj = os.path.join(HERE, "inject_any.py")
    for attempt in range(12):
        try:
            if os.path.exists(DISK_RUN):
                os.chmod(DISK_RUN, 0o666)
                os.remove(DISK_RUN)
            shutil.copyfile(DISK_SRC, DISK_RUN)
            break
        except OSError:
            if attempt == 11:
                raise
            time.sleep(1.5)
    if a["ce"]:
        sh([sys.executable, inj, DISK_RUN,
            os.path.join(CE_BIN, CE_SRC[a["ce"]]), a["ce"]])
    else:
        for f in sorted(os.listdir(BAD_DIR)):
            sh([sys.executable, inj, DISK_RUN, os.path.join(BAD_DIR, f), f])


def run_one(qemu, a, log):
    prepare_disk(a)
    disk_before = sha16(DISK_RUN) if F2 else ""
    p, s = boot(qemu, ISO, PORT, disk=DISK_RUN)
    rec = dict(id=a["id"], name=a["name"], vector=a["vector"],
               ce=a["ce"] + (" " + a["args"] if a["args"] else ""),
               expect=a["expect"], result="?", detail="")
    fatal_out = []
    try:
        if not wait_for_prompt(s, timeout=120):
            rec["result"] = "SETUP-FAIL"
            rec["detail"] = "no shell prompt within 120 s"
            log.write("SETUP FAILED: no prompt\n")
            return rec
        cmd(s, "clear", wait=0.8)
        before = read_screen(s)
        last, live_txt = "", ""
        dead = False
        alive = None

        if a["ce"] == "":
            # ---- loader attack: many crafted images, one boot ----
            bad = sorted(os.listdir(BAD_DIR))
            outcome = None
            for f in bad:
                type_text(s, "run /bin/%s" % f)
                sendkey(s, "ret", 1.0)
                r, scr, why = wait_done(s, p, time.time() + 2.6, fatal_out)
                last = scr
                wd = re.sub(r"\s+", " ", scr)
                # the loader rejects with a negative code -> "cannot load"
                log.write("  %-10s -> %s | %s\n" %
                          (f, r or "ran",
                           tail(scr, 3).replace("\n", " / ")[:200]))
                if r in ("C1", "C2"):
                    outcome = (r, why + " while loading " + f)
                    break
                if "cannot load" not in wd and "ATK-DONE" not in wd:
                    # the malformed image executed instead of being rejected
                    log.write("  !! %s was ACCEPTED by the loader\n" % f)
            if outcome:
                rec["result"], rec["detail"] = outcome
            else:
                rec["result"] = "OK"
                rec["detail"] = "all %d crafted headers rejected" % len(bad)
            # liveness
            type_text(s, "faultinfo")
            sendkey(s, "ret", 2.5)
            live_txt = read_screen(s)
            alive = ("last fault" in live_txt) or ("fault recorded" in live_txt)
            if rec["result"] == "OK" and not alive:
                rec["result"] = "C3"
                rec["detail"] += "; but the shell stopped answering afterwards"
        else:
            # ---- single attack CE ----
            c = "run /bin/%s%s" % (a["ce"], (" " + a["args"]) if a["args"] else "")
            type_text(s, c)
            sendkey(s, "ret", 1.0)
            r, scr, why = wait_done(s, p, time.time() + a["t"], fatal_out)
            last = scr
            if r:
                rec["result"], rec["detail"] = r, why
            # ---- liveness probe: does the shell still answer? (always run:
            # a CE can finish yet leave the console dead, e.g. kill(0)) ----
            type_text(s, "faultinfo")
            sendkey(s, "ret", 2.5)
            live_txt = read_screen(s)
            alive = ("last fault" in live_txt) or ("fault recorded" in live_txt)
            prompt_back = bool(PROMPT_RE.search(live_txt))
            ringm = re.search(r"last fault: \w+ ring0x0*([0-9a-fA-F])", live_txt)
            fault_ring = int(ringm.group(1), 16) if ringm else None
            if rec["result"] == "OK" and not alive:
                rec["result"] = "C5"
                rec["detail"] += (" -- but the shell no longer answers "
                                  "faultinfo: the console is dead")
            elif r is None:
                if alive and fault_ring == 0:
                    rec["result"] = "C4"
                    rec["detail"] = ("kernel survived a fault taken in RING0 "
                                     "(see faultinfo in the log)")
                elif not alive and "killed:" not in live_txt and \
                        "user-space fault" not in live_txt:
                    rec["result"] = "C3"
                    rec["detail"] = ("no ATK-DONE, faultinfo unanswered: "
                                     "console wedged / nothing runnable")
                elif "killed:" in live_txt and prompt_back:
                    rec["result"] = "OK"
                    rec["detail"] = ("attacker task killed by its own ring3 "
                                     "fault, shell prompt back (defence held)")
                elif "killed:" in live_txt:
                    rec["result"] = "C5"
                    rec["detail"] = ("attacker task killed but the shell did "
                                     "not return to a prompt")
                else:
                    rec["result"] = "C5"
                    rec["detail"] = ("shell answers but no ATK-DONE and no "
                                     "task-death banner: attacker wedged or "
                                     "died silently")

        # ---- CPU liveness ----
        # A wedged kernel shows up as EIP never leaving a small window while
        # IF=0 (syscalls run with interrupts masked, so a ring0 busy loop can
        # never be preempted).  The idle task also loops in the kernel, but
        # with IF=1 (sti; hlt), so the IF test keeps the two apart.
        eips, ifs = [], []
        dumps = []
        for _ in range(4):
            d = regs(s)
            dumps.append(d)
            e, f = parse_regs(d)
            if e:
                eips.append(int(e, 16))
            ifs.append(bool(f & 0x200))
            time.sleep(0.6)
        d2 = dumps[-1]
        e2 = "%08x" % eips[-1] if eips else ""
        if_now = ifs[-1] if ifs else True
        frozen = len(eips) == 4 and (max(eips) - min(eips)) <= 0x200
        stuck = frozen and not if_now
        rec["stuck"] = stuck
        rec["eip"] = e2
        rec["if"] = if_now
        window = ("%04x" % (max(eips) - min(eips))) if eips else "?"

        if stuck and rec["result"] != "C1":
            rec["result"] = "C1"
            rec["detail"] = ("CPU frozen at EIP=0x%s (EIP spread 0x%s over 4 "
                             "samples, IF=0: cli/hlt halt or an un-preemptible "
                             "ring0 busy loop)" % (e2, window))
        elif a.get("gfx") and rec["result"] in ("C5", "C3"):
            rec["result"] = "OK(gfx)"
            rec["detail"] = ("console switched to 800x600x24 so the VGA text "
                             "dump is stale; register sampling shows a live "
                             "kernel (IF=1, EIP advancing)")

        # ---- optional: dump the kernel's last-fault record --------------
        # g_fault_last lives in .bss and stays readable through the monitor
        # even after the kernel halted, so this works for C1 too.  Layout:
        # [0]=valid [1]=intno [2]=ring [3]=tid [4..7]=task[16] [8]=err
        # [9]=eip [10]=cr2 [11]=eflags [12]=ticks
        if FAULTREC:
            fr = cmd(s, "xp /13xw %s" % FAULTREC, wait=1.0)
            log.write("\n--- g_fault_last ---\n%s\n" % fr.strip())

        # ---- F2: did the attack damage the DISK IMAGE? ---------------
        # kill the guest cleanly first (the hash must be taken after QEMU
        # has flushed), then re-boot the SAME image and check the volume.
        if F2 and not dead:
            rec["disk_after"] = sha16(DISK_RUN)
            kill_qemu(p)
            time.sleep(1.0)
            try:
                p2, s2 = boot(qemu, ISO, PORT, disk=DISK_RUN)
                if wait_for_prompt(s2, 120):
                    type_text(s2, "ls /"); sendkey(s2, "ret", 1.5)
                    type_text(s2, "fsck"); sendkey(s2, "ret", 4.0)
                    type_text(s2, "run /bin/hello.ce"); sendkey(s2, "ret", 4.0)
                    scr2 = read_screen(s2)
                    ok = ("Hello from CE" in scr2) and ("catos:" in scr2)
                    rec["f2_restart"] = "ok" if ok else "corrupt"
                    log.write("\n--- F2 restart (same image) ---\n%s\n"
                              % tail(scr2))
                else:
                    rec["f2_restart"] = "corrupt(no prompt)"
                kill_qemu(p2)
            except Exception as e:
                rec["f2_restart"] = "boot-error:%s" % type(e).__name__
            rec["disk_before"] = disk_before

        # ---- evidence ----
        log.write("=== %s %s ===\n" % (a["id"], a["name"]))
        log.write("under_test_iso_sha256=%s\n" % iso_sha)
        log.write("host_free_mb=%d guests=%s\n" % (host_free_mb(), guests()))
        if F2:
            log.write("disk_before=%s disk_after=%s F2-restart=%s\n"
                      % (rec.get("disk_before"), rec.get("disk_after"),
                         rec.get("f2_restart")))
        log.write("vector  : %s\n" % a["vector"])
        log.write("run     : run /bin/%s %s\n" % (a["ce"], a["args"]))
        log.write("expect  : %s\n" % a["expect"])
        log.write("result  : %s  (%s)\n" % (rec["result"], rec["detail"]))
        log.write("cpu     : EIP=%s IF=%s stuck=%s\n" % (e2, rec["if"], stuck))
        log.write("\n--- screen BEFORE ---\n%s\n" % tail(before, 6))
        log.write("\n--- screen AFTER ---\n%s\n" % tail(last))
        if live_txt:
            log.write("\n--- faultinfo probe ---\n%s\n" % tail(live_txt, 8))
        log.write("\n--- info registers ---\n%s\n" % d2)
        log.write("\n")
        return rec
    finally:
        if not KEEP:
            try:
                os.unlink(DISK_RUN)
            except OSError:
                pass
        kill_qemu(p)


def main():
    global KEEP, ISO, LOGDIR, F2, iso_sha, FAULTREC
    args = sys.argv[1:]
    KEEP = "--keep" in args
    F2 = "--f2" in args
    if "--faultrec" in args:
        FAULTREC = args[args.index("--faultrec") + 1]
    if "--iso" in args:
        ISO = os.path.abspath(args[args.index("--iso") + 1])
    if "--logs" in args:
        LOGDIR = os.path.join(HERE, args[args.index("--logs") + 1])
    iso_sha = sha16(ISO)
    if "--list" in args:
        for a in ATTACKS:
            print("%s %-12s %-13s %s" % (a["id"], a["name"], a["ce"], a["vector"]))
        return 0
    todo = ATTACKS
    if "--only" in args:
        want = [x.strip().upper() for x in args[args.index("--only") + 1].split(",")]
        todo = [a for a in ATTACKS if a["id"].upper() in want]

    print("ISO   : %s  sha256=%s" % (ISO, iso_sha))
    print("F2    : %s   logs=%s" % (F2, LOGDIR))
    os.makedirs(LOGDIR, exist_ok=True)
    os.makedirs(BAD_DIR, exist_ok=True)
    sh([sys.executable, os.path.join(HERE, "craft_bad_ce.py"), BAD_DIR])
    # per-attack disk prep lives in prepare_disk(); no shared armed copy

    qemu = find_qemu()
    if not qemu:
        print("no qemu-system-i386 found")
        return 2

    results, t0 = [], time.time()
    for a in todo:
        lp = os.path.join(LOGDIR, "%s-%s.txt" % (a["id"], a["name"]))
        with open(lp, "w", encoding="utf-8", errors="replace") as log:
            rec = run_one(qemu, a, log)
        results.append(rec)
        print("%s %-12s -> %-8s %s" % (rec["id"], rec["name"],
                                       rec["result"], rec["detail"][:96]))
        sys.stdout.flush()
        time.sleep(1.0)

    # merge with earlier partial results if this was a --only run
    summ = os.path.join(LOGDIR, "summary.json")
    if os.path.exists(summ) and "--only" in args:
        try:
            with open(summ, encoding="utf-8") as f:
                old = {r["id"]: r for r in json.load(f)["results"]}
            for r in results:
                old[r["id"]] = r
            results = [old[k] for k in sorted(old)]
        except Exception:
            pass
    with open(summ, "w", encoding="utf-8") as f:
        json.dump(dict(elapsed_s=round(time.time() - t0, 1), results=results),
                  f, indent=2)

    bad = [r for r in results if not r["result"].startswith("OK")]
    print("\n%d attacks, %d not-OK, %.1f s -> %s" %
          (len(results), len(bad), time.time() - t0, summ))
    return 0


if __name__ == "__main__":
    KEEP = False
    F2 = False
    iso_sha = ""
    FAULTREC = ""
    sys.exit(main())
