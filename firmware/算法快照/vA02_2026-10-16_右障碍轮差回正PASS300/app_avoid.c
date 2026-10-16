/* 历史快照：2026-10-16 手推轨迹开环复现试验前的完整 A02 工作区版本。 */
/* =============================================================================
 * A02 第二阶段直线试验：右侧障碍触发左偏 -> 低速右弧回原线
 *
 * 按钮由 main.c 识别；A02 先独占停车 1s，再放行 A01 正常循迹。
 * 右探头两次新鲜读数进入试验范围时，A02 接管：
 * 左偏至全白 -> 再左偏 300ms -> 右偏抵消左偏轮差
 *   -> 按编码器名义 300mm 近似平行通过 -> 降速右弧寻找黑线
 *   -> 连续黑位候选（宽黑不立即交接）
 *   -> 黑位收窄、向中心移动 -> 低速捕线、连续居中
 *   -> 松手，把控制交还 A01 跟线。
 *
 * 碰黑后的捕线段仅用低速灰度比例控制；A01 自带循迹不在本文件复制。
 *
 * 本版不加近距或有界失败自动停车；若灰度不见
 * 全白/黑线，可能持续行驶，必须能人工长按/断电。
 * 仅限无弯直线、右侧单个障碍的受控试验；触发不证明安全净距。
 * 旧完整避障版见 Git 提交 628b285。
 * 轮距、轮速和低占空比死区尚未标定，20cm 半径只是目标轨迹，
 * 不能由下面的 PWM 数字推算出实际半径。
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

/* 交还原理：A02 自己松手（Avoid_SetDrive(0)）⇒ Avoid_Mode()==0 ⇒
 * 下一拍 main.c 自动去调 Trace_Step()，A01 接手跟线。
 * 注：A01 的驱动在长按发车时已由 main.c 的 Trace_SetDrive(1) 打开。 */
/* 右侧障碍直线试验初值均待现场标定；PWM 不是实测速度。
 * 斜装探头的350mm读数不是车头纵向净距。 */
#define A02_WAIT_MS            1000u   /* 按钮发车蜂鸣结束后静止 1s */
#define A02_TRIGGER_R_MM        350    /* 右探头试验触发范围，未验证车身间隙 */
#define A02_TRIGGER_FRESH_MS   250u    /* 样本必须是近期测量 */
#define A02_TRIGGER_MAX_GAP_MS 450u    /* 两次确认间隔上限 */
#define A02_TRIGGER_SAMPLES      2u    /* 不因单次跳数接管 */
#define A02_LEFT_L_PWM             8    /* 左偏轮差 4% -> 10%，待实车验证 */
#define A02_LEFT_R_PWM            18
#define A02_ALIGN_L_PWM           18    /* 第一次右偏只回正方向，不搜线 */
#define A02_ALIGN_R_PWM            8
#define A02_HEADING_TOL_COUNTS    80    /* 抵消轮差的容限；不是实测角度 */
#define A02_PASS_MM              300    /* 名义里程；0.115mm/计数尚未精标 */
#define A02_PASS_COUNTS         2609    /* 300 / 0.115 向上取整 */
#define A02_PASS_TRIM_COUNTS      80
#define A02_PASS_TRIM_PWM          3    /* PASS 时纠正累计左右轮差 */
#define A02_PASS_BASE_MAX         22    /* 与 A01 直道基准速度上限一致 */
#define A02_SLOW_PWM             12    /* PASS 后先减速再右弧 */
#define A02_RIGHT_L_PWM          12    /* 第二次右偏：较旧版 10/6 更大轮差 */
#define A02_RIGHT_R_PWM           6
#define A02_CANDIDATE_L_PWM        9    /* 碰黑后继续更慢的右弧，不直穿黑线 */
#define A02_CANDIDATE_R_PWM        5
#define A02_CAPTURE_PWM            6    /* 确认连续黑线后再低速捕线，待实测 */
#define A02_CAPTURE_KP             1.5f /* 黑位偏移时更快增大差速，待实测 */
#define A02_CAPTURE_STEER_MAX      5    /* 捕线/丢线局部找线的单侧差速上限 */
#define A02_CAPTURE_MIN_PWM        4    /* 禁止命令单轮停车/反转；4%能否转待实测 */
#define A02_CAPTURE_CENTER_ERR     0.75f /* 黑位中心距灰度板中心的最大误差 */
#define A02_CAPTURE_INWARD_STEP    0.5f /* 至少见到一次从外侧向中心移动 */
#define A02_CAPTURE_CENTER_MS    150u   /* 连续居中确认，不把扫线当回线 */
#define A02_CAPTURE_HANDOFF_DIFF   2    /* 交接前实际左右 PWM 差不超过 2% */
#define A02_CANDIDATE_MS         120u   /* 连续黑位观察时间，待实车标定 */
#define A02_CANDIDATE_SAMPLES     10u   /* 防止一次读数跨时间被算作连续 */
#define A02_CANDIDATE_MAX_GAP_MS  50u   /* 灰度读取/调度断档时不予确认 */
#define A02_REARM_WHITE_MS        80u   /* 排除候选后需重新走到全白 */
#define A02_LEFT_AFTER_WHITE_MS  300u   /* 首次全白后继续左偏的固定时间 */
#define A02_PWM_SLEW_PER_TICK      1    /* 每 10ms 左右轮各最多变 1% */

