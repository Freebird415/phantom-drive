/* A02 融合入口：平时由 A01 循迹；近障时 A02 接管左右固定轨迹。
 * 轨迹与回摆时限在 avoid_path.c（左右共用一个实现，PWM 自动镜像）；
 * 低速搜线见黑后再走短弧交还 A01，到时未完成则锁存停车。
 * 长按按钮人工停车仍由 main.c 调用 Avoid_SetDrive(0)。 */
#include "app/app_avoid.h"
#include "app/app_log.h"
#include "app/app_telemetry.h"
#include "app/app_trace.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"
#include "drv/drv_ultrasonic.h"

#include <stdint.h>

/* ===================== 共用触发参数，只在这里调 ===================== */
#define A02_PREP_MM          350   /* 新鲜有效测距 <=350mm：A01 提前减速 */
#define A02_PREP_BASE_PWM     30   /* 避障预备速度；只临时覆盖 A01 基准 PWM */
#define A02_TRIGGER_MM       310   /* 任一探头的新鲜有效读数 <=300mm 即触发 */
#define A02_CENTER_SPLIT_MM   30   /* 两侧都近且差值 <30mm：默认向左 */
#define A02_COOLDOWN_MS     3000u  /* 回线交还 A01 后，暂停超声避障触发的时间 */
#define A02_US_FRESH_MS     350u   /* 超过 350ms 的缓存读数不用 */
#define A02_PWM_STEP          2   /* 每 10ms 单轮最多改变 2% */
#define A02_LINE_IGNORE_MS  250u   /* 接管后前 500ms 不以黑位启动回线 */
#define A02_LINE_WINDOW_MS  100u   /* 100ms 内累计各路曾经变黑 */
#define A02_LINE_GAP_MS      30u   /* 采样中断超过 30ms 则重新累计 */
#define A02_LINE_BLACK_COUNT  5u   /* 至少 6 个不同灰度通道见黑 */

#if A02_PREP_MM <= A02_TRIGGER_MM
#error "A02_PREP_MM must exceed A02_TRIGGER_MM"
#endif

/* 满 500ms 后用短时六路黑位启动摆正；摆正后按固定低速弧线搜线。
 * 左避障后左轮 A／右轮 B；右避障时交换。搜线首次见黑后继续转一小段。 */
#define A02_CAPTURE_A_PWM       9
#define A02_CAPTURE_B_PWM      15
#define A02_SEARCH_MS        4500u  /* 摆正结束后，最多低速找线 4.5 秒 */
#define A02_CAPTURE_AFTER_BLACK_MS 500u /* 首次见黑后保持搜线慢弧，再交还 A01 */

/* 回线摆正：左避障后左轮 A、右轮 B；右避障时交换左右轮。
 * _MS 包含升速、可能的保持和降回捕线低速，不是在到达目标后再计时。 */
#define A02_ALIGN_A_PWM         -70
#define A02_ALIGN_B_PWM        5
#define A02_ALIGN_MS         430u
#define A02_ALIGN_STEP         5   /* 仅摆正段每 10ms 最多变化 5%；原轨迹仍用 2% */

/* 左右两份轨迹各自管理转弯时长和 PWM；原型只供本文件调度。 */
/* 固定轨迹引擎（avoid_path.c）：左/右避共用一个实现，PWM 数值自动镜像。
   dir_left = 1 表示"向左避"（右侧有障碍）*/
void    Path_Start(uint8_t dir_left, uint32_t now, int pwm_l, int pwm_r);
void    Path_Stop(void);
void    Path_Step(uint32_t now, int *pwm_l, int *pwm_r);
uint8_t Path_ReturnExpired(uint32_t now);
uint8_t Path_AlignExpired(uint32_t now);

