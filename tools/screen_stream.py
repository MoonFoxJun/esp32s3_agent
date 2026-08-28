#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC 屏幕 → ESP32 TFT 串流工具

把电脑屏幕（2K 也行）实时压缩成 JPEG，通过串口发给 ESP32，
ESP32 用 esp_jpeg 解码后显示在 240x320 的 ST7789 屏幕上。

用法:
    python screen_stream.py [--port COM3] [--baud 2000000] [--quality 75]
                            [--fps 8] [--stretch]

参数:
    --port     串口（默认 COM3）
    --baud     串流波特率（默认 2000000，必须和 stream_player.c 的 STREAM_BAUD 一致）
    --quality  JPEG 质量 1-100（默认 75；质量越高画面越清晰，但帧率越低）
    --fps      目标采集帧率（默认 8；实际帧率受串口带宽限制）
    --stretch  拉伸铺满全屏（默认保持比例，上下留黑边）

停止:
    Ctrl+C 即可。ESP32 会在约 2 秒后自动退出串流，回到 GIF 动画。

依赖:
    pip install mss pillow pyserial
"""

import argparse
import io
import struct
import sys
import time

import mss
import serial
from PIL import Image

W, H = 320, 240            # 屏幕分辨率（横屏）
CONSOLE_BAUD = 115200      # 控制台波特率（和 serial_console.c 一致）
MAGIC = b"\xAA\x55"        # 帧头魔数（和 stream_player.c 的 FRAME_MAGIC 一致）


def main():
    ap = argparse.ArgumentParser(description="PC 屏幕串流到 ESP32 TFT")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--quality", type=int, default=75)
    ap.add_argument("--fps", type=float, default=8.0)
    ap.add_argument("--stretch", action="store_true")
    args = ap.parse_args()

    # 1. 先在控制台波特率下通知 ESP32 进入串流模式
    print(f"[1/3] 连接 {args.port} @ {CONSOLE_BAUD}，发送 'stream on' ...")
    ser = serial.Serial(args.port, CONSOLE_BAUD, timeout=1)
    time.sleep(0.2)
    ser.write(b"stream on\n")
    time.sleep(0.5)                      # 等 ESP32 切换到高速波特率

    # 2. 切换到高速波特率，双方一致后开始发图
    ser.baudrate = args.baud
    print(f"[2/3] 切换到 {args.baud} baud，开始串流（Ctrl+C 停止）...")
    time.sleep(0.2)

    sct = mss.mss()
    monitor = sct.monitors[1]            # 主显示器
    buf = io.BytesIO()
    frame_count = 0
    t0 = time.time()
    fps_window = 0

    try:
        while True:
            # 截屏（原生分辨率，比如 2560x1440）
            shot = sct.grab(monitor)
            img = Image.frombytes("RGB", shot.size, shot.rgb)

            # 缩放到 240x320：默认保持比例 + 上下留黑边；--stretch 则铺满
            if args.stretch:
                img = img.resize((W, H), Image.BILINEAR)
            else:
                scale = min(W / img.width, H / img.height)
                nw, nh = round(img.width * scale), round(img.height * scale)
                img = img.resize((nw, nh), Image.BILINEAR)
                canvas = Image.new("RGB", (W, H), (0, 0, 0))
                canvas.paste(img, ((W - nw) // 2, (H - nh) // 2))
                img = canvas

            # JPEG 压缩后加帧头发送：魔数 + 长度(u32 大端) + JPEG 数据
            buf.seek(0)
            buf.truncate(0)
            img.save(buf, "JPEG", quality=args.quality)
            frame = buf.getvalue()
            ser.write(MAGIC + struct.pack(">I", len(frame)) + frame)

            frame_count += 1
            fps_window += 1
            if frame_count >= 50:
                dt = time.time() - t0
                print(f"    帧率 ≈ {fps_window / dt:.1f} fps, "
                      f"{len(buf.getvalue()) // 1024} KB/帧", flush=True)
                frame_count = 0
                fps_window = 0
                t0 = time.time()

            # 按目标帧率节流（串口本身也会自然限速）
            time.sleep(1.0 / args.fps)
    except KeyboardInterrupt:
        print("\n[3/3] 停止串流。ESP32 约 2 秒后自动回到 GIF 动画。")
    finally:
        sct.close()
        ser.close()


if __name__ == "__main__":
    main()
