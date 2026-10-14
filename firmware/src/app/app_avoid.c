/* A02 v5: valid/fresh gyro + encoder evidence watchdog; no blind fallback.
 * STEP 底板坐标下模型 200x195.5x100mm；用户确认朝驱动轮端前进。
 * 模型未包含现装探头/灰度/线束及全部突出件。下列外廓/安装偏移仍待实测。
 * 25mm 是名义规划余量，不是超声读数或已验收的实车间隙。
 * mm/计数仍未精标；轮侧只做转向活动/方向证据，不当角速度真值。 */
#include "app/app_avoid.h"
#include "app/app_trace.h"
#include "app/app_telemetry.h"
#include "app/app_log.h"
#include "drv/drv_ultrasonic.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_encoder.h"
#include "drv/drv_motor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define AV_CONFIRM_MM       550
#define AV_STOP_MM           30
#define AV_LINE_READY_MS    250u
#define AV_US_FRESH_MS      150u
#define AV_PAIR_MAX_MS      550u
#define AV_BODY_HALF_MM     100.0f /* 模型宽 195.5 向上圆整，未覆盖未知突出件 */
#define AV_BODY_FRONT_MM     40.0f /* 模型轮轴到驱动轮端约 35mm，圆整 */
#define AV_BODY_REAR_MM     170.0f /* 模型轮轴到万向轮端约 165mm，圆整 */
#define AV_PROBE_FRONT_MM    40.0f /* 探头面相对驱动轮轴：安装后待量 */
#define AV_GAP_MM            25.0f /* 用户目标 2~3cm，取中值；非验收结论 */
#define AV_TRACK_PAD_MM       8.0f /* 控制量化/微小偏航的名义跟踪预算，未实测 */
#define AV_RANGE_PAD_MM      10.0f /* 仅名义误差预算，覆盖范围/偏移仍待标测 */
#define AV_MOUNT_PAD_MM      40.0f /* 未量探头前后偏移的试跑预算，不是测量值 */
#define AV_MIN_RADIUS_MM     90.0f /* 前进差速的规划半径，实车待标定 */
#define AV_OBSTACLE_LEN_MM  181.0f /* 最大底面150x100的对角线向上圆整，覆盖任意摆角 */
#define AV_OBSTACLE_WIDTH_MM 181.0f /* 判侧仍不能测出障碍近侧边的实际横向位置 */
#define AV_ROUTE_SPEED       80.0f /* 以下均为编码器名义 mm/s，不是 PWM */
#define AV_SEEK_SPEED        60.0f /* 本轮只软化外移/越障，回线控制响应保持v3 */
#define AV_CAPTURE_SPEED     40.0f /* 首次边缘捕获先压低平移，留出回正时间 */
#define AV_CENTER_SPEED      50.0f
#define AV_LOOKAHEAD_MM      55.0f
#define AV_RETURN_LOOK_MM    65.0f
#define AV_NAV_RATE_MAX      40.0f /* 名义目标角速度上限；非实车已标定上限 */
#define AV_CURVE_MAX          0.0012f /* 只外推缓弯，不把刚驶出的急弯当赛道地图 */
#define AV_CURVE_MATCH        0.00035f
#define AV_SEARCH_LOOK_MM   180.0f /* 低速浅角交线，避免近目标产生75度斜冲 */
#define AV_RETURN_RUN_MM    280.0f /* 平滑回归并在预测线附近回正，不用单点斜冲 */
#define AV_SEARCH_BAND_MM    70.0f
#define AV_SEEK_MAX_MM      750.0f
#define AV_MAX_TRIP_MM     1800.0f
#define AV_SEEK_MAX_MS    13000u
#define AV_TOTAL_MAX_MS   30000u /* 降速20%同步调整总时限；搜索/丢线/里程保护不放宽 */
#define AV_CENTER_HOLD_MS   350u /* 覆盖低速下灰度量化平台，避免120ms短暂中心假稳定 */
#define AV_LINE_LOST_MS     150u
#define AV_LINE_LOST_MM      25.0f /* 名义尺度，只允许短暂续搜 */
#define AV_MM_PER_CNT        0.115f
#define AV_DEG_TO_RAD        0.01745329252f
#define AV_GYRO_MAX_AGE_MS    80u   /* two 40ms read periods; reject at motor write too */
#define AV_BIAS_SAMPLES      16u   /* fresh parked samples only, roughly 0.64s */
#define AV_BIAS_MAX_RAW     655.0f /* +/-5dps raw bound, not gyroscope calibration truth */
#define AV_BIAS_STABLE_RAW  262.0f /* within 2dps of parked running mean */
#define AV_CHECK_WINDOW_MS  100u
#define AV_CHECK_BAD_WINDOWS  3u   /* sustained evidence, not a single noisy frame */
#define AV_CHECK_PWM_DIFF     8
#define AV_CHECK_COUNT_DIFF  12    /* raw quadrature counts, NOT mm or degrees */
#define AV_CHECK_ZERO_DPS     3.0f
#define AV_HANDOFF_SPEED_MAX 85.0f /* nominal only; no scale calibration claimed */

/* Stage 1 ONLY: raw quadrature counts, NOT calibrated mm/turn angles.
 * Burn-time opt-in; no telemetry command can enable this calibration mode.
 * +1 = right (left wheel faster), -1 = left. ALL motion values await field tuning. */
#ifndef AV_STAGE1_ENABLE
#define AV_STAGE1_ENABLE       0
#endif
#ifndef AV_STAGE1_DIRECTION
#define AV_STAGE1_DIRECTION    1
#endif
#define AV_TEST_TRIGGER_CNT 2000u /* average of the two forward wheel counts */
#define AV_TEST_SPEED_CPS     400.0f
#define AV_TEST_SEARCH_CPS    320.0f
#define AV_TEST_CAPTURE_CPS   250.0f
#define AV_TEST_CENTER_CPS    350.0f
#define AV_TEST_TURN            3.0f /* half of L-R PWM difference */
#define AV_TEST_TURN_MAX        4.0f
#define AV_TEST_TURN_SLEW       0.25f /* per 10ms; each wheel <=1 PWM/10ms */
#define AV_TEST_BASE_SLEW       0.75f
#define AV_TEST_WHEEL_MIN       5.0f /* positive command, not proof of wheel movement */
#define AV_TEST_WHEEL_MAX      14.0f
#define AV_TEST_OUT_CNT       700u
#define AV_TEST_FWD_CNT       150u
#define AV_TEST_RETURN_CNT   1400u
#define AV_TEST_CAPTURE_MIN   150u
#define AV_TEST_SEARCH_CNT    800u
#define AV_TEST_TOTAL_CNT    6000u
#define AV_TEST_WHITE_MS       60u
#define AV_TEST_DECEL_MS     1500u
#define AV_TEST_OUT_MS       3000u
#define AV_TEST_FWD_MS       1500u
#define AV_TEST_RETURN_MS    6000u
#define AV_TEST_SEARCH_MS    4000u
#define AV_TEST_TOTAL_MS    20000u
#define AV_TEST_ENCODER_STEP  500  /* implausible single-wheel counts per 10ms */
#define AV_TEST_CHECK_DIFF      4  /* smaller steering still gets v5 health protection */
#define AV_TEST_RATE_MAX       60.0f /* diagnostic stop only, NOT a heading controller */
#define AV_TEST_RATE_HOLD_MS   100u

