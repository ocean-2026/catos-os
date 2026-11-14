import sys

path = r'D:\CatOs\tests\ce_sim.py'
src = open(path).read()

def rep(old, new):
    global src
    assert old in src, "NOT FOUND: " + old[:70]
    src = src.replace(old, new)

# 1) cond(): zf/sf based for e/ne, drop the eax hack
rep("""        # jz/jnz (0x84/0x85) test the value in EAX via `test eax,eax`;
        # model that as a compare against zero so one table serves both.
        if cc in (0x84, 0x85):
            self.last_cmp = (self.regs["eax"], 0)
        a, b = self.last_cmp""",
    """        a, b = self.last_cmp""")

rep("""        cond = {
            0x4: a == b, 0x5: a != b,                  # set e/ne
            0xC: sa < sb, 0xE: sa <= sb,                # set l/le
            0xD: sa >= sb, 0xF: sa > sb,                # set ge/g
            0x84: a == b, 0x85: a != b,                 # jz/jnz (after test)
            0x8C: sa < sb, 0x8E: sa <= sb,              # jl/jle
            0x8D: sa >= sb, 0x8F: sa > sb,              # jge/jg
            # rel8 jcc: js/jns test the sign of (a-b), i.e. the last cmp/sub
            0x8: sa < sb, 0x9: sa >= sb,                # js/jns
        }""",
    """        zf = getattr(self, "zf", 0)
        sf = getattr(self, "sf", 0)
        cond = {
            0x4: zf, 0x5: not zf,                       # set e/ne
            0xC: sa < sb, 0xE: sa <= sb,                # set l/le
            0xD: sa >= sb, 0xF: sa > sb,                # set ge/g
            0x84: zf, 0x85: not zf,                     # jz/jnz (ZF latch)
            0x8C: sa < sb, 0x8E: sa <= sb,              # jl/jle
            0x8D: sa >= sb, 0x8F: sa > sb,              # jge/jg
            0x8: sf, 0x9: not sf,                       # js/jns (SF latch)
        }""")

# 2) zf/sf latch helper after sx32
rep("""def sx32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v""",
    """def sx32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def setzsf(m, res):
    \"\"\"Latch ZF/SF the way x86 arithmetic does: on the written result.\"\"\"
    m.zf = 1 if (res & 0xFFFFFFFF) == 0 else 0
    m.sf = 1 if res & 0x80000000 else 0""")