typedef enum {
    A02_IDLE,
    A02_WAIT,
    A02_FOLLOW,
    A02_LEFT,
    A02_ALIGN,
    A02_PASS,
    A02_SLOW,
    A02_RIGHT,
    A02_CANDIDATE,
    A02_REJECT,
    A02_CAPTURE
} A02Phase;

static A02Phase s_phase;
static uint8_t s_armed;
static uint8_t s_drive_enabled;
static uint32_t s_right_stamp;
static uint32_t s_right_confirm_ms;
static uint8_t s_right_confirm_count;
static uint8_t s_saw_white;
static uint32_t s_phase_ms;
static uint32_t s_white_ms;
static uint8_t s_last_bits;
static int s_pwm_l;
static int s_pwm_r;
static int s_pass_pwm;
static int16_t s_enc_l_prev;
static int16_t s_enc_r_prev;
static int32_t s_heading_counts;
static int32_t s_pass_pair_counts;
static int s_capture_seek_sign;
static uint32_t s_capture_center_ms;
static float s_capture_prev_abs_err;
static uint8_t s_capture_seen_outer;
static uint8_t s_capture_seen_inward;
static uint32_t s_candidate_ms;
static uint32_t s_candidate_last_ms;
static uint32_t s_rearm_white_ms;
static uint8_t s_candidate_samples;
static float s_candidate_outer_err;

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

/* 连续黑位的宽度。全黑只作疑似横穿线，不据此交还 A01。 */
static uint8_t a02_black_width(uint8_t bits, float *err)
{
    int count = 0, sum = 0, first = -1, last = -1;
    for (int i = 0; i < 8; i++) {
        if (bits & (uint8_t)(1u << i)) {
            if (first < 0) { first = i; }
            last = i;
            count++;
            sum += i;
        }
    }
    if (count < 1 || last - first + 1 != count) { return 0u; }
    *err = (float)sum / (float)count - 3.5f;
    return (uint8_t)count;
}

