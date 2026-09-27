/*
 * 控制示例：通过接收 4 字节命令设置 PWM 占空比并持久化
 * 命令格式：00 11 22 xx -> 将占空比设置为 xx(0..100)% ，并保存到 EEPROM
 */

 #include "board.h"
 #include <stdint.h>
 #include <stdbool.h>
 
 #include "chip.h"
 #include "gpio_nss.h"
 #include "iocon_nss.h"
 #include "nfc_nss.h"
 #include "eeprom_nss.h"
 #include "pwm_control.h"
 #include "ndeft2t/ndeft2t.h"
 #include "resistance_measurement.h"
 /* -------------------------------------------------------------------------- */
 
 static volatile bool sNfcMemWriteDetected = false;
 
static volatile bool gFieldOn = false;
static volatile bool gMsgRead = false;
static volatile uint32_t gMeasurementTimer = 0;
static volatile bool gNeedMeasurement = true;

/* 回调实现（由 mods/app_sel.h 映射） */
void App_FieldStatusCb(bool fieldOn) { gFieldOn = fieldOn; }
void App_MsgAvailableCb(void) {}
void App_MsgReadCb(void) { gMsgRead = true; }

 /* EEPROM 持久化定义 */
 #define CMD_MAGIC 0x434D4441u /* 'CMDA' */
 #define CMD_STORE_OFFSET ((EEPROM_NR_OF_RW_ROWS - 1) * EEPROM_ROW_SIZE) /* 最后一个可写行起始偏移 */
 
 static void SaveCommandToEeprom(uint8_t duty)
 {
	 uint32_t magic = CMD_MAGIC;
	 uint8_t data[8];
	 /* magic */
	 data[0] = (uint8_t)(magic >> 24);
	 data[1] = (uint8_t)(magic >> 16);
	 data[2] = (uint8_t)(magic >> 8);
	 data[3] = (uint8_t)(magic);
	 /* command header + duty */
	 data[4] = 0x00;
	 data[5] = 0x11;
	 data[6] = 0x22;
	 data[7] = duty; /* 0..100 占空比 */
 
	 Chip_EEPROM_Write(NSS_EEPROM, CMD_STORE_OFFSET, data, (int)sizeof(data));
	 Chip_EEPROM_Flush(NSS_EEPROM, true);
 }
 
 static bool LoadCommandFromEeprom(uint8_t *pDuty)
 {
	 uint8_t data[8];
	 Chip_EEPROM_Read(NSS_EEPROM, CMD_STORE_OFFSET, data, (int)sizeof(data));
 
	 uint32_t magic = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
	 if (magic != CMD_MAGIC) {
		 return false;
	 }
	 if (data[4] == 0x00 && data[5] == 0x11 && data[6] == 0x22) {
		 if (pDuty) *pDuty = data[7];
		 return true;
	 }
	 return false;
 }
 
 /* 在上电时从 NFC 共享存储中扫描持久化命令（适配 RF-only 场景下由读卡器写入的值） */
 static bool LoadCommandFromNfcShared(uint8_t *pDuty)
 {
	 /* 扫描整个共享区（128 个字 = 512 字节 = 128 页），跳过前 4 页的保留区 */
	 uint8_t *buf = (uint8_t *)NSS_NFC->BUF;
	 for (int i = 0; i <= (NFC_SHARED_MEM_BYTE_SIZE - 4); i++) {
		 if (buf[i] == 0x00 && buf[i + 1] == 0x11 && buf[i + 2] == 0x22) {
			 uint8_t v = buf[i + 3];
			 if (v <= 100) {
				 if (pDuty) *pDuty = v;
				 return true;
			 }
		 }
	 }
	 return false;
 }
 
 /* 将 PIO0_3 配置为 GPIO 输出并设定电平 */
 static void GPIO_PIO0_3_Init(void)
 {
	 /* 选择 GPIO 功能 */
	 Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_PIO0_3, IOCON_FUNC_0);
	 /* 设为输出 */
	 Chip_GPIO_Init(NSS_GPIO);
	 Chip_GPIO_SetPinDIROutput(NSS_GPIO, 0, 3);
	 /* 缺省输出低电平 */
	 Chip_GPIO_SetPinState(NSS_GPIO, 0, 3, false);
 }
 
 /* 解析 NFC 共享内存中的命令并执行：00 11 22 xx (xx=0..100) */
 static bool NFC_ProcessCommand(void)
 {
	 /* 仅在上次 RF 写入区域内查找，避免误触发 */
	 uint32_t startWord = 0, endWord = 0;
	 bool isWrite = Chip_NFC_GetLastAccessInfo(NSS_NFC, &startWord, &endWord);
	 if (!isWrite) {
		 return false;
	 }
 
	 if (startWord >= 128) return false;
	 if (endWord >= 128) endWord = 127;
	 if (endWord < startWord) return false;
 
	 uint8_t *buf = (uint8_t *)NSS_NFC->BUF;
	 int startByte = (int)startWord * 4;
	 int endByte = (int)endWord * 4 + 3;
	 if (startByte < 0) startByte = 0;
	 if (endByte > (NFC_SHARED_MEM_BYTE_SIZE - 1)) endByte = NFC_SHARED_MEM_BYTE_SIZE - 1;
 
	 for (int i = startByte; i + 3 <= endByte; i++) {
		 if (buf[i] == 0x00 && buf[i + 1] == 0x11 && buf[i + 2] == 0x22) {
			 uint8_t duty = buf[i + 3];
			 if (duty <= 100) {
				 SaveCommandToEeprom(duty);
				 pwm_init_module();
				 pwm_set_duty_cycle(duty);
				 pwm_start();
				 return true;
			 }
		 }
	 }
 
	 return false;
 }
 
 /* NFC 中断：记录共享内存被写入 */
 void NFC_IRQHandler(void)
 {
	 NFC_INT_T ris = Chip_NFC_Int_GetRawStatus(NSS_NFC);
	 if (ris & NFC_INT_MEMWRITE) {
		 sNfcMemWriteDetected = true;
	 }
	 /* 清除中断标志（使用驱动 API） */
	 Chip_NFC_Int_ClearRawStatus(NSS_NFC, ris);
 }
 
 int main(void)
 {
	 Board_Init();
 
	 /* 初始化 GPIO */
	 GPIO_PIO0_3_Init();
 
	 /* 初始化 NFC 外设 */
	 Chip_NFC_Init(NSS_NFC);
	 NDEFT2T_Init();
	 uint8_t instance[NDEFT2T_INSTANCE_SIZE] __attribute__((aligned(4)));
	 uint8_t buffer[NFC_SHARED_MEM_BYTE_SIZE] __attribute__((aligned(4)));
	 uint8_t locale[] = "en";
	 NDEFT2T_CREATE_RECORD_INFO_T recordInfo = { .shortRecord = true, .pString = locale };

	/* 初始化 EEPROM，并尝试恢复上次占空比 */
	Chip_EEPROM_Init(NSS_EEPROM);
	
	//上次测量电阻值	
	uint32_t storedOhms = 0xFFFFFFFFu;
	Chip_EEPROM_Read(NSS_EEPROM, 0, &storedOhms, sizeof(storedOhms));
	unsigned int displayOhms = (storedOhms == 0xFFFFFFFFu) ? 0u : (unsigned int)storedOhms;
	 uint8_t lastDuty = 0xFF;
	 if (LoadCommandFromEeprom(&lastDuty) || LoadCommandFromNfcShared(&lastDuty)) {
		 if (lastDuty > 100) lastDuty = 100; /* 兜底 */
		 /* 将来源于 NFC 的指令也同步保存到 EEPROM，便于后续稳定恢复 */
		 SaveCommandToEeprom(lastDuty);
		 /* 应用 PWM */
		 pwm_init_module();
		 pwm_set_duty_cycle(lastDuty);
		 pwm_start();
	 }

		/* 启动即提交一次上次会话测量值（或0）与占空比，确保初次读取不为空 */
		{
			char text[20];
			int len = 0;
			unsigned int ohms = displayOhms;
			if (ohms == 0u) {
				text[len++] = '0';
			}
			else {
				char digits[10];
				int d = 0;
				while (ohms > 0u && d < (int)sizeof(digits)) { digits[d++] = (char)('0' + (ohms % 10u)); ohms /= 10u; }
				while (d-- > 0 && len < (int)sizeof(text)) { text[len++] = digits[d]; }
			}
			/* 读取占空比并追加为 " xx%" */
			{
				bool running = false; uint32_t freq = 0; uint8_t duty = 0; (void)freq; (void)running;
				(void)pwm_get_status(&running, &freq, &duty);
				if (len + 1 < (int)sizeof(text)) text[len++] = ' ';
				/* 写 duty 的十进制 */  
				unsigned int dv = (unsigned int)duty;
				char dd[3]; int nd = 0;
				if (dv == 0u) { if (len < (int)sizeof(text)) text[len++] = '0'; }
				else {
					while (dv > 0u && nd < (int)sizeof(dd)) { dd[nd++] = (char)('0' + (dv % 10u)); dv /= 10u; }
					while (nd-- > 0 && len < (int)sizeof(text)) { text[len++] = dd[nd]; }
				}
				if (len < (int)sizeof(text)) text[len++] = '%';
			}
	
			NDEFT2T_CreateMessage(instance, buffer, NFC_SHARED_MEM_BYTE_SIZE, true);
			if (NDEFT2T_CreateTextRecord(instance, &recordInfo)) {
				if (NDEFT2T_WriteRecordPayload(instance, text, len)) {
					NDEFT2T_CommitRecord(instance);
				}
			}
			(void)NDEFT2T_CommitMessage(instance);
			/* 会话间刷新：同一次贴靠不再更新 */
		}
			/* 提交完成后再进行一次测量，并将结果写入EEPROM，供下一次会话显示 */
		/* 延迟到主循环中执行，避免阻塞NFC初始化 */
		
	 /* 允许 NFC 写共享内存中断 */
	 NVIC_EnableIRQ(NFC_IRQn);
	 Chip_NFC_Int_SetEnabledMask(NSS_NFC, NFC_INT_MEMWRITE);

	 for (;;) {
		 /* 若检测到共享内存写入，解析命令 */
		 if (sNfcMemWriteDetected) {
			 sNfcMemWriteDetected = false;
			 (void)NFC_ProcessCommand();
		 }

		 /* 电阻测量定时器 - 每5秒执行一次 */
		 gMeasurementTimer += 5;
		 if (gMeasurementTimer >= 5000 && gNeedMeasurement) {
			 gMeasurementTimer = 0;
			 gNeedMeasurement = false;

			 /* 执行电阻测量 */
			 ResistanceMeasurement_Init();
			 float r = ResistanceMeasurement_Process(NULL, NULL);

			 uint32_t ohmsSave;
			 if (!(r > 0.0f) || (r != r)) {
				 ohmsSave = 0xFFFFFFFFu;  /* 用全1表示无效 */
			 }
			 else {
				 ohmsSave = (uint32_t)(r + 0.5f);
			 }
			 /* 存在 offset=0 处，使用统一的 NSS_EEPROM 实例 */
			 Chip_EEPROM_Write(NSS_EEPROM, 0, &ohmsSave, sizeof(ohmsSave));
			 Chip_EEPROM_Flush(NSS_EEPROM, true);

			 /* 重新更新NDEF消息显示最新的电阻值 */
			 {
				 char text[20];
				 int len = 0;
				 unsigned int ohms = (ohmsSave == 0xFFFFFFFFu) ? 0u : (unsigned int)ohmsSave;
				 if (ohms == 0u) {
					 text[len++] = '0';
				 }
				 else {
					 char digits[10];
					 int d = 0;
					 while (ohms > 0u && d < (int)sizeof(digits)) { digits[d++] = (char)('0' + (ohms % 10u)); ohms /= 10u; }
					 while (d-- > 0 && len < (int)sizeof(text)) { text[len++] = digits[d]; }
				 }
				 /* 读取占空比并追加为 " xx%" */
				 {
					 bool running = false; uint32_t freq = 0; uint8_t duty = 0; (void)freq; (void)running;
					 (void)pwm_get_status(&running, &freq, &duty);
					 if (len + 1 < (int)sizeof(text)) text[len++] = ' ';
					 /* 写 duty 的十进制 */
					 unsigned int dv = (unsigned int)duty;
					 char dd[3]; int nd = 0;
					 if (dv == 0u) { if (len < (int)sizeof(text)) text[len++] = '0'; }
					 else {
						 while (dv > 0u && nd < (int)sizeof(dd)) { dd[nd++] = (char)('0' + (dv % 10u)); dv /= 10u; }
						 while (nd-- > 0 && len < (int)sizeof(text)) { text[len++] = dd[nd]; }
					 }
					 if (len < (int)sizeof(text)) text[len++] = '%';
				 }

				 NDEFT2T_CreateMessage(instance, buffer, NFC_SHARED_MEM_BYTE_SIZE, true);
				 if (NDEFT2T_CreateTextRecord(instance, &recordInfo)) {
					 if (NDEFT2T_WriteRecordPayload(instance, text, len)) {
						 NDEFT2T_CommitRecord(instance);
					 }
				 }
				 (void)NDEFT2T_CommitMessage(instance);
			 }
		 }

		 Chip_Clock_System_BusyWait_ms(5);
	 }
 
	 /* 不会到达 */
	 // return 0;
 }
 
 /* 文件结束 */