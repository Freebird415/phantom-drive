/* =============================================================================
 *  drv_mpu6050.h —— MPU6050 六轴（挂 I2C2，与 OLED 共用总线）
 *
 *  接线（docs/引脚分配表.md v3.0 / 完整接线.md §3.7）：
 *      VCC → 3V3_LOGIC（**必须 3.3V**，I2C2 是 3.3V 总线）
 *      GND → GND
 *      SCL → PB10（与 OLED 同一点）
 *      SDA → PB11（与 OLED 同一点）
 *      AD0 → GND（→ 7bit 地址 **0x68**；接 3V3 则 0x69）
 *      XDA / XCL / INT → 悬空
 *
 *  ⚠️ 三条注意：
 *   1) **绝不能挂 I2C1（PB8/PB9，灰度那条 5V 总线）** —— MPU 极限约 VDD+0.3=3.6V，会被打坏
 *   2) 与 OLED 共用总线**不需要另加上拉**（OLED 板载已有 4.7k 拉 3V3）；地址不冲突（0x3C vs 0x68）
 *   3) 它的定位是**诊断仪器**（振动/撞障/角速度），不是控制输入 —— 优先级最低（决策 E-24）
 * ========================================================================== */
#ifndef DRV_MPU6050_H
#define DRV_MPU6050_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

void    Mpu_Init(void);
uint8_t Mpu_WhoAmI(void);                       /* 正常应回 0x68（0 = 读失败/不在线） */
uint8_t Mpu_Read(int16_t raw[7]);               /* raw: ax ay az temp gx gy gz（原始值） */
void    Mpu_PrintDebug(void);                   /* WHO_AM_I + 加速度(g) + 角速度(°/s) + 温度(℃) */

#endif /* DRV_MPU6050_H */