/* 只有 1~4 路相邻黑位能用于灰度转向和 A01 交接。 */
static uint8_t a02_narrow_line(uint8_t bits, float *err)
{
    uint8_t width = a02_black_width(bits, err);
    return width >= 1u && width <= 4u;
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

/* 编码器已按“前进为正”归一化；差分先转 int16_t 处理计数器回绕。
 * 轮差抵消只近似恢复接管时的方向，打滑/弯道时不能当作真实航向。 */
static void a02_encoder_delta(int16_t *dl, int16_t *dr)
{
    int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
    *dl = (int16_t)(l - s_enc_l_prev);
    *dr = (int16_t)(r - s_enc_r_prev);
    s_enc_l_prev = l;
    s_enc_r_prev = r;
    s_heading_counts += (int32_t)*dr - (int32_t)*dl;
}

static void a02_drive(int left, int right)
{
    s_pwm_l = a02_slew(s_pwm_l, left);
    s_pwm_r = a02_slew(s_pwm_r, right);
    Mo_Set(s_pwm_l, s_pwm_r);
}

/* A01 当前可调基准档。这里仅复用其直道基准值，不复制 A01 PID；
 * A02 无法读取 A01 这一拍的实际双轮输出。 */
static int a02_pass_base(void)
{
    int base = Tlm_Params()->base_speed;
    if (base <= 0) { base = A02_PASS_BASE_MAX; }
    if (base > A02_PASS_BASE_MAX) { base = A02_PASS_BASE_MAX; }
    if (base < 8) { base = 8; }
    return base;
}

static int a02_slow_base(void)
{
    return s_pass_pwm < A02_SLOW_PWM ? s_pass_pwm : A02_SLOW_PWM;
}

/* 初见黑后改由低速局部控制：线在右则右转，线在左则左转。
 * 全白/读失败时朝最后见线侧继续低速找，不在这里增加自动停车。 */
static uint8_t a02_capture_step(uint32_t now, uint8_t bits, uint8_t gray_ok)
{
    float err = 0.0f;
    uint8_t narrow = gray_ok && a02_narrow_line(bits, &err);
    int left = A02_CAPTURE_PWM, right = A02_CAPTURE_PWM;
    if (narrow) {
        float abs_err = err < 0.0f ? -err : err;
        if (s_capture_seen_outer &&
            s_capture_prev_abs_err - abs_err >= A02_CAPTURE_INWARD_STEP) {
            s_capture_seen_inward = 1u;
        }
        if (abs_err > A02_CAPTURE_CENTER_ERR) { s_capture_seen_outer = 1u; }
        s_capture_prev_abs_err = abs_err;
        if (err > 0.25f) { s_capture_seek_sign = 1; }
        if (err < -0.25f) { s_capture_seek_sign = -1; }
        a02_follow_cfg(bits, A02_CAPTURE_PWM, A02_CAPTURE_KP,
                       A02_CAPTURE_STEER_MAX, &left, &right);
    } else if (gray_ok && a02_black_width(bits, &err) >= 5u) {
        /* 横穿线时可能短暂全黑；保持极低速右弧，等线收窄再按偏差捕线。 */
        left = A02_CANDIDATE_L_PWM;
        right = A02_CANDIDATE_R_PWM;
    } else if (!gray_ok || bits == 0u) {
        left += s_capture_seek_sign * A02_CAPTURE_STEER_MAX;
        right -= s_capture_seek_sign * A02_CAPTURE_STEER_MAX;
    }
    if (left < A02_CAPTURE_MIN_PWM) { left = A02_CAPTURE_MIN_PWM; }
    if (right < A02_CAPTURE_MIN_PWM) { right = A02_CAPTURE_MIN_PWM; }
    a02_drive(left, right);

    int diff = s_pwm_l - s_pwm_r;
    if (diff < 0) { diff = -diff; }
    if (narrow && s_capture_seen_inward &&
        err >= -A02_CAPTURE_CENTER_ERR &&
        err <= A02_CAPTURE_CENTER_ERR &&
        diff <= A02_CAPTURE_HANDOFF_DIFF) {
        if (s_capture_center_ms == 0u) { s_capture_center_ms = now; }
        if (now - s_capture_center_ms >= A02_CAPTURE_CENTER_MS) {
            Tlm_SendEvent(4, "A02 S2 centered; handoff to A01");
            Avoid_SetDrive(0u);
            return 1u;
        }
    } else {
        s_capture_center_ms = 0u;
    }
    return 0u;
}

/* 沿用冻结的 $T 与快照格式：state 2=绕行，3=右弧找线。 */
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
    s_armed = 0u;
    s_drive_enabled = 0u;
    s_phase = A02_IDLE;
    s_phase_ms = 0u;
    s_right_stamp = s_right_confirm_ms = 0u;
    s_right_confirm_count = 0u;
    s_saw_white = 0u;
    s_last_bits = 0u;
    s_pwm_l = s_pwm_r = 0;
    s_pass_pwm = A02_PASS_BASE_MAX;
    s_enc_l_prev = s_enc_r_prev = 0;
    s_heading_counts = s_pass_pair_counts = 0;
    s_capture_seek_sign = 1;
    s_capture_center_ms = 0u;
    s_capture_prev_abs_err = 0.0f;
    s_capture_seen_outer = 0u;
    s_capture_seen_inward = 0u;
    s_candidate_ms = s_candidate_last_ms = s_rearm_white_ms = 0u;
    s_candidate_samples = 0u;
    s_candidate_outer_err = 0.0f;
    Tlm_SendEvent(97, "AVOID build a02-s2-align-pass300-local");
}

