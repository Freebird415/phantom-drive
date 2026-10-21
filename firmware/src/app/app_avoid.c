/* A02 避障入口：平时由 A01 循迹；近障时 A02 接管。
 *   轨迹（推出→回摆）      → avoid_path.c
 *   回线 v3（过线刹停→旋转摆正→交还 A01）→ 本文件
 *   所有可调参数            → app/tuning.h
 * 长按按钮人工停车仍由 main.c 调用 Avoid_SetDrive(0)。 */
#include "app/app_avoid.h"
#include "app/app_log.h"
#include "app/app_telemetry.h"
#include "app/app_trace.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"
#include "drv/drv_ultrasonic.h"

#include <stdint.h>

#include "app/tuning.h"       /* ⭐ 全部可调参数都在这里（唯一出处）*/
#include "app/avoid_path.h"   /* 固定轨迹引擎接口 */

/* 参数已全部移到 app/tuning.h —— 本文件只保留逻辑 */

typedef enum { A02_WAIT, A02_LEFT, A02_RIGHT } A02Direction;
typedef enum { LINE_ARC, LINE_ALIGN, LINE_CAPTURE, LINE_AFTER_BLACK, LINE_FAULT,
               LINE_STOPPED, LINE_ROTATE, LINE_DONE } LinePhase;
static uint8_t s_armed;
static uint8_t s_cooling;
static uint8_t s_preparing;
static uint32_t s_prep_seen_ms;   /* 最后一次看到「≤A02_PREP_MM」新鲜读数的时刻（预备降速的撤销计时）*/
static int s_cruise_base;
static A02Direction s_direction;
static LinePhase s_line_phase;
static uint8_t s_recent_black;
static uint8_t s_line_seen;      /* 已判定"扫到线"的锁存（供"全白瞬间刹停"用）*/
static uint8_t  s_white_run;     /* 正在连续全白 */
static uint8_t  s_left_line;     /* 已确认离开过线（连续全白够久）*/
static uint32_t s_white_t0;      /* 本次连续全白的起点时刻 */
static uint32_t s_wait_ms;       /* 刹停/旋转阶段起点（等待与超时都按它算）*/
static uint32_t s_center_t0;     /* 「居中且保持」的起始时刻 */
static uint32_t s_last_l_stamp, s_last_r_stamp;
static uint32_t s_avoid_ms, s_align_ms, s_search_ms, s_black_found_ms, s_line_last_ms;
static uint32_t s_handoff_ms;
static uint32_t s_black_ms[8];
static int s_pwm_l, s_pwm_r;

#if A02_TEST_FIXED_TRACK
typedef enum { TEST_IDLE = 0, TEST_STRAIGHT, TEST_TRACK, TEST_DONE } TestPhase;
static TestPhase s_test;
static uint32_t  s_test_ms;
#endif

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

/* 「线已居中且姿态摆正」的判据：
 *   第 3~6 路里存在**两路相邻全黑**（3&4 / 4&5 / 5&6），且第 1/2/7/8 路全白。
 *   比"任一路黑"严格得多 —— 单路黑可能只是边缘蹭到线，相邻两路才说明线真的压在中间。*/
