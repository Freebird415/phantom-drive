/* A02 v4：保守曲率初值、柔和低速横移、车尾越障、有界局部搜线。
 * STEP 底板坐标下模型 200x195.5x100mm；用户确认朝驱动轮端前进。
 * 模型未包含现装探头/灰度/线束及全部突出件。下列外廓/安装偏移仍待实测。
 * 25mm 是名义规划余量，不是超声读数或已验收的实车间隙。
 * mm/计数仍未精标；保留原有有方向的陀螺积分，不改驱动/接口。 */
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

typedef enum {
    AV_IDLE, AV_OUT, AV_SHIFT, AV_PARALLEL, AV_PASS,
    AV_RETURN, AV_RECOVER, AV_FAULT, AV_SEARCH
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

static float av_clip(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
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
    s_drive = 0u;
    s_cool_until = 0u;
    s_gz_bias = 0.0f;
    s_speed_ms = s_speed_dt = 0u;
    s_speed_mm_s = s_speed_dist = s_curve_live = 0.0f;
    av_reset();
    printf("# AVOID dynamic lead; gap=%ldmm stop=%dmm; route/search=%ld/%ldmmps nominal\r\n",
           (long)AV_GAP_MM, AV_STOP_MM, (long)AV_ROUTE_SPEED, (long)AV_SEEK_SPEED);
    Tlm_SendEvent(97, "AVOID build a02-v4-soft-entry-trial");
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
    if (s_phase != AV_FAULT) {
        Tlm_SendEvent(88, note);
        char pose[64];
        snprintf(pose, sizeof pose, "AVOID pose p=%d x=%ld y=%ld yaw=%ld",
                 (int)s_phase, (long)s_x_mm, (long)s_y_mm, (long)s_yaw_deg);
        Tlm_SendEvent(88, pose); /* 名义 mm / 度，便于区分距离与回线故障。 */
    }
    s_phase = AV_FAULT;
    Mo_Brake();
    s_pwm_l = s_pwm_r = 0;
}

static void av_drive(int base, float steer)
{
    if (s_phase == AV_FAULT) { return; }
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
    int state = (s_phase == AV_RETURN || s_phase == AV_RECOVER || s_phase == AV_SEARCH) ? 3 : 2;
    Log_Note(bits, err, s_pwm_l, s_pwm_r, state);
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
    /* 方向积分保留正负；原先累加绝对 gz 会把反向摆动也算成转角。 */
    s_yaw_rate = -((float)Log_GetGz() - s_gz_bias) / 131.0f;
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
    Tlm_SendEvent(4, "AVOID line recovered");
    av_emit(ms, bits, count);
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
        int base = fabsf(err) > 1.0f ? av_speed_base(AV_CAPTURE_SPEED, 9) :
                                            av_speed_base(AV_CENTER_SPEED, 10);
        av_drive(base, 3.0f * err + d - 0.12f * s_yaw_rate);
        if (s_phase == AV_FAULT) { return; }
        if (fabsf(err) <= 1.0f && (bits & 0x18u) &&
            fabsf(s_yaw_rate) <= 30.0f && fabsf(s_line_d) <= 1.5f) {
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
            av_fault("AVOID local line lost");
            return;
        }
        float dir = s_line_err < -0.25f ? -1.0f :
                    s_line_err > 0.25f ? 1.0f : -(float)s_away;
        float damping = 0.04f * s_yaw_rate;
        if (damping > 4.0f) { damping = 4.0f; }
        if (damping < -4.0f) { damping = -4.0f; }
        int base = av_speed_base(AV_CAPTURE_SPEED, 8);
        av_drive(base, dir * (float)base - damping);
    }
}

void Avoid_Step(void)
{
    uint32_t ms = HAL_GetTick();
    if (!Trace_GetDrive()) {
        /* 仅在编码器不动的待机状态估计零偏，不把正常弯道当零偏。 */
        int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
        if (l == s_enc_l && r == s_enc_r) {
            s_gz_bias += 0.02f * ((float)Log_GetGz() - s_gz_bias);
        }
        s_enc_l = l; s_enc_r = r;
        s_speed_ms = ms; s_speed_dist = s_speed_mm_s = 0.0f;
        s_speed_dt = 0u; s_curve_live = 0.0f;
        if (s_phase != AV_IDLE) { av_reset(); }
        return;
    }
    if (!s_drive) { return; }

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
        if (live[4] == 1u && count >= 1 && count <= 3 && s_speed_mm_s >= 50.0f) {
            float rate = -((float)Log_GetGz() - s_gz_bias) / 131.0f;
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
        s_obstacle_side = (UsId)side;
        s_away = side == US_L ? 1 : -1;
        float entry_rate = -((float)Log_GetGz() - s_gz_bias) / 131.0f;
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
