# LFN smoke: fresh disk, create/read/list/delete a long-named file via shell.
# Usage: python tests/lfn_smoke.py <iso> [port]
import subprocess, socket, time, os, sys, zlib, struct
sys.path.insert(0, os.path.dirname(__file__))
from qemu_regress import kill_qemu

iso = os.path.abspath(sys.argv[1])
port = int(sys.argv[2]) if len(sys.argv) > 2 else 4491
import tempfile
disk = os.path.join(tempfile.gettempdir(), "lfn_smoke_disk.img")
# build a fresh 64MiB data disk
with open(disk, "wb") as f:
    f.truncate(64 * 1024 * 1024)

qemu = r"C:\qemu-portable\qemu-system-i386.exe"
proc = subprocess.Popen([qemu, "-m", "256", "-no-reboot", "-net", "none",
    "-cdrom", iso, "-boot", "order=d",
    "-drive", "file=%s,format=raw,if=ide" % disk,
    "-display", "none",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def mcmd(s, c, wait=0.5):
    s.sendall((c + "\n").encode()); time.sleep(wait)
    s.setblocking(False)
    try: s.recv(65536)
    except (BlockingIOError, ConnectionResetError): pass
    s.setblocking(True)

KEYMAP = {'/': 'slash', ' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus', '~': 'shift-grave', '>': 'shift-dot', '=': 'equal', ';': 'semicolon'}
def type_cmd(s, cmd):
    for ch in cmd:
        k = KEYMAP.get(ch, ch)
        mcmd(s, "sendkey %s" % k, 0.15)
    mcmd(s, "sendkey ret", 0.8)

def shot(s, f):
    mcmd(s, "screendump tests/qemu_shots/%s" % f.replace(".png", ".ppm"), 1.2)
    time.sleep(0.8)

s = None
for _ in range(25):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5); break
    except OSError: time.sleep(2)
assert s, "monitor unreachable"
mcmd(s, "", 0.3)
time.sleep(10)          # boot; fresh disk -> no autostart, "format" hint

type_cmd(s, "format")
type_cmd(s, "y")   # confirm erase
time.sleep(6)           # mkfs a fresh volume
type_cmd(s, "clear")
# 1) long name create + read back
type_cmd(s, "touch /my-report-2026.md")
shot(s, "lfn1.png")
# 2) ls shows the long name
type_cmd(s, "ls /")
shot(s, "lfn2.png")
# 3) cat-like verify via run-independent: use write/read through 'cat'? CatOS has no cat.
#    verify via reading through the editor-free path: 'syswrite' not needed; use 'ls' + open by shell echo append
type_cmd(s, "mkdir /a-very-long-directory-name")
type_cmd(s, "ls /")
shot(s, "lfn3.png")
# 4) delete
type_cmd(s, "rm /my-report-2026.md")
type_cmd(s, "ls /")
shot(s, "lfn4.png")
try: mcmd(s, "quit", 0.5)
except Exception: pass
time.sleep(1)
kill_qemu(proc)

def ppm2png(src, dst):
    d = open(src, 'rb').read()
    idx = 0; tok = []
    while len(tok) < 4:
        if d[idx:idx+1].isspace(): idx += 1; continue
        if d[idx:idx+1] == b'#':
            while d[idx:idx+1] not in (b'\n', b''): idx += 1
            continue
        j = idx
        while not d[j:j+1].isspace(): j += 1
        tok.append(d[idx:j]); idx = j
    w, h, mv = int(tok[1]), int(tok[2]), int(tok[3])
    idx += 1
    raw = d[idx:idx+w*h*3]
    rows = b''.join(b'\x00' + raw[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)

for n in ("lfn1", "lfn2", "lfn3", "lfn4"):
    try: ppm2png('tests/qemu_shots/%s.ppm' % n, 'tests/qemu_shots/%s.png' % n)
    except FileNotFoundError: print("missing", n)
print("done")
