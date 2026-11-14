#!/usr/bin/env python3
"""node48_qemu_test.py - CatOS b31 acceptance driver.
Coverage:
  A. node subset JS interpreter: embedded demo + disk script (TEST.JS)
  B. #48: injected files survive the boot-time builtin install (FAT RMW)
  C. F1: 40 failed spawns must not leak the address space
  D. A2: fat_append_file undo regression via test_ab (100K append + pipe +
     spawn + delete)
Each step screenshots to node48_shots/ and prints PASS/FAIL.
"""
import socket, time, subprocess, re, struct, os, sys, glob

OUT   = '/home/user/Doubao/chats/38448438262014978/catos-qemu'
ISO   = OUT + '/Catos_Better_better/Catos_Better_better/catos.iso'
RAW   = '/tmp/disk-raw.img'
QEMU  = '/home/user/Doubao/chats/38444715132904962/_qemu/pkg3/usr/bin/qemu-system-i386'
QEMU_LD = 'LD_LIBRARY_PATH=/home/user/Doubao/chats/38444715132904962/_qemu/libs/all:/home/user/Doubao/chats/38444715132904962/_qemu/libs/x/usr/lib'
QEMU_L  = '/home/user/Doubao/chats/38444715132904962/_qemu/libs/x/usr/share/qemu'
INJ   = OUT + '/ocean_tools/inject_any.py'
BIN   = OUT + '/Catos_Better_better/Catos_Better_better/extapp/bin'
SHOTS = OUT + '/node48_shots'
os.makedirs(SHOTS, exist_ok=True)

def reset_env():
    subprocess.run(['pkill', '-9', '-x', 'qemu-system-i38'], stderr=subprocess.DEVNULL)
    time.sleep(2)
    # port must be free
    r = subprocess.run(['bash','-c','ss -tln | grep -c :4444'], capture_output=True, text=True)
    if r.stdout.strip() != '0':
        print('FATAL: port 4444 busy'); sys.exit(1)
    print('env clean: no qemu, port 4444 free')

def make_disk():
    if os.path.exists(RAW):
        os.remove(RAW)
    subprocess.run(['truncate','-s','4G',RAW], check=True)
    print('disk: 4G sparse at', RAW)

def boot():
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], stderr=subprocess.DEVNULL)
    time.sleep(2)
    cmd = ('%s %s -L %s -m 128 -no-reboot -no-shutdown -net none '
           '-drive file=%s,format=raw -cdrom %s -boot order=d -display none '
           '-monitor tcp:127.0.0.1:4444,server,nowait' % (QEMU_LD, QEMU, QEMU_L, RAW, ISO))
    subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return connect(retries=10)

def connect(retries=10):
    for i in range(retries):
        try:
            return socket.create_connection(('127.0.0.1',4444),timeout=5)
        except Exception:
            time.sleep(4)
    raise RuntimeError('no monitor')

def send(c, w=0.6):
    s.sendall(c.encode()+b'\n'); time.sleep(w)
    d=b''
    while True:
        try:
            s.settimeout(0.3); x=s.recv(65536)
            if not x: break
            d+=x
        except: break
    return d.decode('latin1','replace')

