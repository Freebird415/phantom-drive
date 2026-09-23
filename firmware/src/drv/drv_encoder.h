/* =============================================================================
 *  drv_encoder.h —— JGB37-520 自带编码器 ×2（定时器硬件编码器模式）
 *
 *  接线（docs/引脚分配表.md v3.0）：
 *      左：C1 → PB6 (TIM4_CH1)   C2 → PB7 (TIM4_CH2)   编码器 VCC → **5V**
 *      右：C1 → PA6 (TIM3_CH1)   C2 → PA7 (TIM3_CH2)   编码器 VCC → **5V**
 *
 *  ⚠️ 注意：
 *   1) 编码器 VCC 走 **5V**（实测确认）→ A/B 是 0~5V 电平；PB6/PB7/PA6/PA7 都是 FT 脚 ✓
 *   2) **VCC/GND 接反会烧编码器**；A/B 接反只影响计数方向（软件可修）
 *   3) 6 芯线里电机电流与编码器信号混在一起 → 计数若有毛刺，可把 IC1Filter/IC2Filter 调大
 *   4) 硬件计数，**零丢数**；16 位会回绕，读的时候按 int16_t 解释（差分用）
 * ========================================================================== */
#ifndef DRV_ENCODER_H
#define DRV_ENCODER_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

typedef enum { ENC_L = 0, ENC_R = 1 } EncId;

void     En_Init(void);
int16_t  En_Count(EncId id);        /* 当前计数（16 位，按有符号解释） */
void     En_Reset(void);            /* 两个计数器一起清零（方便"转一圈看变化"） */
void     En_PrintDebug(void);       /* 打印左右计数 + 累计变化量 */

#endif /* DRV_ENCODER_H */
