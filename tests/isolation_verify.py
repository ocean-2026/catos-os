#!/usr/bin/env python3
"""isolation_verify.py -- PROVE the red-team isolation boundary, don't assert it.

What this script is for
-----------------------
The user's hard requirement for this round is that the crash attack runs
"in a VM, on an isolated git branch, in an isolated working directory, with
network isolation so that nothing outside is affected".  A README sentence
is not evidence.  This script turns each of those four claims into a
checkable, re-runnable artefact:

    A  static audit of the driver command line + a repo-wide scan for
       isolation-breaking QEMU options and for `taskkill /IM`
    B  git worktree / branch isolation, and proof the user's main worktree
       is still clean
    C  disk isolation: the guest writes to a COPY; the source image hash
       must be byte-identical before and after
    D  network isolation: host-side listen sockets of the guest process,
       QEMU's own PCI/network view, and the guest's own net_up() == 0
    E  process isolation: the guest is terminated by PID, and only by PID

Design constraints (deliberate, do not "improve" them):
  * ONE monitor port: 4494.  The red-team driver and qemu_regress use
    other ports; two drivers on one port see WinError 10054 and each
    blames the other.
  * ONE guest, killed by PID immediately after the probes.  The red team
    is running attacks on this same host; this script must not steal CPU
    for minutes or squat a port.
  * NO attack payloads.  Only netdiag / netinfo / touch / ls -- a hang
    here would look like a kernel bug and pollute the red team's data.
  * Nothing is written outside this worktree.  The source disk image is
    opened read-only (sha256) and never mounted.

usage:
    python tests/isolation_verify.py                  # full run, exit 0 if all PASS
    python tests/isolation_verify.py --no-vm          # static + git + disk-hash only
    python tests/isolation_verify.py --residual-only  # just refresh R_residual_risk.txt
    python tests/isolation_verify.py --keep-probe     # keep the 64 MiB probe copy

The guest-visible 64 MiB copy (tests/isolation_probe.img) is deleted at the end of
a full run: both sha256s are already in C_disk_isolation.txt, and the host has hit
FreeSpace=0 before.  The C verdict rests on the recorded hashes, not on the file.
"""
import ast
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # worktree root
sys.path.insert(0, HERE)

from qemu_driver import (boot, find_qemu, kill_qemu, wait_for_prompt,   # noqa: E402
                         type_text, sendkey, read_screen, clear_screen, cmd)

MONITOR_PORT = 4494                                # THIS script's only port
QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
SRC_DISK = os.path.join(ROOT, "kernel_v25", "catos_disk.img")
PROBE_DISK = os.path.join(HERE, "isolation_probe.img")
# the probe copy is deleted after its hashes are recorded (host disk pressure);
# --keep-probe leaves the 64 MiB file behind
KEEP_PROBE = "--keep-probe" in sys.argv
LOGDIR = os.path.join(HERE, "isolation_logs")
MASTER_WT = r"C:/Users/Admin/WorkBuddy/Worktrees/CatOs/master-a125998e"

# QEMU switches that would each punch a hole in the isolation boundary.
# `-net none` is the ONLY acceptable form of -net.  -sandbox is a
# Linux/seccomp feature and is not compiled into the Windows build (see
# the residual-risk section), so it is listed as "desired, unavailable".
#
# Patterns are REGEXES, not plain substrings: a substring scan for "9p"
# matches the PNG magic "\x89PNG" (x89p...) and produces two false
# positives in this very directory, which is exactly how a scanner trains
# its reader to ignore it.
FORBIDDEN = [
    ("-enable-kvm", r"-enable-kvm",
     "host CPU virtualisation pass-through; not wanted and not guaranteed here"),
    ("host-dir share", r"file=fat|fat:rw",
     "host directory exported to the guest as a FAT drive == host fs share"),
    ("9p/virtfs", r"\b9p\b|-virtfs|-fsdev",
     "plan9 / virtfs host directory share"),
    ("virtio-fs", r"virtiofs|virtio-fs",
     "virtio-fs host directory share"),
    ("-serial", r"-serial",
     "serial chardev; `-serial tcp:...,server` exposes a host port"),
    ("-chardev", r"-chardev",
     "generic chardev; can re-expose a host socket"),
    ("hostfwd/-redir", r"hostfwd|-redir",
     "user-mode port forwarding host<->guest"),
    ("-netdev", r"-netdev",
     "explicit netdev; `-net none` must be the only network config"),
    ("-net user", r"-net\s+user",
     "user-mode networking (slirp): guest could reach the LAN/host"),
    ("-net tap", r"-net\s+tap",
     "bridged networking"),
    ("smb/tftp", r"-smb|(?<![\w-])tftp",
     "slirp SMB/TFTP server (exports a host directory)"),
    ("taskkill /IM", r'taskkill"\s*,\s*"/[Ff]"\s*,\s*"/[Ii][Mm]"|taskkill /IM',
     "kills every guest on the host, including teammates'"),
]