typedef enum { A02_WAIT, A02_LEFT, A02_RIGHT } A02Direction;
typedef enum { LINE_ARC, LINE_ALIGN, LINE_CAPTURE, LINE_AFTER_BLACK, LINE_FAULT } LinePhase;
static uint8_t s_armed;
static uint8_t s_cooling;
static uint8_t s_preparing;
static int s_cruise_base;
static A02Direction s_direction;
static LinePhase s_line_phase;
static uint8_t s_recent_black;
static uint32_t s_last_l_stamp, s_last_r_stamp;
static uint32_t s_avoid_ms, s_align_ms, s_search_ms, s_black_found_ms, s_line_last_ms;
static uint32_t s_handoff_ms;
static uint32_t s_black_ms[8];
static int s_pwm_l, s_pwm_r;

/* A01 的 PID 和循迹速度上限不变：只暂借在线基准速度，交接/停车时归还。
 * 若当前已比预备速度慢，不应因发现障碍反而加速。 */
static void begin_preparation(void)
{
    if (s_preparing) { return; }
    s_cruise_base = Tlm_Params()->base_speed;
    if (s_cruise_base <= 0 || s_cruise_base > A02_PREP_BASE_PWM) {
        Tlm_Params()->base_speed = A02_PREP_BASE_PWM;
    }
    s_preparing = 1u;
    Tlm_SendEvent(3, "A02 prepare speed");
}

static void end_preparation(void)
{
    if (!s_preparing) { return; }
    Tlm_Params()->base_speed = s_cruise_base;
    s_preparing = 0u;
}

static int slew(int current, int target)
{
    if (current < target - A02_PWM_STEP) { return current + A02_PWM_STEP; }
    if (current > target + A02_PWM_STEP) { return current - A02_PWM_STEP; }
    return target;
}

static int align_slew(int current, int target)
{
    if (current < target - A02_ALIGN_STEP) { return current + A02_ALIGN_STEP; }
    if (current > target + A02_ALIGN_STEP) { return current - A02_ALIGN_STEP; }
    return target;
}

/* A02 每拍只发一帧真实灰度；A01 循迹时由 Trace_Step 自行上报。 */
static void record_avoid(uint32_t now, uint8_t bits, int state)
{
    int gray[8];
    for (int i = 0; i < 8; ++i) { gray[i] = (bits >> i) & 1u; }
    Log_Note(bits, 0.0f, s_pwm_l, s_pwm_r, state);
    Tlm_SendT(now, gray, 0.0f, s_pwm_l, s_pwm_r, state);
}

/* 仅在“有效且未过期”时把 mm 当成距离；负数都是驱动状态码。 */
static int32_t fresh_mm(const UsDev *sensor, uint32_t now)
{
    if (sensor->stamp_ms == 0u ||
        (uint32_t)(now - sensor->stamp_ms) > A02_US_FRESH_MS || sensor->mm <= 0) {
        return 0;
    }
    return sensor->mm;
}

/* 与面板约定一致：单侧近就向另一侧；双侧近时避开更近的一侧，
 * 差值不足 30mm 视作中间障碍，左右测试版都统一走左轨迹。 */
static A02Direction choose_direction(int32_t l, int32_t r)
{
    uint8_t l_near = (l > 0 && l <= A02_TRIGGER_MM);
    uint8_t r_near = (r > 0 && r <= A02_TRIGGER_MM);
    if (l_near && r_near) {
        if (r - l >= A02_CENTER_SPLIT_MM) { return A02_RIGHT; }
        return A02_LEFT; /* 右侧更近 >=30mm，或中间差值 <30mm */
    }
    if (l_near) { return A02_RIGHT; }
    if (r_near) { return A02_LEFT; }
    return A02_WAIT;
}

/* 灰度驱动低有效：原始 0=黑，统一翻成遥测约定的 1=黑。 */
static uint8_t read_gray(uint8_t *bits)
{
    uint8_t raw;
    if (!Gr_ReadDigital(&raw)) { return 0u; }
    *bits = (uint8_t)~raw;
    return 1u;
}

/* 每路只计一次，次序不限：中心先黑、边缘先黑或同一帧多路黑均可。
 * 整排全白／读失败会清零；字中只有部分通道变白仍可能被累计。
 * s_black_ms 记录各路最近见黑时刻，使窗口随时间滑动，而非固定分段。 */
