/* =============================================================================
 *  drv_hmi.h —— 人机：用户按键 + 蜂鸣器 + 状态灯
 *
 *  接线（docs/引脚分配表.md v3.0）：
 *      按键  ：PA12 ── 按键 ── GND        （**按下 = 低电平**）
 *      蜂鸣器：PB15 ── 1k ── NPN(S8050) ── 有源蜂鸣器 ── 3V3；蜂鸣器两端并续流二极管
 *
 *  ⚠️ 三条注意：
 *   1) PA12 上**核心板已焊 1.5k 上拉**（USB D+ 那颗 R10）→ 不用再加外部上拉；
 *      也正因为这个上拉，PA12 **只适合做输入**，别拿它驱动什么
 *   2) 蜂鸣器**不能直接挂 GPIO**（20~30mA 超引脚能力）→ 必须经三极管/MOS 驱动
 *   3) PB15 高电平 = 三极管导通 = 响
 * ========================================================================== */
#ifndef DRV_HMI_H
#define DRV_HMI_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

void    Hmi_Init(void);
uint8_t Hmi_Key(void);              /* 1 = 按下 */
void    Hmi_Beep(uint16_t ms);      /* 响 ms 毫秒（**阻塞实现**；未提供非阻塞版本） */
void    Hmi_BeepOn(void);
void    Hmi_BeepOff(void);
void    Hmi_PrintDebug(void);

#endif /* DRV_HMI_H */
