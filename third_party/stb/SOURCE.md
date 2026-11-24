# stb_image 来源与集成口径

## 上游信息

- 仓库：https://github.com/nothings/stb.git
- 固定 commit：`6b5799bae8a1fe3792691ef4b72ae26434c3e799`
- 文件：`stb_image.h`（v2.34，public domain image loader）
- 许可证：**Public Domain / MIT 双许可（二选一）**，版权行
  `Copyright (c) 2017 Sean Barrett`（`stb_image.h:7777`）；
  全文见 `LICENSE-stb.txt`。
- 测量证据：`/home/user/work/browser-spike/SPIKE-MEASURE.md`、`measure.json`。

## 计划编译档（v0 集成时落地，本骨架不含编译接线）

只开 PNG / JPEG，关闭其余格式与宿主依赖：

```c
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_BMP
#define STBI_NO_GIF
#define STBI_NO_TGA
#define STBI_NO_PSD
#define STBI_NO_HDR
#define STBI_NO_PNM
```

实测（i386 32 位、`-Os`、freestanding，`stb-png-jpg` 档）：
`.text` 24.53 KiB、`.rodata` 2.36 KiB、`.data` 0.01 KiB、`.bss` 0.02 KiB。

## 内存口径（硬约束）

- 解码位图**必须走 mmap arena / 页分配器**，不许进 bump 池，更不许进 `.bss` 静态数组。
- 参考体积：800×600 RGB ≈ 1.37 MiB / RGBA ≈ 1.83 MiB；
  1920×1080 RGB ≈ 5.93 MiB / RGBA ≈ 7.91 MiB。
- v0 只取 24bpp 不透明 1:1；超限图片先在浏览器层拒绝或降采样（待 v0 定）。

## 未引入

- myhtml（LGPL-2.1-or-later，copyleft，否决）、lexbor（Apache-2.0 但体积/堆不达标，二期备选）
  的任何源码都不在本目录；本目录只有 stb_image 单文件与其许可证。