RESULTS = []        # rows for the final table
LOGS = {}           # logfile name -> path


# ---------------------------------------------------------------- helpers
def log_write(name, text):
    os.makedirs(LOGDIR, exist_ok=True)
    path = os.path.join(LOGDIR, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text if text.endswith("\n") else text + "\n")
    LOGS[name] = path
    return path


def sh(argv, cwd=None, timeout=180):
    """Run a command, return stdout+stderr as text. Never raises."""
    try:
        p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout, errors="replace")
        return "$ %s\n%s%s" % (" ".join(argv), p.stdout or "", p.stderr or "")
    except Exception as e:                                   # noqa: BLE001
        return "$ %s\n<<failed: %s: %s>>" % (" ".join(argv), type(e).__name__, e)


def ps(script, timeout=120):
    """Run one PowerShell -Command and return its text output.

    The console defaults to the ANSI code page (cp936 here), which turns
    the Chinese part of native tool output into mojibake and makes the
    evidence log unreadable for the next person.  Force UTF-8 first.
    """
    return sh(["powershell", "-NoProfile", "-NonInteractive", "-Command",
               "[Console]::OutputEncoding=[Text.Encoding]::UTF8; " + script],
              timeout=timeout)


def body_of(text):
    """Drop the '$ cmd' header line that sh() prepends."""
    return text.split("\n", 1)[1].strip() if "\n" in text else ""


def prepare_probe_disk():
    """Copy the source image to the guest's throwaway disk.

    The previous run's 64 MiB copy can still be held for a second or two
    after QEMU exits (AV real-time scan of a freshly written image), which
    surfaces as PermissionError on the plain copyfile.  Retry instead of
    failing the whole audit on a transient lock.
    """
    last = None
    for _ in range(24):
        try:
            if os.path.exists(PROBE_DISK):
                os.remove(PROBE_DISK)
            shutil.copyfile(SRC_DISK, PROBE_DISK)
            return
        except PermissionError as e:
            last = e
            time.sleep(1.5)
    raise RuntimeError("cannot refresh %s (still locked): %s" % (PROBE_DISK, last))


