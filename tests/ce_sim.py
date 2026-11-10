#!/usr/bin/env python3
"""ce_sim.py -- host-side runner for CatOS .ce images (cc.ce test harness).

Emulates exactly what kernel task_spawn_ce does: the flat image is loaded
at 0x02000000, `bss_size` bytes after it are zero, and the address of the
system-call table is written into the 32-bit slot at `api_offset`.  Only
the instruction subset that cc.ce emits is implemented; an unmodelled
opcode raises, which proves the generated code stays inside the subset.

usage:
    ce_sim.py program.ce arg...          run a .ce with argv
    ce_sim.py --cc cc.ce SRC.C OUT.CE    run cc.ce against a host file

The host filesystem is emulated as a flat name -> bytes dict so cc.ce can
read its source and write its output; written files are dumped to
--dump-dir when given.
"""
import os
import struct
import sys

LOAD_BASE = 0x02000000
MEM_SIZE = 0x1000000           # 16 MiB user memory (image+bss+heap+mmap arena)
STACK_TOP = LOAD_BASE + 0x280000
ARGV_AREA = LOAD_BASE + 0x240000
MMAP_ARENA = 0x2400000 - LOAD_BASE   # host-side stand-in for the kernel's PDE 9 arena
THUNK_BASE = 0x7F000000      # api thunk markers: never a real code address

API_NAMES = [
    "write", "putc", "cls", "readline", "time_str", "fs_ready", "ls",
    "touch", "mkdir", "cat", "save", "alloc", "free", "exit",
    "print_int", "print_hex", "set_fg", "delay", "atoi", "strlen",
    "strcmp", "strcpy", "getch", "file_exists", "fsize", "net_init",
    "net_up", "http_get", "net_dns", "net_udp_send", "net_udp_recv",
    "net_ping", "net_ip", "getuid", "gettid", "kill", "yield",
    "hw_num", "hw_str", "file_read", "file_write", "file_delete",
    "pipe_create", "pipe_write", "pipe_read", "pipe_close", "spawn", "wait",
    "brk",                        # slot 48 (v2.6, not modelled)
    # v2.7..v2.10 (slots 49..56, append-only).  Only mmap/munmap are
    # modelled; the display/palette slots raise "not modelled".
    "display_mode", "display_info", "set_style",
    "set_style_rgb", "set_palette", "palette",
    "mmap", "munmap",
    # v2.11 (slots 57..60, append-only): SB16 audio.  The host has no
    # sound card, so they are modelled as "device absent" rather than
    # raising "not modelled" -- sndtest.ce then takes its skip path and
    # ce_regress stays FAIL=0.
    "snd_info", "snd_play", "snd_stop", "snd_tone",
]

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


class CeError(Exception):
    pass


def sx32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def sx8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def sx16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def trunc_div(a, d):
    """Signed division truncated toward zero, as the x86 idiv does."""
    q = abs(a) // abs(d)
    if (a < 0) != (d < 0):
        q = -q
    return q, a - q * d


def setzsf(m, res):
    """Latch ZF/SF the way x86 arithmetic does: on the written result."""
    m.zf = 1 if (res & 0xFFFFFFFF) == 0 else 0
    m.sf = 1 if res & 0x80000000 else 0


