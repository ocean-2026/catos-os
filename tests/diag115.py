#!/usr/bin/env python3
"""diag115.py -- chase the intermittent triple-fault seen during the
size-regress flow.  Boots with `-serial file:...` (kernel v2.25 logs an
[intr] breadcrumb for EVERY exception) and replays the regression
sequence until the monitor dies; then dumps the serial log.

usage: python tests/diag115.py <catos.iso> [--qemu PATH] [--rounds N]
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu, sendkey, type_text,
                         wait_for_prompt)

PORT = 4502
SERIAL = "/tmp/catos_serial115.log"


def alive(sock):
    o = cmd(sock, "info version", 0.8)
    return "send failed" not in o and "BrokenPipe" not in o


def sendkey2(sock):
    sendkey(sock, "ret", 0.08)


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    rounds = int(sys.argv[sys.argv.index("--rounds") + 1]) \
        if "--rounds" in sys.argv else 6
    for rnd in range(rounds):
        if os.path.exists(SERIAL):
            os.remove(SERIAL)
        proc, sock = boot(qemu, iso, PORT, extra_args=["-serial", "file:" + SERIAL])
        try:
            wait_for_prompt(sock)
            # replicate the failing regression sequence with fast typing
            for _ in range(4):
                for c, w in (("clear", 0.6), ("display", 1.2)):
                    type_text(sock, c, wait=0.05); sendkey2(sock); time.sleep(w)
            type_text(sock, "gfx24", wait=0.05); sendkey2(sock); time.sleep(3.5)
            type_text(sock, "clear", wait=0.05); sendkey2(sock); time.sleep(1.0)
            type_text(sock, "size normal", wait=0.05); sendkey2(sock); time.sleep(2.0)
            for i in range(20):
                type_text(sock, "echo warm %d" % i, wait=0.05)
                sendkey2(sock); time.sleep(0.3)
            ok = alive(sock)
            print("round %d: %s" % (rnd, "ALIVE" if ok else "DEAD"))
            if not ok:
                time.sleep(2.0)
                if os.path.exists(SERIAL):
                    print("---- serial breadcrumb tail ----")
                    with open(SERIAL, "rb") as fh:
                        print(fh.read().decode("latin-1", "replace")[-2500:])
                else:
                    print("no serial file")
                return 1
        finally:
            kill_qemu(proc)
    print("diag115: no crash in %d rounds" % rounds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
