/* =============================================================================
 *  drv_oled.h —— 0.96" SSD1306 OLED（I2C2：PB10=SCL / PB11=SDA）
 *
 *  实物信息（用户背面丝印照片确认，2026-09-23）：
 *    0.96 OLED · 4P 丝印 GND/VCC/SCL/SDA · "IIC ADRESS SELECT 0x78" → 7bit 0x3C
 *    R5/R6/R7/R10 = 472（4.7k）→ **板载已有 I2C 上拉**
 *
 *  ⚠️ 两条硬约束：
 *   1) **VCC 必须 3.3V**：板载上拉拉到 VCC → 3.3V 供电才能让整条 I2C2 保持 3.3V；
 *      接 5V 会把总线拉到 5V，将来挂 MPU6050（3.3V 器件，极限 VDD+0.3）会被打坏。
 *   2) **绝不能挂到 I2C1（灰度那条 5V 上拉总线）** —— SSD1306 会被 5V 打坏。
 * ========================================================================== */
#ifndef DRV_OLED_H
#define DRV_OLED_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#define OLED_W      128
#define OLED_H      64

void    Ol_Init(void);          /* I2C2 初始化 + 探测地址（0x3C / 0x3D 自动识别） */
uint8_t Ol_Present(void);       /* 返回探测到的地址（0 = 没找到） */
void    Ol_Clear(void);
void    Ol_TestPattern(void);   /* bring-up 用：棋盘格 + 边框，一眼看出整屏是否都好 */
void    Ol_PrintDebug(void);    /* 打状态 + 画测试图案 */

#endif /* DRV_OLED_H */
