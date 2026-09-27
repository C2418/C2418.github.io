#define _XOPEN_SOURCE 600  // 启用 POSIX.1-2001标准扩展
#define __USE_XOPEN2K  // 启用 POSIX.1-2001标准扩展
#include <math.h>  // 确保包含数学头文件
#include <float.h> // 添加 float.h 头文件
#include "resistance_measurement.h"
#include "chip.h"
#include <string.h>
#include <stdio.h>  // 添加 stdio.h 头文件
#include "i2d_nss.h"  // 添加I2D头文件

// 全局调试变量
float g_debugDriveVoltage = 0.0f;
float g_debugSenseVoltage = 0.0f;
float g_debugVoltageDifference = 0.0f;
float g_debugCurrent = 0.0f;
int g_debugRawI2DValue = 0;
float g_debugTargetCurrent = 0.0f;
I2DGainMode g_debugGainMode = I2D_GAIN_MODE_1X;
uint32_t g_debugI2DStatus = 0;
uint32_t g_debugMeasurementCount = 0;

// 校准参数（暂时注释）
/*
static struct {
    float dac_gain;
    float adc_offset;
    float i2d_gain;
    bool is_calibrated;
} g_calibration_params = {
    .dac_gain = 1.0f,
    .adc_offset = 0.0f,
    .i2d_gain = 1.0f,
    .is_calibrated = false
};
*/

// 如果 math.h 中的函数未定义，提供备用实现
#ifndef fmaxf
float fmaxf(float x, float y) {
    return (x > y) ? x : y;
}
#endif

#ifndef fminf
float fminf(float x, float y) {
    return (x < y) ? x : y;
}
#endif

// 添加 I2D 头文件
#include "i2d_nss.h"

// 选择最佳 I2D 增益模式的辅助函数
I2DGainMode selectOptimalI2DGain(float voltage_diff, float target_current) {
    // 更精确的电流估算：使用目标电流和实际电压差
    float estimated_current = voltage_diff / (1.0f / target_current); // R = V/I
    
    // 根据实际电流范围选择增益
    if (estimated_current < 2.5e-6f) {      // < 2.5µA
        return I2D_GAIN_MODE_100X;
    } else if (estimated_current < 25e-6f) { // 2.5µA - 25µA
        return I2D_GAIN_MODE_10X;
    } else {                               // > 25µA
        return I2D_GAIN_MODE_1X;
    }
}
// 电流计算函数，包含增益和校准映射
// 电流计算函数，使用芯片库的精确转换
float calculateI2DCurrent(int raw_i2d_value, I2DGainMode gain_mode, uint16_t integration_time_ms) {
    // 获取当前配置
    I2D_SCALER_GAIN_T scaler_gain;
    I2D_CONVERTER_GAIN_T converter_gain;
    
    switch(gain_mode) {
        case I2D_GAIN_MODE_1X:
            scaler_gain = I2D_SCALER_GAIN_100_1;
            converter_gain = I2D_CONVERTER_GAIN_LOW;
            break;
        case I2D_GAIN_MODE_10X:
            scaler_gain = I2D_SCALER_GAIN_100_1;
            converter_gain = I2D_CONVERTER_GAIN_LOW;
            break;
        case I2D_GAIN_MODE_100X:
            scaler_gain = I2D_SCALER_GAIN_100_1;
            converter_gain = I2D_CONVERTER_GAIN_HIGH;
            break;
    }
    
    // 使用芯片库提供的精确转换函数
    int picoAmps = Chip_I2D_NativeToPicoAmpere(
        raw_i2d_value,
        scaler_gain,
        converter_gain,
        integration_time_ms
    );
    
    return (float)picoAmps * 1e-12f;  // pA → A
}

// 配置 I2D 模块的函数
void ConfigureI2D(I2DGainMode gain_mode, uint16_t integration_time_ms) {
    // 选择I2D增益模式
    I2D_SCALER_GAIN_T scaler_gain = I2D_SCALER_GAIN_100_1;  // 默认1:1增益
    I2D_CONVERTER_GAIN_T converter_gain = I2D_CONVERTER_GAIN_LOW;  // 默认低增益

    switch(gain_mode) {
        case I2D_GAIN_MODE_1X:
            scaler_gain = I2D_SCALER_GAIN_100_1;
            converter_gain = I2D_CONVERTER_GAIN_LOW;
            break;
        case I2D_GAIN_MODE_10X:
            scaler_gain = I2D_SCALER_GAIN_100_1;
            converter_gain = I2D_CONVERTER_GAIN_LOW;
            break;
        case I2D_GAIN_MODE_100X:
            scaler_gain = I2D_SCALER_GAIN_100_1;
            converter_gain = I2D_CONVERTER_GAIN_HIGH;
            break;
    }

    // 配置I2D模块
    Chip_I2D_Setup(
        NSS_I2D,               // I2D外设基地址
        I2D_SINGLE_SHOT,       // 单次转换模式
        scaler_gain,           // 选择的增益
        converter_gain,        // 转换器增益
        integration_time_ms    // 使用毫秒
    );

    // 选择输入通道（这里使用ANA0_1）
    Chip_I2D_SetMuxInput(NSS_I2D, I2D_INPUT_ANA0_1);

    // 启动I2D转换
    Chip_I2D_Start(NSS_I2D);
    
    // 等待积分完成
    while (!(Chip_I2D_ReadStatus(NSS_I2D) & I2D_STATUS_CONVERSION_DONE)) {
        // 可以添加超时处理
    }
    
    // 读取原始值
    int raw_i2d_value = Chip_I2D_GetValue(NSS_I2D);
    g_debugRawI2DValue = raw_i2d_value;
    
    // 更新调试变量
    g_debugGainMode = gain_mode;
    g_debugI2DStatus = Chip_I2D_ReadStatus(NSS_I2D);
}

