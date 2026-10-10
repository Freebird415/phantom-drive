/* 短距离绕障：近障确认 -> 必要外移 -> 车尾越障 -> 看线即居中。
 * 姿态使用有方向的偏航积分；里程仍用未精标的 0.115mm/计数。
 * 所有控制距离是首轮参数，不能等同于实际碰撞间隙。 */
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

#define AV_CONFIRM_MM       260
#define AV_START_MM         200   /* 20cm 试跑触发；3cm 仅用于紧急刹停 */
#define AV_STOP_MM           30
#define AV_LINE_READY_MS    250u
#define AV_US_FRESH_MS      150u
#define AV_PAIR_MAX_MS      550u
#define AV_OFFSET_MM        120.0f /* 车半宽按 100mm + 20mm 余量，实际待量 */
#define AV_OBSTACLE_LEN_MM  150.0f /* 规则中的最大纵向尺寸；摆放朝向待复核 */
#define AV_SENSOR_REAR_MM   200.0f /* 探头至车尾的名义距离，实际待量 */
#define AV_CLEAR_MARGIN_MM   20.0f
#define AV_SEEK_MAX_MM      300.0f
#define AV_MAX_TRIP_MM     1400.0f
#define AV_TARGET_DEG       45.0f
#define AV_TURN_MAX_MS     1400u
#define AV_SEEK_MAX_MS     2500u
#define AV_TOTAL_MAX_MS   10000u
#define AV_MM_PER_CNT        0.115f
#define AV_DEG_TO_RAD        0.01745329252f

typedef enum {
    AV_IDLE, AV_OUT, AV_SHIFT, AV_PARALLEL, AV_PASS,
    AV_RETURN, AV_RECOVER, AV_FAULT
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
static uint32_t s_dt_ms;
static int s_pwm_l, s_pwm_r;

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
    av_reset();
    printf("# AVOID start=%dmm stop=%dmm; line-ready=%ums\r\n",
           AV_START_MM, AV_STOP_MM, (unsigned)AV_LINE_READY_MS);
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
    if (s_phase != AV_FAULT) { Tlm_SendEvent(88, note); }
    s_phase = AV_FAULT;
    Mo_Brake();
    s_pwm_l = s_pwm_r = 0;
}

static void av_drive(int base, float steer)
{
    if (steer > (float)base) { steer = (float)base; }
    if (steer < -(float)base) { steer = -(float)base; }
    float slew = 3.0f * (float)s_dt_ms / 10.0f;
    float change = steer - s_steer_out;
    if (change > slew) { change = slew; }
    if (change < -slew) { change = -slew; }
    s_steer_out += change;
    if (s_steer_out > (float)base) { s_steer_out = (float)base; }
    if (s_steer_out < -(float)base) { s_steer_out = -(float)base; }
    int turn = (int)(s_steer_out + (s_steer_out >= 0.0f ? 0.5f : -0.5f));
    s_pwm_l = base + turn;
    s_pwm_r = base - turn;
    Mo_Set(s_pwm_l, s_pwm_r);
}

static void av_heading(float target, int base)
{
    /* 正舵量 -> 右转；实车日志正 gz -> 左转，已在 av_motion 统一符号。 */
    av_drive(base, 0.8f * (target - s_yaw_deg) - 0.10f * s_yaw_rate);
}

