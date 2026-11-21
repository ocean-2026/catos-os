#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cde_shell.py -- 宿主命令行原型：演示 CatOS "可安装/可卸载图形桌面扩展" 的完整交互。

这是"先出原型再移植"规则的产物：在真正的 CE 桌面程序写进内核之前，
先用这个脚本向用户展示扩展的生命周期与 shell 命令效果。

它模拟：
  * 一个极简 FAT32 卷（内存 dict），含 /bin 与 /pkg；
  * CPKG02 风格签名扩展包 desk.catpkg（这里用校验和代替 RSA，仅演示流程）；
  * pkg list / pkg install desktop / desktop / pkg remove desktop 命令；
  * 桌面内部的窗口/任务栏/事件循环（纯文本 ASCII 模拟，表现内核原语之上
    的用户态桌面长什么样）。

用法：
  python cde_shell.py            # 交互式，输入 help 看命令
  python cde_shell.py --demo     # 自动演示完整生命周期（推荐先看这个）
"""
import sys

# ---- 模拟的 FAT32 卷 ----------------------------------------------------
FS = {
    "/bin": {},
    "/pkg": {
        "PKGS.TXT": "desktop|1.0|detachable desktop environment|desktop.catpkg|desktop\n"
                    "bc|1.0|tiny calculator|bc.catpkg|bc\n",
        "desktop.catpkg": "<CPKG02 signed payload: desk.ce, 24 KiB>",
        "bc.catpkg": "<CPKG02 signed payload: bc.ce, 4 KiB>",
    },
}

DESK_IMAGE = "<desk.ce: window mgr + taskbar + start menu + event loop>"

def installed(name):
    return (name + ".ce") in FS["/bin"]

def cmd_pkg_list():
    print("Package repository (/pkg/):")
    for line in FS["/pkg"]["PKGS.TXT"].strip().splitlines():
        name, ver, desc, f, cmd = line.split("|")
        mark = "[installed]" if installed(cmd) else "[available]"
        print("  %-9s %-5s %-12s - %s" % (name, ver, mark, desc))

def cmd_pkg_install(name):
    idx = FS["/pkg"]["PKGS.TXT"]
    rec = [l.split("|") for l in idx.strip().splitlines() if l.split("|")[0] == name]
    if not rec:
        print("pkg: no such package:", name); return
    cmd = rec[0][4]
    # 演示签名校验：包必须在卷上（真实版本这里做 RSA-512 验签）
    if installed(cmd):
        print("pkg: already installed:", name); return
    FS["/bin"][cmd + ".ce"] = DESK_IMAGE
    print("pkg: verified signature (CPKG02/RSA-512)")
    print("pkg: installed %s -> /bin/%s.ce" % (name, cmd))

def cmd_pkg_remove(name):
    if not installed(name):
        print("pkg: not installed:", name); return
    del FS["/bin"][name + ".ce"]
    print("pkg: removed /bin/%s.ce" % name)
    print("pkg: desktop extension detached; kernel unchanged")

def cmd_desktop():
    if not installed("desktop"):
        print("desktop: not installed (run 'pkg install desktop' first)")
        return
    print("desktop: entering graphical mode (video_mode_set 3)")
    print("+" + "-" * 58 + "+")
    print("| CatOS Desktop 1.0                            [_][X] |")
    print("+" + "-" * 58 + "+")
    print("|  +--------+  +--------+  +--------+                  |")
    print("|  |  My   |  | Termi- |  |  File  |                  |")
    print("|  | Comp. |  |  nal   |  | Mgr    |                  |")
    print("|  +--------+  +--------+  +--------+                  |")
    print("|                                                      |")
    print("|  (double-click an icon to open a window)             |")
    print("+" + "-" * 58 + "+")
    print("| [Start]  Desktop     (event loop: pollkey + mouse)    |")
    print("+" + "-" * 58 + "+")
    print("desktop: Start > Shut Down -> cde_exit -> back to text shell")

def cmd_help():
    print("Commands:")
    for c, d in [
        ("help", "show this help"),
        ("pkg list", "list packages"),
        ("pkg install NAME", "install a signed extension"),
        ("pkg remove NAME", "detach an extension"),
        ("desktop", "launch the installed desktop extension"),
        ("ls /bin", "list installed commands"),
        ("quit", "exit prototype"),
    ]:
        print("  %-20s %s" % (c, d))

def handle(line):
    line = line.strip()
    if not line: return True
    if line == "quit": return False
    if line == "help": cmd_help()
    elif line == "pkg list": cmd_pkg_list()
    elif line.startswith("pkg install "): cmd_pkg_install(line.split()[2])
    elif line.startswith("pkg remove "): cmd_pkg_remove(line.split()[2])
    elif line == "desktop": cmd_desktop()
    elif line == "ls /bin":
        print("  ".join(sorted(FS["/bin"])) or "(empty)")
    else:
        print("unknown command:", line)
    return True

def demo():
    steps = ["help", "pkg list", "pkg install desktop", "ls /bin",
             "desktop", "pkg remove desktop", "ls /bin", "pkg list"]
    for step in steps:
        print("\n$ " + step)
        handle(step)

def main():
    if "--demo" in sys.argv:
        demo()
        return
    print("CatOS CDE extension prototype (type 'help', 'quit' to exit)")
    while True:
        try:
            line = input("catos:/# ")
        except (EOFError, KeyboardInterrupt):
            print(); break
        if not handle(line): break

if __name__ == "__main__":
    main()
