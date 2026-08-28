# ESP32-S3 AI Agent（语音/屏幕/灯带/QQ 全功能助手）

一个基于 **ESP32-S3 (N16R8)** + **ESP-IDF 6.0** 的本地 AI Agent：
通过 QQ 或串口对话，能控制 WS2812B 灯带、ST7789 屏幕、执行 Lua 脚本、学习新技能、修改自己的性格。

## ✨ 功能

| 功能 | 说明 |
|---|---|
| 💬 LLM 对话 | DeepSeek API，QQ 机器人 + 串口双通道，多轮上下文（NVS 持久化） |
| 🛠️ 工具调用 | LLM 可调用 12 个工具：灯带、屏幕、Lua、技能、人设、OTA、内存体检 |
| 💡 WS2812B 灯带 | 45 颗灯珠，`led.hsv/rgb/pixel/set/show` 全套 API，渐变彩虹/呼吸等效果 |
| 🖥️ ST7789 屏幕 | 320×240 横屏，GIF 动画播放（Flash 分区存储）+ PC 画面串流（实验性） |
| 📜 Lua 脚本引擎 | LLM 现场写脚本控制硬件；后台持续效果 + 互斥管理（一次一个效果） |
| 🧠 技能系统 | 验证过的 Lua 脚本存为技能（FATFS），一句话调用，断电不丢 |
| 🎭 人设管理 | 对 LLM 说一句就改性格/名字，NVS 持久化，断电不丢 |
| 📊 内存体检 | 随时查询堆剩余 + 每个任务的栈水位，防栈溢出/OOM |
| 📡 OTA 升级 | HTTPS 固件远程升级 |

## 🔧 硬件

- **芯片**: ESP32-S3 **N16R8**（16MB Flash + 8MB PSRAM），ESP-IDF **v6.0.2**
- **接线**:

| 外设 | 引脚 |
|---|---|
| ST7789 屏幕 (SPI) | SCLK=4, MOSI=5, RES=6, DC=7, CS=15, BL=16 |
| WS2812B 灯带 (RMT) | DATA=10（45 颗灯珠） |
| 串口 (UART0) | TX=43, RX=44（板载 USB-UART） |

> ⚠️ 面包板布线注意：LED 数据线要远离屏幕 SPI 线，平行走线会串扰导致灯带数据错乱。

## 🏗️ 架构

```
main/
├── agent/         AI 逻辑：agent_core（消息队列消费者）、llm_client、agent_tools、lua_engine
├── hardware/      硬件驱动：serial、wifi、led、qq、screen、gif、display_mgr、stream、skill_store
├── tools/         LLM 工具：led_set、lua_run_script、screen_switch、set_persona、skill_*、ota_update
└── main.c         启动编排
tools/              PC 端工具：gif2rgb565.py（GIF 转 RGB565）、screen_stream.py（屏幕串流）
```

关键设计：
- **消息队列**：串口/QQ 是生产者，`agent_core` 是唯一消费者
- **display_mgr 仲裁**：屏幕一次只允许一个应用（GIF/串流）占用
- **脚本互斥**：一次只允许一个 Lua 效果控制灯带，新效果自动停旧效果（结果会明确告知）
- **PSRAM 大缓冲**：帧缓冲/TLS/Lua 状态都在 PSRAM，内部 RAM 留给系统

## 🚀 构建与烧录

```bash
# 1. 安装 ESP-IDF v6.0.2 并设置环境
# 2. 修改配置：API Key、WiFi、QQ 凭证（见下）
# 3. 构建 + 烧录
idf.py build
idf.py flash
```

首次启动会自动连接 WiFi 并初始化所有外设；GIF 数据需单独烧入 `gif` 分区（见 tools/gif2rgb565.py）。

## 🔑 配置（发布前必读）

| 位置 | 配置项 | 说明 |
|---|---|---|
| `main/main.c` | `LLM_API_KEY` | DeepSeek API Key（勿公开！） |
| `main/main.c` | WiFi SSID/密码 | 首次启动的默认 WiFi（已有 NVS 存档则忽略） |
| `main/hardware/qq_channel.c` | `QQ_APP_ID` / `QQ_APP_SECRET` | QQ 开放平台机器人凭证（勿公开！） |

## 🎮 串口命令

```
screen gif | screen idle    切换屏幕显示
stream on                   进入 PC 串流模式（配合 tools/screen_stream.py）
lua <脚本>                  直接执行 Lua（调试/验证用）
lua bg <脚本>               后台持续执行
skill list | skill del <名> 技能管理
```

## 🧠 技能系统

LLM 写好的 Lua 脚本可存成技能复用（存在 Flash 的 `storage` FAT 分区）：
1. 串口验证脚本 → 2. 让 LLM `skill_save` → 3. 以后 QQ 一句话 `skill_run` 调用

## 📦 分区表

| 分区 | 大小 | 用途 |
|---|---|---|
| ota_0 / ota_1 | 4MB ×2 | 双 OTA 固件槽 |
| gif | 4MB | GIF 动画帧数据 |
| storage | 3.9MB | FATFS：技能存储 |

## ⚠️ 已知说明

- **屏幕串流**（`stream on` + `screen_stream.py`）为实验性功能：串口 2M 波特率，实际帧率受带宽限制（约 5fps）
- 改人设/技能后如需重置：串口 `lua led.set('off')` 之类不影响；NVS 存档可用 `clear` 清历史

## 📄 License

MIT License（见 LICENSE）