static uint8_t line_six_seen(uint32_t now, uint8_t bits, uint8_t gray_ok)
{
    if (!gray_ok || bits == 0u) { s_recent_black = 0u; return 0u; }
    if (s_recent_black && now - s_line_last_ms > A02_LINE_GAP_MS) {
        s_recent_black = 0u;
    }
    s_line_last_ms = now;
    uint8_t count = 0u;
    for (int i = 0; i < 8; ++i) {
        uint8_t bit = (uint8_t)(1u << i);
        if (bits & bit) { s_black_ms[i] = now; s_recent_black |= bit; }
        if ((s_recent_black & bit) && now - s_black_ms[i] > A02_LINE_WINDOW_MS) {
            s_recent_black &= (uint8_t)~bit;
        }
        if (s_recent_black & bit) { ++count; }
    }
    return count >= A02_LINE_BLACK_COUNT;
}

/* 前段向 A/B 加速，末段向低速搜线 PWM 减速；总时长始终是 ALIGN_MS。
 * 若时长不足以达到目标 PWM，仍按时降速，不延长摆正阶段。 */
static void align_step(uint32_t now)
{
    int align_l = s_direction == A02_LEFT ? A02_ALIGN_A_PWM : A02_ALIGN_B_PWM;
    int align_r = s_direction == A02_LEFT ? A02_ALIGN_B_PWM : A02_ALIGN_A_PWM;
    int search_l = s_direction == A02_LEFT ? A02_CAPTURE_A_PWM : A02_CAPTURE_B_PWM;
    int search_r = s_direction == A02_LEFT ? A02_CAPTURE_B_PWM : A02_CAPTURE_A_PWM;
    int delta_l = align_l - search_l, delta_r = align_r - search_r;
    if (delta_l < 0) { delta_l = -delta_l; }
    if (delta_r < 0) { delta_r = -delta_r; }
    int delta = delta_l > delta_r ? delta_l : delta_r;
    uint32_t down_ms = (uint32_t)((delta + A02_ALIGN_STEP - 1) /
                                   A02_ALIGN_STEP) * 10u;
    if (down_ms > A02_ALIGN_MS / 2u) { down_ms = A02_ALIGN_MS / 2u; }
    uint32_t elapsed = now - s_align_ms;
    int target_l = elapsed >= A02_ALIGN_MS - down_ms ? search_l : align_l;
    int target_r = elapsed >= A02_ALIGN_MS - down_ms ? search_r : align_r;
    s_pwm_l = align_slew(s_pwm_l, target_l);
    s_pwm_r = align_slew(s_pwm_r, target_r);
    Mo_Set(s_pwm_l, s_pwm_r);
}

/* 固定低速弧线搜线：左避障后 A/B 向左，右避障后 B/A 向右。
 * 首次见黑后仍保持这组命令，直到 AFTER_BLACK_MS 结束。 */
static void capture_step(void)
{
    int target_l = s_direction == A02_LEFT ? A02_CAPTURE_A_PWM : A02_CAPTURE_B_PWM;
    int target_r = s_direction == A02_LEFT ? A02_CAPTURE_B_PWM : A02_CAPTURE_A_PWM;
    s_pwm_l = slew(s_pwm_l, target_l);
    s_pwm_r = slew(s_pwm_r, target_r);
    Mo_Set(s_pwm_l, s_pwm_r);
}

/* 不停车，只撤销 A02 接管；下一 10ms 拍由 main.c 交给 A01。
 * 用已有的起步找线入口让 A01 下一拍清旧 PID／速度量，不重置终点精停。 */
static void handoff(uint32_t now)
{
    Path_Stop();
    end_preparation();
    Trace_SetSeek(1u);
    s_direction = A02_WAIT;
    s_line_phase = LINE_ARC;
    s_handoff_ms = now;
    s_cooling = 1u;
    s_last_l_stamp = Us_Get(US_L)->stamp_ms;
    s_last_r_stamp = Us_Get(US_R)->stamp_ms;
    Tlm_SendEvent(4, "A02 search done; A01 handoff");
}