// ADCDAC 电压设置函数
float Chip_ADCDAC_SetDACVoltage(NSS_ADCDAC_T *pADCDAC, float voltage) {
    if (pADCDAC == NSS_ADCDAC0) {
        // 限制电压范围在 0-3.3V
        const float MAX_VOLTAGE = 1.6f;
        const float MIN_VOLTAGE = 0.0f;
        
        // 使用 fmaxf 和 fminf 限制电压范围
        voltage = fmaxf(MIN_VOLTAGE, fminf(voltage, MAX_VOLTAGE));
        
        // 将电压转换为 12 位 DAC 值
    
        Chip_ADCDAC_WriteOutputDAC(NSS_ADCDAC0, 
            (uint16_t)((voltage / MAX_VOLTAGE) * 4095.0f)
        );
        
        // 不再更新调试变量，保持为1.6V
        return voltage;
    }
    return 0.0f;
}

// ADCDAC 电压读取函数
float Chip_ADCDAC_ReadADCVoltage(NSS_ADCDAC_T *pADCDAC, ADCDAC_IO_T channel) {
    if (pADCDAC == NSS_ADCDAC0) {
        // 配置 ADC 多路复用器
        Chip_ADCDAC_SetMuxADC(NSS_ADCDAC0, channel);
        
        // 启动 ADC 转换
        Chip_ADCDAC_StartADC(NSS_ADCDAC0);
        
        // 等待转换完成
        while (!(Chip_ADCDAC_ReadStatus(NSS_ADCDAC0) & ADCDAC_STATUS_ADC_DONE)) {
            ; // 等待
        }
        
        // 读取 ADC 值
        int raw_adc = Chip_ADCDAC_GetValueADC(NSS_ADCDAC0);
        
        // 将 ADC 值转换为电压（使用1.6V作为参考电压）
        float voltage = ((float)raw_adc / 4095.0f) * 1.6f;
        
        // 更新调试变量
        if (channel == ADCDAC_IO_ANA0_0) {
            // 强制设置驱动电压为6V
            g_debugDriveVoltage = 1.6f;
        } else if (channel == ADCDAC_IO_ANA0_1) {
            g_debugSenseVoltage = voltage;
        }
        
        return voltage;
    }
    return 0.0f;
}

// 定义固定驱动电压
#define FIXED_DRIVE_VOLTAGE 1.6f

// 修改初始化函数，添加I2D详细配置
void ResistanceMeasurement_Init(void) {
    // 初始化ADCDAC和I2D模块
    Chip_ADCDAC_Init(NSS_ADCDAC0);

    Chip_I2D_Init(NSS_I2D);
    
    // 配置引脚
    Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_ANA0_0, IOCON_FUNC_1 | IOCON_RMODE_PULLUP);  // DAC输出
    Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_ANA0_1, IOCON_FUNC_1 );  // ADC输入
 

    
    // 配置DAC
    Chip_ADCDAC_SetModeDAC(NSS_ADCDAC0, ADCDAC_CONTINUOUS);

    
    Chip_ADCDAC_SetInputRangeADC(NSS_ADCDAC0, ADCDAC_INPUTRANGE_WIDE);
    Chip_ADCDAC_SetModeADC(NSS_ADCDAC0, ADCDAC_SINGLE_SHOT);
    Chip_ADCDAC_SetMuxDAC(NSS_ADCDAC0, ADCDAC_IO_ANA0_0);
    // 初始化DAC输出为固定的1.6V
    // 1.6V对应DAC的满量程4095
    uint16_t dac_value = 4095;
    Chip_ADCDAC_WriteOutputDAC(NSS_ADCDAC0, dac_value);

    
    // 详细配置I2D模块
    // 默认使用10倍增益，积分时间为10微秒
    ConfigureI2D(I2D_GAIN_MODE_10X, 100);
    
    // 重置调试变量
    g_debugDriveVoltage = FIXED_DRIVE_VOLTAGE;
    g_debugSenseVoltage = 0.0f;
    g_debugVoltageDifference = 0.0f;
    g_debugCurrent = 0.0f;
    g_debugRawI2DValue = 0;
}

