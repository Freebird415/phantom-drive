/* =============================================================================
 *  app_avoid.c —— 避障与回线骨架（A02 毕烨恺）
 *
 *  ★ 你只改这个文件。下面只是一个"能跑起来的起点"，真正的策略要你来设计：
 *      单测 → 检测 demo → 绕行 + 回线 → 终点精停
 *
 *  ★ 骨架已经做了：
 *      1) 把超声波读数（含状态码）统一换算成"距离多少 cm"
 *      2) 一个**占位**的避障动作（往远的一侧转）
 *      3) 维护"接管中"标志 Avoid_Mode()，让循迹让位（不然循迹 100Hz 会把你的命令冲掉）
 *      4) 打 $E 事件点，日志里能看到"这里避障了"
 *
 *  ⚠️ 骨架故意写得很保守：拿到读数后只在"很近"时才动作，且动作有时限。
 *     真正的绕行/回线/精停全在 Avoid_Step() 里，你自己设计。
 * ========================================================================== */
#include "app/app_avoid.h"
#include "app/app_telemetry.h"
#include "drv/drv_ultrasonic.h"
#include "drv/drv_motor.h"

#include <stdio.h>

#define AV_NEAR_CM      30.0f     /* 进入这个距离就认为"要避障"（先保守，实测再调）*/
#define AV_UNKNOWN_CM  400.0f     /* 读不到（负状态码）时按"很远"处理 —— 不是"很近"！*/
#define AV_ACTION_MS    400u      /* 一次避障动作持续多久，然后交回循迹 */

static uint8_t  s_drive;          /* ⚠️ 0 = 只决策不发车（默认）*/
static uint8_t  s_mode;           /* 0 = 让给循迹；1 = 避障接管中 */
static uint32_t s_until_ms;       /* 接管到什么时候 */

void    Avoid_SetDrive(uint8_t on) { s_drive = on ? 1u : 0u; }
uint8_t Avoid_GetDrive(void)       { return s_drive; }
uint8_t Avoid_Mode(void)           { return s_mode; }

/* 把"读数 + 状态码"统一成一个距离（cm）——**负状态码绝不能当成"很近"** */
static float av_cm(UsId id)
{
    int32_t v = Us_Get(id)->mm;
    if (v > 0) { return (float)v / 10.0f; }     /* 正常：mm -> cm */
    return AV_UNKNOWN_CM;                      /* 没测到/超量程 -> 当"很远" */
}

void Avoid_Init(void)
{
    s_drive    = 0u;                            /* ⚠️ 默认不发车 */
    s_mode     = 0u;
    s_until_ms = 0u;
    printf("# AVOID 驱动=%s 触发距离=%.0fcm\r\n",
           s_drive ? "开" : "关(只判断不发车)", (double)AV_NEAR_CM);
}

void Avoid_Step(void)
{
    uint32_t ms = HAL_GetTick();
    float    l  = av_cm(US_L);
    float    r  = av_cm(US_R);

    /* 动作做完了就把控制权交回循迹 */
    if (s_mode && (int32_t)(ms - s_until_ms) >= 0) {
        s_mode = 0u;
        if (s_drive) { Mo_Stop(); }             /* 交回前先停一下，避免残留命令 */
    }

    if (s_mode) { return; }                     /* 接管中：不重新决策（策略由你决定）*/

    if (l < AV_NEAR_CM || r < AV_NEAR_CM) {
        s_mode     = 1u;
        s_until_ms = ms + AV_ACTION_MS;
        Tlm_SendEvent(80, (l < r) ? "AVOID: left near" : "AVOID: right near");

        /* ===================== 占位动作：往远的一侧转 =====================
         * ★★ A02 要做的：把这里换成真正的策略 ★★
         *    例如：先减速 → 转向避开 → 直行一段 → 回线 → 继续循迹
         *    终点精停也要在这里或循迹那边加（过终点线且车尾不过线）
         * ================================================================ */
        if (s_drive) {
            if (l < r) { Mo_Set(-35, 45); }     /* 左边近 → 往右转 */
            else       { Mo_Set(45, -35); }     /* 右边近 → 往左转 */
        }
    }
}
