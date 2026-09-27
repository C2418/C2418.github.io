#ifndef __FONT_ASCII_8X16_H__
#define __FONT_ASCII_8X16_H__

/**
 * 8x16 ASCII字库定义
 */

// ASCII字符范围
#define FONT_ASCII_FIRST  0x20  // 空格
#define FONT_ASCII_LAST   0x7E  // ~

// 字体尺寸常量
#define FONT_ASCII_WIDTH   16
#define FONT_ASCII_HEIGHT  32
#define FONT_ASCII_BYTES   64  // 每个ASCII字符字节数

/**
 * 获取16x32字符字模数据
 * @param ch 字符（0x20~0x7E）
 * @return 字模数据指针，如果字符不在范围内返回 NULL
 */
const unsigned char* Font_Get16x32(char ch);

/**
 * 36x36 中文字库
 * 单字符大小：36 × 36
 * 存储方式：逐行式（Row Major），顺向，阴码
 * 每个字符：180 bytes (36行 × 5字节/行)
 * 每行结构：5字节表示36列（bit7=最左，bit0=最右）
 * 位顺序：bit7 = 最左/上，bit0 = 最右/下
 */

// 36x36中文字体尺寸常量
#define FONT_CHINESE_WIDTH   36
#define FONT_CHINESE_HEIGHT  36
#define FONT_CHINESE_BYTES   180  // 每个中文字符字节数
#define FONT_CHINESE_BYTES_PER_ROW  5  // 每行字节数(36位需要5字节)

/**
 * 获取36x36汉字字模数据
 * @param name 字符名称（如 "料"）
 * @return 字模数据指针（180 bytes，位于ROM），如果未找到返回 NULL
 */
unsigned char code* Font_Get36x36(char* name);

/**
 * 36x36字体数量（外部可访问）
 */
extern unsigned int Font_36x36_Count;

/**
 * 32x32 符号字库（用于℃等特殊符号）
 * 单字符大小：32 × 32
 * 存储方式：逐行式（Row Major），顺向，阴码
 * 每个字符：128 bytes (32行 × 4字节/行)
 * 每行结构：4字节表示32列（bit7=最左，bit0=最右）
 */

// 32x32符号字体尺寸常量
#define FONT_SYMBOL_WIDTH   32
#define FONT_SYMBOL_HEIGHT  32
#define FONT_SYMBOL_BYTES   128  // 每个符号字符字节数
#define FONT_SYMBOL_BYTES_PER_ROW  4  // 每行字节数(32位需要4字节)

/**
 * 获取32x32符号字模数据
 * @param name 符号名称（如 "℃"）
 * @return 字模数据指针（128 bytes，位于ROM），如果未找到返回 NULL
 */
unsigned char code* Font_Get32x32_Symbol(char* name);

/**
 * 32x32符号数量（外部可访问）
 */
extern unsigned int Font_32x32_Symbol_Count;

// ========================================
// 32x32 中文字库（用于关于页面等）
// ========================================

/**
 * 获取32x32中文字模数据
 * @param name 字符名称（如 "广"）
 * @return 字模数据指针（128 bytes），如果未找到返回 NULL
 */
unsigned char code* Font_Get32x32_Chinese(char* name);

/**
 * 32x32中文数量（外部可访问）
 */
extern unsigned int Font_32x32_Chinese_Count;

// ========================================
// 网络状态图标（36x36 RGB565格式）
// ========================================

/**
 * 网络断开图标（36x36 RGB565）
 * 数组大小：2600字节（8字节头部 + 36*36*2字节图像数据）
 */
extern unsigned char code gImage_off[2600];

/**
 * 网络连接图标（36x36 RGB565）
 * 数组大小：2600字节（8字节头部 + 36*36*2字节图像数据）
 */
extern unsigned char code gImage_on[2600];

#define NET_ICON_WIDTH   36
#define NET_ICON_HEIGHT  36
#define NET_ICON_HEADER_SIZE  8   // Image2Lcd头部字节数

#endif /* __FONT_ASCII_8X16_H__ */
