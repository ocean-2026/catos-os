path = r'D:\CatOs\tests\ce_sim.py'
src = open(path).read()

def rep(old, new):
    global src
    assert old in src, "NOT FOUND: " + old[:70]
    src = src.replace(old, new)

# cond(): carry-flag family reads the latched self.cf (set by arithmetic / bt)
rep("""        # unsigned / carry-flag conditions: normally set by cmp/sub/add on
        # the (unsigned) operands; only `bt` arrives without a preceding
        # compare, in which case fall back to the latched self.cf.
        if cc in (0x2, 0x3, 0x6, 0x7):          # jb/jae/jbe/ja
            if getattr(self, "last_op_bt", False):
                cf = 1 if getattr(self, "cf", 0) else 0
            else:
                cf = 1 if (a & 0xFFFFFFFF) < (b & 0xFFFFFFFF) else 0
            zf = 1 if a == b else 0
            if cc == 0x2: return 1 if cf else 0
            if cc == 0x3: return 0 if cf else 1
            if cc == 0x6: return 1 if (cf or zf) else 0
            return 1 if (not cf and not zf) else 0
        return 1 if cond[cc] else 0""",
    """        # unsigned / carry-flag conditions read the CF latch, which
        # arithmetic (add/sub/cmp) and `bt` both maintain.
        if cc in (0x2, 0x3, 0x6, 0x7):          # jb/jae/jbe/ja
            cf = 1 if getattr(self, "cf", 0) else 0
            zf = getattr(self, "zf", 0)
            if cc == 0x2: return 1 if cf else 0
            if cc == 0x3: return 0 if cf else 1
            if cc == 0x6: return 1 if (cf or zf) else 0
            return 1 if (not cf and not zf) else 0
        return 1 if cond[cc] else 0""")

# op_modrm dword arithmetic: latch CF
rep("        if op == 0x01:                       # add r/m32, r32\n            write_rm(rmv + regv); setzsf(self, rmv + regv)",
    "        if op == 0x01:                       # add r/m32, r32\n            write_rm(rmv + regv); setzsf(self, rmv + regv)\n            self.cf = 1 if (rmv + regv) > 0xFFFFFFFF else 0")
rep("        if op == 0x29:                       # sub r/m32, r32\n            write_rm(rmv - regv); setzsf(self, rmv - regv)",
    "        if op == 0x29:                       # sub r/m32, r32\n            write_rm(rmv - regv); setzsf(self, rmv - regv)\n            self.cf = 1 if (rmv & 0xFFFFFFFF) < (regv & 0xFFFFFFFF) else 0")
rep("        if op == 0x31:                       # xor r/m32, r32\n            write_rm(rmv ^ regv); setzsf(self, 0)",
    "        if op == 0x31:                       # xor r/m32, r32\n            write_rm(rmv ^ regv); setzsf(self, 0)\n            self.cf = 0")
rep("        if op == 0x21:                       # and r/m32, r32\n            write_rm(rmv & regv); setzsf(self, rmv & regv)",
    "        if op == 0x21:                       # and r/m32, r32\n            write_rm(rmv & regv); setzsf(self, rmv & regv)\n            self.cf = 0")
rep("        if op == 0x09:                       # or r/m32, r32\n            write_rm(rmv | regv); setzsf(self, rmv | regv)",
    "        if op == 0x09:                       # or r/m32, r32\n            write_rm(rmv | regv); setzsf(self, rmv | regv)\n            self.cf = 0")
rep("        if op == 0x03:                       # add r32, r/m32\n            self.set_reg(reg, regv + rmv); setzsf(self, regv + rmv)",
    "        if op == 0x03:                       # add r32, r/m32\n            self.set_reg(reg, regv + rmv); setzsf(self, regv + rmv)\n            self.cf = 1 if (regv + rmv) > 0xFFFFFFFF else 0")
rep("        if op == 0x2B:                       # sub r32, r/m32\n            self.set_reg(reg, regv - rmv); setzsf(self, regv - rmv)",
    "        if op == 0x2B:                       # sub r32, r/m32\n            self.set_reg(reg, regv - rmv); setzsf(self, regv - rmv)\n            self.cf = 1 if (regv & 0xFFFFFFFF) < (rmv & 0xFFFFFFFF) else 0")
rep("        if op == 0x33:                       # xor r32, r/m32\n            self.set_reg(reg, regv ^ rmv); setzsf(self, 0)",
    "        if op == 0x33:                       # xor r32, r/m32\n            self.set_reg(reg, regv ^ rmv); setzsf(self, 0)\n            self.cf = 0")
rep("        if op == 0x23:                       # and r32, r/m32\n            self.set_reg(reg, regv & rmv); setzsf(self, regv & rmv)",
    "        if op == 0x23:                       # and r32, r/m32\n            self.set_reg(reg, regv & rmv); setzsf(self, regv & rmv)\n            self.cf = 0")
rep("        if op == 0x0B:                       # or r32, r/m32\n            self.set_reg(reg, regv | rmv); setzsf(self, regv | rmv)",
    "        if op == 0x0B:                       # or r32, r/m32\n            self.set_reg(reg, regv | rmv); setzsf(self, regv | rmv)\n            self.cf = 0")