static void av_emit(uint32_t ms, uint8_t bits, int count)
{
    int gray[8], sum = 0;
    for (int i = 0; i < 8; i++) {
        gray[i] = (bits >> i) & 1u;
        if (gray[i]) { sum += i; }
    }
    float err = (count > 0 && count < 5) ? (float)sum / (float)count - 3.5f : 0.0f;
    int state = (s_phase == AV_RETURN || s_phase == AV_RECOVER) ? 3 : 2;
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
            if (mm > AV_START_MM + 100) {
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
        if (s_seen[i] >= 2u && mm <= AV_START_MM && mm < nearest) {
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
        s_trip_mm += ds;
        s_x_mm += ds * cosf(yaw_mid * AV_DEG_TO_RAD);
        s_y_mm += ds * sinf(yaw_mid * AV_DEG_TO_RAD);
        if (dl + dr >= 4) { s_progress_ms = ms; }
    }
}

static void av_release(uint32_t ms, uint8_t bits, int count)
{
    Tlm_SendEvent(4, "AVOID line recovered");
    av_emit(ms, bits, count);
    s_cool_until = ms + 1000u;
    av_reset(); /* 保持最后前进命令，由下一拍 A01 接手。 */
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
        if (s_phase != AV_IDLE) { av_reset(); }
        return;
    }
    if (!s_drive) { return; }

    if (s_phase == AV_IDLE) {
        /* 日志公开的 8B live frame：byte0=黑位图，byte4=state。
         * 本函数在 A01 后调用，复用本拍灰度，避免再次访问 I2C。 */
        const uint8_t *live = Log_LiveFrame();
        int count = av_count(live[0]);
        if (!Trace_Online() || live[4] != 1u || count < 1 || count > 3) {
            s_line_ready_ms = 0u;
        } else if (!s_line_ready_ms) {
            s_line_ready_ms = ms;
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
        s_clear_x = (float)mm * 0.9659f + AV_OBSTACLE_LEN_MM +
                    AV_SENSOR_REAR_MM + AV_CLEAR_MARGIN_MM;
        s_trip_mm = s_x_mm = s_y_mm = s_yaw_deg = 0.0f;
        s_yaw_rate = 0.0f;
        s_steer_out = ((float)(int8_t)live[2] - (float)(int8_t)live[3]) * 0.5f;
        s_enc_l = En_Count(ENC_L); s_enc_r = En_Count(ENC_R);
        s_tick_ms = s_progress_ms = s_start_ms = ms;
        s_dt_ms = 10u;
        char note[48];
        snprintf(note, sizeof note, "AVOID %c near %ldmm", side == US_L ? 'L' : 'R', (long)mm);
        av_phase(AV_OUT, ms, note);
        av_heading((float)s_away * AV_TARGET_DEG, 20);
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
        av_phase(AV_RECOVER, ms, "AVOID echo cleared, recover");
    }

    switch (s_phase) {
    case AV_OUT:
        av_heading((float)s_away * AV_TARGET_DEG, 20);
        if (fabsf(s_yaw_deg - (float)s_away * AV_TARGET_DEG) <= 5.0f) {
            av_phase(AV_SHIFT, ms, "AVOID short offset");
        } else if ((ms - s_phase_ms) > AV_TURN_MAX_MS) {
            av_fault("AVOID outward turn failed");
        }
        break;
    case AV_SHIFT:
        av_heading((float)s_away * AV_TARGET_DEG, 20);
        if (s_y_mm * (float)s_away >= AV_OFFSET_MM) {
            av_phase(AV_PARALLEL, ms, "AVOID offset reached");
        } else if ((s_trip_mm - s_phase_mm) > 250.0f) {
            av_fault("AVOID offset not reached");
        }
        break;
    case AV_PARALLEL:
        av_heading(0.0f, 20);
        if (fabsf(s_yaw_deg) <= 5.0f) {
            av_phase(AV_PASS, ms, "AVOID pass obstacle");
        } else if ((ms - s_phase_ms) > AV_TURN_MAX_MS) {
            av_fault("AVOID parallel turn failed");
        }
        break;
    case AV_PASS:
        av_heading(0.0f, 20);
        /* 用前向位移判断车尾越障，弯曲路径长度不能替代前向距离。 */
        if (s_x_mm >= s_clear_x) {
            av_phase(AV_RETURN, ms, "AVOID return now");
        }
        break;
    case AV_RETURN:
        if (count >= 1 && count <= 3) {
            av_phase(AV_RECOVER, ms, "AVOID line seen, center now");
        } else {
            av_heading(-(float)s_away * AV_TARGET_DEG, 20);
        }
        if ((s_trip_mm - s_phase_mm) > AV_SEEK_MAX_MM ||
            (ms - s_phase_ms) > AV_SEEK_MAX_MS) {
            av_fault("AVOID line not found");
        }
        break;
    case AV_RECOVER:
        if (count >= 1 && count <= 3) {
            int sum = 0;
            for (int i = 0; i < 8; i++) { if (bits & (1u << i)) { sum += i; } }
            float err = (float)sum / (float)count - 3.5f;
            /* 看见黑线就居中，不再盲转固定 35 度；偏航阻尼限制越线。 */
            av_drive(18, 8.0f * err - 0.35f * s_yaw_deg - 0.10f * s_yaw_rate);
            if (fabsf(err) <= 1.0f && (bits & 0x3cu) &&
                fabsf(s_yaw_deg) <= 18.0f && fabsf(s_yaw_rate) <= 60.0f) {
                if (!s_center_ms) { s_center_ms = ms; }
                if ((ms - s_center_ms) >= 40u) { av_release(ms, bits, count); return; }
            } else {
                s_center_ms = 0u;
            }
        } else {
            s_center_ms = 0u;
            av_heading(-(float)s_away * 25.0f, 16);
        }
        if ((s_trip_mm - s_phase_mm) > 180.0f || (ms - s_phase_ms) > 1600u) {
            av_fault("AVOID centering failed");
        }
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