typedef enum {
    AV_IDLE, AV_OUT, AV_SHIFT, AV_PARALLEL, AV_PASS,
    AV_RETURN, AV_RECOVER, AV_FAULT, AV_SEARCH,
    AV_TEST_DECEL, AV_TEST_OUT, AV_TEST_FWD, AV_TEST_RETURN, AV_TEST_SEARCH
} AvPhase;

static uint8_t s_drive, s_seen[2], s_clear_reads;
static AvPhase s_phase;
static int8_t s_away;
static UsId s_obstacle_side;
static int32_t s_prev_mm[2];
static uint32_t s_us_stamp[2], s_good_us_ms[2];
static uint32_t s_phase_ms, s_start_ms, s_tick_ms, s_cool_until;
static uint32_t s_progress_ms, s_line_ready_ms, s_center_ms;
static int16_t s_enc_l, s_enc_r;
static float s_trip_mm, s_phase_mm, s_x_mm, s_y_mm, s_yaw_deg, s_clear_x;
static float s_gz_bias, s_yaw_rate, s_steer_out;
static float s_line_err, s_line_d;
static uint32_t s_line_ms;
static float s_line_trip;
static uint32_t s_dt_ms;
static int s_pwm_l, s_pwm_r;
static uint32_t s_speed_ms, s_speed_dt, s_search_ms;
static float s_speed_dist, s_speed_mm_s, s_speed_i, s_curve_live, s_curve_entry;
static float s_offset_mm, s_shift_x, s_obstacle_x, s_obstacle_end, s_obstacle_y, s_search_x, s_search_trip;
static float s_pass_y, s_return_offset;
static uint8_t s_plan_valid;
static float s_heading_target;
static uint32_t s_bias_seq, s_gyro_report_ms, s_check_ms;
static uint8_t s_bias_n, s_bias_ready, s_check_bad, s_check_raw_zero;
static int8_t s_check_dir;
static int32_t s_check_diff, s_check_total;
static float s_check_angle;

/* Volatile keeps both branches in the offline ELF; no public runtime setter.
 * s_test_used intentionally survives TRACE/AVOID stop/restart; only Init rearms. */
static volatile uint8_t s_test_enabled = AV_STAGE1_ENABLE;
static volatile int8_t s_test_direction = AV_STAGE1_DIRECTION;
static uint8_t s_test_active, s_test_used, s_test_running, s_test_left_line;
static uint32_t s_test_before2, s_test_trip2, s_test_phase2;
static uint32_t s_test_white_ms, s_test_slow_ms, s_test_search_ms, s_test_search2;
static uint32_t s_test_fast_ms;
static float s_test_base;
static const uint16_t test_limits[] = {
    AV_TEST_DECEL_MS, AV_TEST_OUT_MS, AV_TEST_FWD_MS, AV_TEST_RETURN_MS, AV_TEST_SEARCH_MS
};

static void av_test_search(uint32_t ms);
static void av_test_step(uint32_t ms);