void Avoid_SetDrive(uint8_t on)
{
    /* main.c 按钮顺序是 Avoid_SetDrive(1) 再 Trace_SetDrive(1)。
     * WAIT 期间保持 A02 控制权，不能先跑一拍 A01。 */
    if (!on) {
        s_armed = 0u;
        if (s_drive_enabled) { Mo_Stop(); }
        s_drive_enabled = 0u;
        s_phase = A02_IDLE;
        return;
    }
    s_armed = 1u;
    s_drive_enabled = 1u;
    s_phase = A02_WAIT;
    s_phase_ms = 0u; /* 第一拍才计时，不把按钮蜂鸣的阻塞时间算进去。 */
    s_right_stamp = Us_Get(US_R)->stamp_ms; /* 不用发车前的旧测距确认。 */
    s_right_confirm_ms = 0u;
    s_right_confirm_count = 0u;
    s_pwm_l = s_pwm_r = 0;
    s_pass_pwm = A02_PASS_BASE_MAX;
    s_enc_l_prev = s_enc_r_prev = 0;
    s_heading_counts = s_pass_pair_counts = 0;
    Mo_Stop();
    s_saw_white = 0u;
    s_last_bits = 0u;
    s_white_ms = 0u;
    s_capture_seek_sign = 1;
    s_capture_center_ms = 0u;
    s_capture_prev_abs_err = 0.0f;
    s_capture_seen_outer = 0u;
    s_capture_seen_inward = 0u;
    s_candidate_ms = s_candidate_last_ms = s_rearm_white_ms = 0u;
    s_candidate_samples = 0u;
    s_candidate_outer_err = 0.0f;
    Tlm_SendEvent(3, "A02 S2 wait 1s");
}

uint8_t Avoid_GetDrive(void)
{
    return s_armed;
}

uint8_t Avoid_Mode(void)
{
    return s_drive_enabled; /* WAIT/脱线/回线暂停 A01；FOLLOW 放行。 */
}