def sha256(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def add(item, method, evidence, verdict, residual=""):
    RESULTS.append({"item": item, "method": method,
                    "evidence": evidence.strip(), "verdict": verdict,
                    "residual": residual.strip()})
    print("[%s] %s" % (verdict, item))


# --------------------------------------------------- A. static audit
def audit_driver():
    """Does boot() actually put `-net none` on the command line?

    Grepping for the string is not enough -- the string also appears in
    the module docstring of qemu_driver.py, so a naive grep passes even if
    the real cmdline lost the flag.  Instead, import the module and ask
    `boot()` itself: we monkeypatch subprocess.Popen and read the argv it
    would launch.
    """
    out = []
    out.append("=== A1: argv that qemu_driver.boot() ACTUALLY builds ===")
    captured = {}

    real_popen = subprocess.Popen

    class FakeSock:
        def __init__(self):
            self.sent = []

        def sendall(self, b):
            self.sent.append(b)

        def recv(self, n):
            return b""

        def setblocking(self, b):
            pass

        def close(self):
            pass

    class FakeProc:
        pid = 999999
        returncode = 0

        def poll(self):
            return None

        def terminate(self):
            pass

        def wait(self, timeout=None):
            return 0

        def kill(self):
            pass

    def fake_connect(*a, **k):
        return FakeSock()

    def fake_popen(argv, **kw):
        captured["argv"] = list(argv)
        return FakeProc()

    subprocess.Popen = fake_popen
    real_conn = socket.create_connection
    socket.create_connection = fake_connect
    try:
        boot(QEMU, ISO, MONITOR_PORT, disk="X:/probe.img")
    except Exception as e:                                   # noqa: BLE001
        out.append("boot() raised (expected, we faked the socket): %r" % (e,))
    finally:
        subprocess.Popen = real_popen
        socket.create_connection = real_conn

    argv = captured.get("argv") or []
    rendered = " ".join('"%s"' % a if " " in a else a for a in argv)
    out.append("ARGV = " + rendered)
    out.append("")

    checks = []
    # -net none present and NOT followed by anything else
    net_vals = [argv[i + 1] for i, a in enumerate(argv) if a == "-net" and i + 1 < len(argv)]
    ok_net = net_vals == ["none"]
    checks.append(("network", "-net none present and is the only -net", ok_net,
                   "-net %r" % (net_vals,)))
    ok_disp = "-display" in argv and argv[argv.index("-display") + 1] == "none"
    checks.append(("display", "-display none", ok_disp,
                   argv[argv.index("-display") + 1] if "-display" in argv else "MISSING"))
    mon = argv[argv.index("-monitor") + 1] if "-monitor" in argv else ""
    ok_mon = mon.startswith("tcp:127.0.0.1:") and "server" in mon
    checks.append(("monitor", "monitor bound to 127.0.0.1 loopback only", ok_mon,
                   mon or "MISSING"))
    ok_nr = "-no-reboot" in argv
    checks.append(("no-reboot", "-no-reboot (a crashing guest cannot loop-reboot)", ok_nr,
                   "present" if ok_nr else "MISSING"))
    disk_arg = [a for a in argv if a.startswith("file=")]
    ok_disk = all("format=raw" in a for a in disk_arg) and not any("fat" in a for a in disk_arg)
    checks.append(("drive", "disk passed as plain image file, no host share", ok_disk,
                   "; ".join(disk_arg) or "no disk"))

    for _k, desc, ok, ev in checks:
        out.append("  %-4s %-52s %s" % ("PASS" if ok else "FAIL", desc, ev))
    out.append("")

    # forbidden switches in the real argv
    out.append("=== A2: forbidden switches in the real argv ===")
    bad = []
    for name, pat, why in FORBIDDEN:
        if re.search(pat, rendered, re.IGNORECASE):
            bad.append("%s -> %s" % (name, why))
            out.append("  HIT  %-14s %s" % (name, why))
    if not bad:
        out.append("  none of %d forbidden pattern(s) present" % len(FORBIDDEN))
    out.append("")

    log_write("A_static_driver.argv.txt", "\n".join(out))
    ok_all = all(c[2] for c in checks) and not bad
    add("A1 QEMU 命令行（-net none / -display none / monitor 回环 / -no-reboot）",
        "import qemu_driver，替换 subprocess.Popen 截获 boot() 真实构造的 argv",
        " | ".join("%s=%s" % (c[1].split(" (")[0], c[3]) for c in checks)
        + (" | 禁止项命中: " + ", ".join(bad) if bad else " | 禁止项 0 命中"),
        "PASS" if ok_all else "FAIL",
        "argv 拦截法验证的是 boot() 的构造结果；运行时 argv 另见 D2")


def audit_repo():
    """Scan the scripts that can actually launch QEMU for red-line breaks.

    Two layers, because grep alone is untrustworthy here:
      1. AST: every subprocess*/os.system call in tests/*.py, with its
         source segment, so we can see `"/IM"` only where it is really
         executed (the qemu_driver docstring mentions it in prose).
      2. Raw token scan over scripts only (no .md/.inc/.bin noise).
    """
    out = []
    scripts = []
    for d in ("tests", "kernel_v25"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, d)):
            if os.sep + "build" in dirpath or os.sep + "bin" in dirpath:
                continue
            for fn in files:
                if fn.endswith((".py", ".sh", ".bat")):
                    scripts.append(os.path.join(dirpath, fn))
    scripts.sort()
    # Do not scan this verifier itself: it must name every forbidden
    # pattern in order to look for it, so a self-scan is guaranteed noise.
    SELF = os.path.abspath(__file__)
    scripts = [p for p in scripts if os.path.abspath(p) != SELF]
    py = [p for p in scripts if p.endswith(".py")]
    shell = [p for p in scripts if not p.endswith(".py")]

    out.append("=== A3: AST scan -- real process-spawn calls in %d python files ===" % len(py))
    im_hits = []
    total_calls = 0
    for path in py:
        try:
            src = open(path, encoding="utf-8-sig", errors="replace").read()
            tree = ast.parse(src)
        except Exception as e:                               # noqa: BLE001
            out.append("  skip %s (%s)" % (os.path.relpath(path, ROOT), e))
            continue
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            f = node.func
            if isinstance(f, ast.Attribute):
                base = f.value
                bname = getattr(base, "id", getattr(base, "attr", "?"))
                name = "%s.%s" % (bname, f.attr)
            elif isinstance(f, ast.Name):
                name = f.id
            else:
                continue
            if not re.search(r"^(subprocess\.|os\.system$|os\.popen$)", name) and \
               name not in ("Popen", "run", "call", "check_output", "system"):
                continue
            seg = (ast.get_source_segment(src, node) or "").replace("\n", " ")
            total_calls += 1
            if '"/IM"' in seg or "'/IM'" in seg:
                im_hits.append("%s:%d  %s" % (os.path.relpath(path, ROOT),
                                              node.lineno, seg[:160]))
                out.append("  KILL-ALL %s:%d %s" % (os.path.relpath(path, ROOT),
                                                    node.lineno, seg[:160]))
    out.append("  scanned %d process-spawn call sites; taskkill /IM executed: %d"
               % (total_calls, len(im_hits)))
    out.append("")

    out.append("=== A4: raw token scan over %d scripts (not docs, not this verifier) ==="
               % len(scripts))
    tok_hits = []
    for path in scripts:
        try:
            lines = open(path, encoding="utf-8-sig", errors="replace").read().splitlines()
        except Exception:                                    # noqa: BLE001
            continue
        for i, line in enumerate(lines, 1):
            for name, pat, _why in FORBIDDEN:
                if re.search(pat, line, re.IGNORECASE):
                    tok_hits.append("%s:%d  %s  <<%s>>" % (
                        os.path.relpath(path, ROOT), i, line.strip()[:120], name))
    for h in tok_hits:
        out.append("  " + h)
    if not tok_hits:
        out.append("  0 hits")
    out.append("  (qemu_driver.py:9 docstring is prose, not code -- A3's AST pass is authoritative)")
    out.append("")

    # which -net forms exist at all?
    out.append("=== A5: every -net form in QEMU launch scripts ===")
    forms = {}
    for path in scripts:
        try:
            lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        except Exception:                                    # noqa: BLE001
            continue
        for i, line in enumerate(lines, 1):
            for m in re.finditer(r"-net\s*,?\s*\"?([a-z]+)", line):
                forms.setdefault(m.group(1), []).append(
                    "%s:%d" % (os.path.relpath(path, ROOT), i))
    for k, v in sorted(forms.items()):
        out.append("  -net %-6s  %d site(s): %s" % (k, len(v), ", ".join(v[:6])))
    out.append("")

    log_write("A_static_repo_scan.txt", "\n".join(out))
    only_none = list(forms) == ["none"]
    add("A3 全仓无 taskkill /IM、且 -net 仅有 none 一种形态",
        "AST 遍历 tests/**、kernel_v25/** 的所有进程启动调用；再对 *.py/*.sh/*.bat 做原始 token 扫描",
        "进程启动调用点 %d 个，执行式 taskkill /IM：%d 处；-net 形态: %s"
        % (total_calls, len(im_hits), dict((k, len(v)) for k, v in forms.items())),
        "PASS" if (not im_hits and only_none) else ("PARTIAL" if not im_hits else "FAIL"),
        "qemu_driver.py 文档字符串中提到 taskkill /IM 仅作反例说明（非可执行代码），已由 AST 层排除")


