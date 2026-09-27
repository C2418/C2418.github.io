#ifndef RESISTANCE_MEASUREMENT_H
#define RESISTANCE_MEASUREMENT_H

#include <stdbool.h>
#include <stdint.h>
#include <float.h>
#include <math.h>

// 包含芯片库头文件以获取类型定义
#include "chip.h"
#include "adcdac_nss.h"

// 如果 math.h 中的函数未定义，提供备用声明
#ifndef fmaxf
float fmaxf(float x, float y);
#endif

#ifndef fminf
float fminf(float x, float y);
#endif

// 兼容旧版本的电阻量程枚举
typedef enum {
    RES_RANGE_LOW = 0,    // 低阻范围（约 100Ω 到 1kΩ）
    RES_RANGE_HIGH = 1    // 高阻范围（约 1kΩ 到 100kΩ）
} ResistanceRange_T;

// 兼容旧版本的测量状态结构体
typedef struct {
    float lastRawResistance;     // 上一次测量的原始阻值
    float filteredResistance;    // 移动平均后的阻值
    float movingAverageAlpha;    // 移动平均滤波的平滑因子
    uint16_t stableCount;        // 连续稳定计数
    uint16_t maxStableCount;     // 稳定性判断的最大计数阈值
    ResistanceRange_T currentRange; // 当前使用的电阻测量量程
    bool rangeChangeRequested;   // 是否请求切换测量量程
} ResistanceMeasurementState;

// 测量配置结构体
typedef struct {
    float target_current;       // 目标测量电流
    float min_resistance;       // 最小可测电阻
    float max_resistance;       // 最大可测电阻
    uint8_t max_iterations;     // 最大测量迭代次数
    uint16_t sample_delay_ms;   // 采样间隔延迟
    float voltage_diff_threshold; // 电压差阈值
} ResistanceMeasurementConfig;

// I2D增益模式枚举
typedef enum {
    I2D_GAIN_MODE_1X = 0,   // 1倍增益，低量程
    I2D_GAIN_MODE_10X = 1,  // 10倍增益，中量程
    I2D_GAIN_MODE_100X = 2  // 100倍增益，高量程
} I2DGainMode;

// 测量状态枚举
typedef enum {
    MEASUREMENT_STATUS_OK = 0,
    MEASUREMENT_STATUS_ERROR = 1,
    MEASUREMENT_STATUS_TIMEOUT = 2
} MeasurementStatus;

// 校准参数结构体
typedef struct {
    float lsb_current;      // 最小电流分辨率
    float dac_calibration;  // DAC校准系数
    float adc_calibration;  // ADC校准系数
} CalibrationParameters;

// 调试信息结构体
typedef struct {
    float drive_voltage;
    float sense_voltage;
    float voltage_diff;
    float current;
    int raw_i2d_value;
    I2DGainMode gain_mode;
    float lastRawResistance;     // 兼容旧版本
    float movingAverageAlpha;    // 兼容旧版本
    uint16_t stableCount;        // 兼容旧版本
    bool rangeChangeRequested;   // 兼容旧版本
} ResistanceMeasurementDebug;

// 全局变量声明
extern MeasurementStatus g_measurementStatus;
extern ResistanceMeasurementDebug g_resistanceDebug;
extern const float g_i2dGainTable[];

// 新增的全局调试变量声明
extern float g_debugTargetCurrent;
extern I2DGainMode g_debugGainMode;
extern uint32_t g_debugI2DStatus;
extern uint32_t g_debugMeasurementCount;
extern float g_debugDriveVoltage;
extern float g_debugSenseVoltage;

// 添加 ADC 参考电压常量
#define ADC_REF_VOLTAGE 1.6f

// 添加默认配置
static const ResistanceMeasurementConfig default_config = {
    .target_current = 1e-4f,        // 修改为100µA
    .min_resistance = 10.0f,         // 最小10Ω
    .max_resistance = 100000.0f,     // 最大100kΩ
    .max_iterations = 3,             // 最大迭代次数
    .sample_delay_ms = 10,           // 10ms采样延迟
    .voltage_diff_threshold = 0.001f // 1mV电压差阈值
};

// 函数声明
void ResistanceMeasurement_Init(void);
void ResistanceMeasurement_DeInit(void);
float ResistanceMeasurement_Process(void* state, const void* config);
float MeasureSingleResistance(const ResistanceMeasurementConfig* config);
float calculateI2DCurrent(int raw_i2d_value, I2DGainMode gain_mode, uint16_t integration_time_us);
I2DGainMode selectOptimalI2DGain(float voltage_diff, float target_current);
float adjustDACOutput(float resistance, float target_current);

// 新增的函数声明
void ConfigureADCDAC(void);
void ConfigureI2D(I2DGainMode gain_mode, uint16_t integration_time_us);
float CalibrateADCDAC(void);
float CalibrateI2D(void);

// 添加 ADCDAC 电压设置和读取函数声明
float Chip_ADCDAC_SetDACVoltage(NSS_ADCDAC_T *pADCDAC, float voltage);
float Chip_ADCDAC_ReadADCVoltage(NSS_ADCDAC_T *pADCDAC, ADCDAC_IO_T channel);
float MeasureADCVoltage(ADCDAC_IO_T channel);
float MeasureI2DCurrent(float target_current);

// 新增的调试函数声明
float GetDebugDriveVoltage(void);
float GetDebugSenseVoltage(void);
float GetDebugVoltageDifference(void);
float GetDebugCurrent(void);
int GetDebugRawI2DValue(void);
float GetDebugCalibrationFactor(void);
bool GetDebugIsCalibrated(void);

#endif // RESISTANCE_MEASUREMENT_H