// 单次电阻测量函数
float MeasureSingleResistance(const ResistanceMeasurementConfig* user_config) {
    // 默认配置
    ResistanceMeasurementConfig defaultConfig = {
        .target_current = 1e-4f,      // 100µA
        .min_resistance = 10.0f,       // 最小可测电阻
        .max_resistance = 100000.0f,   // 最大可测电阻
        .max_iterations = 1,           // 只进行一次测量
        .sample_delay_ms = 50,         // 延迟时间
        .voltage_diff_threshold = 0.001f
    };
    
    const ResistanceMeasurementConfig* config = 
        user_config ? user_config : &defaultConfig;
    
    // 等待稳定
    Chip_Clock_System_BusyWait_ms(config->sample_delay_ms);
    
    // 测量电压
    float voltage_drive = FIXED_DRIVE_VOLTAGE;
    float voltage_sense = Chip_ADCDAC_ReadADCVoltage(NSS_ADCDAC0, ADCDAC_IO_ANA0_1);
    
    // 计算电压差
    float voltage_diff = fabsf(voltage_drive - voltage_sense);
    
    // 选择合适的I2D增益模式
    I2DGainMode gain_mode = selectOptimalI2DGain(voltage_diff, config->target_current);
    uint16_t integration_time_ms = 100;  // 默认100ms
    
    // 配置I2D
    ConfigureI2D(gain_mode, integration_time_ms);
    
    // 检测并处理溢出
    I2D_STATUS_T status = Chip_I2D_ReadStatus(NSS_I2D);
    if (status & I2D_STATUS_RANGE_TOO_HIGH) {
        if (gain_mode > I2D_GAIN_MODE_1X) {
            gain_mode = (I2DGainMode)(gain_mode - 1);
            ConfigureI2D(gain_mode, integration_time_ms);
        } else {
            integration_time_ms = 10;
            ConfigureI2D(gain_mode, integration_time_ms);
        }
    }
    
    // 计算电流
    float estimated_current = calculateI2DCurrent(
        g_debugRawI2DValue, 
        gain_mode, 
        integration_time_ms
    );
    
    // 计算电阻
    float resistance = voltage_diff / estimated_current;
    
    // 有效性检查
    if (estimated_current < 1e-9f || estimated_current > 1e-3f) {
        // 电流超出合理范围，返回无效值
        resistance = 0.0f;
    }
    
    // 电阻范围检查
    if (resistance < 1.0f || resistance > 1000000.0f) {
        resistance = 0.0f;
    }
    
    // 更新调试信息（简化版本）
    g_debugSenseVoltage = voltage_sense;
    g_debugVoltageDifference = voltage_diff;
    g_debugCurrent = estimated_current;
    
    return resistance;
}

// 电阻测量处理函数
float ResistanceMeasurement_Process(void* state, const void* config) {
    ResistanceMeasurementState* measurementState = 
        (ResistanceMeasurementState*)state;
    
    // 执行单次测量
    float resistance = MeasureSingleResistance(
        config ? (const ResistanceMeasurementConfig*)config : NULL
    );
    
    // 如果传入了状态，更新状态信息
    if (measurementState != NULL) {
        // 更新原始电阻值
        measurementState->lastRawResistance = resistance;
        
        // 移动平均滤波
        if (measurementState->filteredResistance == 0.0f) {
            // 首次测量，直接赋值
            measurementState->filteredResistance = resistance;
        } else {
            // 应用移动平均
            float alpha = measurementState->movingAverageAlpha;
            measurementState->filteredResistance = 
                alpha * resistance + (1.0f - alpha) * measurementState->filteredResistance;
        }
        
        // 稳定性判断
        if (fabsf(resistance - measurementState->filteredResistance) < 0.1f) {
            measurementState->stableCount++;
        } else {
            measurementState->stableCount = 0;
        }
        
        // 检查是否需要切换量程
        if (measurementState->stableCount >= measurementState->maxStableCount) {
            // 根据测量值动态调整量程
            if (resistance > 1000.0f) {
                measurementState->currentRange = RES_RANGE_HIGH;
                            // 切换到高增益模式
            ConfigureI2D(I2D_GAIN_MODE_100X, 100);
        } else {        
            measurementState->currentRange = RES_RANGE_LOW;
            // 切换到低增益模式
            ConfigureI2D(I2D_GAIN_MODE_1X, 100);
            }
            measurementState->rangeChangeRequested = true;
        }
    }
    
    return resistance;
}