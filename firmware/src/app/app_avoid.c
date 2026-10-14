/* =============================================================================
 * A02 第一阶段：仅测试主动脱线与再次碰线停车（未实车验收）
 *
 * 按钮由 main.c 识别；A02 从发车第一拍独占电机控制：
 *   静止 2s -> 简化低速循迹 1s -> 左偏至全白 -> 继续左偏短暂一段
 *   -> 右弧持续寻找黑线 -> 任意一路见黑，锁存刹车。
 *
 * 本版按现场测试要求不以时长、编码器、陀螺或超声自动停车。
 * 长按按钮仍可随时人工停车。旧完整避障版见 Git 提交 628b285。
 * 轮距、轮速和低占空比死区尚未标定，20cm 半径只是目标轨迹，
 * 不能由下面的 PWM 数字推算出实际半径。
 * ============================================================================= */
#include "app/app_avoid.h"
#include "app/app_log.h"
#include "app/app_telemetry.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"

#include <stdint.h>

/* 现场调参：PWM 单位为百分比。一次只改一项并记录结果。 */
#define A02_FOLLOW_PWM             8    /* 前 1s 简化循迹的共同速度 */
#define A02_FOLLOW_KP              1.0f /* 灰度中心误差到差速的比例 */
#define A02_LEFT_L_PWM             5    /* 左偏：左轮较慢 */
#define A02_LEFT_R_PWM             7    /* 左偏：右轮较快 */
#define A02_RIGHT_L_PWM            7    /* 右弧：左轮较快 */
#define A02_RIGHT_R_PWM            5    /* 右弧：右轮较慢 */
#define A02_LEFT_AFTER_WHITE_MS  300u   /* 首次全白后继续左偏的固定时间 */
#define A02_WAIT_MS             2000u
#define A02_FOLLOW_MS           1000u
#define A02_PWM_SLEW_PER_TICK      1    /* 每 10ms 左右轮各最多变 1% */

typedef enum {
    A02_IDLE,
    A02_WAIT,
    A02_FOLLOW,
    A02_LEFT,
    A02_RIGHT,
    A02_DONE
} A02Phase;

static A02Phase s_phase;
static uint8_t s_drive_enabled;
static uint8_t s_saw_white;
static uint32_t s_phase_ms;
static uint32_t s_white_ms;
static uint8_t s_last_bits;
static int s_pwm_l;
static int s_pwm_r;

static void a02_enter(A02Phase phase, uint32_t now, const char *note)
{
    s_phase = phase;
    s_phase_ms = now;
    if (note) { Tlm_SendEvent(3, note); }
}

/* 原始灰度低有效；成功读取后 bits 的 1 表示黑。读失败不改变状态，
 * 因此一次 I2C 故障既不会假装“已全白”，也不会假装“已碰线”。 */
static uint8_t a02_read_gray(uint8_t *bits)
{
    uint8_t raw = 0xffu;
    if (!Gr_ReadDigital(&raw)) { return 0u; }
    *bits = (uint8_t)~raw;
    return 1u;
}

/* 简化循迹用八路黑位的平均位置；全白时保持直行，不进入 A01 的
 * 丢线找线或终点状态机。左右轮保持正向。 */
static void a02_follow_pwm(uint8_t bits, int *left, int *right)
{
    int count = 0;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        if (bits & (uint8_t)(1u << i)) { count++; sum += i; }
    }
    float err = count ? (float)sum / (float)count - 3.5f : 0.0f;
    int steer = (int)(A02_FOLLOW_KP * err + (err >= 0.0f ? 0.5f : -0.5f));
    if (steer > 3) { steer = 3; }
    if (steer < -3) { steer = -3; }
    *left = A02_FOLLOW_PWM + steer;  /* 线偏左 -> 左轮减速 */
    *right = A02_FOLLOW_PWM - steer;
}

static int a02_slew(int current, int target)
{
    if (current < target) {
        current += A02_PWM_SLEW_PER_TICK;
        if (current > target) { current = target; }
    } else if (current > target) {
        current -= A02_PWM_SLEW_PER_TICK;
        if (current < target) { current = target; }
    }
    return current;
}

