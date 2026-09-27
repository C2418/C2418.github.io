# NFC-IOP 眼压设备原型源码

- 项目开发时间：2025 年 10 月。
- 源码公开整理：2026 年 9 月。
- 来源：作者提供的 `三个项目源码.zip` 中 `NFC-IOP/` 目录。

本目录展示眼压设备原型的数据采集与通信代码，包含 Android、NXP NHS3152 和 STM32 三部分。现有固件直接计算并传输的是电阻数据；此处没有经标定验证的电阻到眼压值换算，也没有临床测量结论。

## 目录

| 路径 | 内容 |
| --- | --- |
| `Android/app/src/main` | 蓝牙设备扫描、连接、特征值收发及数据解析界面 |
| `NXP/NHS3152/src` | 电阻采集、PWM 控制、NFC 共享内存与 NDEF 文本更新 |
| `NXP/NHS3152/mods/app_sel.h` | 应用回调映射 |
| `STM32/Core` | STM32F103 的串口接收与转发逻辑 |
| `STM32/NFC-IOP-1.1.ioc` | STM32CubeMX 外设配置 |
| `STM32/Drivers` | 原工程附带的 ST HAL/CMSIS 组件及其许可文件 |

推荐先看 `NXP/NHS3152/src/Nfc-iop.c` 与 `resistance_measurement.c`，再看 `STM32/Core/Src/main.c` 的串口链路，最后看 `Android/app/src/main/java/com/example/nfcapp/` 的蓝牙连接和解析代码。Android 程序使用 FFF0 服务、FFF1 接收特征与 FFF2 发送特征；这些编号应与实际蓝牙模块配置核对。

## 复现条件

- NXP 工程依赖 NHS3152 SDK 中的芯片、板级与 NDEF 模块。源包中的 NXP 库标注为 `NXP Confidential`，因此公开版没有再分发这些库；需按其许可自行取得并集成。
- STM32 部分基于 STM32F103CBU6，保留了 CubeMX 配置、Core 源文件、启动文件和原工程附带的 HAL/CMSIS。使用匹配的 ARM 工具链与外设配置编译，并核对板上引脚和串口波特率。
- Android 部分保留 Gradle 工程配置与应用源码。用 Android Studio 打开 `Android/`，安装与项目配置匹配的 Android SDK、Gradle 和依赖。

公开版排除了本机 `local.properties`、IDE 私人设置、CMake 缓存、历史编译产物和用于分析的示例数据文件。未修改的应用源码保持提供版本；第三方组件及生成代码的版权声明保留在文件中，不作为个人原创贡献。

## 当前代码边界

源码尚未在此次整理中重新编译、烧录或联调。静态阅读发现以下行为，需要在继续开发时处理：

- `Nfc-iop.c` 的注释写“每 5 秒”采集，但 `gNeedMeasurement` 在第一次执行后置为 `false`，持续供电时不会再次进入该采集分支。
- `STM32/Core/Src/main.c` 在主循环阻塞发送期间，接收中断仍会向同一缓冲区写入；随后清零索引可能丢失新数据。接收缓冲区满时也会直接丢弃字节。
- `resistance_measurement.c` 中部分转换完成等待没有超时退出，外设未返回完成标志时可能停在等待循环。

原理图、PCB 图和实物照片见[项目页面](https://c2418.github.io/posts/eye-pressure-prototype/)。这些材料用于展示原型与实现路径，不能推断眼压测量精度或医疗适用性。