static float av_clip(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t av_gyro(uint32_t ms, LogGyro *g)
{
    Log_GetGyro(g);
    return g->valid && g->seq && (ms - g->stamp_ms) <= AV_GYRO_MAX_AGE_MS;
}

static void av_gyro_report(uint32_t ms)
{
    LogGyro g;
    Log_GetGyro(&g);
    char note[48];
    snprintf(note, sizeof note, "AV G v%u s%u b%u a%lu r%d f%u",
             g.valid, g.status, s_bias_ready, (unsigned long)(ms - g.stamp_ms), g.raw, g.failures);
    Tlm_SendEvent(3, note); /* existing E protocol, raw LSB, age ms; no gz8 ambiguity */
    s_gyro_report_ms = ms;
}

/* 每 50ms 实时计数窗口，补偿测距阻塞；同毫秒追赶不重复估速。 */
static void av_speed_sample(uint32_t ms, float ds)
{
    if (!s_speed_ms) { s_speed_ms = ms; s_speed_dist = 0.0f; return; }
    s_speed_dist += ds;
    uint32_t dt = ms - s_speed_ms;
    if (dt >= 50u) {
        float v = ds >= 0.0f ? s_speed_dist * 1000.0f / (float)dt : 0.0f;
        s_speed_mm_s += av_clip((float)dt / 100.0f, 0.0f, 1.0f) *
                          (av_clip(v, 0.0f, 600.0f) - s_speed_mm_s);
        s_speed_dist = 0.0f; s_speed_ms = ms; s_speed_dt = dt;
    }
}

static int av_speed_base(float target, int cap)
{
    float e = target - s_speed_mm_s;
    if (s_speed_dt) {
        float dt = (float)(s_speed_dt > 100u ? 100u : s_speed_dt) * 0.001f;
        s_speed_i = av_clip(s_speed_i + 0.025f * e * dt, -3.0f, 3.0f);
        s_speed_dt = 0u;
    }
    /* 前馈来自本机日志 PWM20 时约150mm/s，PI 只作有界修正。 */
    float pwm = 4.0f + target * 0.10f + e * 0.025f + s_speed_i;
    return (int)(av_clip(pwm, 6.0f, (float)cap) + 0.5f);
}

static float av_trigger_mm(void)
{
    /* 三次平滑横移所需长度 >= sqrt(6*横移*半径)，另含测距确认/调度迟滞。
     * 0.35s 与误差预算为保守试跑值，不是已测制动能力。 */
    float lateral = AV_BODY_HALF_MM + AV_GAP_MM;
    float lead = sqrtf(6.0f * lateral * AV_MIN_RADIUS_MM) + AV_GAP_MM +
                 AV_RANGE_PAD_MM + 0.35f * s_speed_mm_s;
    return av_clip(lead / 0.8660f, 260.0f, 500.0f); /* 15°安装+名义波束扩展到30° */
}

static float av_line_y(float x)
{
    /* 进入前局部曲率仅用作有限搜索的初值；不是已知赛道地图。 */
    x = av_clip(x, 0.0f, AV_MAX_TRIP_MM + AV_SEARCH_LOOK_MM);
    return 0.5f * s_curve_entry * x * x;
}

/* 模型车身矩形与外扩25mm的名义障碍矩形做四轴 SAT。
 * 可防模型车尾/转弯角切入；不能弥补未知安装偏移、障碍姿态或漏检。 */
static uint8_t av_body_clear(float x, float y, float yaw)
{
    float c = cosf(yaw * AV_DEG_TO_RAD), sn = sinf(yaw * AV_DEG_TO_RAD);
    float half = 0.5f * (AV_BODY_FRONT_MM + AV_BODY_REAR_MM);
    float mid = 0.5f * (AV_BODY_FRONT_MM - AV_BODY_REAR_MM);
    float dx = (s_obstacle_x + s_obstacle_end) * 0.5f - (x + mid * c);
    float dy = s_obstacle_y - (y + mid * sn);
    float ox = (s_obstacle_end - s_obstacle_x) * 0.5f + AV_GAP_MM;
    float oy = AV_OBSTACLE_WIDTH_MM * 0.5f + AV_GAP_MM;
    float ac = fabsf(c), as = fabsf(sn);
    if (fabsf(dx) > ox + half * ac + AV_BODY_HALF_MM * as) { return 1u; }
    if (fabsf(dy) > oy + half * as + AV_BODY_HALF_MM * ac) { return 1u; }
    if (fabsf(dx * c + dy * sn) > half + ox * ac + oy * as) { return 1u; }
    if (fabsf(-dx * sn + dy * c) > AV_BODY_HALF_MM + ox * as + oy * ac) { return 1u; }
    return 0u;
}

static int av_count(uint8_t bits)
{
    int n = 0;
    for (int i = 0; i < 8; i++) { n += (bits >> i) & 1u; }
    return n;
}

static void av_reset(void)
{
    s_test_active = 0u; /* never clear s_test_used here */
    s_phase = AV_IDLE;
    s_seen[0] = s_seen[1] = s_clear_reads = 0u;
    s_prev_mm[0] = s_prev_mm[1] = 0;
    for (int i = 0; i < 2; i++) {
        s_us_stamp[i] = Us_Get((UsId)i)->stamp_ms;
        s_good_us_ms[i] = 0u;
    }
    s_line_ready_ms = s_center_ms = 0u;
    s_line_ms = 0u;
    s_line_err = s_line_d = s_line_trip = 0.0f;
    s_plan_valid = 0u;
    s_search_ms = 0u;
    s_speed_i = 0.0f;
    s_check_ms = 0u; s_check_dir = 0; s_check_bad = 0u;
    s_check_raw_zero = 1u;
    s_check_diff = s_check_total = 0; s_check_angle = 0.0f;
}

void Avoid_SetDrive(uint8_t on)
{
    if (!on && s_phase != AV_IDLE && Trace_GetDrive()) { Mo_Stop(); }
    s_drive = on ? 1u : 0u;
    s_cool_until = 0u;
    av_reset();
}

uint8_t Avoid_GetDrive(void) { return s_drive; }
uint8_t Avoid_Mode(void) { return s_phase != AV_IDLE; }

void Avoid_Init(void)
{
    s_test_used = s_test_running = 0u;
    s_test_before2 = 0u;
    s_drive = 0u;
    s_cool_until = 0u;
    s_gz_bias = 0.0f;
    s_bias_seq = 0u; s_bias_n = s_bias_ready = 0u;
    s_gyro_report_ms = 0u;
    s_speed_ms = s_speed_dt = 0u;
    s_speed_mm_s = s_speed_dist = s_curve_live = 0.0f;
    av_reset();
    Tlm_SendEvent(97, "AVOID build a02-s1-count-trial");
    char test[48];
    snprintf(test, sizeof test, "A02 TEST en%u dir%d trigger%u counts",
             s_test_enabled, s_test_direction, AV_TEST_TRIGGER_CNT);
    Tlm_SendEvent(97, test);
}

static void av_phase(AvPhase next, uint32_t ms, const char *note)
{
    s_phase = next;
    s_phase_ms = ms;
    s_phase_mm = s_trip_mm;
    s_center_ms = 0u;
    if (note) { Tlm_SendEvent(3, note); }
}

static void av_fault(const char *note)
{
    AvPhase old = s_phase;
    /* Brake BEFORE serial/event formatting: logging must never prolong a turn. */
    s_phase = AV_FAULT;
    Mo_Brake();
    s_pwm_l = s_pwm_r = 0;
    if (old != AV_FAULT) {
        Tlm_SendEvent(88, note);
        char pose[64];
        snprintf(pose, sizeof pose, "AVOID pose p=%d x=%ld y=%ld yaw=%ld",
                 (int)old, (long)s_x_mm, (long)s_y_mm, (long)s_yaw_deg);
        Tlm_SendEvent(88, pose); /* 名义 mm / 度，便于区分距离与回线故障。 */
        av_gyro_report(HAL_GetTick());
    }
}

static void av_drive(int base, float steer)
{
    if (s_phase == AV_FAULT) { return; }
    LogGyro g;
    if (!av_gyro(HAL_GetTick(), &g)) { av_fault("AVOID gyro invalid/stale"); return; }
    if (s_test_active) {
        /* Common speed AND differential slew. Seeded from A01 at DECEL entry;
         * normal stage commands are 5..14%, never reverse or pivot in place. */
        float step = (float)s_dt_ms / 10.0f;
        float wanted_base = (float)base;
        if (s_phase != AV_TEST_DECEL) {
            wanted_base = av_clip(wanted_base, AV_TEST_WHEEL_MIN + fabsf(s_steer_out),
                                 AV_TEST_WHEEL_MAX - fabsf(s_steer_out));
        }
        s_test_base += av_clip(wanted_base - s_test_base,
                              -AV_TEST_BASE_SLEW * step, AV_TEST_BASE_SLEW * step);
        steer = av_clip(steer, -AV_TEST_TURN_MAX, AV_TEST_TURN_MAX);
        float room = fminf(s_test_base - AV_TEST_WHEEL_MIN,
                          AV_TEST_WHEEL_MAX - s_test_base);
        if (s_phase != AV_TEST_DECEL) { steer = av_clip(steer, -fmaxf(room, 0.0f), fmaxf(room, 0.0f)); }
        s_steer_out += av_clip(steer - s_steer_out,
                              -AV_TEST_TURN_SLEW * step, AV_TEST_TURN_SLEW * step);
        s_pwm_l = (int)(s_test_base + s_steer_out + 0.5f);
        s_pwm_r = (int)(s_test_base - s_steer_out + 0.5f);
        /* Initial DECEL may start above the stage cap: ramp, don't snap to it. */
        Mo_Set(s_pwm_l > 0 ? s_pwm_l : 0, s_pwm_r > 0 ? s_pwm_r : 0);
        return; /* no yaw, coordinates, route predictor or footprint model */
    }
    if (steer > (float)base) { steer = (float)base; }
    if (steer < -(float)base) { steer = -(float)base; }
    /* 外移/越障每10ms只改变1档差速；回线/边缘纠偏保留原响应。 */
    float slew = (s_phase >= AV_OUT && s_phase <= AV_PASS ? 1.0f : 3.0f) *
                 (float)s_dt_ms / 10.0f;
    float change = steer - s_steer_out;
    if (change > slew) { change = slew; }
    if (change < -slew) { change = -slew; }
    s_steer_out += change;
    if (s_steer_out > (float)base) { s_steer_out = (float)base; }
    if (s_steer_out < -(float)base) { s_steer_out = -(float)base; }
    int turn = (int)(s_steer_out + (s_steer_out >= 0.0f ? 0.5f : -0.5f));
    if (s_plan_valid) {
        float x = s_x_mm, y = s_y_mm, yaw = s_yaw_deg;
        float v = 1.15f * fmaxf(s_speed_mm_s, (float)base * 7.5f);
        /* 检查当前外廓和210ms前向预测；转速是名义模型，非精标执行保证。 */
        float rate = av_clip((float)turn * 7.0f, -100.0f, 100.0f);
        for (int i = 0; i < 4; i++) {
            if (!av_body_clear(x, y, yaw)) { av_fault("AVOID footprint guard"); return; }
            float turn_step = rate * 0.07f;
            if (s_phase != AV_RECOVER && turn_step * (s_heading_target - yaw) >= 0.0f &&
                fabsf(turn_step) > fabsf(s_heading_target - yaw)) {
                turn_step = s_heading_target - yaw; rate = 0.0f;
            }
            float mid = (yaw + turn_step * 0.5f) * AV_DEG_TO_RAD;
            x += v * 0.07f * cosf(mid); y += v * 0.07f * sinf(mid);
            yaw += turn_step;
        }
    }
    s_pwm_l = base + turn;
    s_pwm_r = base - turn;
    Mo_Set(s_pwm_l, s_pwm_r);
}

static void av_heading(float target, int base)
{
    /* 正舵量 -> 右转；实车日志正 gz -> 左转，已在 av_motion 统一符号。 */
    s_heading_target = target;
    if (s_phase == AV_RETURN || s_phase == AV_SEARCH) {
        av_drive(base, 0.8f * (target - s_yaw_deg) - 0.10f * s_yaw_rate);
        return;
    }
    float wanted = av_clip(1.5f * (target - s_yaw_deg), -AV_NAV_RATE_MAX, AV_NAV_RATE_MAX);
    /* 前馈/角速度反馈有界，避免角度误差直接饱和到单轮停转。
     * 7deg/s/PWM仍是名义响应；真正角速度由同方向陀螺反馈修正。 */
    av_drive(base, wanted / 7.0f + 0.12f * (wanted - s_yaw_rate));
}

static void av_nav(float x, float y, float speed, int cap)
{
    float angle = atan2f(y - s_y_mm, x - s_x_mm) / AV_DEG_TO_RAD;
    angle = av_clip(angle, -75.0f, 75.0f); /* 不倒车、不无限转圈找线 */
    av_heading(angle, av_speed_base(speed, cap));
}

static void av_offset_path(void)
{
    float gx = s_x_mm + AV_LOOKAHEAD_MM;
    float t = av_clip(gx / s_shift_x, 0.0f, 1.0f);
    /* 越障时沿障碍近侧边的平行线，不沿弯曲黑线向障碍侧收拢。 */
    float gy = s_pass_y * t * t * (3.0f - 2.0f * t);
    av_nav(gx, gy, AV_ROUTE_SPEED, 18);
}

static void av_emit(uint32_t ms, uint8_t bits, int count)
{
    int gray[8], sum = 0;
    for (int i = 0; i < 8; i++) {
        gray[i] = (bits >> i) & 1u;
        if (gray[i]) { sum += i; }
    }
    float err = (count > 0 && count < 5) ? (float)sum / (float)count - 3.5f : 0.0f;
    int state = (s_phase == AV_RETURN || s_phase == AV_RECOVER || s_phase == AV_SEARCH ||
                 s_phase == AV_TEST_RETURN || s_phase == AV_TEST_SEARCH) ? 3 : 2;
    Log_Note(bits, err, s_pwm_l, s_pwm_r, state);
    /* Log_Note can perform this cycle's MPU transaction. If it just failed,
     * brake now, not after another turn/serial frame. Fault emission never
     * recurses through this check. The stored T records the preceding command. */
    LogGyro g;
    if (s_phase != AV_IDLE && s_phase != AV_FAULT && !av_gyro(HAL_GetTick(), &g)) {
        av_fault("AVOID gyro invalid/stale");
    }
    Tlm_SendT(ms, gray, err, s_pwm_l, s_pwm_r, state);
}

/* 返回 -1 表示紧急刹停，1 表示确认近障；只累计真正的新测量。 */
static int av_scan(uint32_t ms, uint8_t ready, int *side, int32_t *range)
{
    int chosen = -1;
    int32_t nearest = 100000;
    for (int i = 0; i < 2; i++) {
        const UsDev *u = Us_Get((UsId)i);
        if (!u->stamp_ms || u->stamp_ms == s_us_stamp[i]) { continue; }
        s_us_stamp[i] = u->stamp_ms;
        int32_t mm = u->mm;
        if ((ms - u->stamp_ms) > AV_US_FRESH_MS) { s_seen[i] = 0u; continue; }
        if (mm > 0 && mm <= AV_STOP_MM) { return -1; }
        if (s_phase != AV_IDLE && i == (int)s_obstacle_side) {
            if (mm > (int32_t)av_trigger_mm() + 100) {
                if (s_clear_reads < 2u) { s_clear_reads++; }
            } else {
                s_clear_reads = 0u;
            }
        }
        if (!ready || mm <= 0 || mm > AV_CONFIRM_MM) {
            s_seen[i] = 0u;
            continue;
        }
        if (s_seen[i] && (ms - s_good_us_ms[i]) <= AV_PAIR_MAX_MS &&
            mm <= s_prev_mm[i] + 20) {
            if (s_seen[i] < 3u) { s_seen[i]++; }
        } else {
            s_seen[i] = 1u;
        }
        s_good_us_ms[i] = ms;
        s_prev_mm[i] = mm;
        if (s_seen[i] >= 2u && (float)mm <= av_trigger_mm() && mm < nearest) {
            chosen = i;
            nearest = mm;
        }
    }
    if (chosen < 0) { return 0; }
    *side = chosen;
    *range = nearest;
    return 1;
}

static void av_motion(uint32_t ms)
{
    int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
    int32_t dl = (int16_t)(l - s_enc_l), dr = (int16_t)(r - s_enc_r);
    s_enc_l = l; s_enc_r = r;
    uint32_t dt = ms - s_tick_ms;
    s_tick_ms = ms;
    s_dt_ms = dt < 40u ? dt : 40u;
    if (dt > 80u) { av_fault("AVOID control delayed"); return; }
    LogGyro g;
    if (!av_gyro(ms, &g)) { av_fault("AVOID gyro invalid/stale"); return; }
    /* 方向积分保留正负；原先累加绝对 gz 会把反向摆动也算成转角。 */
    s_yaw_rate = -((float)g.raw - s_gz_bias) / 131.0f;
    if (s_test_active) {
        if (fabsf(s_yaw_rate) > AV_TEST_RATE_MAX) {
            if (!s_test_fast_ms) { s_test_fast_ms = ms; }
            if (ms - s_test_fast_ms >= AV_TEST_RATE_HOLD_MS) {
                av_fault("A02 TEST rotation too fast"); return;
            }
        } else { s_test_fast_ms = 0u; }
    }
    /* Sign/turn activity only. ENC_SIGN_L=+1, ENC_SIGN_R=-1 means forward
     * positive on both wheels, so dl-dr and our -gz right yaw must agree.
     * No wheelbase or uncalibrated count->deg conversion enters control. */
    int pwm_diff = s_pwm_l - s_pwm_r;
    int check_diff = s_test_active ? AV_TEST_CHECK_DIFF : AV_CHECK_PWM_DIFF;
    int8_t dir = pwm_diff >= check_diff ? 1 : pwm_diff <= -check_diff ? -1 : 0;
    if (!dir || dir != s_check_dir) {
        s_check_ms = ms; s_check_dir = dir; s_check_bad = 0u;
        s_check_raw_zero = g.raw == 0;
        s_check_diff = s_check_total = 0; s_check_angle = 0.0f;
    } else {
        s_check_diff += dl - dr;
        s_check_total += (dl < 0 ? -dl : dl) + (dr < 0 ? -dr : dr);
        s_check_angle += s_yaw_rate * (float)dt * 0.001f;
        if (g.raw) { s_check_raw_zero = 0u; }
        uint32_t window = ms - s_check_ms;
        if (window >= AV_CHECK_WINDOW_MS) {
            int32_t diff = s_check_diff < 0 ? -s_check_diff : s_check_diff;
            float rate = s_check_angle * 1000.0f / (float)window;
            uint8_t turn = diff >= AV_CHECK_COUNT_DIFF && diff * 4 >= s_check_total;
            /* A dead raw-zero signal minus a previously nonzero bias must not
             * masquerade as a real 3..5dps turn. Require whole-window raw zero,
             * strong wheel evidence and the same 3-window debounce. */
            uint8_t bad = turn && (s_check_raw_zero || fabsf(rate) < AV_CHECK_ZERO_DPS ||
                          rate * (float)s_check_diff < 0.0f);
            s_check_bad = bad ? s_check_bad + 1u : 0u;
            s_check_ms = ms; s_check_diff = s_check_total = 0; s_check_angle = 0.0f;
            s_check_raw_zero = 1u;
            if (s_check_bad >= AV_CHECK_BAD_WINDOWS) {
                av_fault("AVOID gyro/wheel mismatch"); return;
            }
        }
    }
    if (s_test_active) {
        int32_t limit = AV_TEST_ENCODER_STEP * (int32_t)(dt + 10u) / 10;
        if (dl < -2 || dr < -2 || dl > limit || dr > limit) {
            av_fault("A02 TEST encoder direction/jump"); return;
        }
        if (dl > 0 && dr > 0) { s_progress_ms = ms; } /* neither wheel may stall */
        if (dl + dr > 0) { s_test_trip2 += (uint32_t)(dl + dr); }
        float ds = (float)(dl + dr) * 0.5f * AV_MM_PER_CNT;
        av_speed_sample(ms, ds);
        s_trip_mm += ds > 0.0f ? ds : 0.0f; /* compatibility: not route progress */
        return; /* gyro is health evidence, not an integrated path reference */
    }
    float yaw_mid = s_yaw_deg + s_yaw_rate * (float)dt * 0.0005f;
    s_yaw_deg += s_yaw_rate * (float)dt * 0.001f;
    float ds = (float)(dl + dr) * 0.5f * AV_MM_PER_CNT;
    if (ds >= 0.0f && ds < 60.0f) {
        av_speed_sample(ms, ds);
        s_trip_mm += ds;
        s_x_mm += ds * cosf(yaw_mid * AV_DEG_TO_RAD);
        s_y_mm += ds * sinf(yaw_mid * AV_DEG_TO_RAD);
        if (dl + dr > 0) { s_progress_ms = ms; }
    }
}

static void av_release(uint32_t ms, uint8_t bits, int count)
{
    av_emit(ms, bits, count); /* last fresh read before releasing control */
    if (s_phase == AV_FAULT) { return; }
    /* Entry was allowed only from A01 state=1 (not precision stopping).
     * Existing FinReset clears stale I/D/steer and common PWM ramp, but leaves
     * finish arming, odometry and stop distance intact. No A01 source change.
     * Next A01 tick ramps from low PWM instead of reviving pre-obstacle steer. */
    Trace_FinReset();
    LogGyro g;
    if (!av_gyro(HAL_GetTick(), &g)) { av_fault("AVOID gyro invalid/stale"); return; }
    Tlm_SendEvent(4, s_test_active ? "A02 TEST local line stable; A01" : "AVOID line recovered");
    s_cool_until = ms + 1000u;
    av_reset(); /* 保持最后前进命令，由下一拍 A01 接手。 */
}

/* 回线跟随局部黑线，不以“接管时朝向=0”约束弯道上的新切线。
 * 允许 4 路连续黑（斜交线），拒绝宽黑/离散黑点作为交接依据。 */
static uint8_t av_line(uint8_t bits, int count)
{
    if (count < 1 || count > 4) { return 0u; }
    while (!(bits & 1u)) { bits >>= 1; }
    return (bits & (uint8_t)(bits + 1u)) == 0u;
}

static void av_recover(uint32_t ms, uint8_t bits, int count)
{
    if ((s_trip_mm - s_phase_mm) > 250.0f || (ms - s_phase_ms) > 4500u) {
        av_fault("AVOID centering failed");
        return;
    }
    if (av_line(bits, count)) {
        int sum = 0;
        for (int i = 0; i < 8; i++) { if (bits & (1u << i)) { sum += i; } }
        float err = (float)sum / (float)count - 3.5f;
        uint32_t dt = ms - s_line_ms;
        if (s_line_ms && dt && dt <= 50u) {
            float rate = (err - s_line_err) * 1000.0f / (float)dt;
            s_line_d += 0.05f * (rate - s_line_d);
        } else if (!s_line_ms || dt > 50u) {
            s_line_d = 0.0f; s_center_ms = 0u;
        } /* 调度追赶产生同毫秒样本时，不累加也不打断稳定窗口。 */
        s_line_err = err;
        s_line_ms = ms;
        s_line_trip = s_trip_mm;
        float d = av_clip(1.20f * s_line_d, -12.0f, 12.0f);
        /* 边缘见线先慢下来捕获；绝对偏航项会把弯道的新方向拉回旧直线。 */
        float capture = s_test_active ? AV_TEST_CAPTURE_CPS * AV_MM_PER_CNT : AV_CAPTURE_SPEED;
        float center = s_test_active ? AV_TEST_CENTER_CPS * AV_MM_PER_CNT : AV_CENTER_SPEED;
        int base = av_speed_base(fabsf(err) > 1.0f ? capture : center, fabsf(err) > 1.0f ? 9 : 10);
        av_drive(base, 3.0f * err + d - (s_test_active ? 0.0f : 0.12f * s_yaw_rate));
        if (s_phase == AV_FAULT) { return; }
        if (fabsf(err) <= 1.0f && (bits & 0x18u) &&
            fabsf(s_yaw_rate) <= 30.0f && fabsf(s_line_d) <= 1.5f &&
            s_speed_mm_s <= AV_HANDOFF_SPEED_MAX) {
            if (s_test_active && (abs(s_pwm_l - s_pwm_r) > 2 ||
                s_speed_mm_s > AV_TEST_CENTER_CPS * AV_MM_PER_CNT + 20.0f)) {
                s_center_ms = 0u; return;
            }
            if (!s_center_ms) { s_center_ms = ms; }
            if ((ms - s_center_ms) >= AV_CENTER_HOLD_MS) {
                av_release(ms, bits, count);
                return;
            }
        } else { s_center_ms = 0u; }
    } else {
        s_center_ms = 0u;
        s_line_d = 0.0f;
        /* 线从哪侧滑出，就向哪侧低速续搜；不反转到固定的旧偏航目标。
         * 全白/宽黑/离散黑点不证明回线成功，也不能无限盲开。 */
        if (!s_line_ms || (ms - s_line_ms) > AV_LINE_LOST_MS ||
            (s_trip_mm - s_line_trip) > AV_LINE_LOST_MM) {
            if (s_test_active) { av_test_search(ms); return; }
            av_fault("AVOID local line lost");
            return;
        }
        float dir = s_line_err < -0.25f ? -1.0f :
                    s_line_err > 0.25f ? 1.0f : -(float)s_away;
        float damping = 0.04f * s_yaw_rate;
        if (damping > 4.0f) { damping = 4.0f; }
        if (damping < -4.0f) { damping = -4.0f; }
        int base = av_speed_base(s_test_active ? AV_TEST_CAPTURE_CPS * AV_MM_PER_CNT : AV_CAPTURE_SPEED, 8);
        av_drive(base, s_test_active ? dir * AV_TEST_TURN : dir * (float)base - damping);
    }
}

static void av_test_phase(AvPhase phase, uint32_t ms)
{
    s_test_phase2 = s_test_trip2;
    av_phase(phase, ms, NULL);
    char note[48];
    snprintf(note, sizeof note, "A02 TEST p%u c%lu dir%d", (unsigned)phase,
             (unsigned long)(s_test_trip2 / 2u), s_away);
    Tlm_SendEvent(3, note);
}

static void av_test_search(uint32_t ms)
{
    /* One search budget, including all repeated short captures; never restart it. */
    if (!s_test_search_ms) { s_test_search_ms = ms; s_test_search2 = s_test_trip2; }
    s_line_ms = 0u; s_line_d = 0.0f;
    av_test_phase(AV_TEST_SEARCH, ms);
}

static void av_test_step(uint32_t ms)
{
    /* Stage 1 has no obstacle route. Only retain the unchanged 30mm emergency. */
    for (int i = 0; i < 2; i++) {
        const UsDev *u = Us_Get((UsId)i);
        if (u->stamp_ms && (ms - u->stamp_ms) <= AV_US_FRESH_MS &&
            u->mm > 0 && u->mm <= AV_STOP_MM) {
            av_fault("AVOID emergency <=30mm"); return;
        }
    }
    if (!s_test_active) {
        int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
        int32_t dl = (int16_t)(l - s_enc_l), dr = (int16_t)(r - s_enc_r);
        s_enc_l = l; s_enc_r = r;
        if (!s_test_running) { s_test_running = 1u; s_test_before2 = 0u; }
        if (dl < -2 || dr < -2 || dl > AV_TEST_ENCODER_STEP || dr > AV_TEST_ENCODER_STEP) {
            s_line_ready_ms = 0u; return; /* bad pre-trigger data cannot arm */
        }
        if (dl + dr > 0) { s_test_before2 += (uint32_t)(dl + dr); }
        av_speed_sample(ms, (float)(dl + dr) * 0.5f * AV_MM_PER_CNT);
        if (s_test_used) { return; } /* no legacy obstacle trigger in test mode */
        const uint8_t *live = Log_LiveFrame();
        int n = av_count(live[0]);
        if (!Trace_Online() || live[4] != 1u || !av_line(live[0], n) ||
            n > 3 || !(live[0] & 0x18u) || (int8_t)live[1] < -10 || (int8_t)live[1] > 10) {
            s_line_ready_ms = 0u;
        } else if (!s_line_ready_ms) { s_line_ready_ms = ms; }
        if (s_test_before2 < 2u * AV_TEST_TRIGGER_CNT || !s_line_ready_ms ||
            ms - s_line_ready_ms < AV_LINE_READY_MS) { return; }
        s_test_used = 1u; /* consume BEFORE any possible failed entry */
        LogGyro g;
        if (!av_gyro(ms, &g) || !s_bias_ready) { av_fault("AVOID gyro not ready"); return; }
        if (s_test_direction != 1 && s_test_direction != -1) { av_fault("A02 TEST bad direction"); return; }
        s_test_active = 1u; s_away = s_test_direction;
        s_test_trip2 = s_test_phase2 = 0u;
        s_test_white_ms = s_test_slow_ms = s_test_search_ms = s_test_fast_ms = 0u;
        s_test_left_line = s_plan_valid = 0u;
        s_trip_mm = s_x_mm = s_y_mm = s_yaw_deg = 0.0f;
        s_speed_i = 0.0f;
        s_pwm_l = (int8_t)live[2]; s_pwm_r = (int8_t)live[3];
        if (s_pwm_l <= 0 || s_pwm_r <= 0) { av_fault("A02 TEST entry PWM unsafe"); return; }
        s_test_base = (float)(s_pwm_l + s_pwm_r) * 0.5f;
        s_steer_out = (float)(s_pwm_l - s_pwm_r) * 0.5f;
        s_check_ms = 0u; s_check_dir = 0; s_check_bad = 0u;
        s_tick_ms = s_progress_ms = s_start_ms = ms; s_dt_ms = 0u;
        av_gyro_report(ms);
        av_test_phase(AV_TEST_DECEL, ms);
        return;
    }

    av_motion(ms);
    if (s_phase == AV_FAULT) { return; }
    uint8_t raw;
    if (!Gr_ReadDigital(&raw)) { av_fault("AVOID gray read failed"); return; }
    uint8_t bits = (uint8_t)~raw;
    int n = av_count(bits);
    if (ms - s_start_ms > AV_TEST_TOTAL_MS || s_test_trip2 > 2u * AV_TEST_TOTAL_CNT ||
        ms - s_progress_ms > 500u) { av_fault("A02 TEST timeout/no progress"); return; }
    if (s_test_search_ms && (ms - s_test_search_ms > AV_TEST_SEARCH_MS ||
        s_test_trip2 - s_test_search2 > 2u * AV_TEST_SEARCH_CNT)) {
        av_fault("A02 TEST line not found"); return;
    }
    if (s_phase == AV_TEST_OUT || s_phase == AV_TEST_FWD) {
        if (!bits) {
            if (!s_test_white_ms) { s_test_white_ms = ms; }
            if (ms - s_test_white_ms >= AV_TEST_WHITE_MS) { s_test_left_line = 1u; }
        } else { s_test_white_ms = 0u; }
    }
    uint32_t progress = (s_test_trip2 - s_test_phase2) / 2u;
    uint32_t elapsed = ms - s_phase_ms;
    if (s_phase >= AV_TEST_DECEL && s_phase <= AV_TEST_SEARCH &&
        elapsed > test_limits[s_phase - AV_TEST_DECEL]) {
        av_fault("A02 TEST phase timeout"); return;
    }
    int base = 0;
    if (s_phase >= AV_TEST_DECEL && s_phase <= AV_TEST_FWD) {
        base = av_speed_base(AV_TEST_SPEED_CPS * AV_MM_PER_CNT, 10);
    }
    switch (s_phase) {
    case AV_TEST_DECEL:
        if (!av_line(bits, n)) { av_fault("A02 TEST lost line before OUT"); break; }
        av_drive(base, (float)(int8_t)Log_LiveFrame()[1] * 0.1f);
        if (s_phase == AV_FAULT) { break; }
        if (s_speed_mm_s <= AV_TEST_SPEED_CPS * AV_MM_PER_CNT + 20.0f &&
            s_test_base >= AV_TEST_WHEEL_MIN + fabsf(s_steer_out) &&
            s_test_base <= AV_TEST_WHEEL_MAX - fabsf(s_steer_out) &&
            fabsf(s_steer_out) <= AV_TEST_TURN_MAX) {
            if (!s_test_slow_ms) { s_test_slow_ms = ms; }
            if (ms - s_test_slow_ms >= 150u) { av_test_phase(AV_TEST_OUT, ms); }
        } else { s_test_slow_ms = 0u; }
        if (s_phase == AV_TEST_DECEL && progress > 800u) {
            av_fault("A02 TEST decel failed");
        }
        break;
    case AV_TEST_OUT:
    case AV_TEST_FWD:
        av_drive(base, s_phase == AV_TEST_OUT ? (float)s_away * AV_TEST_TURN : 0.0f);
        if (s_phase == AV_FAULT) { break; }
        if (s_phase == AV_TEST_OUT) {
            if (progress >= AV_TEST_OUT_CNT) { av_test_phase(AV_TEST_FWD, ms); }
        } else {
            if (progress >= AV_TEST_FWD_CNT) {
                if (!s_test_left_line) { av_fault("A02 TEST did not leave line"); }
                else { av_test_phase(AV_TEST_RETURN, ms); }
            }
        }
        break;
    case AV_TEST_RETURN:
    case AV_TEST_SEARCH:
        if (s_test_left_line && av_line(bits, n) &&
            (s_phase == AV_TEST_SEARCH || progress >= AV_TEST_CAPTURE_MIN)) {
            s_line_ms = 0u; s_line_d = 0.0f;
            av_test_phase(AV_RECOVER, ms);
            av_recover(ms, bits, n);
        } else {
            av_drive(av_speed_base(AV_TEST_SEARCH_CPS * AV_MM_PER_CNT, 10),
                     n >= 5 ? 0.0f : -(float)s_away * AV_TEST_TURN);
            if (s_phase == AV_TEST_RETURN) {
                if (progress >= AV_TEST_RETURN_CNT) { av_test_search(ms); }
            }
        }
        break;
    case AV_RECOVER:
        av_recover(ms, bits, n);
        break;
    default:
        av_fault("A02 TEST invalid phase"); break;
    }
    if (s_phase != AV_IDLE) { av_emit(ms, bits, n); }
}

void Avoid_Step(void)
{
    uint32_t ms = HAL_GetTick();
    if (!Trace_GetDrive()) {
        s_test_running = 0u;
        /* 仅在编码器不动的待机状态估计零偏，不把正常弯道当零偏。 */
        int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
        LogGyro g;
        uint8_t good = av_gyro(ms, &g);
        if (!good || l != s_enc_l || r != s_enc_r) {
            s_bias_ready = s_bias_n = 0u;
        } else if (g.seq != s_bias_seq) {
            s_bias_seq = g.seq;
            if (fabsf((float)g.raw) > AV_BIAS_MAX_RAW ||
                (s_bias_n && fabsf((float)g.raw - s_gz_bias) > AV_BIAS_STABLE_RAW)) {
                s_bias_ready = s_bias_n = 0u;
            } else {
                if (s_bias_n < AV_BIAS_SAMPLES) { s_bias_n++; }
                s_gz_bias += ((float)g.raw - s_gz_bias) / (float)s_bias_n;
                s_bias_ready = s_bias_n >= AV_BIAS_SAMPLES;
            }
        }
        if ((ms - s_gyro_report_ms) >= 1000u) { av_gyro_report(ms); }
        s_enc_l = l; s_enc_r = r;
        s_speed_ms = ms; s_speed_dist = s_speed_mm_s = 0.0f;
        s_speed_dt = 0u; s_curve_live = 0.0f;
        if (s_phase != AV_IDLE) { av_reset(); }
        return;
    }
    if (!s_drive) { return; }
    if (s_phase == AV_FAULT) {
        Mo_Brake(); s_pwm_l = s_pwm_r = 0;
        uint8_t bits = Log_LiveFrame()[0]; /* cached gray, not invented all-white */
        av_emit(ms, bits, av_count(bits)); /* keep polling, never re-enter control */
        return;
    }

    if (s_test_enabled) { av_test_step(ms); return; }

    if (s_phase == AV_IDLE) {
        int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
        float ds = ((float)(int16_t)(l - s_enc_l) + (float)(int16_t)(r - s_enc_r)) *
                   0.5f * AV_MM_PER_CNT;
        s_enc_l = l; s_enc_r = r;
        av_speed_sample(ms, av_clip(ds, 0.0f, 60.0f));
        /* 日志公开的 8B live frame：byte0=黑位图，byte4=state。
         * 本函数在 A01 后调用，复用本拍灰度，避免再次访问 I2C。 */
        const uint8_t *live = Log_LiveFrame();
        int count = av_count(live[0]);
        if (!Trace_Online() || live[4] != 1u || count < 1 || count > 3) {
            s_line_ready_ms = 0u;
        } else if (!s_line_ready_ms) {
            s_line_ready_ms = ms;
        }
        LogGyro g;
        uint8_t gyro_ok = av_gyro(ms, &g);
        if (!gyro_ok) { s_bias_ready = s_bias_n = 0u; }
        if (gyro_ok && live[4] == 1u && count >= 1 && count <= 3 && s_speed_mm_s >= 50.0f) {
            float rate = -((float)g.raw - s_gz_bias) / 131.0f;
            float curve = av_clip(rate * AV_DEG_TO_RAD / s_speed_mm_s, -0.002f, 0.002f);
            s_curve_live += 0.05f * (curve - s_curve_live);
        }
        uint8_t ready = s_line_ready_ms && (ms - s_line_ready_ms) >= AV_LINE_READY_MS &&
                        (int32_t)(ms - s_cool_until) >= 0;
        int side = 0;
        int32_t mm = 0;
        int found = av_scan(ms, ready, &side, &mm);
        if (found < 0) { av_fault("AVOID emergency <=30mm"); return; }
        if (!found) { return; }
        if (!gyro_ok || !s_bias_ready) { av_fault("AVOID gyro not ready"); return; }
        av_gyro_report(ms);
        char bias[40];
        snprintf(bias, sizeof bias, "AVOID bias=%ld n=%u", (long)s_gz_bias, s_bias_n);
        Tlm_SendEvent(3, bias);
        s_obstacle_side = (UsId)side;
        s_away = side == US_L ? 1 : -1;
        float entry_rate = -((float)g.raw - s_gz_bias) / 131.0f;
        float entry_curve = s_speed_mm_s >= 50.0f ?
                            entry_rate * AV_DEG_TO_RAD / s_speed_mm_s : 0.0f;
        /* 15:44-3先急弯、再直行：滤波器还记着旧曲率，实际角速度已归零。
         * 仅保留与当前实测一致的缓弯；不确定时回到有界局部搜线。 */
        s_curve_entry = fabsf(entry_curve) <= AV_CURVE_MAX &&
                        fabsf(entry_curve - s_curve_live) <= AV_CURVE_MATCH ?
                        av_clip(s_curve_live, -AV_CURVE_MAX, AV_CURVE_MAX) : 0.0f;
        s_offset_mm = AV_BODY_HALF_MM + AV_GAP_MM + AV_TRACK_PAD_MM;
        /* 接近用近界、越障用远界，不能把同一低估距离又用于过早回转。 */
        s_obstacle_x = AV_PROBE_FRONT_MM + (float)mm * 0.8660f - AV_RANGE_PAD_MM;
        s_obstacle_end = AV_PROBE_FRONT_MM + (float)mm + AV_MOUNT_PAD_MM +
                         AV_RANGE_PAD_MM + AV_OBSTACLE_LEN_MM;
        float near_y = av_line_y(s_obstacle_x);
        float far_y = av_line_y(s_obstacle_end - AV_OBSTACLE_LEN_MM);
        /* 相对通过一致性检查的局部线估计留足车宽；旧急弯不再缩小直行外移量。
         * 不能一律把缓弯当直线，否则会横移过远并使低速回线距离暴增。 */
        float edge_y = s_away > 0 ? fmaxf(near_y, far_y) : fminf(near_y, far_y);
        s_obstacle_y = edge_y - (float)s_away * AV_OBSTACLE_WIDTH_MM * 0.5f;
        s_pass_y = edge_y + (float)s_away * s_offset_mm;
        s_shift_x = s_obstacle_x - AV_BODY_FRONT_MM - AV_GAP_MM;
        /* 车尾转向时比直行投影长，先越过整个车尾扫掠半径再向内搜线。 */
        s_clear_x = s_obstacle_end + AV_GAP_MM +
                    sqrtf(AV_BODY_REAR_MM * AV_BODY_REAR_MM + AV_BODY_HALF_MM * AV_BODY_HALF_MM);
        s_trip_mm = s_x_mm = s_y_mm = s_yaw_deg = 0.0f;
        s_yaw_rate = 0.0f;
        s_speed_i = 0.0f; s_plan_valid = 1u;
        s_steer_out = ((float)(int8_t)live[2] - (float)(int8_t)live[3]) * 0.5f;
        s_enc_l = En_Count(ENC_L); s_enc_r = En_Count(ENC_R);
        s_tick_ms = s_progress_ms = s_start_ms = ms;
        s_dt_ms = 10u;
        char note[48];
        snprintf(note, sizeof note, "AVOID %c near %ldmm", side == US_L ? 'L' : 'R', (long)mm);
        av_phase(AV_OUT, ms, note);
        char plan[80];
        snprintf(plan, sizeof plan, "AVOID plan v=%ld lead=%ld offset=%ld clear=%ld",
                 (long)s_speed_mm_s, (long)av_trigger_mm(), (long)s_offset_mm, (long)s_clear_x);
        Tlm_SendEvent(3, plan);
        snprintf(plan, sizeof plan, "AVOID path y=%ld curve=%ldppm rate=%ld",
                 (long)s_pass_y, (long)(s_curve_entry * 1000000.0f), (long)entry_rate);
        Tlm_SendEvent(3, plan);
        /* 晚检出时不把“降速”误当缩短转弯半径；空间不足就停。 */
        if (s_shift_x < 220.0f) { av_fault("AVOID insufficient turn room"); return; }
        av_offset_path();
        return;
    }

    av_motion(ms);
    if (s_phase == AV_FAULT) {
        uint8_t bits = Log_LiveFrame()[0];
        av_emit(ms, bits, av_count(bits)); return;
    }
    uint8_t raw = 0xffu;
    if (!Gr_ReadDigital(&raw)) { av_fault("AVOID gray read failed"); }
    uint8_t bits = (uint8_t)~raw;
    int count = av_count(bits);
    int side = 0;
    int32_t mm = 0;
    if (av_scan(ms, 0u, &side, &mm) < 0) { av_fault("AVOID emergency <=30mm"); }
    if (s_phase != AV_FAULT &&
        ((ms - s_start_ms) > AV_TOTAL_MAX_MS || s_trip_mm > AV_MAX_TRIP_MM ||
         (ms - s_progress_ms) > 500u)) {
        av_fault("AVOID timeout or no progress");
    }

    /* 仅在尚未明显离线时，两个正的远距读数允许取消误接管。 */
    if (s_phase == AV_OUT && s_clear_reads >= 2u && count >= 1 && count <= 3 &&
        fabsf(s_yaw_deg) < 12.0f && fabsf(s_y_mm) < 20.0f) {
        s_plan_valid = 0u;
        av_phase(AV_RECOVER, ms, "AVOID echo cleared, recover");
    }

    switch (s_phase) {
    case AV_OUT:
        av_offset_path();
        if (s_phase == AV_FAULT) { break; }
        if (s_x_mm >= s_shift_x * 0.7f) { av_phase(AV_SHIFT, ms, "AVOID path offset"); }
        break;
    case AV_SHIFT:
        av_offset_path();
        if (s_phase == AV_FAULT) { break; }
        if (s_x_mm >= s_shift_x &&
            (s_y_mm - s_pass_y) * (float)s_away >= -8.0f) {
            av_phase(AV_PARALLEL, ms, "AVOID parallel align");
        } else if (s_x_mm > s_shift_x + 90.0f) { av_fault("AVOID offset not reached"); }
        break;
    case AV_PARALLEL:
    case AV_PASS:
        av_nav(s_x_mm + AV_LOOKAHEAD_MM, s_pass_y, AV_ROUTE_SPEED, 18);
        if (s_phase == AV_FAULT) { break; }
        if (s_phase == AV_PARALLEL && fabsf(s_yaw_deg) <= 25.0f) {
            av_phase(AV_PASS, ms, "AVOID pass obstacle");
        }
        if (s_x_mm >= s_clear_x) {
            s_search_ms = ms; s_search_trip = s_trip_mm;
            s_search_x = s_x_mm;
            s_return_offset = s_y_mm - av_line_y(s_search_x);
            av_phase(AV_RETURN, ms, "AVOID return now");
        }
        break;
    case AV_RETURN:
    case AV_SEARCH:
        if (av_line(bits, count)) {
            /* A single gray bar cannot identify the original line. A candidate
             * outside the bounded predicted corridor is ambiguous: stop rather
             * than latch an arbitrary black mark. Inside still needs centering. */
            if (fabsf(s_y_mm - av_line_y(s_x_mm)) > AV_SEARCH_BAND_MM) {
                av_fault("AVOID ambiguous line"); break;
            }
            av_phase(AV_RECOVER, ms, "AVOID line seen, center now");
            av_recover(ms, bits, count); /* 见线这一拍就纠偏，不再沿用旧舵量。 */
            if (s_phase == AV_IDLE) { return; }
        } else if (count >= 5) {
            /* 斜交/标识区域先降速和抑制旋转，不把全黑当作线已居中。 */
            av_drive(av_speed_base(AV_CAPTURE_SPEED, 8), -0.08f * s_yaw_rate);
        } else if (s_phase == AV_RETURN && s_x_mm < s_search_x + AV_RETURN_RUN_MM) {
            float gx = s_x_mm + AV_RETURN_LOOK_MM;
            float t = av_clip((gx - s_search_x) / AV_RETURN_RUN_MM, 0.0f, 1.0f);
            float gy = av_line_y(gx) + s_return_offset *
                       (1.0f - t * t * (3.0f - 2.0f * t));
            av_nav(gx, gy, AV_SEEK_SPEED, 12);
        } else {
            /* 第一次跨过预测线70mm；仍全白就向另一侧扫一次，禁止单方向盲开。 */
            float gx = s_search_x + (s_phase == AV_RETURN ? 65.0f : 150.0f);
            float gy = av_line_y(gx) + (s_phase == AV_RETURN ? -(float)s_away : (float)s_away) *
                        AV_SEARCH_BAND_MM;
            if (s_phase == AV_RETURN && (s_y_mm - gy) * (float)s_away < 15.0f) {
                av_phase(AV_SEARCH, ms, "AVOID local sweep reverse");
                gx = s_search_x + 150.0f;
                gy = av_line_y(gx) + (float)s_away * AV_SEARCH_BAND_MM;
            }
            av_nav(s_x_mm + AV_SEARCH_LOOK_MM, gy, AV_SEEK_SPEED, 12);
        }
        if ((s_phase == AV_RETURN || s_phase == AV_SEARCH) &&
            ((s_trip_mm - s_search_trip) > AV_SEEK_MAX_MM ||
             (ms - s_search_ms) > AV_SEEK_MAX_MS)) {
            av_fault("AVOID line not found");
        }
        break;
    case AV_RECOVER:
        av_recover(ms, bits, count);
        if (s_phase == AV_IDLE) { return; }
        break;
    case AV_FAULT:
        Mo_Brake();
        s_pwm_l = s_pwm_r = 0;
        break;
    default:
        av_fault("AVOID invalid phase");
        break;
    }
    av_emit(ms, bits, count);
}
