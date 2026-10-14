/* =============================================================================
 * A02 第一阶段：测试主动脱线 -> 再次碰线后交还 A01（未实车验收）
 *
 * 按钮由 main.c 识别；A02 从发车第一拍独占电机控制：
 *   静止 2s -> 简化低速循迹 1s -> 左偏至全白 -> 继续左偏短暂一段
 *   -> 右弧持续寻找黑线 -> 任意一路见黑 -> 松手，把控制交还 A01 跟线。
 *
 * [x2 版本] 四个行驶 PWM 为上一版的 2 倍；回线后循迹：降速 12%、KP 2.5、限幅 ±5、
 *   D 顶防冲过头、丢线时朝最后见线侧转、舵量斜坡限速 + 小积分 I（后四项移植自 A01）。
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

/* 融合开关：1 = 见黑（回到线）后把控制交还 A01；0 = 用本文件的回线循迹（备选）。
 * 交还原理：A02 自己松手（Avoid_SetDrive(0)）⇒ Avoid_Mode()==0 ⇒
 *          下一拍 main.c 自动去调 Trace_Step()，A01 接手跟线。
 * 注：A01 的驱动在长按发车时已由 main.c 的 Trace_SetDrive(1) 打开。 */
#define A02_HANDOFF_TO_A01         1

/* 现场调参：PWM 单位为百分比。一次只改一项并记录结果。
 * 本组为上一版的 2 倍速（数值 ×2）；PWM 不等于实测轮速，半径/速度仍需标定。 */
#define A02_FOLLOW_PWM            16    /* 前 1s 简化循迹的共同速度（原 8） */
#define A02_FOLLOW_KP              1.0f /* 灰度中心误差到差速的比例 */
#define A02_LEFT_L_PWM            10    /* 左偏：左轮较慢（原 5） */
#define A02_LEFT_R_PWM            14    /* 左偏：右轮较快（原 7） */
#define A02_RIGHT_L_PWM           14    /* 右弧：左轮较快（原 7） */
#define A02_RIGHT_R_PWM           10    /* 右弧：右轮较慢（原 5） */
#define A02_LEFT_AFTER_WHITE_MS  300u   /* 首次全白后继续左偏的固定时间 */
#define A02_WAIT_MS             2000u
#define A02_FOLLOW_MS           1000u
#define A02_FOLLOW_AFTER_MS     3000u   /* 见黑后继续循迹的时长 */
/* 回线后（FOLLOW_AFTER）单独一套参数：降速 + 提高转向权限。
 * 依据：logs/2026-10-14_1427 显示 16% 车速下 ±3 差速掰不回线、直接冲过。 */
#define A02_AFTER_PWM              12    /* 回线后基准速度（原先同 16） */
#define A02_AFTER_KP                2.5f /* 回线后比例增益（原先 1.0） */
#define A02_AFTER_STEER_MAX          5   /* 回线后差速限幅 ±（原先 3） */
#define A02_AFTER_KD              0.10f  /* 回线后 D 项增益（抑制冲过线，移植自 A01） */
#define A02_AFTER_D_ALPHA         0.12f  /* D 项低通，抑数字灰度跳变尖峰（同 A01） */
#define A02_AFTER_KI              0.01f  /* 回线后小积分增益（消稳态/弯道偏移，同 A01） */
#define A02_AFTER_I_MAX            3.0f  /* 积分项最大贡献（抗饱和） */
#define A02_AFTER_I_START_MS     300u    /* 进入回线后前 0.3s 不积分（同 A01） */
#define A02_AFTER_STEER_SLEW      1.0f   /* 每拍舵量最大变化 = 本值×(1+|err|)，同 A01 */
#define A02_AFTER_SEEK_STEER         5   /* 丢线时朝最后见线侧的转向量 */
#define A02_PWM_SLEW_PER_TICK      1    /* 每 10ms 左右轮各最多变 1% */

typedef enum {
    A02_IDLE,
    A02_WAIT,
    A02_FOLLOW,
    A02_LEFT,
    A02_RIGHT,
    A02_FOLLOW_AFTER,
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
/* 回线后循迹（FOLLOW_AFTER）的状态：D 项 + 丢线记忆（移植自 A01） */
static float    s_after_err_prev;
static float    s_after_d_filt;
static float    s_after_last_err;
static uint32_t s_after_ms_prev;
static uint8_t  s_after_d_valid;
static float    s_after_ierr;
static float    s_after_steer_out;

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

/* 简化循迹核心：黑位平均值相对中点 3.5 的偏差 -> 比例转向（带限幅）。
 * base=基准速度，kp=比例增益，smax=差速限幅；全白时 err=0 -> 直行。 */
static void a02_follow_cfg(uint8_t bits, int base, float kp, int smax,
                           int *left, int *right)
{
    int count = 0;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        if (bits & (uint8_t)(1u << i)) { count++; sum += i; }
    }
    float err = count ? (float)sum / (float)count - 3.5f : 0.0f;
    int steer = (int)(kp * err + (err >= 0.0f ? 0.5f : -0.5f));
    if (steer > smax) { steer = smax; }
    if (steer < -smax) { steer = -smax; }
    *left = base + steer;   /* 线偏左 -> 左轮减速 */
    *right = base - steer;
}

/* 前 1s 的简化循迹（原参数不变）。 */
static void a02_follow_pwm(uint8_t bits, int *left, int *right)
{
    a02_follow_cfg(bits, A02_FOLLOW_PWM, A02_FOLLOW_KP, 3, left, right);
}