void Avoid_Step(void)
{
    if (!s_armed) { return; }
    uint32_t now = HAL_GetTick();
    if (!s_drive_enabled) {
        /* main 已先执行 Trace_Step；若 A01 停车或灰度离线，不强行接管。 */
        if (!Trace_GetDrive() || !Trace_Online()) {
            Avoid_SetDrive(0u);
            return;
        }
        if (s_phase != A02_FOLLOW) { return; }
        /* A01 已在本拍循迹。只消费右探头的新鲜正值；灰度须为窄线，
         * 避免在宽黑标识上误接管。负值是状态码，绝不是近障碍。 */
        UsDev *right = Us_Get(US_R);
        if (right->stamp_ms == s_right_stamp) { return; }
        s_right_stamp = right->stamp_ms;
        uint8_t trigger_bits = 0u;
        float trigger_err = 0.0f;
        if (right->mm <= 0 || right->mm > A02_TRIGGER_R_MM ||
            now - right->stamp_ms > A02_TRIGGER_FRESH_MS ||
            !a02_read_gray(&trigger_bits) ||
            !a02_narrow_line(trigger_bits, &trigger_err)) {
            s_right_confirm_count = 0u;
            return;
        }
        if (s_right_confirm_count == 0u ||
            now - s_right_confirm_ms > A02_TRIGGER_MAX_GAP_MS) {
            s_right_confirm_count = 1u;
        } else {
            s_right_confirm_count++;
        }
        s_right_confirm_ms = now;
        if (s_right_confirm_count < A02_TRIGGER_SAMPLES) { return; }
        s_drive_enabled = 1u;
        s_pass_pwm = a02_pass_base();
        /* A01 每轮的真实 PWM 没有共享接口，以其基准档近似为限速器起点。 */
        s_pwm_l = s_pwm_r = s_pass_pwm;
        s_last_bits = trigger_bits;
        s_enc_l_prev = En_Count(ENC_L);
        s_enc_r_prev = En_Count(ENC_R);
        s_heading_counts = s_pass_pair_counts = 0;
        a02_enter(A02_LEFT, now, "A02 S2 right confirmed; leave left");
        /* 本拍 A01 已输出；下一拍才由 A02 发第一笔差速。 */
        return;
    }
    uint8_t bits = 0u;
    uint8_t gray_ok = a02_read_gray(&bits);
    if (gray_ok) { s_last_bits = bits; }
    int16_t dl = 0, dr = 0;
    if (s_phase == A02_LEFT || s_phase == A02_ALIGN || s_phase == A02_PASS) {
        a02_encoder_delta(&dl, &dr);
    }

    switch (s_phase) {
    case A02_WAIT:
        if (s_phase_ms == 0u) { s_phase_ms = now; }
        Mo_Stop();
        s_pwm_l = s_pwm_r = 0;
        if (now - s_phase_ms >= A02_WAIT_MS) {
            s_drive_enabled = 0u;
            a02_enter(A02_FOLLOW, now, "A02 S2 A01 follow; sonar armed");
        }
        break;

    case A02_LEFT:
        if (gray_ok && bits == 0u && !s_saw_white) {
            s_saw_white = 1u;
            s_white_ms = now;
            Tlm_SendEvent(3, "A02 S2 all white");
        }
        if (s_saw_white && now - s_white_ms >= A02_LEFT_AFTER_WHITE_MS &&
            s_heading_counts > A02_HEADING_TOL_COUNTS) {
            a02_enter(A02_ALIGN, now, "A02 S2 align heading");
            a02_drive(A02_ALIGN_L_PWM, A02_ALIGN_R_PWM);
        } else {
            a02_drive(A02_LEFT_L_PWM, A02_LEFT_R_PWM);
        }
        break;

    case A02_ALIGN:
        if (s_heading_counts <= A02_HEADING_TOL_COUNTS) {
            s_pass_pair_counts = 0;
            a02_enter(A02_PASS, now, "A02 S2 pass 300mm nominal");
            a02_drive(s_pass_pwm, s_pass_pwm);
        } else {
            a02_drive(A02_ALIGN_L_PWM, A02_ALIGN_R_PWM);
        }
        break;

    case A02_PASS:
        /* 两轮前进增量的平均值计里程；不动 A01 的编码器计数器。
         * PWM 基准跟当前 A01 档（默认22%）一致，并用小轮差修直行。 */
        if (dl >= 0 && dr >= 0) {
            s_pass_pair_counts += (int32_t)dl + (int32_t)dr;
        }
        if (s_pass_pair_counts >= 2 * A02_PASS_COUNTS) {
            a02_enter(A02_SLOW, now, "A02 S2 pass done; slow");
            a02_drive(a02_slow_base(), a02_slow_base());
        } else {
            int trim = 0;
            if (s_heading_counts > A02_PASS_TRIM_COUNTS) {
                trim = A02_PASS_TRIM_PWM;
            } else if (s_heading_counts < -A02_PASS_TRIM_COUNTS) {
                trim = -A02_PASS_TRIM_PWM;
            }
            int left = s_pass_pwm + trim;
            int right = s_pass_pwm - trim;
            if (left > A02_PASS_BASE_MAX) { left = A02_PASS_BASE_MAX; }
            if (right > A02_PASS_BASE_MAX) { right = A02_PASS_BASE_MAX; }
            a02_drive(left, right);
        }
        break;

    case A02_SLOW:
        a02_drive(a02_slow_base(), a02_slow_base());
        if (s_pwm_l == a02_slow_base() && s_pwm_r == a02_slow_base()) {
            a02_enter(A02_RIGHT, now, "A02 S2 arc right");
        }
        break;

    case A02_RIGHT:
        if (gray_ok && bits != 0u) {
            float err = 0.0f;
            uint8_t width = a02_black_width(bits, &err);
            if (width != 0u) {
                /* 窄黑与横穿时的连续宽黑都只作候选，不直接交 A01。 */
                s_candidate_ms = s_candidate_last_ms = now;
                s_candidate_samples = 1u;
                s_candidate_outer_err = width <= 4u ?
                    (err < 0.0f ? -err : err) : 0.0f;
                a02_enter(A02_CANDIDATE, now, "A02 S2 black candidate");
                a02_drive(A02_CANDIDATE_L_PWM, A02_CANDIDATE_R_PWM);
            } else {
                /* 离散黑块不能在下一拍以边缘窄黑重新武装。 */
                s_rearm_white_ms = 0u;
                a02_enter(A02_REJECT, now, "A02 S2 black rejected");
                a02_drive(A02_RIGHT_L_PWM, A02_RIGHT_R_PWM);
            }
        } else {
            a02_drive(A02_RIGHT_L_PWM, A02_RIGHT_R_PWM);
        }
        break;

    case A02_CANDIDATE:
        {
            float err = 0.0f;
            uint8_t width = gray_ok ? a02_black_width(bits, &err) : 0u;
            if (width == 0u ||
                now - s_candidate_last_ms > A02_CANDIDATE_MAX_GAP_MS) {
                /* 白缝、离散黑或采样断档：继续右弧，待全白后再找。 */
                s_rearm_white_ms = 0u;
                a02_enter(A02_REJECT, now, "A02 S2 black rejected");
                a02_drive(A02_RIGHT_L_PWM, A02_RIGHT_R_PWM);
                break;
            }
            s_candidate_last_ms = now;
            if (s_candidate_samples < A02_CANDIDATE_SAMPLES) {
                s_candidate_samples++;
            }
            /* 碰黑后继续低速右弧，避免 120ms 候选直行穿线。 */
            a02_drive(A02_CANDIDATE_L_PWM, A02_CANDIDATE_R_PWM);
            if (now - s_candidate_ms >= A02_CANDIDATE_MS &&
                s_candidate_samples >= A02_CANDIDATE_SAMPLES && width <= 4u) {
                /* 保留首次外侧黑位，以便原捕线逻辑判断向中心移动。 */
                s_capture_prev_abs_err = s_candidate_outer_err;
                s_capture_seen_outer =
                    s_candidate_outer_err > A02_CAPTURE_CENTER_ERR;
                s_capture_seen_inward = 0u;
                s_capture_center_ms = 0u;
                a02_enter(A02_CAPTURE, now, "A02 S2 capture line");
                if (a02_capture_step(now, bits, gray_ok)) { return; }
            }
        }
        break;

    case A02_REJECT:
        /* 同一个字的下一个笔画不能马上再被当成新候选。 */
        if (gray_ok && bits == 0u) {
            if (s_rearm_white_ms == 0u) { s_rearm_white_ms = now; }
            if (now - s_rearm_white_ms >= A02_REARM_WHITE_MS) {
                a02_enter(A02_RIGHT, now, "A02 S2 search rearmed");
            }
        } else {
            s_rearm_white_ms = 0u;
        }
        a02_drive(A02_RIGHT_L_PWM, A02_RIGHT_R_PWM);
        break;

    case A02_CAPTURE:
        if (a02_capture_step(now, bits, gray_ok)) { return; }
        break;

    default:
        /* main 的按钮关闭入口负责停车；其他状态不发新的运动指令。 */
        break;
    }

    a02_emit(now, s_last_bits,
             (s_phase == A02_RIGHT || s_phase == A02_CANDIDATE ||
              s_phase == A02_REJECT || s_phase == A02_CAPTURE) ? 3 : 2);
}
