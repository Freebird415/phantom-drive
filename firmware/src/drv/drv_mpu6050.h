/* =============================================================================
 *  drv_mpu6050.h —— MPU6050 六轴（独占总线 I2C2）
 *
 *  接线（docs/引脚分配表.md / 完整接线.md §3.7）：
 *      VCC → 3V3_LOGIC（**必须 3.3V**，I2C2 是 3.3V 总线）
 *      GND → GND
 *      SCL → PB10
 *      SDA → PB11
 *      AD0 → GND（→ 7bit 地址 **0x68**；接 3V3 则 0x69）
 *      XDA / XCL / INT → 悬空
 *
 *  ⚠️ 三条注意：
 *   1) **绝不能挂 I2C1（PB8/PB9，灰度那条 5V 总线）** —— MPU 极限约 VDD+0.3=3.6V，会被打坏
 *   2) 2026-10-10 起 **OLED 已放弃**，I2C2 上只剩 MPU6050（PCB 侧 3.3V 上拉按《电气连接规范》§7 处理）
 *   3) E-24 originally diagnostic-only; A02 v5 uses the additive validity API
 *      under the user's explicit repair request. No pin/legacy signature change.
 * ========================================================================== */
#ifndef DRV_MPU6050_H
#define DRV_MPU6050_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

void    Mpu_Init(void);
uint8_t Mpu_WhoAmI(void);                       /* 正常应回 0x68（0 = 读失败/不在线） */
uint8_t Mpu_Read(int16_t raw[7]);               /* raw: ax ay az temp gx gy gz（原始值） */
/* Additive health API: legacy signatures/pins remain unchanged. A02 must not
 * confuse a successful bus transaction with valid awake/range-configured data. */
enum { MPU_OK, MPU_NOT_READY, MPU_IO_ERROR, MPU_CONFIG_ERROR, MPU_ZERO_AXES, MPU_SLEEP };
uint8_t Mpu_Status(void);
void    Mpu_Service(uint8_t parked);           /* retry only while drive disabled */
void    Mpu_PrintDebug(void);                   /* WHO_AM_I + 加速度(g) + 角速度(°/s) + 温度(℃) */
uint16_t Mpu_ReinitCount(void);                 /* parked automatic retries, diagnostic */
void    Mpu_DebugReset(void);                   /* 调试：复位回上电默认（模拟断电），用来验证自愈 `MPURESET` */

#endif /* DRV_MPU6050_H */