# 3) arithmetic in op_modrm sets ZF/SF
pairs = [
 ("        if op == 0x01:                       # add r/m32, r32\n            write_rm(rmv + regv)\n            return npc",
  "        if op == 0x01:                       # add r/m32, r32\n            write_rm(rmv + regv); setzsf(self, rmv + regv)\n            return npc"),
 ("        if op == 0x29:                       # sub r/m32, r32\n            write_rm(rmv - regv)\n            return npc",
  "        if op == 0x29:                       # sub r/m32, r32\n            write_rm(rmv - regv); setzsf(self, rmv - regv)\n            return npc"),
 ("        if op == 0x31:                       # xor r/m32, r32\n            write_rm(rmv ^ regv)\n            return npc",
  "        if op == 0x31:                       # xor r/m32, r32\n            write_rm(rmv ^ regv); setzsf(self, 0)\n            return npc"),
 ("        if op == 0x21:                       # and r/m32, r32\n            write_rm(rmv & regv)\n            return npc",
  "        if op == 0x21:                       # and r/m32, r32\n            write_rm(rmv & regv); setzsf(self, rmv & regv)\n            return npc"),
 ("        if op == 0x09:                       # or r/m32, r32\n            write_rm(rmv | regv)\n            return npc",
  "        if op == 0x09:                       # or r/m32, r32\n            write_rm(rmv | regv); setzsf(self, rmv | regv)\n            return npc"),
 ("        if op == 0x03:                       # add r32, r/m32\n            self.set_reg(reg, regv + rmv)\n            return npc",
  "        if op == 0x03:                       # add r32, r/m32\n            self.set_reg(reg, regv + rmv); setzsf(self, regv + rmv)\n            return npc"),
 ("        if op == 0x2B:                       # sub r32, r/m32\n            self.set_reg(reg, regv - rmv)\n            return npc",
  "        if op == 0x2B:                       # sub r32, r/m32\n            self.set_reg(reg, regv - rmv); setzsf(self, regv - rmv)\n            return npc"),
 ("        if op == 0x33:                       # xor r32, r/m32\n            self.set_reg(reg, regv ^ rmv)\n            return npc",
  "        if op == 0x33:                       # xor r32, r/m32\n            self.set_reg(reg, regv ^ rmv); setzsf(self, 0)\n            return npc"),
 ("        if op == 0x23:                       # and r32, r/m32\n            self.set_reg(reg, regv & rmv)\n            return npc",
  "        if op == 0x23:                       # and r32, r/m32\n            self.set_reg(reg, regv & rmv); setzsf(self, regv & rmv)\n            return npc"),
 ("        if op == 0x0B:                       # or r32, r/m32\n            self.set_reg(reg, regv | rmv)\n            return npc",
  "        if op == 0x0B:                       # or r32, r/m32\n            self.set_reg(reg, regv | rmv); setzsf(self, regv | rmv)\n            return npc"),
 ("        if op == 0x39:                       # cmp r/m32, r32\n            self.last_cmp = (rmv, regv)\n",
  "        if op == 0x39:                       # cmp r/m32, r32\n            self.last_cmp = (rmv, regv); setzsf(self, rmv - regv)\n"),
 ("        if op == 0x3B:                       # cmp r32, r/m32\n            self.last_cmp = (regv, rmv)\n",
  "        if op == 0x3B:                       # cmp r32, r/m32\n            self.last_cmp = (regv, rmv); setzsf(self, regv - rmv)\n"),
 ("        if op == 0x85:                       # test r/m32, r32\n            # test only sets ZF (of a AND b); model it as cmp against 0\n            self.last_cmp = (rmv & regv, 0)\n",
  "        if op == 0x85:                       # test r/m32, r32\n            self.last_cmp = (rmv & regv, 0); setzsf(self, rmv & regv)\n"),
 ("        if op == 0x84:                       # test r/m8, r8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            self.last_cmp = (a & (regv & 0xFF), 0)\n",
  "        if op == 0x84:                       # test r/m8, r8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            self.last_cmp = (a & (regv & 0xFF), 0); setzsf(self, a & (regv & 0xFF))\n"),
 ("        if op == 0x38:                       # cmp r/m8, r8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            self.last_cmp = (a, regv & 0xFF)\n",
  "        if op == 0x38:                       # cmp r/m8, r8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            self.last_cmp = (a, regv & 0xFF); setzsf(self, a - (regv & 0xFF))\n"),
]
for o, n in pairs:
    rep(o, n)

# 4) group1 arithmetic sets ZF/SF
rep("""        elif sub == 7:
            self.last_cmp = (cur, imm)
            self.last_test = (cur, imm)
            self.last_op_bt = False
            return npc""",
    """        elif sub == 7:
            self.last_cmp = (cur, imm)
            self.last_test = (cur, imm)
            self.last_op_bt = False
            setzsf(self, cur - imm)
            return npc
        setzsf(self, res)""")

# 5) inc/dec r32 (0x40-0x4F)
rep("""        # push/pop r32
        if 0x50 <= op <= 0x57:""",
    """        if 0x40 <= op <= 0x47:                           # inc r32
            v = (self.get_reg(op - 0x40) + 1) & 0xFFFFFFFF
            self.set_reg(op - 0x40, v); setzsf(self, v)
            return pc + 1
        if 0x48 <= op <= 0x4F:                           # dec r32
            v = (self.get_reg(op - 0x48) - 1) & 0xFFFFFFFF
            self.set_reg(op - 0x48, v); setzsf(self, v)
            return pc + 1
        # push/pop r32
        if 0x50 <= op <= 0x57:""")

open(path, 'w').write(src)
print("zf/sf model installed OK")