/* 回摆或低速搜线到时仍未碰到黑位：锁存停车，禁止 A01 接手。 */
static void no_line_stop(uint32_t now, const char *reason)
{
    Mo_Stop();
    Path_Stop();
    end_preparation();
    s_pwm_l = s_pwm_r = 0;
    s_line_phase = LINE_FAULT;
    Tlm_SendEvent(4, reason);
    Tlm_SendEvent(87, "A02 LATCHED: send AVOID 0 (or reboot) to release");
    {
        uint8_t fb = 0u;
        uint8_t fok = read_gray(&fb);
        record_avoid(now, fok ? fb : 0u, 0);
    }
    Log_Stop();
}

void Avoid_Init(void)
{
    s_armed = 0u;
    s_cooling = 0u;
    s_preparing = 0u;
    s_cruise_base = 0;
    s_direction = A02_WAIT;
    s_line_phase = LINE_ARC;
    s_recent_black = 0u;
    s_line_last_ms = 0u;
    s_avoid_ms = s_align_ms = s_search_ms = s_black_found_ms = 0u;
    s_handoff_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
    s_last_l_stamp = s_last_r_stamp = 0u;
    Path_Stop();
    Tlm_SendEvent(97, "A02 trace-return v3");
}

void Avoid_SetDrive(uint8_t on)
{
    end_preparation();
    Mo_Stop();
    Path_Stop();
    s_armed = on ? 1u : 0u;
    s_cooling = 0u;
    s_direction = A02_WAIT;
    s_line_phase = LINE_ARC;
    s_recent_black = 0u;
    s_line_last_ms = 0u;
    s_avoid_ms = s_align_ms = s_search_ms = s_black_found_ms = 0u;
    s_handoff_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
    if (on) {
        /* 发车前的旧测距不能触发本趟避障。 */
        s_last_l_stamp = Us_Get(US_L)->stamp_ms;
        s_last_r_stamp = Us_Get(US_R)->stamp_ms;
        Tlm_SendEvent(3, "A02 A01 trace; wait obstacle");
    }
}

uint8_t Avoid_GetDrive(void) { return s_armed; }
uint8_t Avoid_Mode(void) { return s_armed && s_direction != A02_WAIT; }

