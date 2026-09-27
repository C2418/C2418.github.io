---
title: 眼压设备硬件原型：主控、电源与 PCB 设计
published: 2026-09-27
description: 2025 年 10 月的眼压设备硬件原型项目，展示主控、电源、线圈板的设计图与实物照片。
image: /projects/eye-pressure/images/prototype-boards.jpg
tags: [嵌入式, 硬件设计, STM32, PCB, 医疗设备原型]
category: 项目经历
draft: false
---

## 项目概览

- **项目开发时间：2025 年 10 月。**
- **项目资料整理：2026 年 9 月。**

这是一组眼压设备硬件原型资料，包含主板与线圈板的 PCB 布局、原理图和实物板卡。现有图片能展示电路组织与打样过程，尚不足以证明眼压测量性能或临床适用性。

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

目前公开的资料限于上述设计图和照片；未附固件源码、完整原理图工程、Gerber、标定记录或眼压测量数据。项目页面展示的是硬件原型与设计过程，不作为医疗用途或性能结论。
