/* =============================================================================
 * A02 第一阶段极简标定程序：左侧主动脱线，再次见线后停车
 *
 * 本文件当前只服务一次无障碍标定，不包含正式避障、路径规划或自动回归 A01：
 *   长按发车 -> 停车等待 2s -> A01 低速循迹 1s -> 固定低速左转
 *   -> 至少见过一次全白 -> 任意灰度再次见黑 -> 刹车并锁存
 *
 * 旧版完整避障实现保留在 Git 提交 628b285；恢复说明见：
 *   firmware/算法快照/v10_2026-10-14_A02_v5完整避障/说明.md
 * ============================================================================= */
#include "app/app_avoid.h"
#include "app/app_log.h"
#include "app/app_telemetry.h"
#include "app/app_trace.h"
#include "drv/drv_encoder.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"
#include "drv/drv_ultrasonic.h"

#include <stdint.h>

/* ---------------------------- 现场调参区 ----------------------------
 * 第一轮建议只改 FOLLOW_PWM、TURN_INNER_PWM、TURN_OUTER_PWM。
 * 所有数值都是 PWM 百分比，不代表经过标定的车轮线速度。 */
#define A02_FOLLOW_PWM          6       /* A01 循迹 1s 时的临时基准速度 */
#define A02_TURN_INNER_PWM      5       /* 左转内侧：左轮 */
#define A02_TURN_OUTER_PWM      7       /* 左转外侧：右轮 */
#define A02_WAIT_MS          2000u      /* 发车后保持停车 */
#define A02_FOLLOW_MS        1000u      /* 交给 A01 低速循迹 */
#define A02_TURN_TIMEOUT_MS 10000u      /* 只作安全兜底，不是轨迹终点 */

/* 以下是安全边界，现场不要为了“跑成功”而优先放宽。 */
#define A02_STOP_MM             30      /* 任一超声波 <=30mm 立即停车 */
#define A02_US_FRESH_MS        150u
#define A02_GYRO_MAX_AGE_MS     80u
#define A02_NO_PROGRESS_MS     500u
#define A02_ENCODER_STEP_MAX   500
#define A02_PWM_SLEW_PER_TICK    1      /* 每个 10ms 控制拍最多改变 1% */

typedef enum {
    A02_IDLE = 0,
    A02_WAIT,
    A02_FOLLOW_A01,
    A02_TURN_LEFT,
    A02_DONE,
    A02_FAULT
} A02Phase;

static A02Phase s_phase;
static uint8_t s_drive_enabled;
static uint8_t s_test_used;
static uint8_t s_saw_all_white;
static uint32_t s_phase_started_ms;
static uint32_t s_last_progress_ms;
static int16_t s_enc_l;
static int16_t s_enc_r;
static int s_pwm_l;
static int s_pwm_r;
static int s_saved_trace_base;

/* 恢复 A01 原来的 base_speed，避免一次标定永久改掉循迹参数。 */
static void a02_restore_trace_speed(void)
{
    if (s_saved_trace_base >= 0) {
        Tlm_Params()->base_speed = s_saved_trace_base;
        s_saved_trace_base = -1;
    }
}

/* 所有失败都先刹车、后发日志；日志阻塞不能延长车辆运动。 */
static void a02_fault(const char *reason)
{
    if (s_phase == A02_FAULT) { return; }
    a02_restore_trace_speed();
    s_phase = A02_FAULT;
    s_pwm_l = s_pwm_r = 0;
    Mo_Brake();
    Tlm_SendEvent(88, reason);
}

static void a02_enter(A02Phase next, uint32_t now, const char *event)
{
    s_phase = next;
    s_phase_started_ms = now;
    if (event) { Tlm_SendEvent(3, event); }
}

/* Log_Note 会刷新 MPU 缓存；T 帧继续沿用冻结协议，state 2=动作、3=找线。 */
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

static uint8_t a02_read_gray(uint8_t *bits)
{
    uint8_t raw = 0xffu;
    if (!Gr_ReadDigital(&raw)) { return 0u; }
    *bits = (uint8_t)~raw;       /* 驱动低有效；算法统一为 1=黑 */
    return 1u;
}

static uint8_t a02_gyro_fresh(uint32_t now)
{
    LogGyro gyro;
    Log_GetGyro(&gyro);
    return gyro.valid && gyro.seq && (now - gyro.stamp_ms) <= A02_GYRO_MAX_AGE_MS;
}

