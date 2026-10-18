/* =============================================================================
 *  a01_tune.h —— A01 循迹的「调参总表」（唯一出处，改这里就够）
 *
 *  为什么单独拉一个文件：
 *    以前 kp/ki/kd/base_speed 写在 app_telemetry.c 的 s_p 初始化里，
 *    而 base_speed 又会被 app_trace.c 的 TR_BASE_DEFAULT 覆盖 —— 两个地方、两套值，
 *    经常出现"我改了 30，为什么 GET 出来是 45"（2026-10-18 实测就是这样）。
 *    现在全部集中到这个文件：**只改这里，两处自动一致**。
 *
 *  单位与含义（详见 app_trace.c 顶部注释）：
 *    kp/ki/kd  —— 位置式 PID，被控量 err = 8 路灰度黑点重心 − 3.5（单位"路"，±3.5）
 *    base_speed —— 巡航 PWM（%），舵量叠加在它上面；运行时被 A01_BASE_MAX 夹住
 *
 *  ⚠️ 两个提醒：
 *    ① `SET kp 35` 这类在线命令只改 RAM，**断电就回到本文件的默认值**；
 *    ② 改完必须重新编译烧录才生效。
 * ==========================================================================*/
#ifndef APP_A01_TUNE_H
#define APP_A01_TUNE_H

/* ---------------- ① 正常巡航 PID ---------------- */
#define A01_KP            40.00f   /* 比例：偏一点打多少舵（太大甩头/振荡，太小切弯）*/
#define A01_KI             0.10f   /* 积分：约 10 秒才积累 1 个 duty ⇒ 现在≈纯 PD */
#define A01_KD             1.5f   /* 微分：误差变化率的阻尼/提前量（限幅 ±12 duty）*/
#define A01_BASE_SPEED       30    /* 巡航 PWM（%）*/

/* ---------------- ② 速度上限 ---------------- */
#define A01_BASE_MAX         50    /* SET 也不能越过；base_speed 运行时被夹到这*/

/* ---------------- ③ 回线后「慢速 A01」窗口（A02 交还 / 自己丢线找回）---------------- */
/* 窗口内基准 PWM 从 PWM0 线性过渡到 PWM1；到点自动交回 base_speed */
#define A01_SLOW_MS       3500u    /* 窗口时长 t（ms）；0 = 关闭该功能 */
#define A01_SLOW_PWM0        20    /* 窗口【起点】PWM（%）；0 = 关闭 */
#define A01_SLOW_PWM1        45    /* 窗口【终点】PWM（%）；线性过渡到这里 */

/* ---------------- ④ 编译期守门（别删）---------------- */
#if (A01_SLOW_PWM0 != 0) && (A01_SLOW_PWM0 < 8)
#error "A01_SLOW_PWM0 must be 0 (off) or >=8; 1..7 is below the motor dead zone"
#endif
#if (A01_SLOW_PWM1 != 0) && (A01_SLOW_PWM1 < 8)
#error "A01_SLOW_PWM1 must be 0 (off) or >=8; 1..7 is below the motor dead zone"
#endif
#if (A01_BASE_MAX < 1) || (A01_BASE_MAX > 100)
#error "A01_BASE_MAX must be 1..100"
#endif

#endif /* APP_A01_TUNE_H */
