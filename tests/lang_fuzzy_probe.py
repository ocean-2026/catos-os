#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.82 P1-5 (B5): trhelp fuzzy fallback regression probe.

Three stages:

  A. Static coverage -- extract every help_row/help_head/help_note literal
     from shell.c and match it against the real help_zh[] table in lang.c,
     first exact, then normalised (case / whitespace / trailing punctuation),
     then loose first-English-word.  Reports the actual hit counts.

  B. Host-compiled unit run -- compile the real kernel lang.c against tiny
     fs/hive stubs on the host, drive trhelp() with the exact baseline plus
     three synthetic variants (multi-space / trailing punctuation / case),
     the unknown-string English fallback and the English-mode passthrough.

  C. On-metal smoke -- boot QEMU, `lang zh`, run `help` and `keys` (no
     fault / no halt), then `lang en` and confirm the English surface is
     unchanged.  Screenshots saved under artifacts/v282/p1-5/.

Usage:
  python3 tests/lang_fuzzy_probe.py [catos.iso]
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu_driver as q

ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
OUT = os.path.join(ROOT, "artifacts", "v282", "p1-5")
os.makedirs(OUT, exist_ok=True)
SHIPPED = os.path.join(ROOT, "kernel_v25", "catos_disk.img")
PORT = 4555

results = []


