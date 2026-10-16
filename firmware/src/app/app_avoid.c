/* A02 手推轨迹开环复现试验（2026-10-16，未上车标定）
 *
 * 长按车载按钮启动后：直行 2s -> 左弧 -> 右弧 -> 停车。
 * 本文件独占电机控制，不读取灰度、超声、编码器或陀螺仪作控制。
 * 仅依据手推日志 dl/dr 和旧实跑日志估出下面的 PWM / 时长；
 * 不能据此保证真实轨迹、避障间隙或回线。首次测试必须架空。
 *
 * 原右侧障碍 A02 版本已存放在 firmware/算法快照/，不编入本固件。
 */
#include "app/app_avoid.h"
#include "app/app_log.h"
#include "app/app_telemetry.h"
#include "drv/drv_motor.h"

#include <stdint.h>

/* ===================== 平时调参只看这里 =====================
 * _MS = 毫秒：1000u = 1 秒；_PWM = 电机占空比命令，不等于实测转速。
 * 三段动作按“直行 -> 左弧 -> 右弧”依次执行，最后自动停车。
 * 想改变转弯半径，先调两轮 PWM 的差；想改变转弯走多久，才调 _MS。
 * 右弧的 21/11 是毕烨恺现场改过的值，整理注释时不改动。
 *
 * 原始 0~3s 手推直行不重播，改由按钮后直行 2s 代替。
 * 手推 3~5s：左轮约 128mm / 右轮约 201mm；
 * 手推 5~13s：左轮约 910mm / 右轮约 558mm。
 * 这些 mm 用尚未精标的 0.115mm/计数及 BLE 2 倍分频估算。
 */
#define A02_STRAIGHT_MS   2000u  /* 直行 2.0 秒 */
#define A02_STRAIGHT_PWM    16   /* 直行时，左右轮同为 16% */

#define A02_LEFT_MS       1000u  /* 向左推出：1.0 秒 */
#define A02_LEFT_L_PWM       9   /* 左轮慢、右轮快 -> 车向左转 */
#define A02_LEFT_R_PWM      23

#define A02_RIGHT_MS     10000u  /* 向右回摆：10.0 秒，到时停车 */
#define A02_RIGHT_L_PWM     21   /* 左轮快、右轮慢 -> 车向右转 */
#define A02_RIGHT_R_PWM     11   /* 轮差 10；轮差越小，通常转弯越缓 */

#define A02_PWM_STEP         2   /* 每 10ms 单轮最多改变 2%，避免突然变速 */

/* 这几个名字只是程序内部的“当前阶段”，不需要为调速而修改。 */
typedef enum {
    A02_OFF,
    A02_STRAIGHT,
    A02_LEFT,
    A02_RIGHT,
    A02_DONE
} A02Phase;

static A02Phase s_phase;    /* 当前走到直行、左弧、右弧还是停车 */
static uint8_t s_armed;     /* 1 = 按钮已发车，A02 独占电机 */
static uint32_t s_phase_ms; /* 当前阶段开始时刻，单位毫秒 */
static int s_pwm_l, s_pwm_r; /* 此刻实际下发的左右 PWM 命令 */

/* “缓变”：每拍向目标 PWM 靠近一点，不会从 9% 瞬间跳到 21%。
 * 这是辅助函数，不决定直行/转弯持续时间。 */
static int slew(int current, int target)
{
    if (current < target - A02_PWM_STEP) { return current + A02_PWM_STEP; }
    if (current > target + A02_PWM_STEP) { return current - A02_PWM_STEP; }
    return target;
}

/* 保留 $T 和板载快照中的左右 PWM / state，便于比对首次实跑与手推日志。
 * 灰度统一填白（0），表示本试验完全不使用灰度；不能用于分析回线。 */
static void record(uint32_t now, int state)
{
    static const int white[8] = {0};
    Log_Note(0u, 0.0f, s_pwm_l, s_pwm_r, state);
    Tlm_SendT(now, white, 0.0f, s_pwm_l, s_pwm_r, state);
}

/* 上电初始化：只清状态、打版本标记；不会启动电机。 */
void Avoid_Init(void)
{
    s_phase = A02_OFF;
    s_armed = 0u;
    s_phase_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
    Tlm_SendEvent(97, "A02 handpush replay v1");
}

/* 主程序识别长按按钮后调用这里：on=1 开始，on=0 人工停止。
 * 实际按钮识别代码在 main.c，本文件只接收“开始/停止”结果。 */
void Avoid_SetDrive(uint8_t on)
{
    if (!on) {
        Mo_Stop();
        s_armed = 0u;
        s_phase = A02_OFF;
        s_pwm_l = s_pwm_r = 0;
        return;
    }
    /* 第一拍才起计时，按钮蜂鸣器的阻塞时间不算在直行 2s 内。 */
    Mo_Stop();
    s_armed = 1u;
    s_phase = A02_STRAIGHT;
    s_phase_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
    Tlm_SendEvent(3, "A02 replay straight");
}

/* 供 main.c 查询是否已经发车。 */
uint8_t Avoid_GetDrive(void) { return s_armed; }

/* DONE 仍保持接管，避免停车后 main.c 放行 A01 自动发车。 */
uint8_t Avoid_Mode(void) { return s_armed; }

/* 每约 10ms 调用一次：① 到时间换阶段 ② 算目标 PWM ③ 下发电机。
 * 全程没有根据黑线或超声波修正路线，是纯定时开环试验。 */
void Avoid_Step(void)
{
    if (!s_armed) { return; }
    uint32_t now = HAL_GetTick();
    if (s_phase_ms == 0u) { s_phase_ms = now; }

    /* 阶段切换：elapsed 是“现在 - 本阶段开始时刻”，单位毫秒。
     * 每换一段就重置 s_phase_ms，所以三个 _MS 是各段时长，不是总时长。 */
    uint32_t elapsed = now - s_phase_ms;
    if (s_phase == A02_STRAIGHT && elapsed >= A02_STRAIGHT_MS) {
        s_phase = A02_LEFT;
        s_phase_ms = now;
        Tlm_SendEvent(3, "A02 replay left");
    } else if (s_phase == A02_LEFT && elapsed >= A02_LEFT_MS) {
        s_phase = A02_RIGHT;
        s_phase_ms = now;
        Tlm_SendEvent(3, "A02 replay right");
    } else if (s_phase == A02_RIGHT && elapsed >= A02_RIGHT_MS) {
        s_phase = A02_DONE;
        s_pwm_l = s_pwm_r = 0;
        Mo_Stop();
        Tlm_SendEvent(4, "A02 replay stop");
    }

    if (s_phase == A02_DONE) {
        /* 锁存停车，不调用 A01；需人工再次长按关闭发车开关。 */
        Mo_Stop();
        record(now, 0);
        return;
    }

    /* 选本阶段的目标速度。真正送到电机前还会经过下方的缓变处理。 */
    int target_l = A02_STRAIGHT_PWM, target_r = A02_STRAIGHT_PWM;
    if (s_phase == A02_LEFT) {
        target_l = A02_LEFT_L_PWM;
        target_r = A02_LEFT_R_PWM;
    } else if (s_phase == A02_RIGHT) {
        target_l = A02_RIGHT_L_PWM;
        target_r = A02_RIGHT_R_PWM;
    }
    s_pwm_l = slew(s_pwm_l, target_l);
    s_pwm_r = slew(s_pwm_r, target_r);
    Mo_Set(s_pwm_l, s_pwm_r);
    record(now, 2);
}
