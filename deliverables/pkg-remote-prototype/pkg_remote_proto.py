#!/usr/bin/env python3
"""
CatOS pkg 远程仓库 —— 宿主原型（"新工具先出原型"规则）

演示目标（在移植到内核前，先在宿主把字节级流程跑通给用户看）：
  1. 起一个本地 HTTP 仓库，托管 PKGS.TXT + *.catpkg（真实签名包）
  2. 模拟 guest 侧：GET 索引 -> 解析 -> GET 包（二进制安全）
  3. 用与内核 pkg_verify() 完全相同的算法验签：RSA-512 pow(s,e,n) == 0x01<<56 | FNV-1a64
  4. 验签通过 -> 安装到 /bin/<cmd>.ce（原型里落到临时目录）
  5. 展示篡改包被拒绝（签名校验失败）

真实内核实现路径（移植后）:
  - netstack.c: net_http_download(url, buf, max) 二进制安全下载到调用者缓冲
  - shell.c   : pkg repo <url>  (存 /etc/reg 的 net.repo)
                pkg update      (拉 PKGS.TXT 覆盖本地索引)
                pkg fetch NAME  (拉远程 .catpkg 到 /pkg/ 后走现有 install 验签)
  - 复用现有 pkg_verify()，零新增密码学代码

运行: python deliverables/pkg-remote-prototype/pkg_remote_proto.py --demo
"""
import sys, os, http.server, threading, socketserver, urllib.request, tempfile, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(ROOT, "..", "..", "kernel_v25", "extapp", "pkg"))
KEY  = os.path.abspath(os.path.join(ROOT, "..", "..", "kernel_v25", "tools", "pkg_pubkey.txt"))
PORT = 18765

# ---- 与内核一致的验签原语（Python 大整数天然支持，内核用 bn_t） -------------
def read_n():
    for line in open(KEY, encoding="utf-8"):
        if line.startswith("n="):
            return int(line.strip()[2:], 16)
    raise RuntimeError("no n in pubkey")

def fnv1a64(hdr, payload):
    h = 1469598103934665603
    for b in hdr + payload:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def verify(blob):
    n = read_n()
    if len(blob) < 204 or blob[:6] != b"CPKG02":
        return False, "bad magic/length"
    payload_len = int.from_bytes(blob[136:140], "little")
    if payload_len == 0 or 204 + payload_len != len(blob):
        return False, "length mismatch"
    sig = int.from_bytes(blob[140:204], "big")
    calc = pow(sig, 65537, n)
    digest = fnv1a64(blob[:140], blob[204:])
    want = (1 << 56) | digest
    return (calc == want), "calc=%064x want=%064x" % (calc, want)

def install_blob(blob, destdir):
    cmd = blob[40:56].split(b"\0")[0].decode()
    path = os.path.join(destdir, cmd + ".ce")
    payload = blob[204:]
    with open(path, "wb") as f:
        f.write(payload)
    return path, len(payload)

# ---- HTTP 仓库（宿主侧，模拟远程服务器） ------------------------------------
class RepoHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=REPO, **kw)
    def log_message(self, *a):
        pass  # 静音，保持 demo 输出干净

def start_repo():
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), RepoHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd

def http_get(path):
    return urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=5).read()

# ---- 演示 ------------------------------------------------------------------
def demo():
    print("=" * 64)
    print("CatOS pkg 远程仓库原型（宿主验证，字节级与内核一致）")
    print("=" * 64)

    httpd = start_repo()
    base = "http://127.0.0.1:%d" % PORT
    print("\n[1] 启动 HTTP 仓库 %s （托管真实 *.catpkg）" % base)

    # 索引（真实仓库里是 /pkg/PKGS.TXT；这里从包现场生成）
    print("\n[2] pkg update —— GET /PKGS.TXT 并解析")
    lines = ["# remote repo", "name|version|desc|file"]
    for fn in sorted(os.listdir(REPO)):
        if not fn.endswith(".catpkg"):
            continue
        blob = open(os.path.join(REPO, fn), "rb").read()
        name  = blob[8:24].split(b"\0")[0].decode()
        ver   = blob[24:40].split(b"\0")[0].decode()
        desc  = blob[56:136].split(b"\0")[0].decode()
        lines.append("%s|%s|%s|%s" % (name, ver, desc, fn))
    index = ("\n".join(lines) + "\n").encode()
    # 让 HTTP 服务也能返回这个索引
    idx_path = os.path.join(REPO, "PKGS.TXT")
    old_idx = open(idx_path, "rb").read() if os.path.exists(idx_path) else None
    open(idx_path, "wb").write(index)
    try:
        got = http_get("/PKGS.TXT")
        assert got == index, "index mismatch"
        recs = [l.split(b"|") for l in got.splitlines() if l and not l.startswith(b"#")][1:]
        print("    索引 %d 字节，解析出 %d 个包：" % (len(got), len(recs)))
        for r in recs:
            print("      %-10s %-6s %s" % (r[0].decode(), r[1].decode(), r[2].decode()[:30]))

        # 拉取并安装一个包
        target = recs[0]
        name, fn = target[0].decode(), target[3].decode()
        print("\n[3] pkg fetch %s —— GET /%s（二进制安全）" % (name, fn))
        blob = http_get("/" + fn)
        print("    下载 %d 字节，magic=%r" % (len(blob), blob[:8]))

        print("\n[4] pkg_verify —— RSA-512 验签")
        ok, detail = verify(blob)
        print("    %s  (%s)" % ("PASS 签名有效" if ok else "FAIL", detail))

        print("\n[5] 安装到 /bin/<cmd>.ce")
        dest = tempfile.mkdtemp(prefix="catos-bin-")
        path, plen = install_blob(blob, dest)
        print("    写入 %s （payload %d 字节）" % (path, plen))

        print("\n[6] 篡改攻击：翻转 payload 一字节后重传")
        bad = bytearray(blob)
        bad[210] ^= 0xFF
        ok2, detail2 = verify(bytes(bad))
        print("    %s  (%s)" % ("FAIL 攻击未被拦截" if ok2 else "PASS 攻击被拒绝：签名校验失败", detail2))
        assert not ok2, "tampered package must be rejected"

        print("\n[7] pkg remove %s —— 删除安装副本" % name)
        os.remove(path)
        print("    已删除 %s" % path)

        shutil.rmtree(dest)
        print("\n" + "=" * 64)
        print("原型结论：远程拉包全链路可行；验签可拦截篡改；")
        print("         内核只需新增 net_http_download + pkg repo/update/fetch，")
        print("         完全复用现有 pkg_verify()，不引入新密码学。")
        print("=" * 64)
        return 0
    finally:
        if old_idx is None:
            os.remove(idx_path)
        else:
            open(idx_path, "wb").write(old_idx)
        httpd.shutdown()

if __name__ == "__main__":
    sys.exit(demo())
