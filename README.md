# 🚀 Feishu Desk HUD (飞书桌面任务看板)

这是一个基于 **ESP32-S3** 的桌面端任务管理看板，能够通过 **MQTT** 实时同步 **飞书 (Feishu/Lark)** 的待办任务。项目包含服务端（Node.js）和两款硬件终端（墨水屏 & LCD Sparkbot）的完整源码。

## ✨ 主要功能

- **双向同步**：后端自动拉取飞书任务状态。
- **实时推送**：通过 MQTT 协议将任务毫秒级推送到桌面硬件。
- **多终端支持**：
  - **E-Paper 版本**：1.54寸墨水屏，超低功耗，支持断电显示。
  - **Sparkbot 版本**：带彩屏的机器人终端。

## 📂 项目结构

```text
FEISHU_DESK_HUD/
├── backend/                # 服务端：负责飞书 API 鉴权与数据转发 (Node.js)
│   ├── index.js            # 核心逻辑
│   └── .env.example        # 配置模板 (AppID, MQTT Token)
│
├── firmware/               # 硬件端：ESP-IDF 项目源码
│   ├── ESP32-S3-ePaper-1.54/   # 墨水屏版本源码 (LVGL + EPD驱动)
│   └── ESP32-sparkbot/         # LCD屏版本源码 (LVGL + BSP)
│
└── README.md               # 项目说明

```

## 🛠️ 硬件准备

| 硬件名称 | 核心芯片 | 备注 |
| --- | --- | --- |
| **E-Paper Board** | ESP32-S3 | 需注意 GPIO 17 电源维持引脚 |
| **Sparkbot** | ESP32-S3 | 带LCD屏与六轴传感器 |
| **锂电池** | 3.7V LiPo | 需确认正负极线序 |

## 🚀 快速开始

### 第一步：配置飞书应用

1. 前往 [飞书开放平台](https://open.feishu.cn/app) 创建企业自建应用。
2. 开启机器人能力，并申请权限：`task:read` (获取任务信息)。
3. 获取 `App ID` 和 `App Secret`。

### 第二步：启动服务端 (Backend)

```bash
cd backend
npm install

# 复制配置文件并修改 (填入飞书 ID 和 MQTT 信息)
cp .env.example .env

# 启动服务
node index.js

```

### 第三步：烧录固件 (Firmware)

以墨水屏版本为例：

1. 需安装 **ESP-IDF v5.x** 环境。
2. 修改 `main/main.cpp` 中的 WiFi 和 MQTT 配置（或使用 menuconfig）。
3. 编译与烧录：

```bash
cd firmware/ESP32-S3-ePaper-1.54
idf.py build flash monitor

```

## ⚠️ 关键注意事项 (Troubleshooting)

1. **拔线掉电/无法开机问题**：
* 本项目使用的开发板采用**软开关机电路**。
* 代码初始化时**必须**将 `GPIO 17` 拉高 (`gpio_set_level(17, 1)`) 以锁定电源，否则松开电源键会立即断电。本项目源码已修复此问题。


2. **MQTT 证书**：
* 固件中嵌入了 DigiCert Global Root G2 证书，用于连接 EMQX Serverless 版本。如使用自建 MQTT 服务器，请替换 `mqtt_ca.crt`。


3. **LVGL 中文显示**：
* 包含自定义字体 `ui_font_FontCN16.c`，支持常用汉字显示。



## 🤝 贡献与致谢

* 图形库：[LVGL](https://lvgl.io/)
* 硬件驱动：Espressif BSP & Custom EPD Drivers
* API 支持：Feishu Open Platform

## 📄 开源协议

本项目采用 [MIT License](https://www.google.com/search?q=LICENSE) 开源。
