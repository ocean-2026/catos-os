#!/usr/bin/env python3
"""宿主侧 FAT32 读取器：列出 CatOS 格式化后的磁盘镜像里的真实内容。

用来核实「内核到底把哪些内置文件装进了 /bin」——屏幕只有 25 行，
`ls /bin` 会被截断，看不全。

用法: python tests/fat32_list.py <disk.img>
"""
import struct
import sys


class Fat32:
    def __init__(self, path):
        self.f = open(path, "rb")
        d = self.f.read(512)
        if d[510:512] != b"\x55\xAA":
            raise SystemExit("no MBR signature")
        # partition entry 1
        pe = d[446:462]
        self.lba_start = struct.unpack("<I", pe[8:12])[0]
        self.num_fats = d[16]
        self.root_clus = struct.unpack("<I", d[44:48])[0]
        self.fat_sz = struct.unpack("<I", d[36:40])[0]
        self.spc = d[13]
        self.reserved = struct.unpack("<H", d[14:16])[0]
        self.f = open(path, "rb")
        self.f.seek(self.lba_start * 512)
        dbr = self.f.read(512)
        self.spc = dbr[13]
        self.reserved = struct.unpack("<H", dbr[14:16])[0]
        self.num_fats = dbr[16]
        self.fat_sz = struct.unpack("<I", dbr[36:40])[0]
        self.root_clus = struct.unpack("<I", dbr[44:48])[0]
        self.data_start = self.lba_start + self.reserved + self.num_fats * self.fat_sz
        self.csz = self.spc * 512
        self.fat = self._read_fat()

    def _read_fat(self):
        self.f.seek((self.lba_start + self.reserved) * 512)
        return self.f.read(self.fat_sz * 512)

    def fat_get(self, c):
        off = c * 4
        v = (self.fat[off] | (self.fat[off + 1] << 8) |
             (self.fat[off + 2] << 16) | (self.fat[off + 3] << 24))
        return v & 0x0FFFFFFF

    def read_cluster(self, c):
        self.f.seek((self.data_start + (c - 2) * self.spc) * 512)
        return self.f.read(self.csz)

    def chain(self, c):
        out = []
        while 2 <= c < 0x0FFFFFF0:
            out.append(c)
            c = self.fat_get(c)
        return out

    def read_file(self, c, size):
        b = b"".join(self.read_cluster(x) for x in self.chain(c))
        return b[:size]

    def listdir(self, c):
        data = b"".join(self.read_cluster(x) for x in self.chain(c))
        entries = []
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if len(e) < 32 or e[0] in (0x00,):
                break
            if e[0] == 0xE5:
                continue
            attr = e[11]
            if attr == 0x0F:
                continue
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            # CatOS writes STANDARD FAT32 directory entries after all:
            #   name[11] @0, attr @11, FstClusLO/hi u16 @26, size u32 @28.
            # The u16 cluster field caps the volume at 64k clusters, which
            # is fine for the 64 MiB test image (128k clusters would need
            # the high word -- worth noting as a real limitation).
            clus = struct.unpack("<H", e[26:28])[0]
            size = struct.unpack("<I", e[28:32])[0]
            full = (name + "." + ext) if ext else name
            entries.append((full, attr, clus, size))
        return entries


def main():
    path = sys.argv[1]
    fs = Fat32(path)
    print("volume: lba_start=%d spc=%d cluster=%dB fat_sz=%d sectors root_clus=%d"
          % (fs.lba_start, fs.spc, fs.csz, fs.fat_sz, fs.root_clus))
    root = fs.listdir(fs.root_clus)
    print("\n== root (%d entries) ==" % len(root))
    for full, attr, clus, size in root:
        kind = "DIR " if (attr & 0x10) else "FILE"
        print("  %s %-16s clus=%-6d size=%d" % (kind, full, clus, size))
        if (attr & 0x10) and full.upper() in ("BIN", "APPSDEVELOP"):
            sub = fs.listdir(clus)
            print("      -> %d entries:" % len(sub))
            for f2, a2, c2, s2 in sub:
                k2 = "DIR " if (a2 & 0x10) else "FILE"
                print("         %s %-16s clus=%-6d size=%d" % (k2, f2, c2, s2))
                if not (a2 & 0x10) and s2 and f2.upper().endswith(".GO"):
                    data = fs.read_file(c2, s2)
                    txt = data.decode("latin-1")
                    print("            first line: %r" % txt.split("\n")[0][:60])
                    print("            bytes read: %d/%d" % (len(data), s2))


if __name__ == "__main__":
    main()
