#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CatOS 命令重设计样式预览 -- 基于 Textual 8.x TUI 组件库

用途：让用户先在 PowerShell / cmd 里看到"命令重设计后长什么样"，
      再决定是否值得移植到 CatOS 内核的 tui.c。

运行（Windows）：
    set PYTHONPATH=E:\\Tools\\textual
    python D:\\CatOs\\deliverables\\tui-preview\\catos_tui_preview.py

说明：本程序【只做样式示例，不含任何真实功能】。所有数据都是写死的演示数据。
左边选命令，右边用真实的 TUI 组件（Table / Tree / ProgressBar / Sparkline /
Markdown / Static）渲染该命令重设计后的界面。

按键：↑↓ 选择命令 / Enter 切换 / q 退出
"""
import os
import sys

# 让脚本在没设 PYTHONPATH 时也能直接双击运行
_LIB = r"E:\Tools\textual"
if os.path.isdir(_LIB) and _LIB not in sys.path:
    sys.path.insert(0, _LIB)

from textual.app import App, ComposeResult
from textual.widgets import (
    Header, Footer, ListView, ListItem, Label, Static, DataTable,
    Tree, ProgressBar, Sparkline, Markdown,
)
from textual.containers import Horizontal, Vertical, ScrollableContainer
from textual.binding import Binding
from textual.reactive import reactive

# ---- CatOS vivid 配色（与 v2.63 高对比主题一致）--------------------------
CSS = """
Screen { background: #0b0e14; color: #e6edf3; }
Header  { background: #12b886; color: #04140f; }
Footer  { background: #161b22; color: #8b949e; }

#sidebar {
    width: 28;
    background: #0f141b;
    border-right: solid #1f6feb;
}
#sidebar Label.title {
    color: #58a6ff;
    text-style: bold;
    padding: 1 2;
    height: 3;
}
ListView { background: #0f141b; color: #c9d1d9; }
ListItem { padding: 0 2; height: 2; }
ListItem:hover { background: #1f6feb; color: #ffffff; }
ListView > ListItem.--highlight { background: #1f6feb; color: #ffffff; }

#content { padding: 1 2; background: #0b0e14; }
#cmdtitle {
    color: #7ee787;
    text-style: bold;
    height: 2;
    content-align: left middle;
}
#cmddesc { color: #8b949e; height: 2; }

.panel {
    background: #11161f;
    border: round #30363d;
    padding: 1 2;
    margin: 0 0 1 0;
}
.panel-title { color: #58a6ff; text-style: bold; }

DataTable { height: auto; background: #11161f; color: #c9d1d9; }
DataTable > .datatable--header { background: #1f6feb; color: #ffffff; text-style: bold; }
ProgressBar { background: #161b22; }
Markdown { background: #11161f; }
Tree { background: #11161f; color: #c9d1d9; }
Sparkline { color: #7ee787; background: #11161f; }

.kbd { color: #f0883e; text-style: bold; }
.accent { color: #58a6ff; }
.ok { color: #7ee787; }
.warn { color: #f2cc60; }
.err { color: #ff7b72; }
.dim { color: #6e7681; }
"""


COMMANDS = [
    ("help",     "命令帮助",         "分组卡片 + 两列表格，替代原来的一长条"),
    ("ls",       "列目录 / 树",      "eza/lsd 风格：布局×列块×分级着色；按 1/2/3/4/5/6 切换"),
    ("cat",      "查看文件",         "语法高亮 + 行号面板"),
    ("ps",       "进程列表",         "任务表格，ring/状态用颜色区分"),
    ("top",      "系统监视",         "Sparkline 曲线 + 进度条 + 实时指标"),
    ("theme",    "主题切换",         "色卡预览 + 当前主题高亮"),
    ("reg",      "注册表",           "分节树 + 键值表格"),
    ("net",      "网络状态",         "NIC 状态面板 + 流量进度条"),
    ("caps",     "权限能力",         "13 项能力勾选清单 + 预设标签"),
    ("whoami",   "当前用户",         "身份卡片 + 能力徽章"),
    ("uptime",   "运行时间",         "大数字面板 + 启动时间"),
]


def ls_flags(mode: int) -> str:
    """ls 展示模式对应的命令行参数后缀。

    设计参考 eza + lsd 的正交维度：布局(grid/long/tree) × 列块(--blocks)
    × 分级着色(--color-scale)。6 个模式是这套正交空间的代表性组合。
    """
    flags = [
        "",                              # 0 默认 grid（eza 默认，目录优先）
        " -l",                           # 1 详细 long
        " -t",                           # 2 树（lsd 风格 -t=tree）
        " -lt",                          # 3 树 + 详情
        " -l --color-scale=size",        # 4 eza: 按大小分级着色
        " -l --blocks=perm,size,name",   # 5 lsd: 列块自由拼装
    ]
    return flags[mode] if 0 <= mode < len(flags) else ""


class CommandItem(ListItem):
    def __init__(self, key: str, label: str) -> None:
        super().__init__()
        self.key = key
        self.label = label

    def compose(self) -> ComposeResult:
        yield Label(f"  {self.key:<10} {self.label}")


class CatOSTUIDemo(App[None]):
    CSS = CSS
    TITLE = "CatOS 命令重设计 · 样式预览"
    SUB_TITLE = "Textual TUI 组件库 · 仅示例样式，无真实功能"
    BINDINGS = [
        Binding("q", "quit", "退出"),
        Binding("enter", "select", "预览", show=True),
        # ls 参数模式：仅在 ls 视图下生效（数字键全局绑定，内部判断 current）
        Binding("1", "ls_mode(0)", "ls 默认", show=False),
        Binding("2", "ls_mode(1)", "ls -l", show=False),
        Binding("3", "ls_mode(2)", "ls -t", show=False),
        Binding("4", "ls_mode(3)", "ls -lt", show=False),
        Binding("5", "ls_mode(4)", "ls 分级着色", show=False),
        Binding("6", "ls_mode(5)", "ls 列块拼装", show=False),
    ]

    current = reactive("help")
    ls_mode = reactive(0)   # 见 ls_flags(): 0..5 六种正交组合

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal():
            with Vertical(id="sidebar"):
                yield Label("CatOS 命令", classes="title")
                yield ListView()
            with ScrollableContainer(id="content"):
                yield Static(id="cmdtitle")
                yield Static(id="cmddesc")
                yield Static(id="preview")
        yield Footer()

    def on_mount(self) -> None:
        lv = self.query_one(ListView)
        # Textual 要求 widget 挂载后才能 append；compose() 阶段不行
        lv.extend([CommandItem(key, label) for key, label, _ in COMMANDS])
        lv.focus()
        self._render_preview("help")

    def on_list_view_highlighted(self, event: ListView.Highlighted) -> None:
        if event.item and isinstance(event.item, CommandItem):
            self._render_preview(event.item.key)

    def action_select(self) -> None:
        item = self.query_one(ListView).highlighted_child
        if item and isinstance(item, CommandItem):
            self._render_preview(item.key)

    def action_ls_mode(self, mode: int) -> None:
        """数字键 1..6 切换 ls 的展示模式（仅 ls 视图响应）。"""
        if self.current != "ls":
            return
        self.ls_mode = mode
        self._render_preview("ls")

    # ---- 渲染分发 --------------------------------------------------------
    def _render_preview(self, key: str) -> None:
        self.current = key
        desc = next((d for k, _, d in COMMANDS if k == key), "")
        # ls 标题带上当前参数，直观显示模式切换
        cmdline = f"ls{ls_flags(self.ls_mode)}" if key == "ls" else key
        self.query_one("#cmdtitle", Static).update(f"▸ catos:{cmdline}")
        self.query_one("#cmddesc", Static).update(desc)
        host = self.query_one("#preview", Static)
        renderer = getattr(self, f"_view_{key}", None)
        if renderer:
            host.update(renderer())
        else:
            host.update(f"[dim]（{key} 预览待补）[/dim]")

    # ---- 各命令样式 ------------------------------------------------------
    def _view_help(self) -> str:
        return """[panel-title]核心命令[/panel-title]
  [accent]help[/accent]       显示本帮助                    [accent]clear[/accent]      清屏
  [accent]ls[/accent]         列目录(6 模式见下)          [accent]cat[/accent]       查看文件
  [accent]cd[/accent]         切换目录                      [accent]mkdir[/accent]     创建目录

[panel-title]系统[/panel-title]
  [accent]ps[/accent]         列出任务                      [accent]top[/accent]       系统监视
  [accent]uname[/accent]      系统信息                      [accent]uptime[/accent]    运行时间

[dim]────────────────────────────────────────────────────[/dim]
[warn]↑↓ 选择命令 · Enter 确认 · q 退出[/warn]  —— 分组卡片取代原来的长条列表
"""

    def _view_ls(self) -> str:
        m = self.ls_mode
        if m == 1:
            return self._ls_long()
        if m == 2:
            return self._ls_tree(detail=False)
        if m == 3:
            return self._ls_tree(detail=True)
        if m == 4:
            return self._ls_color_scale()
        if m == 5:
            return self._ls_blocks()
        return self._ls_default()

    # -- ls 默认：紧凑多列，按类型着色 -------------------------------------
    def _ls_default(self) -> str:
        return f"""[warn]模式 [1] 默认[/warn]  [dim]2 -l · 3 -t · 4 -lt[/dim]
[dim]────────────────────────────────────────────────────[/dim]
[accent]  boot/[/accent]     [accent]  etc/[/accent]      [accent]  home/[/accent]     [accent]  tmp/[/accent]
[ok]  shell.ce*[/ok]   [ok]  hello.ce*[/ok]    [warn]  env[/warn]         [warn]  reg[/warn]
[warn]  passwd[/warn]     [warn]  group[/warn]      [dim]  .hidden[/dim]      [dim]  notes.txt[/dim]

[dim]12 items · 4 dir · 2 exec · 6 file[/dim]  [accent]蓝=目录[/accent] [ok]绿=可执行[/ok] [warn]黄=文件[/warn] [dim]灰=隐藏[/dim]
"""

    # -- ls -l：详细，权限位 r/w/x 分色 ------------------------------------
    def _ls_long(self) -> str:
        return f"""[warn]模式 [2] -l 详细[/warn]  [dim]1 默认 · 3 -t · 4 -lt[/dim]
[dim]────────────────────────────────────────────────────[/dim]
[dim]PERM [/dim]    UID  GID    SIZE   DATE        NAME
[ok]d[/ok][accent]rwx[/accent][warn]r-x[/warn][accent]r-x[/accent]  root root   4.0K  11-20 09:12  [accent]boot/[/accent]
[ok]d[/ok][accent]rwx[/accent][warn]r-x[/warn][accent]r-x[/accent]  root root   4.0K  11-20 09:12  [accent]etc/[/accent]
[ok]d[/ok][accent]rwx[/accent][warn]rwx[/warn][accent]rwx[/accent]  root root   4.0K  11-20 09:12  [accent]tmp/[/accent]
[dim]-[/dim][accent]rw-[/accent][warn]---[/warn][dim]---[/dim]   root root    128  11-20 09:12  [warn]env[/warn]
[dim]-[/dim][accent]rw-[/accent][warn]---[/warn][dim]---[/dim]   root root   2.1K  11-20 09:12  [warn]reg[/warn]
[dim]-[/dim][accent]rwx[/accent][warn]r-x[/warn][accent]r-x[/accent]  root root   820K  11-20 09:12  [ok]shell.ce*[/ok]

[accent]r 蓝=读[/accent] [warn]w 黄=写[/warn] [ok]x 绿=执行[/ok] [dim]- 灰=无[/dim]
"""

    # -- ls -t / -lt：树状（-lt 每节点带权限+大小）-------------------------
    def _ls_tree(self, detail: bool) -> str:
        mode_tag = "[warn]模式 [4] -lt 树状+详情[/warn]" if detail else "[warn]模式 [3] -t 树状[/warn]"
        others = "1 默认 · 2 -l · 4 -lt" if not detail else "1 默认 · 2 -l · 3 -t"
        if not detail:
            body = """[accent]/[/accent]
├── [accent]bin/[/accent]
│   ├── [ok]shell.ce*[/ok]
│   └── [ok]hello.ce*[/ok]
├── [accent]etc/[/accent]
│   ├── [ok]reg[/ok]
│   ├── [warn]passwd[/warn]
│   └── [warn]group[/warn]
├── [accent]home/[/accent]
│   └── [accent]user/[/accent]
│       └── [dim]readme.txt[/dim]
└── [accent]tmp/[/accent]
"""
        else:
            body = """[accent]/[/accent]                              [dim]PERM       SIZE[/dim]
├── [accent]bin/[/accent]                           [ok]drwxr-xr-x[/ok]  4.0K
│   ├── [ok]shell.ce*[/ok]                    [accent]rwxr-xr-x[/accent]  820K
│   └── [ok]hello.ce*[/ok]                    [accent]rwxr-xr-x[/accent]   12K
├── [accent]etc/[/accent]                           [ok]drwxr-xr-x[/ok]  4.0K
│   ├── [ok]reg[/ok]                               [accent]rw-------[/accent]  2.1K
│   └── [warn]passwd[/warn]                         [accent]rw-------[/accent]  1.2K
└── [accent]home/[/accent]                          [ok]drwxr-xr-x[/ok]  4.0K
    └── [accent]user/[/accent]                     [ok]drwx------[/ok]  4.0K
"""
        return f"""{mode_tag}  [dim]{others}[/dim]
[dim]────────────────────────────────────────────────────[/dim]
{body}
[dim]tree 为 ls -t 的别名，不再单独占一个命令[/dim]
"""

    # -- 模式 5：eza --color-scale —— 按文件大小/年龄分级着色 ---------------
    # 借鉴点：eza 不只用颜色区分"类型"，还能用 --color-scale=age|size|all
    # 对同一类型按数值渐变成色阶，一眼看出"哪些是大文件/老文件"。
    # CatOS 取舍：8x16 ROM 字体无 Nerd Font 私有字形，用 ASCII 色块 + 16 色
    # 模拟 eza 的 gradient；theme.yml 的位置由 /etc/reg [display] 承担。
    def _ls_color_scale(self) -> str:
        return """[warn]模式 [5] -l --color-scale=size[/warn]  [dim]eza 分级着色[/dim]
[dim]────────────────────────────────────────────────────[/dim]
[dim]  env         128 B  ▏[/dim]              [dim]最冷[/dim]
[dim]  group      1.2 K  ▎[/dim]
[warn]  reg        2.1 K  ▍[/warn]
[warn]  passwd    12.4 K  ▌[/warn]
[err]  hello.ce  124 K  ▐[/err]
[err]  shell.ce  820 K  █[/err]              [err]最热[/err]

[dim]色阶按 size 单调递增；--color-scale=age 则按修改时间[/dim]
[accent]蓝→黄→红 渐变[/accent] [dim]（fixed / gradient 两种模式）[/dim]
[dim]CatOS: 用 /etc/reg [display] 存色阶配置，无需 theme.yml[/dim]
"""

    # -- 模式 6：lsd blocks —— 长格式列自由拼装 ----------------------------
    # 借鉴点：lsd 的 config.yaml 用 blocks: [permission, user, group,
    # size, date, name] 定义列，可任意增删/换序；colours.yaml / icons.yaml
    # 三件套彼此独立覆盖。这比 eza 的一堆 --no-user/--no-permissions 开关
    # 更正交——一个 --blocks 参数替代十几个否定开关。
    # CatOS 取舍：无 Nerd Font，icons.yaml 那套字形不适用；权限列换成
    # CatOS 的 rwx+ACL 标记，owner 列显示 uid 名（/etc/passwd）。
    def _ls_blocks(self) -> str:
        return """[warn]模式 [6] -l --blocks=perm,size,name[/warn]  [dim]lsd 列块[/dim]
[dim]────────────────────────────────────────────────────[/dim]
[dim]可用列块（勾选即出现，可换序）：[/dim]
[ok]✓[/ok] perm  [ok]✓[/ok] size  [ok]✓[/ok] name   [dim]□ user  □ group  □ date  □ inode[/dim]

[warn]PERM       SIZE     NAME[/warn]
[ok]drwxr-xr-x[/ok]   4.0K     [accent]boot/[/accent]
[ok]drwxr-xr-x[/ok]   4.0K     [accent]etc/[/accent]
[accent]rw-------[/accent]    2.1K     reg
[accent]rw-------[/accent]    1.2K     passwd
[accent]rwxr-xr-x[/accent]  820K     [ok]shell.ce[/ok]  [dim][RO][/dim]

[dim]一个 --blocks 替代 eza 的 --no-user/--no-permissions 等 6+ 否定开关[/dim]
[dim]CatOS: 配置存 /etc/reg [display].lsblocks，无需 config.yaml[/dim]
"""

    def _view_cat(self) -> str:
        return """[panel-title]/etc/reg — 行号 + 语法高亮[/panel-title]
 1  [accent][system][/accent]
 2  lang=zh
 3  theme=dark
 4  videomode=gfx24
 5
 6  [accent][env][/accent]
 7  PATH=/bin
 8  [ok]EDITOR=edit[/ok]
"""
    def _view_ps(self) -> str:
        return f"""[panel-title]TID  UID   RING  STATE   NAME[/panel-title]
  1  [err]  0[/err]  ring0  [ok]RUN [/ok]  kernel
  4  [accent]100[/accent]  ring3  [ok]RUN [/ok]  shell
  7  [accent]100[/accent]  ring3  [warn]SLP [/warn]  http.ce
  9  [accent]100[/accent]  ring3  [err]ZMB [/err]  yes.ce   [dim](zombie)[/dim]

[dim]4 tasks · 1 foreground · 2 background[/dim]
"""
    def _view_top(self) -> str:
        bars = "▁▂▃▅▆▇█▆▅▃▂▁▃▅▆▇█▇▆▅▃▂"
        return f"""[panel-title]CPU[/panel-title]  [ok]{bars}[/ok]  37%
[panel-title]MEM[/panel-title]  [accent]{'█'*18}{'░'*10}[/accent]  64%  (16M / 25M)
[panel-title]NET[/panel-title]  [warn]{'█'*9}{'░'*19}[/warn]  32%  rx 1.2M  tx 340K

[dim]uptime 00:42:17 · 4 tasks · 100Hz tick[/dim]
"""
    def _view_theme(self) -> str:
        return """[panel-title]可用主题[/panel-title]
[ok]● dark[/ok]   [accent]████████[/accent]  #0b0e14  #58a6ff  #7ee787  ← 当前
[dim]○ light[/dim]  ████████  #f6f8fa  #0969da  #1a7f37

[dim]token 预览：canvas / surface / accent / success / error[/dim]
[accent]accent[/accent] [ok]success[/ok] [warn]warning[/warn] [err]error[/err]
"""
    def _view_reg(self) -> str:
        return """[accent][system][/accent]
  lang       = [ok]zh[/ok]
  theme      = [ok]dark[/ok]
  videomode  = gfx24
  panicmode  = reboot
  size       = 1
[accent][env][/accent]
  PATH       = /bin
  EDITOR     = edit

[dim]reg list [SECTION] · reg get SEC KEY · reg set SEC KEY VAL[/dim]
"""
    def _view_net(self) -> str:
        return """[panel-title]PCnet (ISA) — eth0[/panel-title]
  MAC        [accent]00:50:56:xx:xx:xx[/accent]
  Link       [ok]UP[/ok]  100 Mbps  full-duplex
  IP         [accent]10.0.2.15[/accent]
  RX         [ok]████████████[/ok][dim]████████[/dim]  1.2 MB
  TX         [warn]████[/warn][dim]████████████████[/dim]  340 KB

[dim]ping 8.8.8.8 · dns NAME · netdiag[/dim]
"""
    def _view_caps(self) -> str:
        return """[panel-title]能力 (CAP_*) · 预设：[ok]operator[/ok][/panel-title]
[ok][x][/ok] NET      网络
[ok][x][/ok] KILL     杀他人任务
[dim][ ][/dim] FSADMIN  文件系统管理
[ok][x][/ok] AUDIO    音频
[dim][ ][/dim] USERADM  用户管理
[dim][ ][/dim] DEVICE   裸设备
[ok][x][/ok] FSCK     fsck
[ok][x][/ok] HWINFO   硬件信息
[ok][x][/ok] DIAG     内核诊断

[dim]caps 0x... · chcaps HEX NAME · useredit[/dim]
"""
    def _view_whoami(self) -> str:
        return """╭─ [accent]用户卡片[/accent] ─────────────────────────╮
│  [ok]root[/ok]  [dim]uid=0  gid=0[/dim]                        │
│  home=/root   shell=/bin/shell.ce              │
│  能力  [err]CAP_ALL ●●●●●[/err]                       │
│  组    [accent]root[/accent]                               │
╰─────────────────────────────────────────────╯
"""
    def _view_uptime(self) -> str:
        return """[panel-title]运行时间[/panel-title]
  [ok]00:42:17[/ok]   [dim]since boot[/dim]
  ────────────────────
  4 tasks · 100 Hz tick · 25 MB RAM
"""


def main() -> None:
    if __name__ == "__main__":
        CatOSTUIDemo().run()


main()