rep("        if op == 0x39:                       # cmp r/m32, r32\n            self.last_cmp = (rmv, regv); setzsf(self, rmv - regv)",
    "        if op == 0x39:                       # cmp r/m32, r32\n            self.last_cmp = (rmv, regv); setzsf(self, rmv - regv)\n            self.cf = 1 if (rmv & 0xFFFFFFFF) < (regv & 0xFFFFFFFF) else 0")
rep("        if op == 0x3B:                       # cmp r32, r/m32\n            self.last_cmp = (regv, rmv); setzsf(self, regv - rmv)",
    "        if op == 0x3B:                       # cmp r32, r/m32\n            self.last_cmp = (regv, rmv); setzsf(self, regv - rmv)\n            self.cf = 1 if (regv & 0xFFFFFFFF) < (rmv & 0xFFFFFFFF) else 0")
rep("        if op == 0x38:                       # cmp r/m8, r8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            self.last_cmp = (a, regv & 0xFF); setzsf(self, a - (regv & 0xFF))",
    "        if op == 0x38:                       # cmp r/m8, r8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            self.last_cmp = (a, regv & 0xFF); setzsf(self, a - (regv & 0xFF))\n            self.cf = 1 if a < (regv & 0xFF) else 0")
rep("        if op == 0x3A:                       # cmp r8, r/m8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            b = regv & 0xFF\n            self.last_cmp = (a, b); setzsf(self, a - b)",
    "        if op == 0x3A:                       # cmp r8, r/m8\n            a = (self.get_reg(rm) & 0xFF) if mod == 3 else self.rb(ea)\n            b = regv & 0xFF\n            self.last_cmp = (a, b); setzsf(self, a - b)\n            self.cf = 1 if a < b else 0")

# group1: CF for add/sub/cmp (byte and dword), clear for logicals
rep("""        if sub == 0:
            res = cur + imm
        elif sub == 1:
            res = cur | imm
        elif sub == 2:
            res = cur ^ imm                # xor (adc not emitted by cc)
        elif sub == 4:
            res = cur & imm
        elif sub == 5:
            res = cur - imm
        elif sub == 6:
            res = cur ^ imm
        elif sub == 7:
            self.last_cmp = (cur, imm)
            self.last_test = (cur, imm)
            self.last_op_bt = False
            setzsf(self, cur - imm)
            return npc
        setzsf(self, res)""",
    """        mask = 0xFF if byte_op else 0xFFFFFFFF
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
        setzsf(self, res)""")

# eax/al imm forms
rep("        if op == 0x3D:                                   # cmp eax, imm32\n            a = self.regs[\"eax\"]; b = self.rw(pc + 1)\n            self.last_cmp = (a, b)\n            self.last_op_bt = False\n            setzsf(self, a - b)",
    "        if op == 0x3D:                                   # cmp eax, imm32\n            a = self.regs[\"eax\"]; b = self.rw(pc + 1)\n            self.last_cmp = (a, b)\n            self.last_op_bt = False\n            setzsf(self, a - b)\n            self.cf = 1 if (a & 0xFFFFFFFF) < (b & 0xFFFFFFFF) else 0")
rep("        if op == 0x2D:                                   # sub eax, imm32\n            self.regs[\"eax\"] = (self.regs[\"eax\"] - self.rw(pc + 1)) & 0xFFFFFFFF\n            setzsf(self, self.regs[\"eax\"])",
    "        if op == 0x2D:                                   # sub eax, imm32\n            a = self.regs[\"eax\"]; b = self.rw(pc + 1)\n            self.regs[\"eax\"] = (a - b) & 0xFFFFFFFF\n            setzsf(self, self.regs[\"eax\"])\n            self.cf = 1 if (a & 0xFFFFFFFF) < (b & 0xFFFFFFFF) else 0")
rep("        if op == 0x05:                                   # add eax, imm32\n            self.regs[\"eax\"] = (self.regs[\"eax\"] + self.rw(pc + 1)) & 0xFFFFFFFF\n            setzsf(self, self.regs[\"eax\"])",
    "        if op == 0x05:                                   # add eax, imm32\n            a = self.regs[\"eax\"]; b = self.rw(pc + 1)\n            self.regs[\"eax\"] = (a + b) & 0xFFFFFFFF\n            setzsf(self, self.regs[\"eax\"])\n            self.cf = 1 if (a + b) > 0xFFFFFFFF else 0")
rep("        if op == 0x04:                                   # add al, imm8\n            al = (self.regs[\"eax\"] & 0xFF)\n            imm = self.rb(pc + 1)\n            res = (al + imm) & 0xFF",
    "        if op == 0x04:                                   # add al, imm8\n            al = (self.regs[\"eax\"] & 0xFF)\n            imm = self.rb(pc + 1)\n            self.cf = 1 if (al + imm) > 0xFF else 0\n            res = (al + imm) & 0xFF")

open(path, 'w').write(src)
print("CF latches installed OK")