void Avoid_Step(void)
{
    if (!s_armed) { return; }
    uint32_t now = HAL_GetTick();
    if (s_line_phase == LINE_FAULT) {
        /* 锁存停车：电机不动，但**仍然上报真实灰度**。
           以前这里固定发 0 ⇒ BLE/日志里"灰度全 0"，看起来像蓝牙坏了/传感器死了 ✗
           （2026-10-18 实测：GR 明明看到 4 路黑，而 $T 全 0，真因就是这一行）*/
        uint8_t fb = 0u;
        uint8_t fok = read_gray(&fb);
        record_avoid(now, fok ? fb : 0u, 0);
        return;
    }
    if (s_line_phase == LINE_CAPTURE && now - s_search_ms >= A02_SEARCH_MS) {
        no_line_stop(now, "A02 search timeout stop");
        return;
    }
    /* 已进入低速搜线后，给它完整 4.5 秒；之前仍保留固定回摆的 5 秒保护。 */
    if ((s_line_phase == LINE_ARC || s_line_phase == LINE_ALIGN) &&
        (Path_ReturnExpired(now) || Path_AlignExpired(now))) {
        no_line_stop(now, "A02 return timeout stop");
        return;
    }

    if (s_direction == A02_WAIT) {
        if (s_cooling) {
            /* 冷却时只记时间戳供日志/去旧缓存：不判障碍，也不预减速。
             * 期满当拍也不触发；下一次真正的新测量才重新判断。 */
            s_last_l_stamp = Us_Get(US_L)->stamp_ms;
            s_last_r_stamp = Us_Get(US_R)->stamp_ms;
            if ((uint32_t)(now - s_handoff_ms) >= A02_COOLDOWN_MS) {
                s_cooling = 0u;
            }
            return;
        }
        /* main.c 此拍已执行 Trace_Step(1)：保持 A01 的电机命令、日志和精停。
         * A01 已停或灰度离线时，绝不由 A02 独自重新启动车轮。 */
        if (!Trace_GetDrive() || !Trace_Online()) { return; }
        const uint8_t *live = Log_LiveFrame();
        if (live[4] != 1u) { return; } /* A01 正常循迹，不从丢线/精停接管 */
        const UsDev *left = Us_Get(US_L);
        const UsDev *right = Us_Get(US_R);
        uint8_t new_l = left->stamp_ms != s_last_l_stamp;
        uint8_t new_r = right->stamp_ms != s_last_r_stamp;
        s_last_l_stamp = left->stamp_ms;
        s_last_r_stamp = right->stamp_ms;
        int32_t mm_l = fresh_mm(left, now), mm_r = fresh_mm(right, now);
        /* 只让本趟新测量进入 350mm 预备区；不能凭旧缓存降速。
         * 进入后保持低速，直到 300mm 真正触发或本趟被人工停止。 */
        if ((new_l && mm_l > 0 && mm_l <= A02_PREP_MM) ||
            (new_r && mm_r > 0 && mm_r <= A02_PREP_MM)) {
            begin_preparation();
        }
        /* 必须有一侧产生本趟的新鲜近障样本；另一侧的新鲜缓存只参与选向。
         * 不再累计票数，也不让同一个缓存值反复触发。 */
        if (!((new_l && mm_l > 0 && mm_l <= A02_TRIGGER_MM) ||
              (new_r && mm_r > 0 && mm_r <= A02_TRIGGER_MM))) { return; }
        A02Direction choice = choose_direction(mm_l, mm_r);
        if (choice == A02_WAIT) { return; }
        s_direction = choice;
        s_avoid_ms = now;
        s_search_ms = 0u;
        s_line_phase = LINE_ARC;
        s_recent_black = 0u;
        s_line_last_ms = 0u;
        /* Trace_Step 本拍刚写入的公开快照里 byte2/3 是实际下发的 PWM。
         * 以它们为斜坡起点，避免把 A01 巡航直接拉到 0。 */
        s_pwm_l = (int)(int8_t)live[2];
        s_pwm_r = (int)(int8_t)live[3];
        Path_Start((uint8_t)(choice == A02_LEFT), now, s_pwm_l, s_pwm_r);
    }

    uint8_t bits = 0u;
    uint8_t gray_ok = read_gray(&bits);
    uint8_t completed = 0u;
    if (s_line_phase == LINE_ARC) {
        Path_Step(now, &s_pwm_l, &s_pwm_r);
        if (now - s_avoid_ms >= A02_LINE_IGNORE_MS &&
            line_six_seen(now, bits, gray_ok)) {
            s_line_phase = LINE_ALIGN;
            s_align_ms = now;
            Tlm_SendEvent(3, "A02 straighten on line");
            align_step(now);
        }
    } else if (s_line_phase == LINE_ALIGN) {
        if (now - s_align_ms >= A02_ALIGN_MS) {
            s_line_phase = LINE_CAPTURE;
            s_search_ms = now;
            Tlm_SendEvent(3, "A02 capture line");
            capture_step();
            if (gray_ok && bits != 0u) {
                s_line_phase = LINE_AFTER_BLACK;
                s_black_found_ms = now;
                Tlm_SendEvent(3, "A02 search black; keep arc");
            }
        } else { align_step(now); }
    } else if (s_line_phase == LINE_CAPTURE) {
        capture_step();
        if (gray_ok && bits != 0u) {
            s_line_phase = LINE_AFTER_BLACK;
            s_black_found_ms = now;
            Tlm_SendEvent(3, "A02 search black; keep arc");
        }
    } else if (s_line_phase == LINE_AFTER_BLACK) {
        capture_step();
        completed = now - s_black_found_ms >= A02_CAPTURE_AFTER_BLACK_MS;
    }
    record_avoid(now, gray_ok ? bits : 0u,
                 completed || s_line_phase != LINE_ARC ? 3 : 2);
    if (completed) { handoff(now); }
}
