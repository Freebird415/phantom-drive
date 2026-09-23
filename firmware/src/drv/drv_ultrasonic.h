/* =============================================================================
 *  drv_ultrasonic.h —— HC-SR04 ×2 驱动
 *
 *  接线（见 docs/引脚分配表.md v3.0）：
 *      左：Trig = PA4    Echo = PA5
 *      右：Trig = PA1    Echo = PB5
 *
 *  三条硬约束：
 *   1) 模块 VCC 必须 5V（3.3V 带不动；只插 ST-Link 时蓝板 5V 脚无输出）
 *   2) Echo 是 5V 输出 → 正式版串 2k/3.3k 分压；PA5/PB5 是 FT（5V 容忍）脚
 *   3) 两颗不能同时触发（会收到对方的回波）→ 调用方负责轮流 + 间隔
 *
 *  提醒（已知约束）：PA5 与 PB5 共用 EXTI5，同一时刻只能用其中一个做外部中断。
 *  现在用的是“阻塞轮询”，不受影响；若以后改中断/输入捕获方式，需要换脚。
 * ========================================================================== */
#ifndef DRV_ULTRASONIC_H
#define DRV_ULTRASONIC_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#define US_COUNT   2
#define US_MIN_GAP_MS  60u    /* 同一颗两次触发的最小间隔（HC-SR04 手册建议 ≥60ms）：
                                 触发挨太近时模块的 Echo 会拖成一条长高电平 → 假 -2 */
#define US_RECOVER_MS  600u   /* **上一次没测到**之后的恢复间隔。
                                 HC-SR04 在“没回波”时会输约 200ms 高电平，此时继续密集重触发
                                 很容易把它锁在“永远无回波”状态 —— 给足安静时间才能缓过来 */

typedef enum { US_L = 0, US_R = 1 } UsId;

/* 负数状态码（直接写进 $U 帧，日志里一眼能看出为什么没测到） */
typedef enum {
    US_OK        =  0,   /* 测到了（mm 为正） */
    US_NO_RISE   = -1,   /* 触发后没等到上升沿：没回波 / 模块没供电 / 接错脚 */
    US_TOO_LONG  = -2,   /* 高电平 >30ms：超出量程（HC-SR04 无回波时输出约 200ms 高） */
    US_BUSY      = -3,   /* 触发前 Echo 还是高的：上一发没结束，本轮跳过 */
    US_TOO_SHORT = -4,   /* 高电平 <100µs：不合理的窄脉冲 */
    US_OUT_RANGE = -5,   /* 换算后 <20mm 或 >4000mm */
    US_TOO_SOON  = -6,   /* 距上次触发不足 US_MIN_GAP_MS：被驱动拦下（不算故障） */
} UsStatus;

typedef struct {
    GPIO_TypeDef *trig_port; uint16_t trig_pin;
    GPIO_TypeDef *echo_port; uint16_t echo_pin;
    char          name[6];        /* "L" / "R"，打印用 */
    /* ---- 最近一次测量的结果 ---- */
    int32_t  mm;                  /* mm（正）或 UsStatus（负） */
    uint32_t width_us;            /* Echo 高电平宽度（µs） */
    uint32_t stamp_ms;            /* 测量时刻 HAL_GetTick() */
    uint8_t  last_ok;             /* 上一次是否真的测到（决定用哪个恢复间隔） */
} UsDev;

void    Us_Init(void);
int32_t Us_Read(UsId id);         /* 测一次：返回 mm 或负状态码，并更新内部记录 */
UsDev  *Us_Get(UsId id);
void    Us_PrintDebug(UsId id);   /* 打一行人可读的调试信息（bring-up 用） */

#endif /* DRV_ULTRASONIC_H */