class Machine:
    def __init__(self, image, bss, api_off, files=None, max_steps=3_000_000):
        if len(image) + bss > MEM_SIZE - 0x40000:
            raise CeError("image+bss too large for sim memory")
        self.mem = bytearray(MEM_SIZE)
        self.mem[:len(image)] = image
        self.files = files if files is not None else {}
        self.out = []
        self.heap = (len(image) + bss + 15) & ~15
        # v2.10 anonymous-mmap model: first-fit over free holes in the arena
        self.mmap_holes = [(MMAP_ARENA, MEM_SIZE - MMAP_ARENA)]  # list of (off, size)
        self.regs = dict.fromkeys(REG32, 0)
        self.last_cmp = (0, 0)
        self.cf = 0
        self.of = 0
        self.last_test = (0, 0)
        self.steps = 0
        self.max_steps = max_steps
        self.api_off = api_off
        self.installed = set()
        self.pc = LOAD_BASE
        self.entry = 0
        self.progname = "APP.CE"

    # ---- memory ----
    # ---- memory (bounds-checked: a stray pointer is a bug, not a wrap) ----
    def rb(self, a):
        i = a - LOAD_BASE
        if i < 0 or i >= MEM_SIZE:
            raise CeError("read outside user memory at 0x%08x (esp=0x%08x, pc=0x%08x)" % (a, self.regs["esp"], self.pc))
        return self.mem[i]

    def rw(self, a):
        i = a - LOAD_BASE
        if i < 0 or i + 4 > MEM_SIZE:
            raise CeError("read32 outside user memory at 0x%08x (esp=0x%08x, pc=0x%08x)" % (a, self.regs["esp"], self.pc))
        return struct.unpack_from("<I", self.mem, i)[0]

    def wb(self, a, v):
        i = a - LOAD_BASE
        if i < 0 or i >= MEM_SIZE:
            raise CeError("write outside user memory at 0x%08x (pc=0x%08x)" % (a, self.pc))
        self.mem[i] = v & 0xFF

    def ww(self, a, v):
        i = a - LOAD_BASE
        if i < 0 or i + 4 > MEM_SIZE:
            raise CeError("write32 outside user memory at 0x%08x (pc=0x%08x)" % (a, self.pc))
        struct.pack_into("<I", self.mem, i, v & 0xFFFFFFFF)
    def push(self, v):
        self.regs["esp"] = (self.regs["esp"] - 4) & 0xFFFFFFFF
        self.ww(self.regs["esp"], v)

    def pop(self):
        v = self.rw(self.regs["esp"])
        self.regs["esp"] = (self.regs["esp"] + 4) & 0xFFFFFFFF
        return v

    def cstr(self, a):
        out = bytearray()
        while True:
            c = self.rb(a)
            if not c:
                break
            out.append(c)
            a += 1
        return bytes(out)

    # ---- catos api ----
    def install_api(self):
        """Point the image slot at a table of thunk markers.

        Each slot holds THUNK_BASE+slot, an address outside both the image
        and the stack, so any call through it is unambiguously a host api
        call even when the program has cached the table entry in a register.
        """
        table = ARGV_AREA - 0x1000
        self.ww(LOAD_BASE + self.api_off, table)
        for slot in range(len(API_NAMES)):
            self.ww(table + slot * 4, THUNK_BASE + slot)
        self.api_table = table

    def call_api(self, slot):
        name = API_NAMES[slot] if slot < len(API_NAMES) else "?"
        esp = self.regs["esp"]
        args = [self.rw((esp + 4 + i * 4) & 0xFFFFFFFF) for i in range(4)]
        arg0 = lambda: self.cstr(args[0]).decode("latin-1")  # noqa: E731
        if name == "write":
            self.out.append(arg0()); return 0
        if name == "putc":
            self.out.append(chr(args[0] & 0xFF)); return 0
        if name == "print_int":
            self.out.append(str(sx32(args[0]))); return 0
        if name == "print_hex":
            self.out.append("0x%08x" % args[0]); return 0
        if name == "strlen":
            return len(self.cstr(args[0]))
        if name == "atoi":
            # strtol(s, NULL, 10) truncated to int.  The Go subset
            # interpreter calls it to parse the numeric literals in a
            # .GO source, so without it the interpreter cannot get
            # past the first constant in TEST.GO.
            s = self.cstr(args[0]).decode("latin-1")
            i = 0
            while i < len(s) and s[i] in " \t\n\r\f\v":
                i += 1
            sign = 1
            if i < len(s) and s[i] in "+-":
                if s[i] == "-":
                    sign = -1
                i += 1
            val = 0
            while i < len(s) and s[i].isdigit():
                val = val * 10 + (ord(s[i]) - 48)
                i += 1
            val *= sign
            # clamp to the 32-bit range the guest would wrap to
            val &= 0xFFFFFFFF
            if val >= 0x80000000:
                val -= 0x100000000
            return val
        if name == "time_str":
            s = b"2026-10-30 12:00:00\x00"
            self.mem[args[0] - LOAD_BASE: args[0] - LOAD_BASE + len(s)] = s
            return 0
        if name == "getuid":
            return 0
        if name == "exit":
            raise SystemExit(sx32(args[0]))
        if name == "fsize":
            d = self.files.get(arg0().upper())
            return -1 if d is None else len(d)
        if name == "file_exists":
            return 1 if arg0().upper() in self.files else 0
        if name == "file_write":
            # file_write(path, data, size, mode)
            path = arg0().upper()
            n = sx32(args[2])
            mode = sx32(args[3])
            blob = bytes(self.mem[args[1] - LOAD_BASE: args[1] - LOAD_BASE + n])
            if mode == 0:
                self.files[path] = blob
            else:
                self.files[path] = self.files.get(path, b"") + blob
            return 0
        if name == "file_read":
            # file_read(path, buf, max, offset)
            path = arg0().upper()
            data = self.files.get(path)
            if data is None:
                return -1
            mx = sx32(args[2])
            off = sx32(args[3])
            chunk = data[off: off + mx]
            self.mem[args[1] - LOAD_BASE: args[1] - LOAD_BASE + len(chunk)] = chunk
            return len(chunk)
        if name == "alloc":
            p = LOAD_BASE + self.heap
            self.heap += (sx32(args[0]) + 15) & ~15
            return p
        if name == "free":
            return 0
        if name == "mmap":
            # mmap(addr, len, prot, flags): anonymous, first-fit, 4k granular
            if args[0] != 0:
                raise CeError("mmap: only addr=NULL is modelled")
            ln = (sx32(args[1]) + 0xFFF) & ~0xFFF
            for i, (off, size) in enumerate(self.mmap_holes):
                if size >= ln:
                    self.mmap_holes[i] = (off + ln, size - ln)
                    if self.mmap_holes[i][1] == 0:
                        del self.mmap_holes[i]
                    self.mmap_taken = getattr(self, "mmap_taken", []) + [(off, ln)]
                    return LOAD_BASE + off
            raise CeError("mmap: arena exhausted")
        if name == "munmap":
            off = sx32(args[0]) - LOAD_BASE
            ln = (sx32(args[1]) + 0xFFF) & ~0xFFF
            taken = getattr(self, "mmap_taken", [])
            for i, (o, l) in enumerate(taken):
                if o == off and l == ln:
                    del taken[i]
                    self.mem[off:off + ln] = b"\x00" * ln
                    self.mmap_holes.append((off, ln))
                    self.mmap_holes.sort()
                    # coalesce adjacent holes
                    j = 0
                    while j + 1 < len(self.mmap_holes):
                        (o1, s1), (o2, s2) = self.mmap_holes[j], self.mmap_holes[j + 1]
                        if o1 + s1 == o2:
                            self.mmap_holes[j] = (o1, s1 + s2); del self.mmap_holes[j + 1]
                        else:
                            j += 1
                    return 0
            return -22
        # ---- v2.11 SB16 audio: no device on the host ----
        if name == "snd_info":
            # 0 = no SB16 for SND_INFO_READY; 0 for every other selector
            # is fine too because the app stops at the ready check.
            return 0
        if name == "snd_play" or name == "snd_stop" or name == "snd_tone":
            return -8                     # ENOSYS (no card)
        raise CeError("api '%s' is not modelled by ce_sim" % name)

    # ---- run ----
    def start(self, argc, argv):
        """Push argc/argv like the kernel's initial CE frame and set ESP."""
        self.regs["esp"] = STACK_TOP
        p = ARGV_AREA
        ptrs = []
        for a in argv:
            s = a.encode("latin-1") + b"\x00"
            self.mem[p - LOAD_BASE: p - LOAD_BASE + len(s)] = s
            ptrs.append(p)
            p += len(s)
            p = (p + 3) & ~3
        argv_addr = p
        for q in ptrs:
            self.ww(p, q); p += 4
        self.ww(p, 0)
        # Kernel CE frame (cdecl): the entry is entered with
        #   [esp]   = return address
        #   [esp+4] = argc
        #   [esp+8] = argv
        # so push argv first, then argc, then the return address last.
        self.push(argv_addr)
        self.push(argc)
        self.push(0)                 # fake return address (ends the program)
        return LOAD_BASE + self.entry

    def run(self):
        pc = self.start(0, [])
        raise CeError("use run_from()")

    def run_program(self, argv):
        # A CE program sees its own path as argv[0], exactly like a
        # kernel `run /bin/x.ce a b` invocation.
        argv = [self.progname or "APP.CE"] + list(argv)
        pc = self.start(len(argv), argv)
        while True:
            self.steps += 1
            if self.steps > self.max_steps:
                raise CeError("step limit %d hit at pc=0x%08x"
                              % (self.max_steps, pc))
            # returning into the fake return address (0) ends the program
            if pc == 0:
                return
            pc = self.step(pc)

    # ---- instruction dispatch ----
    def step(self, pc):
        self.pc = pc
        op = self.rb(pc)
        r = self.regs
        if op in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF2, 0xF3):
            return self.step(pc + 1)                 # segment/rep prefixes: no-op
        # mov r32, imm32
        if 0xB8 <= op <= 0xBF:
            self.set_reg(op - 0xB8, self.rw(pc + 1)); return pc + 5
        if 0x40 <= op <= 0x47:                           # inc r32
            v = (self.get_reg(op - 0x40) + 1) & 0xFFFFFFFF
            self.set_reg(op - 0x40, v); setzsf(self, v)
            return pc + 1
        if 0x48 <= op <= 0x4F:                           # dec r32
            v = (self.get_reg(op - 0x48) - 1) & 0xFFFFFFFF
            self.set_reg(op - 0x48, v); setzsf(self, v)
            return pc + 1
        # push/pop r32
        if 0x50 <= op <= 0x57:
            self.push(self.get_reg(op - 0x50)); return pc + 1
        if 0x58 <= op <= 0x5F:
            self.set_reg(op - 0x58, self.pop()); return pc + 1
        if op == 0x68:
            self.push(self.rw(pc + 1)); return pc + 5
        if op == 0x90:
            return pc + 1
        if op == 0xC3:
            ret = self.pop()
            return ret
        if op == 0x99:                                   # cdq
            r["edx"] = 0xFFFFFFFF if (r["eax"] & 0x80000000) else 0
            return pc + 1
        if op == 0x87:                                   # xchg r/m32, r32
            mod, reg, rm, npc, ea = self.modrm(pc + 1)
            if mod == 3:
                a, b = self.get_reg(reg), self.get_reg(rm)
                self.set_reg(reg, b)
                self.set_reg(rm, a)
            else:
                v = self.rw(ea)
                self.ww(ea, self.get_reg(reg))
                self.set_reg(reg, v)
            return npc
        if op == 0xA1:                                   # mov eax,[disp32]
            r["eax"] = self.rw(self.rw(pc + 1)); return pc + 5
        if op == 0xA0:                                   # mov al,[disp32]
            r["eax"] = (r["eax"] & ~0xFF) | (self.rb(self.rw(pc + 1)) & 0xFF)
            return pc + 5
        if op == 0xA2:                                   # mov [disp32],al
            self.wb(self.rw(pc + 1), r["eax"] & 0xFF); return pc + 5
        if op == 0xA3:
            self.ww(self.rw(pc + 1), r["eax"]); return pc + 5
        if op in (0x01, 0x03, 0x09, 0x0B, 0x21, 0x23, 0x29, 0x2B, 0x31,
                  0x33, 0x38, 0x39, 0x3A, 0x3B, 0x84, 0x85, 0x88, 0x89, 0x8A,
                  0x8B, 0x8D):
            return self.op_modrm(op, pc)
        if op == 0xC7:                                   # mov r/m32, imm32
            mod, reg, rm, npc, ea = self.modrm(pc + 1)
            imm = self.rw(npc); npc += 4
            if mod == 3:
                self.set_reg(rm, imm)
            else:
                self.ww(ea, imm)
            return npc
        if op == 0xC6:                                   # mov r/m8, imm8
            mod, reg, rm, npc, ea = self.modrm(pc + 1)
            imm = self.rb(npc); npc += 1
            if mod == 3:
                raise CeError("c6 reg form not emitted by cc")
            self.wb(ea, imm)
            return npc
        if op == 0x83:
            return self.op_group1(op, pc)
        if op in (0x80, 0x81, 0x83):
            return self.op_group1(op, pc)
        if op == 0x88:
            return self.op_mov8(pc)
        if op == 0xF7:
            return self.op_group3(pc)
        if op == 0xF6:
            return self.op_group3(pc, byte_form=True)
        if op == 0xD3:
            return self.op_group2(pc)
        if op in (0xD0, 0xD1):                 # group2, count == 1
            b = self.rb(pc + 1)
            sub = (b >> 3) & 7
            mod, reg, rm, npc, ea = self.modrm(pc + 1)
            v = (self.get_reg(rm) & 0xFF) if op == 0xD0 else self.get_reg(rm)
            if sub == 4:
                res = v << 1
            elif sub == 5:
                res = (v & 0xFFFFFFFF) >> 1
            elif sub == 7:
                res = sx32(v) >> 1
            else:
                raise CeError("group2 /%d count=1" % sub)
            if op == 0xD0:
                self.set_reg(rm, (self.get_reg(rm) & ~0xFF) | (res & 0xFF))
            else:
                self.set_reg(rm, res)
            return npc
        if op in (0xC0, 0xC1):                 # group2, imm8 count
            return self.op_group2(pc, imm8=True)
        if op == 0xFF:
            return self.op_call_indirect(pc)
        if op == 0x69:
            return self.op_imul_imm(pc)
        if op == 0x6B:                                   # imul r32, r/m32, imm8
            return self.op_imul_imm(pc, imm8=True)
        if op == 0xE8 or op == 0xE9:
            rel = sx32(self.rw(pc + 1))
            if op == 0xE8:
                self.push(pc + 5)
                return (pc + 5 + rel) & 0xFFFFFFFF
            return (pc + 5 + rel) & 0xFFFFFFFF
        if op == 0x0F:
            return self.op_0f(pc)
        if op == 0xC9:                                   # leave
            self.regs["esp"] = self.regs["ebp"]
            self.regs["ebp"] = self.pop()
            return pc + 1
        if op == 0xEB:                                   # jmp rel8
            rel = self.rb(pc + 1)
            if rel & 0x80:
                rel -= 0x100
            return (pc + 2 + rel) & 0xFFFFFFFF
            return pc + 1
        if 0x70 <= op <= 0x7F:                           # jcc rel8 (signed sf)
            rel = self.rb(pc + 1)
            if rel & 0x80:
                rel -= 0x100
            if self.cond(op - 0x70):
                return (pc + 2 + rel) & 0xFFFFFFFF
            return pc + 2
        if op == 0xA8:                                   # test al, imm8
            al = self.regs["eax"] & 0xFF
            res = al & self.rb(pc + 1)
            self.last_cmp = (res, 0)
            self.zf = 1 if res == 0 else 0
            self.sf = 1 if res & 0x80 else 0
            return pc + 2
        if op == 0x04:                                   # add al, imm8
            al = (self.regs["eax"] & 0xFF)
            imm = self.rb(pc + 1)
            self.cf = 1 if (al + imm) > 0xFF else 0
            res = (al + imm) & 0xFF
            self.regs["eax"] = (self.regs["eax"] & ~0xFF) | res
            self.zf = 1 if res == 0 else 0
            self.sf = 1 if res & 0x80 else 0
            return pc + 2
        if op in (0x0C, 0x24, 0x2C, 0x34):               # al, imm8 ops
            al = self.regs["eax"] & 0xFF
            imm = self.rb(pc + 1)
            if op == 0x0C:
                al |= imm
            elif op == 0x24:
                al &= imm
            elif op == 0x2C:
                self.cf = 1 if al < (imm & 0xFF) else 0
                al = (al - imm) & 0xFF
            else:
                al ^= imm
            self.regs["eax"] = (self.regs["eax"] & ~0xFF) | al
            self.zf = 1 if al == 0 else 0
            self.sf = 1 if al & 0x80 else 0
            return pc + 2
        if op == 0x3C:                                   # cmp al, imm8
            al = self.regs["eax"] & 0xFF
            imm = self.rb(pc + 1)
            res = (al - imm) & 0xFF
            self.last_cmp = (al - 0x100 if al & 0x80 else al,
                             imm - 0x100 if imm & 0x80 else imm)
            self.zf = 1 if res == 0 else 0
            self.sf = 1 if res & 0x80 else 0
            # CF (unsigned borrow) MUST be maintained here: gcc -O2
            # inlines cc's `is_idc()` helper into
            #     and $0xffffffdf,%eax / sub $0x41,%eax /
            #     cmp $0x19,%al   (0x3C) / jbe   (0x76)
            # and `jbe` (CF|ZF) reads the CF latch.  Leaving CF stale
            # made the simulated lexer take the wrong branch and stall
            # the Go interpreter in its tokenizer loop.
            # Every other `cmp` form below (0x38/0x39/0x3A/0x3D) already
            # maintained CF; 0x3C was the only gap.
            self.cf = 1 if al < (imm & 0xFF) else 0
            self.last_op_bt = False
            return pc + 2
        if op == 0xA9:                                   # test eax, imm32
            res = self.regs["eax"] & self.rw(pc + 1)
            self.last_cmp = (res, 0)
            self.last_test = (res, 0)
            self.zf = 1 if res == 0 else 0
            self.sf = 1 if res & 0x80000000 else 0
            return pc + 5
        if op == 0x05:                                   # add eax, imm32
            a = self.regs["eax"]; b = self.rw(pc + 1)
            self.regs["eax"] = (a + b) & 0xFFFFFFFF
            setzsf(self, self.regs["eax"])
            self.cf = 1 if (a + b) > 0xFFFFFFFF else 0
            return pc + 5
        if op == 0x2D:                                   # sub eax, imm32
            a = self.regs["eax"]; b = self.rw(pc + 1)
            self.regs["eax"] = (a - b) & 0xFFFFFFFF
            setzsf(self, self.regs["eax"])
            self.cf = 1 if (a & 0xFFFFFFFF) < (b & 0xFFFFFFFF) else 0
            return pc + 5
        if op == 0x3D:                                   # cmp eax, imm32
            a = self.regs["eax"]; b = self.rw(pc + 1)
            self.last_cmp = (a, b)
            self.last_op_bt = False
            setzsf(self, a - b)
            self.cf = 1 if (a & 0xFFFFFFFF) < (b & 0xFFFFFFFF) else 0
            return pc + 5
        if op == 0x0D:                                   # or eax, imm32
            self.regs["eax"] |= self.rw(pc + 1)
            setzsf(self, self.regs["eax"])
            return pc + 5
        if op == 0x25:                                   # and eax, imm32
            self.regs["eax"] &= self.rw(pc + 1)
            setzsf(self, self.regs["eax"])
            return pc + 5
        if op == 0x35:                                   # xor eax, imm32
            self.regs["eax"] ^= self.rw(pc + 1)
            setzsf(self, self.regs["eax"])
            return pc + 5
        if 0x40 <= op <= 0x47:                           # inc r32
            self.set_reg(op - 0x40, self.get_reg(op - 0x40) + 1); return pc + 1
        if 0x48 <= op <= 0x4F:                           # dec r32
            self.set_reg(op - 0x48, self.get_reg(op - 0x48) - 1); return pc + 1
        raise CeError("unsupported opcode 0x%02x at 0x%08x" % (op, pc))

    def get_reg(self, i):
        return self.regs[REG32[i]]

    def set_reg(self, i, v):
        self.regs[REG32[i]] = v & 0xFFFFFFFF

    def modrm(self, pc):
        """Decode a modrm at pc; returns (mod, reg, rm, next_pc, ea or None)."""
        b = self.rb(pc)
        mod, reg, rm = b >> 6, (b >> 3) & 7, b & 7
        pc += 1
        if mod == 3:
            return mod, reg, rm, pc, None
        if rm == 4:                                    # SIB
            sib = self.rb(pc); pc += 1
            base, index, scale = sib & 7, (sib >> 3) & 7, sib >> 6
            if base == 5 and mod == 0:                 # disp32, no base reg
                ea = self.rw(pc); pc += 4
            else:
                ea = self.get_reg(base)
            if index != 4:
                ea += self.get_reg(index) << scale
        elif rm == 5 and mod == 0:
            return mod, reg, rm, pc + 4, self.rw(pc)
        else:
            ea = self.get_reg(rm)
        if mod == 1:
            d = self.rb(pc); pc += 1
            ea += d - 0x100 if d & 0x80 else d
        elif mod == 2:
            ea += self.rw(pc); pc += 4
        return mod, reg, rm, pc, ea & 0xFFFFFFFF

    def op_modrm(self, op, pc):
        """Shared modrm opcodes.  `regv` is the reg field, `rmv` the r/m
        operand value; writes go to the correct side per opcode."""
        mod, reg, rm, npc, ea = self.modrm(pc + 1)
        regv = self.get_reg(reg)

        # lea computes the effective address only -- it never dereferences,
        # so it must not touch memory even when the address is unmapped.
        if op == 0x8D:
            if ea is None:
                raise CeError("lea with register operand")
            self.set_reg(reg, ea)
            return npc

        rmv = self.get_reg(rm) if mod == 3 else self.rw(ea)

        def write_rm(v):
            if mod == 3:
                self.set_reg(rm, v)
            else:
                self.ww(ea, v)

        if op == 0x89:                       # mov r/m32, r32
            write_rm(regv)
            return npc
        if op == 0x8B:                       # mov r32, r/m32
            self.set_reg(reg, rmv)
            return npc
            return npc
        if op == 0x39:                       # cmp r/m32, r32
            self.last_cmp = (rmv, regv); setzsf(self, rmv - regv)
            self.cf = 1 if (rmv & 0xFFFFFFFF) < (regv & 0xFFFFFFFF) else 0
            self.last_op_bt = False
            return npc
        if op == 0x85:                       # test r/m32, r32
            self.last_cmp = (rmv & regv, 0); setzsf(self, rmv & regv)
            self.last_op_bt = False
            return npc
        if op == 0x84:                       # test r/m8, r8
            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)
            self.last_cmp = (a & (regv & 0xFF), 0); setzsf(self, a & (regv & 0xFF))
            self.last_op_bt = False
            return npc
        if op == 0x38:                       # cmp r/m8, r8
            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)
            self.last_cmp = (a, regv & 0xFF); setzsf(self, a - (regv & 0xFF))
            self.cf = 1 if a < (regv & 0xFF) else 0
            self.last_op_bt = False
            return npc
        if op == 0x3A:                       # cmp r8, r/m8
            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)
            b = regv & 0xFF
            self.last_cmp = (a, b); setzsf(self, a - b)
            self.cf = 1 if a < b else 0
            self.last_op_bt = False
            return npc
        if op == 0x88:                       # mov r/m8, r8
            if mod == 3:
                self.set_reg(rm, (self.get_reg(rm) & ~0xFF) | (regv & 0xFF))
            else:
                self.wb(ea, regv & 0xFF)
            return npc
        if op == 0x8A:                       # mov r8, r/m8
            v = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)
            self.set_reg(reg, (self.get_reg(reg) & ~0xFF) | v)
            return npc
        if op == 0x01:                       # add r/m32, r32
            write_rm(rmv + regv); setzsf(self, rmv + regv)
            self.cf = 1 if (rmv + regv) > 0xFFFFFFFF else 0
            return npc
        if op == 0x29:                       # sub r/m32, r32
            write_rm(rmv - regv); setzsf(self, rmv - regv)
            self.cf = 1 if (rmv & 0xFFFFFFFF) < (regv & 0xFFFFFFFF) else 0
            return npc
        if op == 0x31:                       # xor r/m32, r32
            write_rm(rmv ^ regv); setzsf(self, 0)
            self.cf = 0
            return npc
        if op == 0x21:                       # and r/m32, r32
            write_rm(rmv & regv); setzsf(self, rmv & regv)
            self.cf = 0
            return npc
        if op == 0x09:                       # or r/m32, r32
            write_rm(rmv | regv); setzsf(self, rmv | regv)
            self.cf = 0
            return npc
        if op == 0x03:                       # add r32, r/m32
            self.set_reg(reg, regv + rmv); setzsf(self, regv + rmv)
            self.cf = 1 if (regv + rmv) > 0xFFFFFFFF else 0
            return npc
        if op == 0x2B:                       # sub r32, r/m32
            self.set_reg(reg, regv - rmv); setzsf(self, regv - rmv)
            self.cf = 1 if (regv & 0xFFFFFFFF) < (rmv & 0xFFFFFFFF) else 0
            return npc
        if op == 0x33:                       # xor r32, r/m32
            self.set_reg(reg, regv ^ rmv); setzsf(self, 0)
            self.cf = 0
            return npc
        if op == 0x23:                       # and r32, r/m32
            self.set_reg(reg, regv & rmv); setzsf(self, regv & rmv)
            self.cf = 0
            return npc
        if op == 0x0B:                       # or r32, r/m32
            self.set_reg(reg, regv | rmv); setzsf(self, regv | rmv)
            self.cf = 0
            return npc
        if op == 0x3B:                       # cmp r32, r/m32
            self.last_cmp = (regv, rmv); setzsf(self, regv - rmv)
            self.cf = 1 if (regv & 0xFFFFFFFF) < (rmv & 0xFFFFFFFF) else 0
            self.last_op_bt = False
            return npc
        if op == 0x87:                       # xchg r/m32, r32
            write_rm(regv)
            self.set_reg(reg, rmv)
            return npc
        raise CeError("modrm opcode 0x%02x at 0x%08x" % (op, pc))

    def op_mov8(self, pc):
        mod, reg, rm, npc, ea = self.modrm(pc + 1)
        if mod == 3:
            raise CeError("mov8 reg,reg not emitted")
        self.wb(ea, self.get_reg(reg) & 0xFF)
        return npc

    def op_group1(self, op, pc):
        b = self.rb(pc + 1)
        sub = (b >> 3) & 7
        mod, reg, rm, npc, ea = self.modrm(pc + 1)
        byte_op = (op == 0x80)               # 0x80: BYTE r/m, imm8
        if op == 0x81:
            imm = self.rw(npc); npc += 4
        else:
            imm = self.rb(npc); npc += 1
            if imm & 0x80:
                imm -= 0x100
        if byte_op:
            cur = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)
            if cur & 0x80:
                cur -= 0x100                 # signed byte compare domain
        else:
            cur = self.get_reg(rm) if mod == 3 else self.rw(ea)
            imm &= 0xFFFFFFFF
        mask = 0xFF if byte_op else 0xFFFFFFFF
        if sub == 0:
            res = cur + imm
            self.cf = 1 if res > mask else 0
        elif sub == 1:
            res = cur | imm
            self.cf = 0
        elif sub == 2:
            res = cur ^ imm                # xor (adc not emitted by cc)
            self.cf = 0
        elif sub == 4:
            res = cur & imm
            self.cf = 0
        elif sub == 5:
            res = cur - imm
            self.cf = 1 if (cur & mask) < (imm & mask) else 0
        elif sub == 6:
            res = cur ^ imm
            self.cf = 0
        elif sub == 7:
            self.last_cmp = (cur, imm)
            self.last_test = (cur, imm)
            self.last_op_bt = False
            setzsf(self, cur - imm)
            self.cf = 1 if (cur & mask) < (imm & mask) else 0
            return npc
        setzsf(self, res)
        if byte_op:
            if mod == 3:
                self.set_reg(rm, (self.get_reg(rm) & ~0xFF) | (res & 0xFF))
            else:
                self.wb(ea, res & 0xFF)
        elif mod == 3:
            self.set_reg(rm, res)
        else:
            self.ww(ea, res)
        return npc

    def op_group3(self, pc, byte_form=False):
        """Group3 (opcode 0xF6 / 0xF7): test/not/neg/mul/imul/div/idiv.

        The sub-opcode lives in the modrm `reg` field.  Both the
        register form and the memory form are implemented: gcc emits
        `idiv dword ptr [ebp-0x18c]` for a 32-bit signed division, and
        a register-only implementation made the simulator abort with
        "group3 memory operand not emitted" instead of executing it.

        `byte_form` selects the 0xF6 (8-bit) encoding; 0xF7 is 32-bit.
        """
        b = self.rb(pc + 1)
        sub = (b >> 3) & 7
        mod, reg, rm, npc, ea = self.modrm(pc + 1)

        mask = 0xFF if byte_form else 0xFFFFFFFF

        def read_rm():
            if mod == 3:
                return self.get_reg(rm) & mask
            return self.rb(ea) if byte_form else self.rw(ea)

        def write_rm(val):
            if mod == 3:
                if byte_form:
                    self.set_reg(rm, (self.get_reg(rm) & ~0xFF) | (val & 0xFF))
                else:
                    self.set_reg(rm, val & 0xFFFFFFFF)
            else:
                if byte_form:
                    self.wb(ea, val & 0xFF)
                else:
                    self.ww(ea, val & 0xFFFFFFFF)

        v = read_rm()

        if sub in (0, 1):                                    # test r/m, imm
            imm = self.rb(npc) if byte_form else self.rw(npc)
            npc += 1 if byte_form else 4
            res = v & imm
            self.last_cmp = (res, 0)
            self.last_test = (res, 0)
            self.last_op_bt = False
            if byte_form:
                self.zf = 1 if (res & 0xFF) == 0 else 0
                self.sf = 1 if res & 0x80 else 0
            else:
                setzsf(self, res)
            self.cf = 0
            return npc

        if sub == 2:                                        # not r/m
            write_rm(~v & mask)
            return npc

        if sub == 3:                                        # neg r/m
            write_rm((-v) & mask)
            self.cf = 1 if v != 0 else 0
            return npc

        if sub == 4:                                        # mul r/m (unsigned)
            if byte_form:
                res = (self.regs["eax"] & 0xFF) * v
                self.regs["eax"] = (self.regs["eax"] & 0xFFFF0000) | (res & 0xFFFF)
                self.cf = self.of = 1 if (res >> 8) else 0
            else:
                res = self.regs["eax"] * v
                self.regs["eax"] = res & 0xFFFFFFFF
                self.regs["edx"] = (res >> 32) & 0xFFFFFFFF
                self.cf = self.of = 1 if (res >> 32) else 0
            return npc

        if sub == 5:                                        # imul r/m (signed, 1-op)
            if byte_form:
                res = sx8(self.regs["eax"] & 0xFF) * sx8(v)
                self.regs["eax"] = (self.regs["eax"] & 0xFFFF0000) | (res & 0xFFFF)
                self.cf = self.of = 1 if (res >> 8) else 0
            else:
                res = sx32(self.regs["eax"]) * sx32(v)
                self.regs["eax"] = res & 0xFFFFFFFF
                self.regs["edx"] = (res >> 32) & 0xFFFFFFFF
                self.cf = self.of = 1 if (res >> 32) else 0
            return npc

        if sub == 6:                                        # div r/m (unsigned)
            if byte_form:
                if v == 0:
                    raise CeError("div by zero")
                dividend = self.regs["eax"] & 0xFFFF
                q = dividend // v
                if q > 0xFF:
                    raise CeError("divide overflow")
                r = dividend % v
                self.regs["eax"] = (self.regs["eax"] & 0xFFFF0000) | ((r & 0xFF) << 8) | (q & 0xFF)
            else:
                if v == 0:
                    raise CeError("div by zero")
                dividend = (self.regs["edx"] << 32) | self.regs["eax"]
                q = dividend // v
                if q > 0xFFFFFFFF:
                    raise CeError("divide overflow")
                self.regs["eax"] = q & 0xFFFFFFFF
                self.regs["edx"] = (dividend % v) & 0xFFFFFFFF
            return npc

        if sub == 7:                                        # idiv r/m (signed)
            if byte_form:
                if v == 0:
                    raise CeError("idiv by zero")
                dividend = sx16(self.regs["eax"] & 0xFFFF)
                q, r = trunc_div(dividend, sx8(v))
                if not (-128 <= q <= 127):
                    raise CeError("divide overflow")
                self.regs["eax"] = (self.regs["eax"] & 0xFFFF0000) | ((r & 0xFF) << 8) | (q & 0xFF)
            else:
                if v == 0:
                    raise CeError("idiv by zero")
                dividend = (self.regs["edx"] << 32) | self.regs["eax"]
                if dividend & 0x8000000000000000:
                    dividend -= 0x10000000000000000
                q, r = trunc_div(dividend, sx32(v))
                if not (-0x80000000 <= q <= 0x7FFFFFFF):
                    raise CeError("divide overflow")
                self.regs["eax"] = q & 0xFFFFFFFF
                self.regs["edx"] = r & 0xFFFFFFFF
            return npc

        raise CeError("group3 /%d" % sub)

    def op_group2(self, pc, imm8=False):
        b = self.rb(pc + 1)
        sub = (b >> 3) & 7
        mod, reg, rm, npc, ea = self.modrm(pc + 1)
        if imm8:
            cnt = self.rb(npc) & 0x1F; npc += 1
        else:
            cnt = self.regs["ecx"] & 0x1F
        v = self.get_reg(rm)
        if sub == 4:
            res = v << cnt
        elif sub == 5:
            res = (v & 0xFFFFFFFF) >> cnt          # shr (logical)
        elif sub == 7:
            res = sx32(v) >> cnt
        else:
            raise CeError("group2 /%d" % sub)
        self.set_reg(rm, res)
        return npc

    def op_call_indirect(self, pc):
        """call dword [eax+disp] -- the CatOS api table call.

        cdecl semantics: the caller already pushed the arguments and
        `add esp,n` follows the call.  We push a return address, run the
        host thunk (which reads args just above it), pop it, and leave
        ESP exactly where it was after the call.
        """
        b = self.rb(pc + 1)
        sub = (b >> 3) & 7
        mod, reg, rm, npc, ea = self.modrm(pc + 1)
        if sub == 4:                                     # jmp r/m32 (no push)
            if mod == 3:
                return self.get_reg(rm)
            return self.rw(ea)
        if sub != 2:
            raise CeError("ff /%d not emitted" % sub)
        if mod == 3:
            target = self.get_reg(rm)
            ret_addr = npc
            if THUNK_BASE <= target < THUNK_BASE + 4 * len(API_NAMES):
                slot = target - THUNK_BASE
                self.push(ret_addr)
                ret = self.call_api(slot)
                self.regs["eax"] = ret & 0xFFFFFFFF
                got = self.pop()
                if got != ret_addr:
                    raise CeError("thunk stack mismatch: api=%s esp-delta=%d"
                                  % (API_NAMES[slot] if slot < len(API_NAMES) else "?",
                                     (got - ret_addr) & 0xFFFFFFFF))
                return ret_addr
            self.push(ret_addr)
            return target
        target = self.rw(ea)
        ret_addr = npc                       # next instruction after the call
        esp0 = self.regs["esp"]
        if THUNK_BASE <= target < THUNK_BASE + 4 * len(API_NAMES):
            slot = target - THUNK_BASE
            self.push(ret_addr)
            ret = self.call_api(slot)
            self.regs["eax"] = ret & 0xFFFFFFFF
            got = self.pop()
            if got != ret_addr:
                args = [self.rw((esp0 + 4 + i*4) & 0xFFFFFFFF) for i in range(4)]
                raise CeError("thunk stack mismatch: api=%s esp-delta=%d args=%s"
                              % (API_NAMES[slot] if slot < len(API_NAMES) else "?",
                                 (got - ret_addr) & 0xFFFFFFFF,
                                 [hex(x) for x in args]))
            return ret_addr
        self.push(ret_addr)
        return target

    def op_imul_imm(self, pc, imm8=False):
        mod, reg, rm, npc, ea = self.modrm(pc + 1)
        if imm8:
            imm = self.rb(npc); npc += 1
            if imm & 0x80:
                imm -= 0x100
        else:
            imm = self.rw(npc); npc += 4
        a = self.get_reg(rm) if mod == 3 else self.rw(ea)
        self.set_reg(reg, sx32(a) * sx32(imm))
        return npc

    def op_0f(self, pc):
        op2 = self.rb(pc + 1)
        if 0x90 <= op2 <= 0x9F:                          # setcc r/m8
            cc = op2 - 0x90
            mod, reg, rm, npc, ea = self.modrm(pc + 2)
            v = self.cond(cc) & 0xFF
            if mod == 3:
                self.set_reg(rm, (self.get_reg(rm) & ~0xFF) | v)
            else:
                self.wb(ea, v)
            self.cc_src = cc
            return npc
        if op2 == 0xB6:                                  # movzx eax, al
            mod, reg, rm, npc, ea = self.modrm(pc + 2)
            if mod == 3 and rm == 0 and reg == 0:
                self.regs["eax"] &= 0xFF
            elif ea is not None:
                self.set_reg(reg, self.rb(ea))
            else:
                self.set_reg(reg, self.get_reg(rm) & 0xFF)
            return npc
            if self.rb(pc + 2) != 0xC0:
                raise CeError("movzx non-al")
            self.regs["eax"] &= 0xFF
            return pc + 3
        if op2 == 0xBE:                                  # movsx r32, r/m8
            mod, reg, rm, npc, ea = self.modrm(pc + 2)
            v = self.rb(ea) if ea is not None else (self.get_reg(rm) & 0xFF)
            if v & 0x80:
                v -= 0x100
            self.set_reg(reg, v)
            return npc
        if op2 == 0xAF:                                  # imul r32, r/m32
            mod, reg, rm, npc, ea = self.modrm(pc + 2)
            b = self.get_reg(rm) if mod == 3 else self.rw(ea)
            self.set_reg(reg, sx32(self.get_reg(reg)) * sx32(b))
            return npc
        if 0x80 <= op2 <= 0x8F:                          # jcc rel32
            rel = sx32(self.rw(pc + 2))
            if self.cond(op2 - 0x80):
                return (pc + 6 + rel) & 0xFFFFFFFF
            return pc + 6
        if op2 == 0xA3:                                  # bt r/m32, r32
            mod, reg, rm, npc, ea = self.modrm(pc + 2)
            v = self.get_reg(rm) if mod == 3 else self.rw(ea)
            bit = self.get_reg(reg) & 31
            self.cf = (v >> bit) & 1
            self.last_op_bt = True
            return npc
        if 0x40 <= op2 <= 0x4F:                          # cmovcc r32, r/m32
            mod, reg, rm, npc, ea = self.modrm(pc + 2)
            if self.cond(op2 - 0x40):
                self.set_reg(reg, self.get_reg(rm) if mod == 3 else self.rw(ea))
            return npc
        raise CeError("0F opcode 0x%02x" % op2)

    def cond(self, cc):
        """Condition codes cc.ce emits.

        ZF-based (sete/setne/jz/jnz) use last_cmp; the flag family
        setl/setle/setge/setg also come from the same signed compare,
        which is exact here because cc only compares right after a cmp.
        """
        a, b = self.last_cmp
        sa, sb = sx32(a), sx32(b)
        # setcc opcodes (0x4..0xF) and jcc opcodes (0x84..0x8F) name the
        # same conditions in different encodings; normalise both.
        zf = getattr(self, "zf", 0)
        sf = getattr(self, "sf", 0)
        cond = {
            0x4: zf, 0x5: not zf,                       # set e/ne
            0xC: sa < sb, 0xE: sa <= sb,                # set l/le
            0xD: sa >= sb, 0xF: sa > sb,                # set ge/g
            0x84: zf, 0x85: not zf,                     # jz/jnz (ZF latch)
            0x8C: sa < sb, 0x8E: sa <= sb,              # jl/jle
            0x8D: sa >= sb, 0x8F: sa > sb,              # jge/jg
            0x8: sf, 0x9: not sf,                       # js/jns (SF latch)
        }
        # unsigned / carry-flag conditions read the CF latch, which
        # arithmetic (add/sub/cmp) and `bt` both maintain.
        if cc in (0x2, 0x3, 0x6, 0x7):          # jb/jae/jbe/ja
            cf = 1 if getattr(self, "cf", 0) else 0
            zf = getattr(self, "zf", 0)
            if cc == 0x2: return 1 if cf else 0
            if cc == 0x3: return 0 if cf else 1
            if cc == 0x6: return 1 if (cf or zf) else 0
            return 1 if (not cf and not zf) else 0
        return 1 if cond[cc] else 0


