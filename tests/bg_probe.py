"""Live probe: background commands (`run ... &`, `jobs`, `log TID`).

Confirms the shell's #59 background-output capture still works after the
v2.47 ABI extension: a CE app started with '&' keeps running while the
shell stays responsive, `jobs` lists it, and `log TID` drains its output.
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, sendkey,
                         type_text, wait_for_prompt, screen_text)

ISO = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
SHOTS = os.path.join(HERE, "qemu_shots_bg")
PORT = 4498
os.makedirs(SHOTS, exist_ok=True)

disk = os.path.join(SHOTS, "disk.img")
if not os.path.exists(disk):
    open(disk, "wb").truncate(64 * 1024 * 1024)

p, s = boot(find_qemu(), ISO, PORT, disk=disk,
            log=os.path.join(SHOTS, "qemu.log"))
try:
    time.sleep(9)
    type_text(s, "text"); sendkey(s, "ret"); time.sleep(0.6)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(1.5)
    if not wait_for_prompt(s, 30):
        raise RuntimeError("no shell prompt after text switch")
    type_text(s, "format"); sendkey(s, "ret"); time.sleep(0.4)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(5)

    # 60 ticks * 500 ms = ~30 s of background work; shell prompt must return
    # immediately even while the job is still alive.
    type_text(s, "run /bin/spinner.ce 60 &"); sendkey(s, "ret"); time.sleep(0.3)
    prompt = screen_text(s)
    assert "root@catos:" in prompt.splitlines()[-1], "shell blocked on '&' job"

    type_text(s, "jobs"); sendkey(s, "ret"); time.sleep(0.3)
    jobs = screen_text(s)
    print("=== jobs ===\n" + jobs[-900:])
    assert "STATE" in jobs and "spinner" in jobs, "jobs did not list the background task"

    m = re.search(r"(\d+)\s+sleep\s+spinner", jobs)
    assert m, "could not read spinner TID from jobs"
    tid = m.group(1)
    time.sleep(3)                         # let the job emit several ticks

    type_text(s, "log " + tid); sendkey(s, "ret"); time.sleep(0.8)
    log = screen_text(s)
    print("=== log %s ===\n" % tid + log[-1400:])
    assert "tick" in log and "spinner" in log, "captured log missing spinner output"

    print("BACKGROUND COMMAND LIVE PROBE PASS (tid=%s)" % tid)
finally:
    kill_qemu(p)