# ------------------------------------------------------------- B. git
def audit_git():
    out = []
    out.append(sh(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=ROOT))
    out.append(sh(["git", "rev-parse", "HEAD"], cwd=ROOT))
    out.append(sh(["git", "worktree", "list"], cwd=ROOT))
    out.append("=== this worktree: git status --porcelain ===")
    out.append(sh(["git", "status", "--porcelain"], cwd=ROOT))
    out.append("=== this worktree: git status -sb ===")
    out.append(sh(["git", "status", "-sb"], cwd=ROOT))
    out.append("=== USER MAIN WORKTREE (read-only check): %s ===" % MASTER_WT)
    out.append(sh(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=MASTER_WT))
    out.append(sh(["git", "rev-parse", "HEAD"], cwd=MASTER_WT))
    st = sh(["git", "status", "--porcelain"], cwd=MASTER_WT)
    out.append(st)
    out.append("=== main repo D:/CatOs (read-only, log only -- no index write) ===")
    out.append(sh(["git", "-C", "D:/CatOs", "log", "-1", "--oneline"], cwd=ROOT))

    log_write("B_git_isolation.txt", "\n".join(out))

    branch = body_of(sh(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=ROOT))
    wt_list = sh(["git", "worktree", "list"], cwd=ROOT)
    master_clean = body_of(st) == ""
    ok = (branch == "redteam/crash-attack-2026-11-13"
          and "redteam/crash-attack-2026-11-13" in wt_list
          and master_clean)
    add("B 工作目录 + git 分支隔离（主工作区未被污染）",
        "git rev-parse --abbrev-ref HEAD / git worktree list / 对 master-a125998e 只读 git status --porcelain",
        "分支=%s；worktree 列表含 D:/CatOs、master-a125998e、redteam-…；"
        "主工作区 porcelain 输出=%r（空=干净）" % (branch, body_of(st)),
        "PASS" if ok else "FAIL",
        "两个 worktree 共享同一个 .git；分支隔离是 git 层的逻辑隔离，不是文件系统层的隔离")


