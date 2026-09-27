#ifndef __FONT_ASCII_8X16_H__
#define __FONT_ASCII_8X16_H__

/**
 * ASCII 8x16 字库
 * 字符范围：0x20 (' ') ~ 0x7E ('~')
 * 字符数量：95
 * 单字符大小：8 × 16
 * 存储方式：行扫描（Row Major）
 * 每行：1 byte
 * bit7 → 最左像素，bit0 → 最右像素
 */

#define FONT_ASCII_FIRST  0x20
#define FONT_ASCII_LAST   0x7E

// 字库数据声明（实现在 font_ascii_8x16.c 中）
extern const unsigned char Font_ASCII_8x16[95][16];

// 函数声明
const unsigned char* Font_Get8x16(char ch);

#endif /* __FONT_ASCII_8X16_H__ */
