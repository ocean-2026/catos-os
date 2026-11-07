path = r'D:\CatOs\tests\ce_sim.py'
src = open(path).read()

def rep(old, new):
    global src
    assert old in src, "NOT FOUND: " + old[:70]
    src = src.replace(old, new)

# --- byte al,imm8 group: set ZF/SF from the byte result ---
rep("""        if op == 0xA8:                                   # test al, imm8
            self.regs["eax"] &= 0xFFFF00FF
            return pc + 2
        if op == 0x04:                                   # add al, imm8
            al = (self.regs["eax"] & 0xFF)
            imm = self.rb(pc + 1)
            self.regs["eax"] = (self.regs["eax"] & ~0xFF) | ((al + imm) & 0xFF)
            return pc + 2
        if op in (0x0C, 0x24, 0x2C, 0x34):               # al, imm8 ops
            al = self.regs["eax"] & 0xFF
            imm = self.rb(pc + 1)
            if op == 0x0C:
                al |= imm
            elif op == 0x24:
                al &= imm
            elif op == 0x2C:
                al = (al - imm) & 0xFF
            else:
                al ^= imm
            self.regs["eax"] = (self.regs["eax"] & ~0xFF) | al
            return pc + 2
        if op == 0x3C:                                   # cmp al, imm8
            self.last_cmp = (self.regs["eax"] & 0xFF, self.rb(pc + 1))
            return pc + 2""",
    """        if op == 0xA8:                                   # test al, imm8
            al = self.regs["eax"] & 0xFF
            res = al & self.rb(pc + 1)
            self.last_cmp = (res, 0)
            self.zf = 1 if res == 0 else 0
            self.sf = 1 if res & 0x80 else 0
            return pc + 2
        if op == 0x04:                                   # add al, imm8
            al = (self.regs["eax"] & 0xFF)
            imm = self.rb(pc + 1)
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
            self.last_op_bt = False
            return pc + 2""")

# --- eax,imm32 group: flags + fix the EAX-destroying test ---
rep("""        if op == 0xA9:                                   # test eax, imm32
            self.regs["eax"] &= 0  # flags only; zero the compare
            self.last_test = (self.regs["eax"], 0)
            return pc + 5
        if op == 0x05:                                   # add eax, imm32
            self.regs["eax"] = (self.regs["eax"] + self.rw(pc + 1)) & 0xFFFFFFFF
            return pc + 5
        if op == 0x2D:                                   # sub eax, imm32
            self.regs["eax"] = (self.regs["eax"] - self.rw(pc + 1)) & 0xFFFFFFFF
            return pc + 5
        if op == 0x3D:                                   # cmp eax, imm32
            self.last_cmp = (self.regs["eax"], self.rw(pc + 1))
            return pc + 5
        if op == 0x0D:                                   # or eax, imm32
            self.regs["eax"] |= self.rw(pc + 1)
            return pc + 5
        if op == 0x25:                                   # and eax, imm32
            self.regs["eax"] &= self.rw(pc + 1)
            return pc + 5
        if op == 0x35:                                   # xor eax, imm32
            self.regs["eax"] ^= self.rw(pc + 1)""",
    """        if op == 0xA9:                                   # test eax, imm32
            res = self.regs["eax"] & self.rw(pc + 1)
            self.last_cmp = (res, 0)
            self.last_test = (res, 0)
            self.zf = 1 if res == 0 else 0
            self.sf = 1 if res & 0x80000000 else 0
            return pc + 5
        if op == 0x05:                                   # add eax, imm32
            self.regs["eax"] = (self.regs["eax"] + self.rw(pc + 1)) & 0xFFFFFFFF
            setzsf(self, self.regs["eax"])
            return pc + 5
        if op == 0x2D:                                   # sub eax, imm32
            self.regs["eax"] = (self.regs["eax"] - self.rw(pc + 1)) & 0xFFFFFFFF
            setzsf(self, self.regs["eax"])
            return pc + 5
        if op == 0x3D:                                   # cmp eax, imm32
            a = self.regs["eax"]; b = self.rw(pc + 1)
            self.last_cmp = (a, b)
            self.last_op_bt = False
            setzsf(self, a - b)
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
            setzsf(self, self.regs["eax"])""")

open(path, 'w').write(src)
print("al/eax imm8/imm32 flag fixes OK")
