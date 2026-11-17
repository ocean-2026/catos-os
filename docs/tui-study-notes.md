# TUI 学习储备笔记（2026-11-17）

> 目的：对比 ncurses / Ratatui(tui-rs) / notcurses / DOS EDIT 的组件模型，落到 CatOS `gfx/console` 层可借鉴的设计要点，为后续 TTF/OTF 轮廓字体渲染与 TUI 应用铺路。
> 基线：HEAD `6cf1ff6`（v2.45）。本文为调研储备，不含代码改动。

## 1. CatOS 现有控制台 / 图形层盘点（源码已核实）

> 待填充：前端 render.c 双缓冲/脏追踪/scrollback；后端 be_text/be_gfx/be_vbe/be_vbe24；键盘抽象；位图字体；现有 TUI 雏形。

## 2. 对标组件模型

### 2.1 ncurses（经典 C TUI 库）

> 待填充：虚拟屏/实屏 diff、window/panel、keypad 转义解析、alternate screen、box 画线、属性与颜色对、鼠标。

### 2.2 Ratatui / tui-rs（即时模式 Rust TUI）

> 待填充：immediate-mode + 中间缓冲、每帧全量重绘 + diff、Layout 约束分栏、Widget/Frame、事件交后端、alternate screen + raw mode 进出场、Text/Line/Span。

### 2.3 notcurses（平面叠加现代 TUI）

> 待填充：ncplane z 序、行级 damage map、生成→blit 两阶段、24-bit RGB、EGC 簇。

### 2.4 DOS EDIT / QBasic IDE（字符界面经典范式）

> 待填充：菜单栏+下拉、编辑区、底部状态/快捷键行、modeless、双 pane 分屏、可选鼠标、字符画框、无 widget 工具箱。

## 3. 可借鉴的 CatOS 设计要点

### 3.1 双缓冲与面板/窗口抽象
### 3.2 备用屏幕（alternate screen）语义
### 3.3 box-drawing 原语
### 3.4 焦点管理与事件路由
### 3.5 按键抽象增强
### 3.6 事件循环与渲染模式选择
### 3.7 约束式布局
### 3.8 文本样式模型
### 3.9 鼠标（后续项）

## 4. TTF/OTF 轮廓字体渲染铺路

> 待填充：现位图字体限制；轮廓字体管线要点；CatOS 约束下的分阶段路线（宿主侧预光栅化 → 精简运行时光栅器）；与 gfx24 帧缓冲衔接。

## 5. TUI 视角差距清单（增量 / 设计级 / 暂缓）

> 待填充。

## 6. 参考来源

> 待填充。