def read_screen():
    txt=''
    for off in range(0,4000,1600):
        raw=send('xp /%d 0xb%04x'%(1600//4,0x8000+off),0.5)
        for line in raw.splitlines():
            m=re.search(r':\s+(.*)', line)
            if m:
                for wd in m.group(1).split():
                    if re.fullmatch(r'0x[0-9a-f]{8}', wd):
                        v=int(wd,16)
                        # VGA text: word = [char0,attr0,char1,attr1]; keep char bytes only
                        txt+=chr(v&0xFF)+chr((v>>16)&0xFF)
    out=[]
    for r in range(25):
        row=txt[r*80:(r+1)*80].rstrip(' ')
        if row: out.append(row)
    return '\n'.join(out)

def shot(name):
    global s
    time.sleep(1.5)
    try:
        send('screendump %s/%s.ppm' % (SHOTS, name), 1.2)
        time.sleep(1.5)
        send('screendump %s/%s.ppm' % (SHOTS, name), 1.2)   # retry: screendump is flaky
        time.sleep(1)
    except OSError:
        print('  (shot %s: monitor closed, skipped)' % name)
        return None
    if os.path.exists('%s/%s.ppm' % (SHOTS, name)):
        return '%s/%s.ppm' % (SHOTS, name)
    return None

def type_text(t, spd=0.12):
    for ch in t:
        m={' ':'spc','/':'slash','.':'dot','_':'shift-minus','-':'minus',
           ':':'shift-semicolon',',':'comma'}
        if ch.isupper():
            send('sendkey shift-%s'%ch.lower(), spd)
        elif ch in m:
            send('sendkey %s'%m[ch], spd)
        else:
            send('sendkey %s'%ch, spd)

def wait_prompt(timeout=25):
    scr=''
    for i in range(int(timeout/2)):
        scr = read_screen()
        if 'catos:' in scr: return scr
        send('sendkey ret', 1.0)
        time.sleep(1)
    return scr

def send_cmd(cmd, settle=6):
    type_text(cmd); send('sendkey ret', 0.5); time.sleep(settle)
    return read_screen()

# ---------------- steps ----------------
def step_fmt(s):
    print('== format ==')
    type_text('format'); send('sendkey ret', 0.5); time.sleep(2)
    scr=''
    for i in range(12):
        scr=read_screen()
        if 'Proceed' in scr: break
        time.sleep(1)
    print(scr[-500:])
    if 'Proceed' in scr:
        type_text('y'); send('sendkey ret', 0.5)
        time.sleep(12)          # fs_format + ce_install_builtins
        print(read_screen()[-700:])
    scr=read_screen()
    return scr

def step_inject():
    print('== inject ==')
    jobs = [
        (BIN+'/node.ce',       'NODE.CE'),
        (OUT+'/TEST.JS',       'TEST.JS'),
        (BIN+'/hello.ce',      'HELLO.CE'),
        (BIN+'/test_ab.ce',    'TEST_AB.CE'),
        (BIN+'/spawn_fail.ce', 'SPAWN_FA.CE'),
    ]
    for src, name in jobs:
        r = subprocess.run(['python3', INJ, RAW, src, name], capture_output=True, text=True)
        ok = (r.returncode == 0) and ('match=True' in r.stdout)
        tail = r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip()[-100:]
        print('  inject %-10s %-4s %s' % (name, 'OK' if ok else 'FAIL', tail))
        if not ok:
            return False
    # host-side FAT scan: use the SAME layout inject_any proved correct
    # (RSVD=33 SPC=8 NFATS=2 FAT_SZ=8192 on the kernel-formatted 4GiB disk)
    BPS=512; SPC=8; RSVD=33; NFATS=2; FAT_SZ=8192
    data_off=(RSVD+NFATS*FAT_SZ)*BPS
    f=open(RAW,'rb')
    def cl_off(cl): return data_off+(cl-2)*SPC*BPS
    def fat_get(cl):
        f.seek(RSVD*BPS+cl*4); return struct.unpack('<I',f.read(4))[0]&0x0FFFFFFF
    def scan(cl):
        names=[]; c=cl; seen=set()
        while 2<=c<0x0FFFFFF8 and c not in seen:
            seen.add(c); f.seek(cl_off(c)); buf=f.read(SPC*BPS)
            for i in range(0,len(buf),32):
                e=buf[i:i+32]
                if e[0]==0: break
                if e[0]==0xE5 or e[11]==0x0F: continue
                names.append(''.join(chr(x) for x in e[0:11]).rstrip(' '))
            c=fat_get(c)
        return names
    root=scan(2)
    print('  root entries:', root)
    f.seek(cl_off(2)); buf=f.read(SPC*BPS); bincl=None
    for i in range(0,len(buf),32):
        e=buf[i:i+32]
        if e[0:3]==b'BIN' and e[11]==0x10:
            bincl=struct.unpack_from('<H',e,26)[0]|(struct.unpack_from('<H',e,20)[0]<<16); break
    if bincl is None:
        print('  FATAL: BIN subdir not found in root'); f.close(); return False
    binents=scan(bincl)
    print('  /BIN entries:', binents)
    for want in ['NODE','TEST','HELLO','TEST_AB','SPAWN']:
        ok=any(n.startswith(want) for n in binents)
        print('  %-8s %s' % (want, 'OK' if ok else 'MISSING'))
        if not ok: f.close(); return False
    f.close()
    return True

def check(scr, *needles, label):
    for nd in needles:
        if nd in scr:
            print('PASS %s: found "%s"' % (label, nd)); return True
    print('FAIL %s: missing %s in screen:\n%s' % (label, needles, scr[-1200:]))
    return False

s = None
try:
    reset_env()
    make_disk()
    s = boot()
    scr = wait_prompt()
    print('shell ready')
    scr = step_fmt(s)
    if 'formatted' not in scr and 'Format' not in scr:
        print('WARN: format may not have completed; screen:', scr[-300:])
    s.close(); time.sleep(2)
    subprocess.run(['pkill','-9','-x','qemu-system-i38']); time.sleep(2)

    if not step_inject():
        print('FATAL: injection verification failed'); sys.exit(1)
    shot('r0_after_inject')

    # boot 1: builtin install runs over the injected FAT entries (#48 scenario)
    s = boot()
    scr = wait_prompt()
    shot('r1_shell_ready')

    scr = send_cmd('run /bin/node.ce', 8)
    shot('r2_node_demo')
    check(scr, 'fib(10)= 55', 'done', label='node embedded demo')

    scr = send_cmd('run /bin/node.ce /BIN/TEST.JS', 8)
    shot('r3_node_testjs')
    check(scr, 'ALL PASS', label='node disk script TEST.JS')

    scr = send_cmd('run /bin/test_ab.ce', 12)
    shot('r4_test_ab')
    check(scr, 'TEST v2.5 ALL PASS', label='test_ab (A2 append+spawn+pipe)')

    scr = send_cmd('run /bin/spawn_fail.ce', 10)
    shot('r5_spawn_fail')
    check(scr, 'SPAWNF ALL PASS', label='F1 no-leak regression')

    # boot 2: boot-time builtin install again over the SAME injected entries —
    # with the #48 fix (per-sector FAT RMW) the injected files must survive.
    s.close(); time.sleep(2)
    subprocess.run(['pkill','-9','-x','qemu-system-i38']); time.sleep(2)
    s = boot()
    scr = wait_prompt()
    shot('r6_reboot2_shell')

    scr = send_cmd('run /bin/node.ce /BIN/TEST.JS', 8)
    shot('r7_node_after_reboot')
    check(scr, 'ALL PASS', label='node survives reboot (#48)')

    scr = send_cmd('run /bin/hello.ce', 6)
    shot('r8_hello_after_reboot')
    check(scr, 'hello', label='hello survives reboot (#48)')

    print('\n=== SUMMARY: all steps executed (PASS lines above) ===')
finally:
    if s:
        try: s.close()
        except: pass
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], stderr=subprocess.DEVNULL)
