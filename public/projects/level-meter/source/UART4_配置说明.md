# UART4 配置完整说明文档

## 📋 目录
1. [基本参数](#基本参数)
2. [引脚配置](#引脚配置)
3. [寄存器配置](#寄存器配置)
4. [初始化函数](#初始化函数)
5. [发送函数](#发送函数)
6. [接收函数](#接收函数)
7. [使用示例](#使用示例)
8. [波特率计算](#波特率计算)

---

## 基本参数

| 参数 | 值 | 说明 |
|------|-----|------|
| **串口编号** | UART4 | STC8H的第四路串口 |
| **发送引脚** | P0.3 (TXD4) | 推挽输出模式 |
| **接收引脚** | P0.2 (RXD4) | 准双向口模式 |
| **波特率** | 9600 bps | 固定波特率 |
| **主频** | 24 MHz | Fosc = 24000000 Hz |
| **数据位** | 8位 | 无校验位，1停止位 |
| **工作模式** | 全双工 | 同时支持发送和接收 |
| **通信方式** | 查询方式 | 非中断方式 |

---

## 引脚配置

### P0.2 (RXD4 - 接收引脚)
```c
P0M0 &= ~0x04;   // 清除P0M0的bit2
P0M1 &= ~0x04;   // 清除P0M1的bit2
```
- **模式**: 准双向口 (P0M1[2]=0, P0M0[2]=0)
- **功能**: 接收数据输入

### P0.3 (TXD4 - 发送引脚)
```c
P0M0 |= 0x08;    // 设置P0M0的bit3
P0M1 &= ~0x08;   // 清除P0M1的bit3
```
- **模式**: 推挽输出 (P0M1[3]=0, P0M0[3]=1)
- **功能**: 发送数据输出

---

## 寄存器配置

### 1. S4CON 寄存器 (地址: 0x84)

**位定义:**
```
bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 │     │     │     │     │     │     │     │
 │     │     │     │     │     │     │     └─ RI4: 接收中断标志 (只读)
 │     │     │     │     │     │     └─ TI4: 发送中断标志 (只读)
 │     │     │     │     │     └─ 保留
 │     │     │     │     └─ 保留
 │     │     │     └─ S4REN: 接收使能 (1=使能接收)
 │     │     └─ S4BRT: 波特率源选择 (1=Timer4, 0=Timer2)
 │     └─ S4SMOD: 数据位模式 (1=9位, 0=8位)
 └─ 保留
```

**配置值:**
```c
S4CON = 0x10;    // 初始值: 0b0001 0000 (bit4=1, 接收使能)
S4CON |= 0x40;   // 最终值: 0b0101 0000 = 0x50
```

**最终配置:**
- `bit4 (S4REN) = 1`: 接收使能
- `bit6 (S4BRT) = 1`: 使用Timer4作为波特率发生器
- `bit7 (S4SMOD) = 0`: 8位数据模式
- `bit1 (TI4) = 0`: 发送中断标志（发送时自动置位）
- `bit0 (RI4) = 0`: 接收中断标志（接收时自动置位）

---

### 2. T4T3M 寄存器 (地址: 0xD1)

**位定义:**
```
bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 │     │     │     │     │     │     │     │
 │     │     │     │     │     │     │     └─ T3CLKO: Timer3时钟输出
 │     │     │     │     │     │     └─ T3C/T: Timer3模式
 │     │     │     │     │     └─ T3CLKS[1]: Timer3时钟选择高位
 │     │     │     │     └─ T3CLKS[0]: Timer3时钟选择低位
 │     │     │     └─ T4CLKO: Timer4时钟输出
 │     │     └─ T4C/T: Timer4模式 (0=定时器, 1=计数器)
 │     └─ T4CLKS[1]: Timer4时钟选择高位
 └─ T4CLKS[0]: Timer4时钟选择低位 + T4R: Timer4运行控制
```

**配置步骤:**
```c
T4T3M |= 0x20;   // 0b0010 0000
// bit5 = 1: T4C/T = 0 (定时器模式)
// bit6 = 0: T4CLKS[1] = 0
// bit5+bit6 = 01: Timer4时钟源 = Fosc (1T模式)

T4T3M |= 0x80;   // 0b1000 0000
// bit7 = 1: T4R = 1 (启动Timer4)
```

**最终配置:**
- `bit5 (T4C/T) = 1`: 定时器模式
- `bit6 (T4CLKS[1]) = 0`: Timer4时钟选择高位
- `bit5+bit6 = 01`: Timer4时钟源 = Fosc (1T模式，24MHz)
- `bit7 (T4R) = 1`: Timer4运行中

---

### 3. T4H / T4L 寄存器 (地址: 0xD2 / 0xD3)

**配置值:**
```c
T4L = 0x8F;      // 低字节 = 0x8F (143)
T4H = 0xFD;      // 高字节 = 0xFD (253)
```

**重装载值计算:**
```
重装载值 = T4H × 256 + T4L
        = 0xFD × 256 + 0x8F
        = 253 × 256 + 143
        = 64896 + 143
        = 64911 (0xFD8F)
```

---

## 初始化函数

### 完整代码
```c
// -------------------------------
// UART4 Init, TXD4=P0.3, RXD4=P0.2, 9600bps @24MHz
// -------------------------------
void Uart4_Init(void)
{
    // 1. GPIO引脚配置
    P0M0 &= ~0x04;   // P0.2 (RXD4) 配置为准双向口
    P0M1 &= ~0x04;
    
    P0M0 |= 0x08;    // P0.3 (TXD4) 配置为推挽输出
    P0M1 &= ~0x08;
    
    // 2. UART4控制寄存器配置
    S4CON = 0x10;    // 基础配置: 接收使能
    // S4CON = 0x51;  // 备用配置(使用Timer2)
    
    S4CON |= 0x40;   // 使用Timer4作为波特率发生器
    
    // 3. 定时器4配置为波特率发生器
    T4T3M |= 0x20;   // Timer4: 定时器模式, 1T模式 (Fosc)
    T4L = 0x8F;      // 波特率重装载值低字节
    T4H = 0xFD;      // 波特率重装载值高字节
    T4T3M |= 0x80;   // 启动Timer4
}
```

### 初始化顺序
1. ✅ 配置GPIO引脚模式
2. ✅ 配置UART4控制寄存器
3. ✅ 配置定时器4作为波特率发生器
4. ✅ 启动定时器4

---

## 发送函数

### 1. Uart4_SendChar() - 发送单个字符
```c
void Uart4_SendChar(char c)
{
    S4BUF = c;                    // 写入数据到发送缓冲区
    while(!(S4CON & 0x02));       // 等待TI4标志置位（发送完成）
    S4CON &= ~0x02;               // 清除TI4标志
}
```

**功能说明:**
- 阻塞式发送，等待发送完成
- 自动清除发送中断标志

**使用示例:**
```c
Uart4_SendChar('A');  // 发送字符'A'
```

---

### 2. Uart4_SendString() - 发送字符串
```c
void Uart4_SendString(char *s)
{
    while(*s) Uart4_SendChar(*s++);
}
```

**功能说明:**
- 循环发送字符串直到遇到'\0'
- 用于发送命令字符串

**使用示例:**
```c
Uart4_SendString("<mAm>");        // 发送测距命令
Uart4_SendString("Hello\r\n");   // 发送字符串
```

---

### 3. Uart4_SendNumber() - 发送数字
```c
void Uart4_SendNumber(long num)
{
    char neg = 0;
    char buf[12];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    
    if(num < 0){
        neg = 1;
        num = -num;
    }
    
    do {
        *--p = (num % 10) + '0';
        num /= 10;
    } while(num > 0);
    
    if(neg){
        *--p = '-';
    }
    
    Uart4_SendString(p);
}
```

**功能说明:**
- 将长整型数字转换为字符串发送
- 支持负数
- 内部使用12字节缓冲区

**使用示例:**
```c
Uart4_SendNumber(1234);   // 发送 "1234"
Uart4_SendNumber(-100);   // 发送 "-100"
```

---

## 接收函数

### 1. Uart4_RecvChar() - 接收单个字符
```c
int Uart4_RecvChar(unsigned int timeout_ms)
{
    unsigned int t = 0;
    
    while(!(S4CON & 0x01))        // 等待RI4标志置位（接收完成）
    {
        Delay_us(500);
        if(++t >= (timeout_ms * 2))
            return -1;             // 超时返回-1
    }
    
    S4CON &= ~0x01;                // 清除RI4标志
    return S4BUF;                  // 返回接收到的字符
}
```

**功能说明:**
- 带超时机制的接收函数
- 超时时间单位：毫秒
- 超时返回：-1
- 成功返回：接收到的字符 (0-255)

**使用示例:**
```c
int c = Uart4_RecvChar(1000);  // 等待1秒
if(c >= 0) {
    // 接收到字符
} else {
    // 超时
}
```

---

### 2. Uart4_RecvString() - 接收字符串
```c
unsigned char Uart4_RecvString(char *buf, unsigned char len, unsigned int timeout_ms)
{
    unsigned char i = 0;
    int c;
    
    if(len == 0)
        return 0;
    
    while(i < len - 1)
    {
        c = Uart4_RecvChar(timeout_ms);   // 接收一个字符
        
        if(c < 0)                         // 超时
            break;
        
        buf[i++] = (char)c;               // 保存字符
        
        if(c == '>')                      // 遇到结束符'>'
            break;
    }
    
    buf[i] = '\0';                        // 字符串结束符
    return i;                             // 返回接收到的字符数
}
```

**功能说明:**
- 接收字符串直到遇到`>`字符或超时
- 自动添加字符串结束符`\0`
- 返回值：接收到的字符数（不包括'\0'）

**参数说明:**
- `buf`: 接收缓冲区指针
- `len`: 缓冲区最大长度
- `timeout_ms`: 每个字符的超时时间（毫秒）

**使用示例:**
```c
xdata char recv_buf[64];
unsigned char len;

len = Uart4_RecvString(recv_buf, sizeof(recv_buf), 5000);
if(len > 0) {
    // 成功接收到数据
    // recv_buf 包含接收到的字符串
} else {
    // 超时或未接收到数据
}
```

---

## 使用示例

### 完整通信示例
```c
// 初始化
Uart4_Init();

// 发送测距命令
Uart4_SendString("<mAm>");
Delay_ms(10);

// 接收响应（5秒超时）
xdata char recv_buf[64];
unsigned char len = Uart4_RecvString(recv_buf, sizeof(recv_buf), 5000);

if(len > 0) {
    // 处理接收到的数据
    // recv_buf 包含类似 "<m0m1234>" 的响应
} else {
    // 超时处理
}
```

### 激光模块命令示例
```c
// 1. 测量距离
Uart4_SendString("<mAm>");
len = Uart4_RecvString(recv_buf, 64, 5000);

// 2. 读取偏移量
Uart4_SendString("<RDAD>");
len = Uart4_RecvString(recv_buf, 64, 5000);

// 3. 设置偏移量 +100
Uart4_SendString("<STAD+0100>");
len = Uart4_RecvString(recv_buf, 64, 5000);

// 4. 设置偏移量 -100
Uart4_SendString("<STAD-0100>");
len = Uart4_RecvString(recv_buf, 64, 5000);
```

---

## 波特率计算

### 计算公式
```
波特率 = Fosc / (4 × (65536 - 重装载值))
```

### 当前配置计算
```
Fosc = 24,000,000 Hz
重装载值 = 0xFD8F = 64911

波特率 = 24,000,000 / (4 × (65536 - 64911))
       = 24,000,000 / (4 × 625)
       = 24,000,000 / 2500
       = 9600 bps ✓
```

### 修改波特率方法

**例如：修改为115200 bps**
```
重装载值 = 65536 - (Fosc / (4 × 波特率))
        = 65536 - (24,000,000 / (4 × 115200))
        = 65536 - (24,000,000 / 460800)
        = 65536 - 52.08
        = 65483.92
        ≈ 65484 (0xFFCC)

T4H = 65484 / 256 = 255 (0xFF)
T4L = 65484 % 256 = 204 (0xCC)
```

**代码修改:**
```c
T4L = 0xCC;
T4H = 0xFF;
```

---

## 函数接口汇总

### 头文件声明 (uart.h)
```c
// UART4 函数声明
void Uart4_Init(void);
void Uart4_SendChar(char c);
void Uart4_SendString(char *s);
void Uart4_SendNumber(long num);
int Uart4_RecvChar(unsigned int timeout_ms);
unsigned char Uart4_RecvString(char *buf, unsigned char len, unsigned int timeout_ms);
```

### 函数功能表

| 函数名 | 功能 | 参数 | 返回值 |
|--------|------|------|--------|
| `Uart4_Init()` | 初始化UART4 | 无 | 无 |
| `Uart4_SendChar()` | 发送单个字符 | `char c` | 无 |
| `Uart4_SendString()` | 发送字符串 | `char *s` | 无 |
| `Uart4_SendNumber()` | 发送数字 | `long num` | 无 |
| `Uart4_RecvChar()` | 接收单个字符 | `unsigned int timeout_ms` | `int` (成功:0-255, 超时:-1) |
| `Uart4_RecvString()` | 接收字符串 | `buf, len, timeout_ms` | `unsigned char` (字符数) |

---

## 注意事项

1. **初始化顺序**: 必须先配置GPIO，再配置UART寄存器，最后启动定时器
2. **超时处理**: 接收函数都有超时机制，避免程序死等
3. **字符串结束符**: `RecvString`函数会自动添加`\0`结束符
4. **协议格式**: 激光模块使用`<...>`格式，接收函数以`>`作为结束标志
5. **阻塞发送**: 发送函数是阻塞式的，会等待发送完成
6. **缓冲区大小**: 接收缓冲区建议至少64字节（根据协议定义）

---

## 寄存器地址速查

| 寄存器 | 地址 | 功能 |
|--------|------|------|
| `S4CON` | 0x84 | UART4控制寄存器 |
| `S4BUF` | 0x85 | UART4数据缓冲区 |
| `T4T3M` | 0xD1 | 定时器3/4模式控制寄存器 |
| `T4H` | 0xD2 | 定时器4高字节 |
| `T4L` | 0xD3 | 定时器4低字节 |
| `P0M0` | - | P0口模式寄存器0 |
| `P0M1` | - | P0口模式寄存器1 |

---

**文档版本**: v1.0  
**最后更新**: 2024  
**适用芯片**: STC8H系列