static void a02_drive(int left, int right)
{
    s_pwm_l = a02_slew(s_pwm_l, left);
    s_pwm_r = a02_slew(s_pwm_r, right);
    Mo_Set(s_pwm_l, s_pwm_r);
}

/* 沿用冻结的 $T 与快照格式：state 2=脱线动作，3=右弧找线。 */
static void a02_emit(uint32_t now, uint8_t bits, int state)
{
    int gray[8];
    int count = 0;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        gray[i] = (bits >> i) & 1u;
        if (gray[i]) { count++; sum += i; }
    }
    float err = count ? (float)sum / (float)count - 3.5f : 0.0f;
    Log_Note(bits, err, s_pwm_l, s_pwm_r, state);
    Tlm_SendT(now, gray, err, s_pwm_l, s_pwm_r, state);
}

void Avoid_Init(void)
{
    s_drive_enabled = 0u;
    s_phase = A02_IDLE;
    s_saw_white = 0u;
    s_last_bits = 0u;
    s_pwm_l = s_pwm_r = 0;
    Tlm_SendEvent(97, "AVOID build a02-s1-left-right-arc-unverified");
}

void Avoid_SetDrive(uint8_t on)
{
    s_drive_enabled = on ? 1u : 0u;
    s_pwm_l = s_pwm_r = 0;
    Mo_Stop();
    if (on) {
        /* 在 main 打开 TRACE 之前占住控制权；下一拍不会先执行完整 A01。 */
        s_saw_white = 0u;
        s_last_bits = 0u;
        s_white_ms = 0u;
        a02_enter(A02_WAIT, HAL_GetTick(), "A02 TEST wait 2s");
    } else {
        s_phase = A02_IDLE;
    }
}

uint8_t Avoid_GetDrive(void)
{
    return s_drive_enabled;
}

uint8_t Avoid_Mode(void)
{
    return s_drive_enabled; /* 测试期间 main 只调用 Avoid_Step，不运行 A01。 */
}

void Avoid_Step(void)
{
    if (!s_drive_enabled) { return; }
    uint32_t now = HAL_GetTick();
    if (s_phase == A02_DONE) {
        Mo_Brake();
        s_pwm_l = s_pwm_r = 0;
        return;
    }

    uint8_t bits = 0u;
    uint8_t gray_ok = a02_read_gray(&bits);
    if (gray_ok) { s_last_bits = bits; }

    switch (s_phase) {
    case A02_WAIT:
        Mo_Stop();
        if (now - s_phase_ms >= A02_WAIT_MS) {
            a02_enter(A02_FOLLOW, now, "A02 TEST follow 1s");
        }
        break;

    case A02_FOLLOW:
        {
            int left = A02_FOLLOW_PWM;
            int right = A02_FOLLOW_PWM;
            if (gray_ok) { a02_follow_pwm(bits, &left, &right); }
            a02_drive(left, right);
            if (now - s_phase_ms >= A02_FOLLOW_MS) {
                a02_enter(A02_LEFT, now, "A02 TEST leave left");
            }
        }
        break;

    case A02_LEFT:
        if (gray_ok && bits == 0u && !s_saw_white) {
            s_saw_white = 1u;
            s_white_ms = now;
            Tlm_SendEvent(3, "A02 TEST all white");
        }
        if (s_saw_white && now - s_white_ms >= A02_LEFT_AFTER_WHITE_MS) {
            a02_enter(A02_RIGHT, now, "A02 TEST arc right");
        }
        a02_drive(A02_LEFT_L_PWM, A02_LEFT_R_PWM);
        break;

    case A02_RIGHT:
        if (gray_ok && bits != 0u) {
            s_phase = A02_DONE;
            s_pwm_l = s_pwm_r = 0;
            Mo_Brake();
            Tlm_SendEvent(4, "A02 TEST black found; stopped");
        } else {
            a02_drive(A02_RIGHT_L_PWM, A02_RIGHT_R_PWM);
        }
        break;

    default:
        /* main 的按钮关闭入口负责停车；其他状态不发新的运动指令。 */
        break;
    }

    a02_emit(now, s_last_bits, s_phase == A02_RIGHT || s_phase == A02_DONE ? 3 : 2);
}
