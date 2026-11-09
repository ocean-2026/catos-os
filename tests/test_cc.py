#!/usr/bin/env python3
"""test_cc.py -- regression suite for the CatOS tiny C compiler (cc.c).

Two layers:

1. HOST LAYER (always runs): cc.c is compiled natively with a shim that
   backs the catos_api_t file/memory calls with the real filesystem.  The
   compiler logic is identical, so this validates the whole pipeline --
   lexer, parser, x86 codegen, CE image layout -- without needing QEMU.

2. SIM LAYER (optional): if ce_sim.py can execute the produced .ce image,
   the compiled program is run and its stdout compared with the expected
   value.  That is what proves the emitted machine code is right, not just
   that the compiler did not crash.

usage:
    python tests/test_cc.py            # build + host compile + sim run
    python tests/test_cc.py --no-sim   # build + host compile only
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTAPP = os.path.join(ROOT, "kernel_v25", "extapp")
TESTS = os.path.join(ROOT, "tests")
WORK = os.path.join(TESTS, "cc_work")

MINGW = r"C:\msys64\mingw32\bin"
MSYS = r"C:\msys64\usr\bin"
SIM_ENV = dict(os.environ, PATH=r"C:\msys64\mingw32\bin;" + os.environ.get("PATH", ""))

# name -> (source, expected stdout when run through ce_sim)
CASES = {
    "hello": ('api->write("hi\\n");',
              "hi\n"),
    "add": ('api->print_int(add(2,3));',
            "5"),
    "arith": ('int a; int b; a = 7; b = 3;'
              ' api->print_int(a - b); api->write(" ");'
              ' api->print_int(a / b); api->write(" ");'
              ' api->print_int(a % b); api->write(" ");'
              ' api->print_int(a * b);',
            "4 2 1 21"),
    "loop": ('int i; int s; s = 0;'
             ' for (i = 0; i < 10; i = i + 1) { if (i == 5) break; s = s + i; }'
             ' api->print_int(s);',
             "10"),
    "forcont": ('int i; int s; s = 0;'
                ' for (i = 0; i < 10; i = i + 1) { if (i == 5) continue; s = s + i; }'
                ' api->print_int(s);',
                "40"),
    "fornostep": ('int i; int s; s = 0; i = 0;'
                  ' for (; i < 5; ) { s = s + i; i = i + 1; }'
                  ' api->print_int(s);',
                  "10"),
    "while": ('int x; x = 1; while (x < 100) { x = x * 2; } api->print_int(x);',
              "128"),
    "recursion": ('api->print_int(fib(10));',
                  "55"),
    "global": ('msg = "global string"; api->write(msg);',
               "global string"),
    "compare": ('int a; int b; a = 5; b = 7;'
                ' api->print_int(a < b); api->print_int(a == b);'
                ' api->print_int(a > b);',
                "100"),
}

PRELUDE = {
    "add": "int add(int a, int b) { return a + b; }\n",
    "recursion": ("int fib(int n) {\n"
                  "    if (n < 2) return n;\n"
                  "    return fib(n - 1) + fib(n - 2);\n"
                  "}\n"),
    "global": "char *msg;\n",
}


def find_tool(name):
    for d in (os.path.join(MINGW, name + ".exe"), os.path.join(MSYS, name + ".exe")):
        if os.path.exists(d):
            return d
    return shutil.which(name)


def build_host_cc():
    """Compile cc.c into a native executable (uses host_shim.c for the api)."""
    gcc = find_tool("gcc")
    if not gcc:
        print("SKIP: no host gcc found")
        return None
    os.makedirs(WORK, exist_ok=True)
    src = os.path.join(EXTAPP, "cc.c")
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        text = f.read().lstrip("?")
    # The live compiler is cc_self.c's content, which has two build shapes:
    #   #ifdef CC_HOST -> the real 57-slot CE ABI (ce_abi.h) + the api slot
    #   #else          -> freestanding type shims, catos_api_t = void
    # The host build wants the CC_HOST shape (it needs the real struct
    # members), so -DCC_HOST is passed on the gcc command line below.
    #
    # the CE-only bits: the api slot section attribute and its definition.
    # The attribute is meaningless on the host, so the definition becomes an
    # extern declaration resolved by tests/host_shim.c.
    text = text.replace(
        'catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;',
        'extern catos_api_t *__ce_api;')
    # the entry attribute means nothing on the host but is harmless
    host_c = os.path.join(WORK, "cc_host.c")
    with open(host_c, "w", encoding="ascii", errors="replace") as f:
        f.write(text)
    # ce_abi.h must be visible for the type definition
    inc = os.path.join(ROOT, "kernel_v25", "include")
    out = os.path.join(WORK, "cc_host.exe")
    # MinGW gcc under MSYS does not accept backslash paths
    def posix(p):
        return p.replace("\\", "/")
    # -DCC_HOST selects the real-ABI branch of the compiler source (see the
    # comment above).  Without it catos_api_t is `typedef void` and every
    # api->write / api->alloc call fails to compile.
    cmd = [gcc, "-O1", "-w", "-DCC_HOST", "-I" + posix(inc), "-o", posix(out),
           posix(host_c), posix(os.path.join(TESTS, "host_shim.c"))]
    # capture to a file instead of pipes: under a restricted sandbox a
    # piped grandchild spawn fails, so redirect stdout/stderr to disk.
    # gcc needs its own directory on PATH when spawned from the harness
    env = dict(os.environ)
    env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
    logp = os.path.join(WORK, "build.log")
    with open(logp, "w") as logf:
        rc = subprocess.call(cmd, stdout=logf, stderr=subprocess.STDOUT, env=env)
    with open(logp, "r", errors="replace") as logf:
        log = logf.read()
    if rc != 0:
        print("HOST BUILD FAILED:\n" + log[-2000:])
        return None
    return out


def ce_header_ok(path):
    with open(path, "rb") as f:
        d = f.read(20)
    if len(d) < 20 or d[:4] != b"CE01":
        return False, "bad magic"
    import struct
    entry, bss, api, img = struct.unpack_from("<IIII", d, 4)
    if img > 0x40000:
        return False, "image %d > CE_MAX_IMAGE" % img
    if bss > 0x40000:
        return False, "bss %d > CE_MAX_BSS" % bss
    return True, "entry=0x%x bss=%d api=0x%x image=%d" % (entry, bss, api, img)


def run_sim(ce, py):
    sim = os.path.join(TESTS, "ce_sim.py")
    with open(os.path.join(WORK, "sim.log"), "w") as f:
        rc = subprocess.call([py, sim, ce], stdout=f, stderr=subprocess.STDOUT, env=SIM_ENV)
    with open(os.path.join(WORK, "sim.log"), "r", errors="replace") as f:
        return rc, f.read()


def main():
    no_sim = "--no-sim" in sys.argv
    py = sys.executable
    print("== building host cc from cc.c ==")
    cc = build_host_cc()
    if not cc:
        return 1

    os.makedirs(WORK, exist_ok=True)
    passed = failed = 0
    for name, (body, expected) in CASES.items():
        pre = PRELUDE.get(name, "")
        src = ("/* %s -- compiler regression case */\n"
               "%s"
               "int ce_main(int argc, char **argv) {\n"
               "    %s\n"
               "    return 0;\n"
               "}\n" % (name, pre, body))
        cfile = os.path.join(WORK, name + ".c")
        cefile = os.path.join(WORK, name + ".ce")
        with open(cfile, "w", encoding="ascii") as f:
            f.write(src)
        with open(os.path.join(WORK, name + ".log"), "w") as f:
            rc = subprocess.call([cc, cfile, cefile], stdout=f, stderr=subprocess.STDOUT, env=SIM_ENV)
        with open(os.path.join(WORK, name + ".log"), "r", errors="replace") as f:
            log = f.read()
        if rc != 0:
            print("FAIL %-10s compile: %s" % (name, log.strip()[-200:]))
            failed += 1
            continue
        ok, info = ce_header_ok(cefile)
        if not ok:
            print("FAIL %-10s header: %s" % (name, info))
            failed += 1
            continue
        if no_sim:
            print("PASS %-10s %s" % (name, info))
            passed += 1
            continue
        rc, out = run_sim(cefile, py)
        # ce_sim appends its own summary line; drop it and compare exactly
        body_out = "".join(l + "\n" for l in out.splitlines()
                           if not l.startswith("[ce_sim]"))
        if "[ce_sim] ERROR" in out:
            print("FAIL %-10s sim: %s" % (name, out.strip().splitlines()[-1]))
            failed += 1
        elif body_out.rstrip("\n") != expected.rstrip("\n"):
            print("FAIL %-10s output=%r expected=%r" % (name, body_out, expected))
            failed += 1
        else:
            print("PASS %-10s output=%r" % (name, body_out))
            passed += 1

    print("\n%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