/* 回线后循迹：在简化 P 上移植 A01 的关键做法——
 *   ① D 项：抑制“刚碰线就把舵打满 → 冲过头”；
 *   ② 丢线记忆：全白/读失败时朝“最后一次看到线的方向”转，而不是直行跑掉；
 *   ③ 舵量斜坡限速：每拍最多变 SLEW×(1+|err|)，治“灰度跳变 ⇒ 每半秒踹一脚”的振荡；
 *   ④ 小积分 I：消稳态/弯道固定偏移。
 * 参数用 A02_AFTER_*；丢线转有本阶段总时限兜底，不会无限转。 */
static void a02_after_pwm(uint8_t bits, uint8_t gray_ok, uint32_t now,
                          int *left, int *right)
{
    int count = 0;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        if (bits & (uint8_t)(1u << i)) { count++; sum += i; }
    }
    if (gray_ok && count > 0) {
        float err = (float)sum / (float)count - 3.5f;
        s_after_last_err = err;

        uint32_t dt = now - s_after_ms_prev;
        s_after_ms_prev = now;
        if (dt == 0u || dt > 50u) { dt = 10u; s_after_d_valid = 0u; }

        /* ---- D 项（低通后的 err 变化率）---- */
        float d_rate = 0.0f;
        if (s_after_d_valid) {
            d_rate = (err - s_after_err_prev) * 1000.0f / (float)dt;
        }
        s_after_d_valid = 1u;
        s_after_err_prev = err;
        s_after_d_filt += A02_AFTER_D_ALPHA * (d_rate - s_after_d_filt);

        /* ---- I 项（前 0.3s 不积，带抗饱和）---- */
        if ((now - s_phase_ms) >= A02_AFTER_I_START_MS) {
            s_after_ierr += err * (float)dt * 0.001f;
        }
        float i_term = A02_AFTER_KI * s_after_ierr;
        if (i_term >  A02_AFTER_I_MAX) { i_term =  A02_AFTER_I_MAX; s_after_ierr =  A02_AFTER_I_MAX / A02_AFTER_KI; }
        if (i_term < -A02_AFTER_I_MAX) { i_term = -A02_AFTER_I_MAX; s_after_ierr = -A02_AFTER_I_MAX / A02_AFTER_KI; }

        float steer_f = A02_AFTER_KP * err + i_term + A02_AFTER_KD * s_after_d_filt;

        /* ---- 舵量斜坡限速（同 A01）---- */
        float ae = (err < 0.0f) ? -err : err;
        float slew = A02_AFTER_STEER_SLEW * (1.0f + ae);
        float dstep = steer_f - s_after_steer_out;
        if (dstep >  slew) { dstep =  slew; }
        if (dstep < -slew) { dstep = -slew; }
        s_after_steer_out += dstep;
        if (s_after_steer_out >  A02_AFTER_STEER_MAX) { s_after_steer_out =  A02_AFTER_STEER_MAX; }
        if (s_after_steer_out < -A02_AFTER_STEER_MAX) { s_after_steer_out = -A02_AFTER_STEER_MAX; }

        int steer = (int)(s_after_steer_out + (s_after_steer_out >= 0.0f ? 0.5f : -0.5f));
        *left = A02_AFTER_PWM + steer;
        *right = A02_AFTER_PWM - steer;
    } else {
        /* 丢线：朝最后看到线的一侧转（最后在线左 → 左转）；顺便清控制状态。 */
        s_after_d_filt = 0.0f;
        s_after_d_valid = 0u;
        s_after_ierr = 0.0f;
        s_after_steer_out = 0.0f;
        int turn = (s_after_last_err < -0.5f) ? -A02_AFTER_SEEK_STEER :
                   (s_after_last_err >  0.5f) ?  A02_AFTER_SEEK_STEER : 0;
        *left = A02_AFTER_PWM + turn;
        *right = A02_AFTER_PWM - turn;
    }
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
    Tlm_SendEvent(97, "AVOID build a02-s1-handoff-a01-unverified");
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
        s_after_err_prev = 0.0f;
        s_after_d_filt = 0.0f;
        s_after_last_err = 0.0f;
        s_after_ms_prev = 0u;
        s_after_d_valid = 0u;
        s_after_ierr = 0.0f;
        s_after_steer_out = 0.0f;
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
            /* 任意一路有效见黑 = 已回到线。 */
#if A02_HANDOFF_TO_A01
            Tlm_SendEvent(4, "A02 TEST line found; handoff to A01");
            Avoid_SetDrive(0u);     /* 松手；下一拍 main 调 Trace_Step，A01 接手 */
            return;                 /* 本拍不再由 A02 发运动命令 */
#else
            a02_enter(A02_FOLLOW_AFTER, now, "A02 TEST black; follow 3s");
#endif
        } else {
            a02_drive(A02_RIGHT_L_PWM, A02_RIGHT_R_PWM);
        }
        break;

    case A02_FOLLOW_AFTER:
        {
            int left = A02_AFTER_PWM;
            int right = A02_AFTER_PWM;
            a02_after_pwm(bits, gray_ok, now, &left, &right);
            a02_drive(left, right);
            if (now - s_phase_ms >= A02_FOLLOW_AFTER_MS) {
                s_phase = A02_DONE;
                s_pwm_l = s_pwm_r = 0;
                Mo_Brake();
                Tlm_SendEvent(4, "A02 TEST follow 3s done; stopped");
            }
        }
        break;

    default:
        /* main 的按钮关闭入口负责停车；其他状态不发新的运动指令。 */
        break;
    }

    a02_emit(now, s_last_bits,
             (s_phase == A02_RIGHT || s_phase == A02_FOLLOW_AFTER ||
              s_phase == A02_DONE) ? 3 : 2);
}