static uint8_t line_centered(uint8_t bits)
{
    uint8_t nib = (uint8_t)((bits & A02_MID_MASK) >> 2);   /* 3~6 路 → 低 4 位 */
    if ((nib & (uint8_t)(nib >> 1)) == 0u) { return 0u; }  /* 没有相邻两路黑 */
    if ((bits & A02_OUT_MASK) != 0u) { return 0u; }        /* 外四路必须全白 */
    return 1u;
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

#if A02_TEST_FIXED_TRACK
/* 临时测试：直行 → 左避固定轨迹 → 轨迹跑完就停（全程不判线）*/
static void test_step(uint32_t now)
{
    uint8_t bits = 0u;
    uint8_t ok = read_gray(&bits);

    if (s_test == TEST_STRAIGHT) {
        s_pwm_l = slew(s_pwm_l, A02_TEST_PWM);
        s_pwm_r = slew(s_pwm_r, A02_TEST_PWM);
        Mo_Set(s_pwm_l, s_pwm_r);
        if ((uint32_t)(now - s_test_ms) >= A02_TEST_MS) {
            s_test = TEST_TRACK;
            Tlm_SendEvent(3, "A02 test: straight done, start left track");
            Path_Start(1u, now, s_pwm_l, s_pwm_r);   /* 1 = 向左避（右侧障碍）*/
        }
        record_avoid(now, ok ? bits : 0u, 2);
        return;
    }

    if (s_test == TEST_TRACK) {
        Path_Step(now, &s_pwm_l, &s_pwm_r);
        /* 轨迹最后一段走完（有回正段就等回正，没有就等回摆）⇒ 停车 */
        if (Path_ReturnExpired(now)) {
            Path_Stop();
            s_pwm_l = s_pwm_r = 0;
            Mo_Stop();
            s_test = TEST_DONE;
            Tlm_SendEvent(3, "A02 test: track done, STOP");
            Log_Stop();
        }
        record_avoid(now, ok ? bits : 0u, 3);
        return;
    }

    /* TEST_DONE：保持停止（s_direction 仍非 WAIT ⇒ A01 不会被调用）*/
    s_pwm_l = s_pwm_r = 0;
    record_avoid(now, ok ? bits : 0u, 0);
}
#endif

void Avoid_Init(void)
{
#if A02_TEST_FIXED_TRACK
    s_test = TEST_IDLE;
    s_test_ms = 0u;
#endif
    s_armed = 0u;
    s_cooling = 0u;
    s_preparing = 0u;
    s_prep_seen_ms = 0u;
    s_cruise_base = 0;
    s_direction = A02_WAIT;
    s_line_phase = LINE_ARC;
    s_recent_black = 0u;
    s_line_last_ms = 0u;
    s_white_run = 0u;
    s_left_line = 0u;
    s_white_t0 = 0u;
    s_wait_ms = 0u;
    s_center_t0 = 0u;
    s_line_seen = 0u;
    s_avoid_ms = s_align_ms = s_search_ms = s_black_found_ms = 0u;
    s_handoff_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
    s_last_l_stamp = s_last_r_stamp = 0u;
    Path_Stop();
    Tlm_SendEvent(97, "A02 trace-return v3");
}

void Avoid_SetDrive(uint8_t on)
{
#if !A02_ENABLE
    (void)on;
    s_armed = 0u;                  /* 避障已关闭：不武装、**不碰电机**（否则发车就被停）*/
    return;
#else
    end_preparation();
    Mo_Stop();
    Path_Stop();
    s_prep_seen_ms = HAL_GetTick();   /* 发车/停车：预备计时从这里重新起算 */
    s_armed = on ? 1u : 0u;
    s_cooling = 0u;
    s_direction = A02_WAIT;
    s_line_phase = LINE_ARC;
    s_recent_black = 0u;
    s_line_last_ms = 0u;
    s_white_run = 0u;
    s_left_line = 0u;
    s_white_t0 = 0u;
    s_wait_ms = 0u;
    s_center_t0 = 0u;
    s_line_seen = 0u;
    s_avoid_ms = s_align_ms = s_search_ms = s_black_found_ms = 0u;
    s_handoff_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
#if A02_TEST_FIXED_TRACK
    s_test = TEST_IDLE;
    s_test_ms = 0u;
#endif
    if (on) {
        /* 发车前的旧测距不能触发本趟避障。 */
        s_last_l_stamp = Us_Get(US_L)->stamp_ms;
        s_last_r_stamp = Us_Get(US_R)->stamp_ms;
        Tlm_SendEvent(3, "A02 A01 trace; wait obstacle");
#if A02_TEST_FIXED_TRACK
        /* 测试模式：一按按钮就接管（A01 不参与），先直行再跑左避轨迹 */
        s_direction = A02_LEFT;              /* 让 Avoid_Mode()==1，主循环只调 Avoid_Step */
        s_test = TEST_STRAIGHT;
        s_test_ms = HAL_GetTick();
        s_pwm_l = s_pwm_r = 0;
        Tlm_SendEvent(3, "A02 test mode: straight then left track");
#endif
    }
#endif /* A02_ENABLE */
}

uint8_t Avoid_GetDrive(void) { return s_armed; }
uint8_t Avoid_Mode(void)
{
#if !A02_ENABLE
    return 0u;                     /* 避障关闭：A01 全程接管 */
#endif
#if A02_TEST_FIXED_TRACK
    if (s_armed && s_test != TEST_IDLE) { return 1u; }   /* 测试模式：A02 独占 */
#endif
    return s_armed && s_direction != A02_WAIT;
}

void Avoid_Step(void)
{
#if !A02_ENABLE
    return;                        /* 避障关闭：什么都不做 */
#endif
    if (!s_armed) { return; }
    uint32_t now = HAL_GetTick();
#if A02_TEST_FIXED_TRACK
    if (s_test != TEST_IDLE) { test_step(now); return; }  /* 测试模式：跳过正常避障逻辑 */
#endif
    if (s_line_phase == LINE_STOPPED) {          /* ① 已刹停：保持 A02_STOP_WAIT_MS */
        uint8_t sb = 0u;
        uint8_t sok = read_gray(&sb);
        Mo_Brake();
        s_pwm_l = s_pwm_r = 0;
        record_avoid(now, sok ? sb : 0u, 0);
        if ((uint32_t)(now - s_wait_ms) >= A02_STOP_WAIT_MS) {
            s_line_phase = LINE_ROTATE;
            s_wait_ms = now;
            Tlm_SendEvent(3, "A02 rotate to center line");
        }
        return;
    }
    if (s_line_phase == LINE_ROTATE) {           /* ② 原地旋转，等黑线落到 3~6 路中间 */
        uint8_t rb = 0u;
        uint8_t rok = read_gray(&rb);
        /* s_direction==A02_LEFT 表示「向左避」（右侧障碍）；镜像时左右对调 */
        int rl = (s_direction == A02_LEFT) ? A02_ROT_R_PWM : A02_ROT_L_PWM;
        int rr = (s_direction == A02_LEFT) ? A02_ROT_L_PWM : A02_ROT_R_PWM;
        s_pwm_l = slew(s_pwm_l, rl);
        s_pwm_r = slew(s_pwm_r, rr);
        Mo_Set(s_pwm_l, s_pwm_r);
        record_avoid(now, rok ? rb : 0u, 3);
        if (rok && line_centered(rb)) {
            Mo_Brake();                          /* 黑线居中（相邻两路黑）⇒ 先刹停，再验稳 */
            s_pwm_l = s_pwm_r = 0;
            s_line_phase = LINE_DONE;
            s_center_t0 = now;
            Tlm_SendEvent(3, "A02 line centered -> BRAKE (verify)");
        } else if ((uint32_t)(now - s_wait_ms) >= A02_ROT_TIMEOUT_MS) {
            /* 转这么久还不居中 ⇒ 锁存停车、**不交还 A01**（否则 A01 见全白会直行冲出去）*/
            Mo_Brake();
            s_pwm_l = s_pwm_r = 0;
            s_line_phase = LINE_FAULT;
            Tlm_SendEvent(3, "A02 rotate timeout -> LATCH (no handoff)");
        }
        return;
    }
    if (s_line_phase == LINE_DONE) {             /* ③ 居中验证：连续保持够久才交还 A01 */
        uint8_t db = 0u;
        uint8_t dok = read_gray(&db);
        Mo_Brake();
        s_pwm_l = s_pwm_r = 0;
        record_avoid(now, dok ? db : 0u, 0);
        if (!(dok && line_centered(db))) {
            /* 停了之后线又不在中间（假居中/车滑了）⇒ 回去继续旋转 */
            s_line_phase = LINE_ROTATE;
            Tlm_SendEvent(3, "A02 center lost -> rotate again");
            return;
        }
        if ((uint32_t)(now - s_center_t0) >= A02_CENTER_HOLD_MS) {
            /* 稳定居中 ⇒ 再等 A02_STOP_WAIT_MS 的余量后交还 A01 */
            if ((uint32_t)(now - s_center_t0) >=
                (uint32_t)A02_CENTER_HOLD_MS + (uint32_t)A02_HANDOFF_WAIT_MS) {
                Mo_Stop();                       /* 撤刹车，下一拍交给 A01 驱动 */
                handoff(now);                    /* 还原 base / Trace_SetSeek(1) /
                                                    s_direction=WAIT / 冷却计时 / $E 4 */
            }
        }
        return;
    }
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
    /* 轨迹（推出+回摆）跑完仍没回线 ⇒ 锁存停车（回线 v3 下由 A02_ROT_TIMEOUT_MS 接管）*/
    if ((s_line_phase == LINE_ARC || s_line_phase == LINE_ALIGN) &&
        Path_ReturnExpired(now)) {
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
        /* ⭐ 只在 A01「正常循迹(1)」时接管，会把最危险的两种状态挡在门外：
         *   state=5 起步找线、state=6 丢线找线 —— 这两种状态**车仍在往前冲**
         *   （丢线找线是 26/26 直行），恰恰是最需要躲障碍的时候。
         *   仍不接管：0 待机/锁存、4 精停（车已停）、2/3（A02 自己已接管）。
         *   2026-10-18 放行 5/6，见经验与踩坑「看见障碍却不躲」。*/
        uint8_t a01_state = live[4];
        if (a01_state != 1u && a01_state != 5u && a01_state != 6u) { return; }
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
            s_prep_seen_ms = now;      /* 还在"看着障碍" ⇒ 续命 */
        } else if (s_preparing &&
                   (uint32_t)(now - s_prep_seen_ms) >= A02_PREP_HOLD_MS) {
            /* ⭐ 300~400mm 是"只降速、不接管"的灰色地带：障碍拐出波束 / 一次假近读数
               ⇒ 永远不会触发 handoff()，end_preparation() 也永远不会被调用
               ⇒ base_speed 会永久停在 PREP_BASE_PWM（整趟慢 40%）。
               这里给一条恢复路径：连续 A02_PREP_HOLD_MS 没再看到近障就把速度还回去。*/
            end_preparation();
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
        s_white_run = 0u;
        s_left_line = 0u;
        s_white_t0 = 0u;
        s_wait_ms = 0u;
        s_center_t0 = 0u;
        s_line_seen = 0u;
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
        /* ---- 回线判定（三步，缺一不可）----
           ① 先确认「已经离开线」：连续八路全白 ≥ A02_LEAVE_WHITE_MS
              （否则刚接管时线还在车底下，会被当成"回线成功"⇒ 刚出发就停 ✗）
           ② 离开之后才把 line_six_seen() 当作「回到线」⇒ 锁存 s_line_seen
           ③ 已经回到线、并且又走到八路全白 ⇒ 线完整穿过阵列，此时刹停 */
        if (gray_ok && bits == 0u) {
            if (!s_white_run) { s_white_run = 1u; s_white_t0 = now; }
            if (!s_left_line &&
                (uint32_t)(now - s_white_t0) >= A02_LEAVE_WHITE_MS) {
                s_left_line = 1u;
                Tlm_SendEvent(3, "A02 left line (white)");
            }
        } else {
            s_white_run = 0u;              /* 见到黑 ⇒ 连续全白被打断 */
        }

        if (s_left_line && (uint32_t)(now - s_avoid_ms) >= A02_LINE_IGNORE_MS &&
            line_six_seen(now, bits, gray_ok)) {
#if A02_LINE_STOP_TEST
            s_line_seen = 1u;              /* ② 回到线：锁存，等它整个过去 */
#else
            s_line_phase = LINE_ALIGN;
            s_align_ms = now;
            Tlm_SendEvent(3, "A02 straighten on line");
            align_step(now);
#endif
        }
#if A02_LINE_STOP_TEST
        /* ③ 已回到线、且又走到全白 ⇒ 线完整穿过 ⇒ 刹停 */
        if (s_line_seen && gray_ok && bits == 0u) {
            Path_Stop();
            s_pwm_l = s_pwm_r = 0;
            Mo_Brake();
            s_line_phase = LINE_STOPPED;
            s_wait_ms = now;
            Tlm_SendEvent(3, "A02 line passed -> BRAKE (max)");
            record_avoid(now, 0u, 0);
            return;
        }
#endif
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
