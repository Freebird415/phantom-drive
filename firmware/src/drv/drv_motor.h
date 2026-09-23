/* =============================================================================
 *  drv_motor.h —— TB6612FNG 双电机驱动
 *
 *  接线（docs/引脚分配表.md v3.0）：
 *      PWM_L = PA8  (TIM1_CH1)      PWM_R = PA11 (TIM1_CH4)    ← 同一定时器，左右同频
 *      AIN1 = PB0   AIN2 = PB1      ← 左轮方向
 *      BIN1 = PB12  BIN2 = PB13     ← 右轮方向
 *      STBY = PB14                  ← 高电平使能（硬件另有 10k 上拉 + 跳线）
 *      VM = 12V_BUS（不走排针）      VCC = 3V3_LOGIC
 *
 *  ⚠️ 三条注意：
 *   1) **先不接电机也能验**：用万用表量 AO1/AO2 之间的电压，随占空比变化即说明 PWM 与方向都对
 *   2) **电机很强**（JGB37-520 + 大轮 ≈1m/s+）：第一次上电务必**限速**（先给 ±20~30）
 *   3) TB6612 的真值表：AIN1/AIN2 = 10 正转、01 反转、00 滑行(coast)、11 刹车(brake)
 * ========================================================================== */
#ifndef DRV_MOTOR_H
#define DRV_MOTOR_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#define MOT_LIMIT   100         /* 命令幅值上限（±100） */

void Mo_Init(void);
void Mo_Set(int l, int r);      /* 左右占空比 −100~100（负=反转）；超限自动夹到 ±100 */
void Mo_Stop(void);             /* 停：方向=00（滑行）+ 占空比 0 */
void Mo_Brake(void);            /* 刹车：方向=11 + 占空比 0 */
void Mo_PrintDebug(void);       /* 打印当前 PWM / 方向 / STBY 引脚电平 */

#endif /* DRV_MOTOR_H */
