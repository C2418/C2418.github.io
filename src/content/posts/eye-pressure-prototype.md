---
title: NFC-IOP 眼压设备原型：硬件、NFC 与蓝牙数据链路
published: 2026-09-27
description: 2025 年 10 月的 NFC-IOP 眼压设备原型，展示硬件设计、NXP 与 STM32 固件、Android 应用及实物照片。
image: /projects/eye-pressure/images/prototype-boards.jpg
tags: [嵌入式, 硬件设计, STM32, PCB, 医疗设备原型]
category: 项目经历
draft: false
---

## 项目概览

- **项目开发时间：2025 年 10 月。**
- **项目资料整理：2026 年 9 月。**

NFC-IOP 的资料包含主板与线圈板的 PCB 布局、原理图、实物板卡，以及 NXP、STM32 和 Android 三部分源码。现有代码直接处理的是电阻数据和通信链路；项目资料中未提供经过标定验证的电阻到眼压值换算。

[浏览项目源码与复现说明](https://github.com/C2418/C2418.github.io/tree/main/public/projects/eye-pressure/source) · [下载源码 ZIP](/projects/eye-pressure/nfc-iop-source.zip)

## 软件组成

| 部分 | 源码中的职责 |
| --- | --- |
| NXP NHS3152 | 电阻采集、PWM 控制、NFC 共享内存和 NDEF 文本更新 |
| STM32F103 | 串口接收与转发，连接板上模块的数据通路 |
| Android 应用 | 蓝牙设备扫描、连接、FFF1/FFF2 特征收发和数据解析 |

阅读顺序可以从 `NXP/NHS3152/src/Nfc-iop.c` 和 `resistance_measurement.c` 开始，再看 `STM32/Core/Src/main.c`，最后看 Android 应用的 `NFCBluetoothManager.java` 与解析代码。源码目录 README 列出了复现依赖和当前代码边界。

## 原理图与 PCB

主板原理图中可见 STM32F103CBU6 主控、USB Type-C 接口、电池供电与充电、电量检测，以及标注为 MX-02P 的模块。图中还给出了调试或连接接口，便于结合实际板卡核对信号与供电路径。

![主板原理图：供电、主控和模块接口](/projects/eye-pressure/images/main-schematic.png)

另一张原理图展示独立电路及连接器。图片不足以确认全部器件型号和测量原理，因此这里仅按图展示其连接关系。

![独立电路原理图与连接器](/projects/eye-pressure/images/front-end-schematic.png)

![主板 PCB 布局图](/projects/eye-pressure/images/main-pcb-layout.png)

![线圈板 PCB 布局图，可见环形走线与中心电路](/projects/eye-pressure/images/coil-pcb-layout.png)

线圈板布局图显示环形铜线、中心电路和焊盘。图像用于说明布线与板级实现，不据此推断感应距离、灵敏度或测量精度。

## 实物照片

![线圈板实物照片](/projects/eye-pressure/images/coil-pcb-photo.jpg)

![原型板卡、电池与连接线的实物照片](/projects/eye-pressure/images/prototype-boards.jpg)

公开资料包含应用源码、设计图和照片；未附完整原理图工程、Gerber、标定记录或眼压测量数据。NXP 厂商库因原文件的许可声明未随源码再分发。源码尚未在本次整理中重新编译或实机联调，不能据此推断医疗测量性能。