def load_ce(path):
    data = open(path, "rb").read()
    if data[:4] != b"CE01":
        raise SystemExit("%s: not a CE image" % path)
    entry, bss, api_off, image_size = struct.unpack_from("<IIII", data, 4)
    return data[20:20 + image_size], bss, api_off, entry


def main():
    args = sys.argv[1:]
    dump_dir = None
    if "--dump-dir" in args:
        i = args.index("--dump-dir")
        dump_dir = args[i + 1]
        del args[i:i + 2]
    files = {}
    if "--file" in args:
        while "--file" in args:
            i = args.index("--file")
            host, name = args[i + 1], args[i + 2]
            files[name.upper()] = open(host, "rb").read()
            del args[i:i + 3]
    if not args:
        print(__doc__)
        return 2
    image, bss, api_off, entry = load_ce(args[0])
    m = Machine(image, bss, api_off, files)
    m.entry = entry
    m.progname = args[0].replace(chr(92), "/").split("/")[-1].upper()
    m.install_api()
    try:
        m.run_program(args[1:])
        rc = 0
    except SystemExit as e:
        rc = e.code
    except CeError as e:
        sys.stdout.write("".join(m.out))
        print("\n[ce_sim] ERROR: %s" % e)
        return 3
    sys.stdout.write("".join(m.out))
    print("\n[ce_sim] steps=%d rc=%s" % (m.steps, rc))
    if dump_dir:
        os.makedirs(dump_dir, exist_ok=True)
        for name, blob in m.files.items():
            safe = name.replace("/", "_").replace("\\", "_")
            with open(os.path.join(dump_dir, safe), "wb") as f:
                f.write(blob)
            print("[ce_sim] wrote %s (%d bytes)" % (safe, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
