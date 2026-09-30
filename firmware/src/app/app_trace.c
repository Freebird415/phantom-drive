/* =============================================================================
 *  app_trace.c —— 循迹算法骨架（A01 熊雅琦）
 *
 *  ★ 你只改这个文件。下面是"能跑起来的起点"，不是最终方案。
 *  ★ 骨架已经做了三件你会用到的事：
 *      1) 读灰度 → 算位置误差 err（−3.5 ~ +3.5，负=线在左边）
 *      2) 单 P 控制 → 算出左右占空比
 *      3) 上报 $T 帧（电脑/曲线图看的就是它）
 *  ★ 你要做的是把它变成"能跑完全赛道"的策略：加 I/D、处理丢线、过弯减速…
 *
 *  参数不用重烧就能改（串口，回车结尾）：
 *      SET kp 8        改比例
 *      SET ki 0.02     改积分
 *      SET kd 1.5      改微分
 *      SET base_speed 45   改基准速度
 *      GET             看当前参数
 * ========================================================================== */
#include "app/app_trace.h"
#include "app/app_telemetry.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"

#include <stdio.h>

#define TR_BASE_DEFAULT   45      /* 基准速度的兜底值（base_speed 没设时用）*/
#define TR_POS_CENTER     3.5f    /* 8 路灰度的中心：0..7 的中点是 3.5 */

static uint8_t s_online;          /* 灰度是否在线（不在线就发模拟数据，保住 P01 链路）*/
static uint8_t s_drive;           /* ⚠️ 0 = 只算不发（默认，安全）；1 = 真的驱动电机 */
static int     s_state;           /* 上报用：0 = 丢线停车，1 = 正常巡线 */

void    Trace_SetDrive(uint8_t on) { s_drive = on ? 1u : 0u; }
uint8_t Trace_GetDrive(void)       { return s_drive; }

void Trace_Init(void)
{
    s_drive  = 0u;                /* ⚠️ 默认不驱动：第一次一定要让轮子悬空再 `TRACE 1` */
    s_state  = 0;
    s_online = Gr_Ping();

    printf("# TRACE 灰度=%s 驱动=%s\r\n",
           s_online ? "在线" : "不在线", s_drive ? "开" : "关(只算不发)");
    if (!s_online) {
        printf("#       灰度不在线 -> 先发模拟数据（保住 P01 链路自检）；接上就自动走真实读数\r\n");
    }
}

void Trace_Step(uint8_t allow_drive)
{
    uint32_t ms = HAL_GetTick();
    Params  *p  = Tlm_Params();
    int      s[8];
    float    err;

    if (s_online) {
        /* ---- 读灰度：1 字节，bit0 = 第 1 路 … bit7 = 第 8 路，黑 = 1 ---- */
        uint8_t d = Gr_ReadDigital();
        int sum = 0, cnt = 0;
        for (int i = 0; i < 8; i++) {
            s[i] = (int)((d >> i) & 1u);
            if (s[i]) { sum += i; cnt++; }
        }

        if (cnt == 0) {
            /* 全白 = 丢线。最安全的处理是先停；想"循着上次方向找线"就改这里。 */
            if (s_drive && allow_drive) { Mo_Stop(); }
            Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
            s_state = 0;
            return;
        }
        /* 位置误差 = 黑点的平均位置 − 中心（负 = 线偏左，要往左修）*/
        err = (float)sum / (float)cnt - TR_POS_CENTER;
    } else {
        /* ---- 灰度没接时的模拟数据（接上后这段永远走不到，可以删）---- */
        int32_t k   = (int32_t)((ms / 10u) % 200u);
        int32_t e10 = (k < 100) ? (k - 50) : (150 - k);
        int center  = 3 + (int)(e10 / 25);
        if (center < 0) { center = 0; }
        if (center > 7) { center = 7; }
        for (int i = 0; i < 8; i++) { s[i] = (i == center || i == center + 1) ? 1 : 0; }
        err = (float)e10 / 10.0f;
    }

    /* ---- 控制器：现在只有单 P ---- */
    int base = p->base_speed;
    if (base <= 0) { base = TR_BASE_DEFAULT; }

    float steer_f = p->kp * err;
    int   steer   = (int)(steer_f + (steer_f >= 0.0f ? 0.5f : -0.5f));   /* 四舍五入 */

    int l = base + steer;     /* 线偏左 → 往左修：左轮慢、右轮快 */
    int r = base - steer;

    s_state = 1;
    if (s_drive && allow_drive) {
        Mo_Set(l, r);         /* Mo_Set 内部会自动夹到 ±100 */
    }

    /* $T 里的 pwm 是"算法打算给的值"；驱动关掉时电机实际不动 */
    Tlm_SendT(ms, s, err, l, r, s_state);
}
