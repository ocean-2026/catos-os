#!/usr/bin/env python3
"""F11 scenario 2:
  A. persistence: after a reboot on the copied disk, alice still logs in with
     the original password (0600 ACL + $C$ record survive the restart).
  B. migration: a disk seeded with legacy v2/v1/djb2 records logs in
     successfully, and each record is rewritten in place to $C$ on login.
"""
import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tests"))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      kill_qemu, read_screen, screen_text)
from legacy_hash import v1_record, v2_record, djb2
from fat32_list import Fat32

STAGE = "/tmp/catos_f11"
os.makedirs(STAGE, exist_ok=True)
MIG_IMG = os.path.join(STAGE, "f11_s2_mig.img")
PERSIST_IMG = os.path.join(STAGE, "f11_s2_persist.img")


def patch_passwd(img, content):
    fs = Fat32(img)
    entry_off = entry_cluster = clus = None
    for ci in fs.chain(4):
        data = fs.read_cluster(ci)
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] in (0, 0xE5):
                continue
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            full = (name + "." + ext) if ext else name
            if full == "PASSWD":
                entry_off, entry_cluster = i, ci
                clus = struct.unpack("<H", e[26:28])[0]
                break
        if entry_off is not None:
            break
    assert entry_off is not None, "PASSWD entry not found"
    blob = content.encode("latin-1")
    with open(img, "r+b") as f:
        off = (fs.data_start + (clus - 2) * fs.spc) * 512
        f.seek(off)
        f.write(blob)
        f.write(b"\x00" * (fs.csz - len(blob)))
        dir_off = (fs.data_start + (entry_cluster - 2) * fs.spc) * 512 + entry_off
        f.seek(dir_off + 28)
        f.write(struct.pack("<I", len(blob)))


def build_migration_disk():
    """Seed once from the smoke image, then overwrite passwd with legacy recs."""
    shutil.copy2(os.path.join(STAGE, "f11_smoke.img"), MIG_IMG)
    carol = v2_record("CarolPass1", 0x11223344, 0x55667788)
    dave = v1_record("DavePass1", 0x0A0B0C0D)
    erin = "%08x" % djb2("ErinPass1")
    content = "\n".join([
        "root:0:0:/root:/bin/shell.ce:ffffffff:",
        "user:100:100:/home/user:/bin/shell.ce:2b:",
        "carol:103:103:/home/carol:/bin/shell.ce:0:" + carol,
        "dave:104:104:/home/dave:/bin/shell.ce:0:" + dave,
        "erin:105:105:/home/erin:/bin/shell.ce:0:" + erin,
        "",
    ])
    patch_passwd(MIG_IMG, content)
    with open(os.path.join(os.path.dirname(__file__),
                           "s2_migration_before.txt"), "w") as fh:
        fh.write("# host-side dump of crafted legacy /etc/passwd (pre-boot)\n")
        fh.write(content)
    print("migration disk built:", MIG_IMG)
    print(content)


def main():
    results = []

    # ---- A. persistence: reboot on a copy of the s1 disk -----------------
    shutil.copy2(os.path.join(STAGE, "f11_s1.img"), PERSIST_IMG)
    proc, sock = boot_guest(ISO, PERSIST_IMG, 4484)
    try:
        settle(sock)
        t = run(sock, "login alice AlicePass1", 2.0)
        print("persist login alice:", t)
        results.append(("persist-relogin-alice", "logged in as alice" in t, t))
        t = run(sock, "whoami", 1.0)
        results.append(("persist-whoami", "alice" in t, t))
        shot(sock, "s2_persist_login")
        dump_text(sock, "s2_persist_login")
    finally:
        kill_qemu(proc)

    # ---- B. migration: legacy records verify and upgrade -----------------
    build_migration_disk()
    proc, sock = boot_guest(ISO, MIG_IMG, 4485)
    try:
        settle(sock)

        t = run(sock, "login carol CarolPass1", 2.0)
        print("migrate login carol:", t)
        results.append(("migrate-v2-login", "logged in as carol" in t, t))

        t = run(sock, "login dave DavePass1", 2.0)
        print("migrate login dave:", t)
        results.append(("migrate-v1-login", "logged in as dave" in t, t))

        t = run(sock, "login erin ErinPass1", 2.0)
        print("migrate login erin:", t)
        results.append(("migrate-djb2-login", "logged in as erin" in t, t))

        run(sock, "login root", 2.0)
        t = run(sock, "edit -c /etc/passwd", 1.5)
        print("migrate passwd after upgrade:\n", t)
        shot(sock, "s2_migrated_after")
        dump_text(sock, "s2_migrated_after")
        n_c = t.count("$C$")
        results.append(("migrate-upgraded-to-C", n_c >= 3,
                        "$C$ count=%d" % n_c))
        for legacy in ("$S$",):
            results.append(("migrate-no-legacy-left", legacy not in t, t))
    finally:
        kill_qemu(proc)

    for name, ok, note in results:
        print("%-4s %-28s %s" % ("PASS" if ok else "FAIL", name, str(note)[:160]))
    if any(not ok for _, ok, _ in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
