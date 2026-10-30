#!/usr/bin/env python3
import socket, time, subprocess, sys, os, re

OUT='/home/user/Doubao/chats/38448438262014978/catos-qemu'
ISO=OUT+'/Catos_Better_better/Catos_Better_better/catos.iso'
RAW='/tmp/disk-raw.img'
QEMU='/home/user/Doubao/chats/38444715132904962/_qemu/pkg3/usr/bin/qemu-system-i386'
LD='LD_LIBRARY_PATH=/home/user/Doubao/chats/38444715132904962/_qemu/libs/all:/home/user/Doubao/chats/38444715132904962/_qemu/libs/x/usr/lib'
QL='/home/user/Doubao/chats/38444715132904962/_qemu/libs/x/usr/share/qemu'

subprocess.run(['pkill','-9','-x','qemu-system-i386'],stderr=subprocess.DEVNULL)
time.sleep(3)
cmd=('%s %s -L %s -m 128 -no-reboot -no-shutdown -net none '
     '-drive file=%s,format=raw -cdrom %s -boot order=d -display none '
     '-monitor tcp:127.0.0.1:4444,server,nowait'%(LD,QEMU,QL,RAW,ISO))
q=subprocess.Popen(cmd,shell=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def connect():
    for i in range(15):
        try: return socket.create_connection(('127.0.0.1',4444),timeout=5)
        except Exception: time.sleep(3)
    raise SystemExit('no monitor')
s=connect()
time.sleep(20)  # let kernel boot to shell
def send(c,w=0.5):
    s.sendall(c.encode()+b'\n'); time.sleep(w)
    d=b''
    while True:
        try:
            s.settimeout(0.3); x=s.recv(65536)
            if not x: break
            d+=x
        except: break
    return d.decode('latin1','replace')
def shot(n):
    time.sleep(1.0)
    send('screendump %s/probe_%s.ppm'%(OUT,n),1.0)
    time.sleep(1.5)
def read_vga():
    # read 80*2*25 = 4000 bytes as words; extract char bytes (0 and 2)
    txt=''
    raw=send('xp /%d 0xb%04x'%(4000//2,0x8000),0.8)
    for line in raw.splitlines():
        m=re.search(r':\s+(.*)',line)
        if not m: continue
        for wd in m.group(1).split():
            if re.fullmatch(r'0x[0-9a-f]{8}',wd):
                v=int(wd,16)
                lo=v&0xFF; hi=(v>>16)&0xFF
                txt+=chr(lo)+chr(hi)
    rows=[]
    for r in range(25):
        rows.append(txt[r*80:(r+1)*80].rstrip(' '))
    return '\n'.join(x for x in rows if x)

print('=== BEFORE TYPING ===')
print(read_vga())
shot('before')
# press enter once to settle at prompt
send('sendkey ret',1.0); time.sleep(1)
print('=== AFTER RET ===')
print(read_vga())
# type ls
for ch in 'ls':
    send('sendkey %s'%ch,0.2)
send('sendkey ret',0.5); time.sleep(3)
print('=== AFTER "ls"+enter ===')
print(read_vga())
shot('after_ls')
print('shots written')
s.close(); q.terminate()
