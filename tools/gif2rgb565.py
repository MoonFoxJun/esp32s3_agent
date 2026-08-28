#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GIF → RGB565 帧数据转换工具（给 ESP32 ST7789 屏幕用）

用法:
    python gif2rgb565.py <输入.gif> <输出.bin> [--bg R,G,B] [--max-w 240]

说明:
  - 把 GIF 每一帧解码成完整的合成画面（PIL 自动处理局部帧/透明/残留帧）
  - 等比缩放到屏幕宽度（默认 240），保持正方形 GIF 不变形
  - 每个像素转 RGB565（R5 G6 B5），按小端字节序打包（uint16 低字节在前）
  - 输出格式:
        [u32 帧数][u32 宽][u32 高]
        然后每帧: [u16 延时ms][RGB565 像素 宽*高*2 字节]

  --bg: 透明像素用什么底色填充（默认黑色 0,0,0）
  --max-w: 缩放后的目标宽度（高度按比例，超出屏幕高度会报错）
"""

import sys
import struct
from array import array

from PIL import Image

SCREEN_W = 320
SCREEN_H = 240


def parse_args(argv):
    if len(argv) < 3:
        print(__doc__)
        sys.exit(1)
    src, dst = argv[1], argv[2]
    bg = (0, 0, 0)
    max_w = 240   # GIF 保持 240x240 正方形（在 320x240 横屏上居中显示）
    i = 3
    while i < len(argv):
        if argv[i] == "--bg":
            bg = tuple(int(v) for v in argv[i + 1].split(","))
            i += 2
        elif argv[i] == "--max-w":
            max_w = int(argv[i + 1])
            i += 2
        else:
            i += 1
    return src, dst, bg, max_w


def rgb565_of(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def frame_to_rgb565_pixels(frame, bg):
    """把 PIL 帧转成 RGB565 小端字节串。"""
    if frame.mode == "RGBA":
        # 透明像素用背景色填充
        bg_img = Image.new("RGBA", frame.size, bg + (255,))
        frame = Image.alpha_composite(bg_img, frame)
    frame = frame.convert("RGB")
    raw = frame.tobytes()  # R,G,B 交错
    n = len(raw) // 3
    px = array("H")  # 本机小端 → 正好是面板要的字节序
    for i in range(n):
        r = raw[i * 3]
        g = raw[i * 3 + 1]
        b = raw[i * 3 + 2]
        px.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return px.tobytes()


def main():
    src, dst, bg, max_w = parse_args(sys.argv)

    im = Image.open(src)
    n = getattr(im, "n_frames", 1)
    fmt = im.format or "?"
    print(f"格式: {fmt}  尺寸: {im.size}  帧数: {n}  模式: {im.mode}")

    # 先看完整第一帧，确定缩放目标尺寸（保持宽高比）
    im.seek(0)
    first = im.convert("RGBA") if im.mode == "RGBA" else im.convert("RGB")
    w, h = first.size
    scale = max_w / w
    out_w = int(round(w * scale))
    out_h = int(round(h * scale))
    if out_w > SCREEN_W or out_h > SCREEN_H:
        print(f"错误: 缩放后 {out_w}x{out_h} 超出屏幕 {SCREEN_W}x{SCREEN_H}")
        sys.exit(1)
    print(f"缩放: {w}x{h} -> {out_w}x{out_h}")

    delays = []
    total = 0
    with open(dst, "wb") as f:
        f.write(struct.pack("<III", n, out_w, out_h))
        for i in range(n):
            im.seek(i)
            delay = im.info.get("duration", 100) or 100
            delays.append(delay)
            frame = im.convert("RGBA") if im.mode == "RGBA" else im.convert("RGB")
            frame = frame.resize((out_w, out_h), Image.LANCZOS)
            px = frame_to_rgb565_pixels(frame, bg)
            f.write(struct.pack("<H", delay))
            f.write(px)
            total += len(px)
            if i < 8 or i % 10 == 0:
                print(f"  帧 {i:3d}: 延时 {delay:3d}ms")

    size_mb = total / 1024 / 1024
    avg = sum(delays) // len(delays)
    loop_sec = sum(delays) / 1000.0
    print(f"完成: {n} 帧, 像素数据 {size_mb:.2f} MB, 平均延时 {avg}ms, 循环一圈 {loop_sec:.1f}s")
    print(f"写出: {dst}")


if __name__ == "__main__":
    main()