static uint8_t a02_emergency_clear(uint32_t now)
{
    for (int i = 0; i < 2; i++) {
        const UsDev *u = Us_Get((UsId)i);
        if (u->stamp_ms && now - u->stamp_ms <= A02_US_FRESH_MS &&
            u->mm > 0 && u->mm <= A02_STOP_MM) {
            return 0u;
        }
    }
    return 1u;
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

/* 左转期间唯一的运动保护：反馈必须新鲜、编码器不能反向/跳变/长期无进度。 */
static uint8_t a02_motion_safe(uint32_t now)
{
    if (!a02_gyro_fresh(now)) {
        a02_fault("A02 TEST gyro invalid/stale");
        return 0u;
    }

    int16_t l = En_Count(ENC_L);
    int16_t r = En_Count(ENC_R);
    int32_t dl = (int16_t)(l - s_enc_l);
    int32_t dr = (int16_t)(r - s_enc_r);
    s_enc_l = l;
    s_enc_r = r;

    if (dl < -2 || dr < -2 || dl > A02_ENCODER_STEP_MAX || dr > A02_ENCODER_STEP_MAX) {
        a02_fault("A02 TEST encoder direction/jump");
        return 0u;
    }
    if (dl + dr > 0) { s_last_progress_ms = now; }
    if (now - s_last_progress_ms > A02_NO_PROGRESS_MS) {
        a02_fault("A02 TEST no progress");
        return 0u;
    }
    return 1u;
}

static void a02_start_once(uint32_t now)
{
    s_test_used = 1u;            /* 每次上电最多一次，不因手动停车重新武装 */
    s_saw_all_white = 0u;
    s_pwm_l = s_pwm_r = 0;
    s_enc_l = En_Count(ENC_L);
    s_enc_r = En_Count(ENC_R);
    s_last_progress_ms = now;
    Mo_Stop();
    a02_enter(A02_WAIT, now, "A02 TEST wait 2s");
}

void Avoid_Init(void)
{
    s_phase = A02_IDLE;
    s_drive_enabled = 0u;
    s_test_used = 0u;
    s_saved_trace_base = -1;
    s_pwm_l = s_pwm_r = 0;
    Tlm_SendEvent(97, "AVOID build a02-s1-min-left-arc");
}

void Avoid_SetDrive(uint8_t on)
{
    s_drive_enabled = on ? 1u : 0u;
    if (!on) {
        a02_restore_trace_speed();
        s_phase = A02_IDLE;
        s_pwm_l = s_pwm_r = 0;
        Mo_Stop();
    }
}

uint8_t Avoid_GetDrive(void)
{
    return s_drive_enabled;
}

/* FOLLOW 阶段必须返回 0：main 才会先调用 Trace_Step(1)，让 A01 真正控车。 */
uint8_t Avoid_Mode(void)
{
    return s_phase != A02_IDLE && s_phase != A02_FOLLOW_A01;
}

void Avoid_Step(void)
{
    uint32_t now = HAL_GetTick();

    if (!Trace_GetDrive() || !s_drive_enabled) { return; }
    if (!a02_emergency_clear(now)) {
        a02_fault("A02 TEST emergency <=30mm");
        return;
    }
    if (s_phase == A02_FAULT || s_phase == A02_DONE) {
        Mo_Brake();
        s_pwm_l = s_pwm_r = 0;
        return;
    }
    if (s_phase == A02_IDLE) {
        if (!s_test_used) { a02_start_once(now); }
        return;
    }

    if (s_phase == A02_FOLLOW_A01) {
        /* 本拍 A01 已完成灰度读取、PID、低速电机输出和 T 帧发送。 */
        if (!a02_gyro_fresh(now)) {
            a02_fault("A02 TEST gyro invalid/stale");
            return;
        }
        if (now - s_phase_started_ms >= A02_FOLLOW_MS) {
            const uint8_t *live = Log_LiveFrame();
            a02_restore_trace_speed();
            s_pwm_l = (int8_t)live[2];
            s_pwm_r = (int8_t)live[3];
            if (s_pwm_l < 0) { s_pwm_l = 0; }
            if (s_pwm_r < 0) { s_pwm_r = 0; }
            s_enc_l = En_Count(ENC_L);
            s_enc_r = En_Count(ENC_R);
            s_last_progress_ms = now;
            a02_enter(A02_TURN_LEFT, now, "A02 TEST turn left");
        }
        return;
    }

    uint8_t bits = 0u;
    if (!a02_read_gray(&bits)) {
        a02_fault("A02 TEST gray read failed");
        return;
    }

    if (s_phase == A02_WAIT) {
        Mo_Stop();
        s_pwm_l = s_pwm_r = 0;
        a02_emit(now, bits, 2);
        if (now - s_phase_started_ms >= A02_WAIT_MS) {
            if (!a02_gyro_fresh(now)) {
                a02_fault("A02 TEST gyro invalid/stale");
                return;
            }
            s_saved_trace_base = Tlm_Params()->base_speed;
            Tlm_Params()->base_speed = A02_FOLLOW_PWM;
            Trace_FinReset();
            a02_enter(A02_FOLLOW_A01, now, "A02 TEST A01 follow 1s");
        }
        return;
    }

    if (s_phase == A02_TURN_LEFT) {
        if (!a02_motion_safe(now)) { return; }

        /* 极简回线判据：先见全白，随后任意一路见黑即停车。 */
        if (bits == 0u) { s_saw_all_white = 1u; }
        if (s_saw_all_white && bits != 0u) {
            s_phase = A02_DONE;
            s_pwm_l = s_pwm_r = 0;
            Mo_Brake();
            Tlm_SendEvent(4, "A02 TEST black found; stopped");
            a02_emit(now, bits, 3);
            return;
        }
        if (now - s_phase_started_ms >= A02_TURN_TIMEOUT_MS) {
            a02_fault("A02 TEST turn timeout");
            return;
        }

        s_pwm_l = a02_slew(s_pwm_l, A02_TURN_INNER_PWM);
        s_pwm_r = a02_slew(s_pwm_r, A02_TURN_OUTER_PWM);
        Mo_Set(s_pwm_l, s_pwm_r);
        a02_emit(now, bits, 3);
        return;
    }

    a02_fault("A02 TEST invalid phase");
}