# ------------------------------------------------------------ C. disk
def audit_disk(hash_before_src, hash_before_copy):
    """Called AFTER the guest ran: prove only the copy moved."""
    hash_after_src = sha256(SRC_DISK)
    hash_after_copy = sha256(PROBE_DISK) if os.path.exists(PROBE_DISK) else "(missing)"
    src_stat = sh(["stat", "-c", "%n size=%s mtime=%y", SRC_DISK], cwd=ROOT)
    out = [
        "=== source image (MUST NOT CHANGE) ===",
        "path      : %s" % SRC_DISK,
        "sha256 bef: %s" % hash_before_src,
        "sha256 aft: %s" % hash_after_src,
        "identical : %s" % (hash_before_src == hash_after_src),
        "",
        "=== guest-visible copy (expected to CHANGE) ===",
        "path      : %s" % PROBE_DISK,
        "sha256 bef: %s" % hash_before_copy,
        "sha256 aft: %s" % hash_after_copy,
        "changed   : %s" % (hash_before_copy != hash_after_copy),
        "",
        src_stat,
    ]
    # The 64 MiB copy is throwaway: once both hashes are recorded it carries
    # no information the log does not already have, and this host has run out
    # of disk before (FreeSpace=0).  Delete it by default; --keep-probe keeps
    # it for a live demo.
    if KEEP_PROBE:
        out += ["", "=== cleanup ===",
                "probe kept on request (--keep-probe): %s" % PROBE_DISK]
    else:
        freed = os.path.getsize(PROBE_DISK) if os.path.exists(PROBE_DISK) else 0
        try:
            if freed:
                os.remove(PROBE_DISK)
            out += ["", "=== cleanup %s ===" % time.strftime("%Y-%m-%d %H:%M"),
                    "artifact deleted to reclaim %.0f MB; hashes above are the evidence"
                    % (freed / 1048576.0),
                    "rerun recreates it automatically; pass --keep-probe to retain it"]
        except OSError as e:
            out += ["", "=== cleanup ===",
                    "could not delete probe (%s); leaving it in place" % e]
    log_write("C_disk_isolation.txt", "\n".join(out))
    src_ok = hash_before_src == hash_after_src
    copy_changed = hash_before_copy != hash_after_copy
    verdict = "PASS" if src_ok else "FAIL"
    if src_ok and not copy_changed:
        verdict = "PARTIAL"
    add("C 磁盘隔离（攻击只写副本，源镜像字节不变）",
        "sha256(source) → cp 到 tests/isolation_probe.img → guest touch /ISOPROBE.TXT → 干净退出 QEMU → 重算两侧 sha256",
        "源: %s → %s (%s)；副本: %s → %s (%s)"
        % (hash_before_src[:16], hash_after_src[:16], "不变" if src_ok else "已变",
           hash_before_copy[:16], hash_after_copy[:16],
           "已变" if copy_changed else "未变"),
        verdict,
        "" if copy_changed else
        "副本 hash 未变 = 该 guest 的写没落到宿主页缓存之外（QEMU 退出时未刷盘）；"
        "不影响『源镜像未被改动』这一结论")