def check(name, ok, note=""):
    results.append((name, bool(ok)))
    print("%-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note))


# --------------------------------------------------------------------------
# Stage A: static coverage against the real sources
# --------------------------------------------------------------------------

def unescape(s):
    return s.encode("utf-8").decode("unicode_escape")


def h_is_ws(c):
    return c in " \t\r\n"


def h_is_punct(c):
    return c in ".,;:!)]"


def h_norm(s):
    out, ws = [], True
    for ch in s.lower():
        if h_is_ws(ch):
            ws = True
            continue
        if h_is_punct(ch):
            continue
        if ws and out and out[-1] != " ":
            out.append(" ")
        out.append(ch)
        ws = False
    while out and out[-1] == " ":
        out.pop()
    return "".join(out)


def h_first_word(s):
    w = ""
    for ch in s.lower():
        if ch.isalnum():
            w += ch
        elif w:
            break
    return w


def stage_static():
    shell = open(os.path.join(ROOT, "kernel_v25", "src", "shell.c"),
                 encoding="utf-8").read()
    lang = open(os.path.join(ROOT, "kernel_v25", "src", "lang.c"),
                encoding="utf-8").read()

    rows = re.findall(r'help_row\(\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"',
                      shell)
    heads = re.findall(r'help_head\(\s*"((?:[^"\\]|\\.)*)"', shell)
    notes = re.findall(r'help_note\(\s*"((?:[^"\\]|\\.)*)"', shell)
    table = re.findall(r'\{"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\}', lang)

    en = [unescape(k) for k, _ in table]
    norm_en = [h_norm(e) for e in en]
    fw_en = [h_first_word(e) for e in en]

    strings = [("row", c, unescape(d)) for c, d in rows]
    strings += [("head", "", unescape(h)) for h in heads]
    strings += [("note", "", unescape(n)) for n in notes]

    exact = normhit = loose = miss = 0
    missing = []
    for kind, cmd, s in strings:
        if s in en:
            exact += 1
        elif h_norm(s) in norm_en:
            normhit += 1
        elif h_first_word(s) in fw_en:
            loose += 1
        else:
            miss += 1
            missing.append((kind, cmd, s))

    print("static: %d help strings, table entries=%d" % (len(strings), len(en)))
    print("static: exact=%d normalised=%d loose-first-word=%d unmapped=%d"
          % (exact, normhit, loose, miss))
    for kind, cmd, s in missing:
        print("  unmapped: [%s] %s -- %s" % (kind, cmd, s))

    # Baseline captured on fbab3a7: 115 exact, 2 loose, 4 unmapped.  The
    # fuzzy layer must not lose any previously exact hit.
    check("static exact hits >= 115", exact >= 115, "exact=%d" % exact)
    check("static unmapped set unchanged (4 pre-existing)",
          miss == 4 and len(missing) == 4, "unmapped=%d" % miss)
    return {"total": len(strings), "exact": exact, "norm": normhit,
            "loose": loose, "miss": miss}


# --------------------------------------------------------------------------
# Stage B: host-compiled run against the real lang.c
# --------------------------------------------------------------------------

HARNESS_C = r"""
#include <stdio.h>
#include <string.h>

int fs_fs_ready(void) { return 0; }
int fs_read_text(const char *p, char *b, int m) { (void)p; (void)b; (void)m; return -1; }
int fs_write_text(const char *p, const char *d, unsigned int s) { (void)p; (void)d; (void)s; return -1; }
const char *hive_sz(const char *p, const char *def) { (void)p; return def; }
int hive_set_sz(const char *p, const char *v) { (void)p; (void)v; return 0; }

#include "lang.c"

static int fails = 0;
static void check(const char *name, const char *got, const char *want) {
    int ok = got && want && strcmp(got, want) == 0;
    if (!ok) fails++;
    printf("%-4s %-30s got=%s want=%s\n", ok ? "PASS" : "FAIL", name,
           got ? got : "(null)", want ? want : "(null)");
}

int main(void) {
    lang_set(LANG_ZH);
    check("exact baseline", trhelp("show this message"), "显示本帮助");
    /* variant 1: leading / extra / trailing whitespace */
    check("variant multi-space", trhelp("   show   this   message   "), "显示本帮助");
    /* variant 2: trailing punctuation */
    check("variant trailing dot", trhelp("show this message."), "显示本帮助");
    /* variant 3: mixed case */
    check("variant case", trhelp("Show This Message"), "显示本帮助");
    /* heading-level variants */
    check("heading multi-space", trhelp("  network   (PCnet)  "), "网络（PCnet）");
    check("heading case+punct", trhelp("Network (PCnet):"), "网络（PCnet）");
    /* unknown still falls back to English */
    check("unknown -> English", trhelp("zzz no such entry qqx"), "zzz no such entry qqx");
    /* English mode passthrough */
    lang_set(LANG_EN);
    check("en passthrough", trhelp("show this message"), "show this message");
    printf(fails ? "\n%d FAIL\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
"""


def stage_host_harness():
    tmp = tempfile.mkdtemp(prefix="lang_fuzzy_")
    try:
        c_path = os.path.join(tmp, "harness.c")
        bin_path = os.path.join(tmp, "harness")
        with open(c_path, "w", encoding="utf-8") as f:
            f.write(HARNESS_C)
        inc = [os.path.join(ROOT, "kernel_v25", "include"),
               os.path.join(ROOT, "kernel_v25", "src")]
        cmd = ["gcc"] + ["-I" + p for p in inc] + [c_path, "-o", bin_path]
        cc = subprocess.run(cmd, capture_output=True, text=True)
        if cc.returncode != 0:
            print(cc.stderr)
            check("host harness compiles", False, "gcc rc=%d" % cc.returncode)
            return
        run = subprocess.run([bin_path], capture_output=True, text=True)
        print(run.stdout.strip())
        if run.stderr.strip():
            print(run.stderr.strip())
        npass = run.stdout.count("PASS ")
        check("host harness all PASS", run.returncode == 0,
              "%d PASS, rc=%d" % (npass, run.returncode))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------
# Stage C: on-metal smoke
# --------------------------------------------------------------------------

def boot_fixed(port, disk):
    qemu = q.find_qemu()
    if not qemu:
        raise RuntimeError("qemu-system-i386 not found on PATH")
    proc = subprocess.Popen(
        [qemu, "-m", "256", "-no-reboot", "-net", "none",
         "-cdrom", ISO, "-boot", "order=d", "-display", "none",
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
         "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % disk],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(2)
    q.kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def shoot(sock, name):
    ppm = os.path.join(OUT, name + ".ppm")
    png = os.path.join(OUT, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    q.cmd(sock, "screendump %s" % ppm, 1.0)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        from PIL import Image
        Image.open(ppm).save(png)
        os.remove(ppm)
    return png


def do(sock, line, wait=1.2):
    q.type_text(sock, line, 0.01)
    q.sendkey(sock, "ret", wait)


def enter_shell(sock):
    time.sleep(12.0)
    q.sendkey(sock, "esc", 1.0)
    time.sleep(1.0)
    q.force_text(sock)
    return q.wait_for_prompt(sock, 120.0)


def fault_free(text):
    # 'faultinfo' / 'panic record' are normal help strings; the real crash
    # banner is "!!! PANIC !!!" (bsod.c) plus the fault/halt markers.
    return not re.search(r"!!! PANIC|page fault|kernel fault|system halted",
                         text, re.I)


def stage_qemu():
    disk = os.path.join(tempfile.gettempdir(), "lang_fuzzy_%d.img" % os.getpid())
    if os.path.exists(disk):
        os.remove(disk)
    shutil.copy2(SHIPPED, disk)
    p, s = boot_fixed(PORT, disk)
    try:
        if not enter_shell(s):
            print("FATAL: no prompt")
            return 3

        do(s, "clear", 0.5)
        do(s, "lang zh", 1.2)
        do(s, "help", 2.5)
        zh = q.screen_text(s)
        open(os.path.join(OUT, "zh_help_screen.txt"), "w").write(zh)
        check("help under zh: no fault", fault_free(zh), zh[-120:])
        check("help under zh: command names still English",
              "edit" in zh and "crashlog" in zh, zh[-120:])
        shoot(s, "01-zh-help")

        do(s, "clear", 0.5)
        do(s, "keys", 2.0)
        kz = q.screen_text(s)
        check("keys under zh: no fault", fault_free(kz), kz[-120:])
        shoot(s, "02-keys-zh")

        do(s, "clear", 0.5)
        do(s, "lang en", 1.2)
        do(s, "help", 2.5)
        en = q.screen_text(s)
        open(os.path.join(OUT, "en_help_screen.txt"), "w").write(en)
        check("help under en: no fault", fault_free(en), en[-120:])
        check("help under en: English description visible",
              "full-screen editor" in en, en[-120:])
        shoot(s, "03-en-help")
    finally:
        try:
            do(s, "poweroff", 1.0)
        except (ConnectionResetError, OSError):
            pass
        q.kill_qemu(p)
    return 0


def main():
    iso = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ISO
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2
    stage_static()
    stage_host_harness()
    rc = stage_qemu()
    if rc:
        return rc
    n = sum(1 for _, ok in results if ok)
    print("\n%d/%d PASS" % (n, len(results)))
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