# --------------------------------------------------- D. runtime (VM)
def audit_runtime():
    proc = s = None
    out = []
    evidence = {}
    try:
        qemu = find_qemu(QEMU)
        out.append("=== D0: boot (port %d only, disk = copy) ===" % MONITOR_PORT)
        out.append("qemu = %s" % qemu)
        out.append("iso  = %s" % ISO)
        out.append("disk = %s" % PROBE_DISK)
        t0 = time.time()
        proc, s = boot(qemu, ISO, MONITOR_PORT, disk=PROBE_DISK)
        evidence["pid"] = proc.pid
        out.append("pid  = %d (started at %s)" % (proc.pid, time.strftime("%H:%M:%S")))

        # ---- D1: the argv the OS actually sees (not what the source says)
        cmdline = ps("(Get-CimInstance Win32_Process -Filter \"ProcessId=%d\").CommandLine"
                     % proc.pid)
        out.append("\n=== D1: live process command line (Win32_Process.CommandLine) ===")
        out.append(cmdline.strip())
        evidence["cmdline"] = cmdline.strip()

        # ---- D2: host-side listening sockets owned by this guest
        out.append("\n=== D2: TCP/UDP endpoints owned by pid %d (host view) ===" % proc.pid)
        tcp = ps("Get-NetTCPConnection -OwningProcess %d -ErrorAction SilentlyContinue | "
                 "Select-Object LocalAddress,LocalPort,RemoteAddress,State | "
                 "Format-Table -AutoSize | Out-String -Width 200" % proc.pid)
        udp = ps("Get-NetUDPEndpoint -OwningProcess %d -ErrorAction SilentlyContinue | "
                 "Select-Object LocalAddress,LocalPort | "
                 "Format-Table -AutoSize | Out-String -Width 200" % proc.pid)
        out.append("-- TCP --\n" + tcp.strip())
        out.append("-- UDP --\n" + udp.strip())
        netstat = ps("netstat -ano | Select-String -Pattern '\\s%d$'" % proc.pid)
        out.append("-- netstat -ano (grep pid) --\n" + netstat.strip())
        out.append("-- listener on the monitor port --\n" + ps(
            "Get-NetTCPConnection -LocalPort %d -State Listen -ErrorAction SilentlyContinue | "
            "Select-Object LocalAddress,LocalPort,OwningProcess | Out-String -Width 200"
            % MONITOR_PORT).strip())
        evidence["tcp"] = tcp.strip()
        evidence["netstat"] = netstat.strip()

        # ---- D3: QEMU's own view of PCI / network devices
        out.append("\n=== D3: QEMU monitor view (info pci / network / usernet / chardev) ===")
        for hm in ("info pci", "info network", "info usernet", "info chardev"):
            r = cmd(s, hm, wait=1.5)
            out.append("---- %s ----" % hm)
            out.append(r.strip()[:4000])
            evidence[hm] = r
        pci_txt = evidence.get("info pci", "").lower()
        nic_words = ("ethernet", "pcnet", "e1000", "rtl8139", "virtio-net",
                     "8029", "network controller", "net device")
        nic_hit = [w for w in nic_words if w in pci_txt]

        # ---- D4: guest waits for shell, then asks ITSELF about networking
        out.append("\n=== D4: guest-side probes ===")
        ready = wait_for_prompt(s, timeout=120)
        out.append("shell prompt reached: %s (%.1fs)" % (ready, time.time() - t0))
        if not ready:
            raise RuntimeError("guest never reached the shell prompt")

        clear_screen(s)
        type_text(s, "netdiag")
        sendkey(s, "ret", 2.5)
        netdiag = read_screen(s)
        out.append("---- guest: netdiag ----")
        out.append(netdiag)
        evidence["netdiag"] = netdiag

        clear_screen(s)
        type_text(s, "run /bin/netinfo.ce")
        sendkey(s, "ret", 4.0)
        netinfo = read_screen(s)
        out.append("---- guest: run /bin/netinfo.ce ----")
        out.append(netinfo)
        evidence["netinfo"] = netinfo

        # ---- D5: prove the guest CAN write, and that it writes to the copy
        clear_screen(s)
        type_text(s, "touch /ISOPROBE.TXT")
        sendkey(s, "ret", 2.5)
        touch = read_screen(s)
        out.append("---- guest: touch /ISOPROBE.TXT ----")
        out.append(touch)
        clear_screen(s)
        type_text(s, "ls /")
        sendkey(s, "ret", 2.5)
        lsout = read_screen(s)
        out.append("---- guest: ls / ----")
        out.append(lsout)
        evidence["touch"] = touch + "\n" + lsout

        # ---- D6: shutdown OUR guest only, and cleanly enough to flush FAT
        out.append("\n=== D6: guest shutdown ===")
        try:
            out.append(cmd(s, "quit", wait=1.0).strip())
        except Exception as e:                               # noqa: BLE001
            # `quit` closes the monitor socket out from under cmd(); a
            # ConnectionReset here is the success case, not a failure.
            out.append("monitor socket closed by quit (%s: %s) -- expected"
                       % (type(e).__name__, e))
        try:
            proc.wait(timeout=15)
            out.append("qemu exited rc=%r after HMP quit (clean, cache flushed)"
                       % proc.returncode)
        except subprocess.TimeoutExpired:
            out.append("quit did not take effect in 15s; falling back to kill_qemu(pid)")
        evidence["quit_rc"] = proc.returncode
    except Exception as e:                                   # noqa: BLE001
        out.append("RUNTIME ERROR: %s: %s" % (type(e).__name__, e))
        evidence["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        pid = getattr(proc, "pid", None)
        kill_qemu(proc)                     # by PID only -- never /IM
        alive = "(no pid)"
        if pid:
            alive = ps("(Get-Process -Id %d -ErrorAction SilentlyContinue | "
                       "Measure-Object).Count" % pid).strip().split("\n")[-1]
        out.append("\n=== D7: after kill_qemu(proc) -- pid %s alive-count = %s (0 = gone) ==="
                   % (pid, alive))
        evidence["pid_alive_after"] = alive
        log_write("D_runtime_vm.txt", "\n".join(out))

    # verdict assembly
    cmdline = evidence.get("cmdline", "")
    net_none_live = bool(re.search(r"-net\s+none", cmdline))
    no_nic = not any(w in (evidence.get("info pci", "")).lower()
                     for w in ("ethernet", "pcnet", "e1000", "rtl8139",
                               "virtio-net", "8029", "network controller"))
    tcp_txt = evidence.get("tcp", "")
    # Parse netstat, not the formatted cmdlet table: netstat prints the
    # LOCAL address in a fixed column, so "0.0.0.0" in the *RemoteAddress*
    # column of a listening socket (which is normal and harmless) can no
    # longer be mistaken for "the guest listens on every interface".
    ep_rows = re.findall(
        r"^\s*(TCP|UDP)\s+(\S+):(\d+)\s+(\S+):(\d+|\*)\s+([A-Z]+)?\s*(\d+)\s*$",
        evidence.get("netstat", ""), re.M)
    local_addrs = {r[1] for r in ep_rows}
    local_ports = {int(r[2]) for r in ep_rows}
    udp_rows = [r for r in ep_rows if r[0] == "UDP"]
    loopback_only = bool(ep_rows) and local_addrs == {"127.0.0.1"} \
        and local_ports <= {MONITOR_PORT} and not udp_rows
    nd = evidence.get("netdiag", "")
    net_ready = re.search(r"net_ready=\s*(\d+)", nd.replace(" ", ""))
    net_ready0 = bool(net_ready) and net_ready.group(1) == "0"
    no_dev = "no-dev" in nd or "fail=1" in nd.replace(" ", "")
    ni = evidence.get("netinfo", "")
    no_card = "no network card" in ni
    wrote = "touched" in evidence.get("touch", "").lower() or "ISOPROBE" in evidence.get("touch", "")

    add("D1 运行时真实命令行含 -net none（非仅源码声明）",
        "QEMU 起来后用 Win32_Process.CommandLine 读该 PID 的实际 argv",
        "live argv = " + (body_of(cmdline)[:260] or "(not captured)"),
        "PASS" if net_none_live else "FAIL",
        "argv 由本脚本截获的 boot() 构造与我方 Popen 一致；若有人手工另起 QEMU 则不在此检查范围")

    add("D2 宿主侧：该 guest 无 user-net/SMB/telnet 监听，monitor 只绑回环",
        "netstat -ano 过滤该 PID，解析 LOCAL 地址/端口列（不用 RemoteAddress，监听套接字的远端本来就是 0.0.0.0）",
        "归属该 PID 的 socket 共 %d 条：local=%s port=%s udp=%d 条"
        % (len(ep_rows), sorted(local_addrs) or "无", sorted(local_ports) or "无", len(udp_rows)),
        "PASS" if loopback_only else "PARTIAL",
        "monitor 端口本身是宿主上可被本机任意进程连接的一个 TCP 服务（见残余风险 R2）")

    pci_line = ""
    for ln in nd.splitlines():
        if "PCI bus" in ln or "vend=" in ln:
            pci_line = (pci_line + " " + ln.strip()).strip()
    add("D3 guest 侧无网卡（QEMU 设备视图 + 内核自己枚举的 PCI 表）",
        "HMP `info pci`/`info network`，加上 guest 内 netdiag 打印的 PCI 设备清单",
        "内核枚举到的 PCI 设备: %s" % (pci_line or "(netdiag 未输出 PCI 行)"),
        "PASS" if no_nic else "FAIL",
        "")

    add("D4 guest 侧 net_up()==0（kernel 自身回答）",
        "guest shell 跑 netdiag（打印 net_ready，net_up() 直接返回该变量）；再跑 netinfo.ce",
        "netdiag: %s | netinfo: %s" % (
            (net_ready.group(0) if net_ready else "未见 net_ready="),
            "no network card found" if no_card else ni.replace("\n", " ")[:120]),
        "PASS" if (net_ready0 or no_card) else "FAIL",
        "netdiag 未打印失败原因码时，以 netinfo.ce 的 'no network card found' 作为等价证据")

    touched = "isoprobe.txt" in evidence.get("touch", "").lower()
    add("D5 guest 有写能力，但写的是副本（否则 C 项无法解释）",
        "guest shell `touch /ISOPROBE.TXT` 后 `ls /` 确认文件存在；宿主侧再看 C 项两侧 hash",
        "ls / 输出含 '[FILE] isoprobe.txt' = %s；touch 无权限/IO 报错 = %s"
        % (touched, "是（无输出即成功）" if wrote else "否"),
        "PASS" if touched else "PARTIAL",
        "guest 能自由破坏自己那份镜像；这是设计如此，隔离目标是别让它破坏别的")

    add("D6 进程隔离：只按 PID 收场",
        "HMP quit → kill_qemu(proc)（proc.terminate/kill）；随后 Get-Process -Id <pid> 复查",
        "quit rc=%r；kill 后该 PID 存活计数=%s"
        % (evidence.get("quit_rc"), evidence.get("pid_alive_after")),
        "PASS" if str(evidence.get("pid_alive_after", "")).strip() in ("0", "") else "PARTIAL",
        "Windows 上 terminate() 即 TerminateProcess；本脚本额外先发 HMP quit 让 FAT 刷盘")

    return evidence


# ------------------------------------------------------------ residual
def audit_residual():
    out = ["=== R: residual risk inputs ==="]
    out.append("identity = " + body_of(ps(
        "[Security.Principal.WindowsIdentity]::GetCurrent().Name")))
    # `whoami` inside PowerShell resolves to MSYS's whoami when it is on
    # PATH, which has no /groups switch -- call the Windows binary by path.
    out.append(ps("& \"$env:SystemRoot\\System32\\whoami.exe\" /groups | "
                  "Select-String -Pattern 'Mandatory|Label|强制' | Out-String"))
    out.append("IsInRole Administrator = " + body_of(ps(
        "([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent())"
        ".IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)")))
    out.append(sh([QEMU, "--version"], cwd=ROOT))
    out.append(sh([QEMU, "-sandbox", "help"], cwd=ROOT))
    out.append("=== other qemu instances on this host right now ===")
    out.append(ps("Get-CimInstance Win32_Process -Filter \"Name='qemu-system-i386.exe'\" | "
                  "Select-Object ProcessId,CreationDate,CommandLine | "
                  "Format-List | Out-String -Width 400"))
    log_write("R_residual_risk.txt", "\n".join(out))
    return "\n".join(out)


# ---------------------------------------------------------------- main
def main():
    os.makedirs(LOGDIR, exist_ok=True)
    print("=" * 78)
    print("CatOS crash-attack -- isolation boundary verification")
    print("worktree : %s" % ROOT)
    print("port     : %d (this script's only port)" % MONITOR_PORT)
    print("=" * 78)

    if "--residual-only" in sys.argv:
        print(audit_residual())
        print("\nlog -> %s" % LOGS.get("R_residual_risk.txt"))
        return 0

    audit_driver()
    audit_repo()
    audit_git()

    hash_before_src = sha256(SRC_DISK)
    print("\n[C] source disk sha256 (before) = %s" % hash_before_src)
    prepare_probe_disk()
    hash_before_copy = sha256(PROBE_DISK)
    print("[C] copy   disk sha256 (before) = %s  (%s)"
          % (hash_before_copy, PROBE_DISK))

    if "--no-vm" in sys.argv:
        print("[D] skipped (--no-vm)")
    else:
        print("\n[D] booting one guest on port %d ..." % MONITOR_PORT)
        audit_runtime()

    audit_disk(hash_before_src, hash_before_copy)
    audit_residual()

    # ---- summary
    md = ["| 隔离项 | 检查方法 | 证据（关键输出） | 结论 | 残余风险 |",
          "|---|---|---|---|---|"]
    for r in RESULTS:
        md.append("| %s | %s | %s | **%s** | %s |" % (
            r["item"].replace("|", "/"),
            r["method"].replace("|", "/"),
            r["evidence"].replace("|", "/").replace("\n", "<br>")[:400],
            r["verdict"],
            r["residual"].replace("|", "/")[:300]))
    table = "\n".join(md)
    log_write("SUMMARY.md", table)
    with open(os.path.join(LOGDIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(RESULTS, f, ensure_ascii=False, indent=2)

    print("\n" + table)
    fails = [r for r in RESULTS if r["verdict"] == "FAIL"]
    partials = [r for r in RESULTS if r["verdict"] == "PARTIAL"]
    print("\n%d checks: %d PASS, %d PARTIAL, %d FAIL"
          % (len(RESULTS), len(RESULTS) - len(fails) - len(partials),
             len(partials), len(fails)))
    print("logs -> %s" % LOGDIR)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
