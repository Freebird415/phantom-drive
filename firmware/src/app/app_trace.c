/* =============================================================================
 *  app_trace.c —— A01 循迹控制（熊雅琦）
 *
 *  数字灰度 → 位置误差 → PID → 分档速度/差速混控；独立处理起步找线、
 *  灰度有效但全白时直行找线，以及终点精停，并上报 $T/板载快照。
 *
 *  参数不用重烧就能改（串口，回车结尾）：
 *      SET kp 8        改比例
 *      SET ki 0.02     改积分
 *      SET kd 1.5      改微分
 *      SET base_speed 18   改基准速度（本版循迹上限 22）
 *      GET             看当前参数
 * ========================================================================== */
#include "app/app_trace.h"
#include "app/app_telemetry.h"
#include "app/tuning.h"      /* ⭐ A01 调参总表（唯一出处）：kp/ki/kd/base/慢速窗口 */
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"
#include "app/app_log.h"       /* 板载 RAM 快照（跑车时插不了串口，靠它事后读）*/
#include "drv/drv_hmi.h"       /* 精停蜂鸣两声 */
#include "drv/drv_encoder.h"    /* 终点精停靠编码器计距 */
#include "bsp/bsp_board.h"      /* Bsp_FmtFix：不用浮点 printf（省 ~6KB）*/

#include <stdio.h>

/* ===================== A01 调参区：只改右侧数值 =====================
 * PWM 是占空比命令，不等于实测轮速；_MS 单位毫秒。改宏后需重新编译烧录。
 * kp/ki/kd 与在线 base_speed 属于 Tlm_Params，可用 SET 临时调整；
 * 它们的上电默认值定义在 app_telemetry.c，TR_BASE_MAX 仍约束 A01 上限。 */
/* ⭐ 下面 4 个值现在来自 app/tuning.h（唯一出处）——不要在别处再写一份 */
#define TR_BASE_DEFAULT    A01_BASE_SPEED   /* 来自 tuning.h */
#define TR_BASE_MAX        A01_BASE_MAX     /* 来自 tuning.h */
#define TR_ALL_WHITE_PWM   26      /* 灰度正常但八路全白：左右轮各自的直行 PWM */


/* §1b 旋转数值：与 A02 回线共用（单一出处 tuning.h §3.4）*/
#if A01_LOST_SEARCH
#define TR_ROT_L_PWM       A02_ROT_L_PWM
#define TR_ROT_R_PWM       A02_ROT_R_PWM
#endif

/* ================= 回线后「慢速 A01」（数值在 app/tuning.h §2）=================
 * A02 交还控制权后，先在慢速下跑 A01_SLOW_MS，基准 PWM 从 A01_SLOW_PWM0 线性升到
 * A01_SLOW_PWM1，窗口内可用 A01_SLOW_KP/KI/KD 单独整定；窗口结束自动回 base_speed。
 * 窗口内只压【共同速度】，差速照算（该修方向照修，只是整体慢）。任何参数设 0 = 关闭。
 * 日志：$E 85 `SLOW A01` 开始 / $E 86 `SLOW A01 end` 结束。*/
/* ================= 回线后「慢速 A01」：从起始速度过渡到结束速度 =================
 * 场景：A02 绕完障看到黑线、把控制权交回 A01 的那一瞬，车还是斜的 / 带着惯性；
 *       以前是**直接满速 A01** 接手 ⇒ 第一拍就按满速去抢方向，回线那一下容易甩。
 * 做法：回线（A02 交还，或 A01 自己丢线找回）之后 TR_SLOW_MS 毫秒内，
 *       A01 的基准 PWM 从 TR_SLOW_PWM0 **线性**过渡到 TR_SLOW_PWM1；
 *       窗口结束后自动交回正常 base_speed（速度斜坡会接着往上爬）。
 *
 *        速度
 *   PWM1 -+                          /   <- 窗口末尾（到点交回 base_speed）
 *         |                        /
 *         |                     /
 *   PWM0 -+------------------/
 *         +--+--------------+--------- 时间
 *            0        TR_SLOW_MS
 *
 *   例：PWM0 25 / PWM1 30 / MS 500u  ⇒ 0ms 25%、250ms 27.5%、500ms 30%
 *   想"固定 25" ⇒ PWM0 与 PWM1 都填 25
 *
 * 关掉：TR_SLOW_MS = 0，或 PWM0 / PWM1 任一填 0 ⇒ 直接不开窗口，
 *       行为与加这个功能之前完全一样（也不会去动 PID / 速度）。
 * ⚠️ 别填 1~7：窗口内只降「共同速度」，而差速上限 = base ⇒ 太小会"打舵无力"
 *    （电机低速死区实测约 5%，8% 是留余量的下限），下面用编译期检查拦住。
 * ⚠️ 窗口只【压低】不抬高：若当前 base_speed 比插值更慢，就按更慢的走。
 * 日志里看：$E 85 `SLOW A01` 开始 / $E 86 `SLOW A01 end` 结束；$T 的 pwm 会明显压低。*/
/* ⭐ 这三个值也来自 app/tuning.h（A01_SLOW_MS / A01_SLOW_PWM0 / A01_SLOW_PWM1）*/
#define TR_SLOW_MS         A01_SLOW_MS      /* 来自 tuning.h */
#define TR_SLOW_PWM0       A01_SLOW_PWM0    /* 来自 tuning.h */
#define TR_SLOW_PWM1       A01_SLOW_PWM1    /* 来自 tuning.h */
#define TR_POS_CENTER      3.5f    /* 8 路探头编号 0..7 的中心，不是车速 */
#define TR_WIDE_BLACK_N    5u      /* 同时 >=5 路黑按宽黑区处理，不用质心拐弯 */
#define TR_WIDE_HOLD_MS    80u     /* 离开宽黑区后保持直行的时间 */

/* 终点：全黑连续帧 + 窄线武装时间 + 最小里程三道条件。 */
#define TR_MM_PER_CNT_DEF  0.115f  /* 名义 mm/编码器计数；现场 DISTCAL 可临时改 */

/* 控制器：需要调弯道灵敏度时先用 SET kp/ki/kd，以下是内部门槛。 */
#define TR_STEER_SLEW      2.0f    /* 每拍舵量最大变化的基数 */
#define TR_I_MAX          25.0f   /* 积分项最大贡献，防积分饱和 */
#define TR_I_START_MS     300u    /* 起步后延迟启用积分 */
#define TR_D_ALPHA        0.12f   /* 数字灰度误差微分的低通系数 */
#define TR_ANA_DIV        2u      /* ANALOG 1 时每 N 拍读取一次模拟灰度 */
#define TR_ANA_SPAN_MIN   30.0f   /* 黑白基线差不足时退回数字灰度 */
#define TR_GYROD_K        0.12f   /* GYROD 1 时的偏航阻尼系数 */
/* ===================== 参数区结束：以下为运行状态与控制逻辑 ===================== */

static uint8_t s_online;          /* 灰度是否在线（不在线就发模拟数据，保住 P01 链路）*/
static uint8_t s_drive;           /* ⚠️ 0 = 只算不发（默认，安全）；1 = 真的驱动电机 */
static int     s_state;           /* 上报用：0 = 丢线停车，1 = 正常巡线，4 = 精停 */

/* 终点精停与 PID 的历史调参依据见项目记录；下方仅保存运行状态。 */

static uint8_t  s_fin;            /* 0=巡线 1=走最后一段 2=已停 */
static uint8_t  s_fin_need;       /* 兼容保留（不再使用）*/
static uint32_t s_fin_t0;         /* 进入冲刺段的时刻 */
static uint8_t  s_seek;           /* 1 = 起步找线阶段（发车后还没看到线）*/
static uint16_t s_track_ms;       /* 连续“在线上跑”的毫秒数（积分启用门槛用）*/
static uint32_t s_run_t0;         /* 本趟发车时刻（终点判定的 25s 时间闸）*/
static uint32_t s_fin_black_ms[8];/* 各路"最后一次变黑"的时刻（300ms 窗口判定用）*/
static uint8_t  s_fin_swept;      /* 1 = 窗口内八路都黑过（线已从阵列下扫过）*/
static uint32_t s_wide_until_ms;  /* 离开宽黑区后短暂直行的实际截止时刻 */
static uint32_t s_startt_t0;      /* 上一次报“经过起始线 T 头”的时刻（限流：2s 最多一条）*/
/* 名义编码器尺度，允许 DISTCAL 在线覆盖；断电后回到顶部默认值。 */
static float    s_mm_per_cnt = TR_MM_PER_CNT_DEF;
static uint32_t s_online_t0;      /* 上次重探灰度的时刻（离线时每 500ms 探一次）*/
static uint8_t  s_offline_warn;   /* 离线警告计数（降噪：只在第 1 次和每 5 秒提醒）*/
static float    s_err_prev;       /* 上一拍 err（算 D 项用）*/
static uint32_t s_ms_prev;        /* 上一拍时刻（D 项要用真实 dt，不能用假设的 10ms）*/
static float    s_d_filt;         /* 数字灰度量化跳变的一阶低通，单位 err/s */
static float    s_last_line_err;  /* 正常丢线前最后看到线的方向 */
static float    s_last_line_err_raw; /* ⭐ 同一帧、宽黑 clamp **之前**的误差：丢线判方向只用它 */
static uint32_t s_slow_until_ms;  /* ⭐ 慢速 A01 窗口的截止时刻（0 = 不在窗口内）*/
static uint32_t s_slow_t0;        /* 慢速 A01 窗口的起始时刻（线性插值用）*/
static uint32_t s_white_t0;       /* 本次连续全白的起点（丢线刹停用）*/
static uint8_t  s_white_run;      /* 1 = 正在连续全白 */
static uint8_t  s_lost_stop;      /* 1 = 已因丢线锁存刹车（直到下一趟发车）*/
static int      s_base_actual;    /* 有斜率限制的共同速度 */
static uint8_t  s_launch_fast;     /* 1 = 发车第一拍直接给到目标速度（不做 0→目标 的爬升）*/
static uint8_t  s_d_valid, s_lost, s_risk, s_clear_frames;

/* ---- §1b 丢线兜底（参数在 tuning.h）---- */
#if A01_LOST_SEARCH
typedef enum { LP_IDLE = 0, LP_HOLD, LP_STOPWAIT, LP_ROTATE, LP_VERIFY } LostPhase;
static volatile uint8_t s_lost_cnt;   /* 本趟找回次数（诊断）*/
static uint8_t   s_lost_mode;      /* 0=保守；1=停车旋转找线 */
static int8_t    s_lost_turn;      /* +1=车头右摆；-1=左摆 */
static uint8_t   s_lost_active;    /* 1=找回进行中（接管电机）*/
static LostPhase s_lost_phase;
static uint32_t  s_lost_phase_ms;
static int       s_lost_pwm_l, s_lost_pwm_r;
static uint32_t  s_all8_ms;        /* 最近一次「同一拍八路全黑」时刻 */
#endif

/* ---- v3 新增状态（接管“段落感”：斜坡限速 + 小积分 + 可选的模拟量/陀螺仪）---- */
static float    s_ierr;           /* 积分累加器（err·s）*/
static float    s_steer_out;      /* 斜坡限速后已施加的舵量 */
static uint8_t  s_analog;         /* ANALOG 0|1：用模拟量求误差 */
static uint8_t  s_gyrod;          /* GYROD 0|1：陀螺仪阻尼（默认关、未验证）*/
static uint8_t  s_ana_div;
static uint8_t  s_ana_w[8];       /* 每路“白”基线（自标定）*/
static uint8_t  s_ana_b[8];       /* 每路“黑”基线（自标定）*/
static uint8_t  s_ana_ok;         /* 本次 err 是否来自模拟量 */
static float    s_ana_err;        /* 模拟量误差缓存 */

void    Trace_SetAnalog(uint8_t on) { s_analog = on ? 1u : 0u; }
uint8_t Trace_GetAnalog(void)       { return s_analog; }
void    Trace_SetGyroD(uint8_t on)  { s_gyrod = on ? 1u : 0u; }
uint8_t Trace_GetGyroD(void)        { return s_gyrod; }

/* 模拟量 → 连续位置误差（−3.5 ~ +3.5）
   自标定：拿数字位当“老师”——数字说“黑”就把该路模拟值往黑基线拉（反之向白）
   ⇒ 不需人工标定，也不依赖传感器极性（用 |a−白|/|白−黑| 取幅值）；
   基线差点不够（TR_ANA_SPAN_MIN）时**自动退回数字量** ✓*/
static float tr_err_from_analog(const uint8_t a[8], const int s[8])
{
    const float alpha = 0.10f;
    float sum = 0.0f, wsum = 0.0f;
    for (int i = 0; i < 8; i++) {
        if (s[i]) {
            s_ana_b[i] = (uint8_t)((float)s_ana_b[i] * (1.0f - alpha) + (float)a[i] * alpha);
        } else {
            s_ana_w[i] = (uint8_t)((float)s_ana_w[i] * (1.0f - alpha) + (float)a[i] * alpha);
        }
        float span = (float)s_ana_w[i] - (float)s_ana_b[i];
        if (span < 0.0f) { span = -span; }
        float black = 0.0f;
        if (span >= TR_ANA_SPAN_MIN) {
            black = (float)a[i] - (float)s_ana_w[i];
            if (black < 0.0f) { black = -black; }
            black /= span;
            if (black > 1.0f) { black = 1.0f; }
            black = black * black;          /* 平方：压低噪声、突出真正压线的那几路 */
        }
        if (black > 0.05f) { sum += black * (float)i; wsum += black; }
    }
    if (wsum < 0.15f) { s_ana_ok = 0u; return 0.0f; }
    s_ana_ok = 1u;
    return sum / wsum - TR_POS_CENTER;
}

static void tr_reset_ctrl(void)
{
    s_d_valid = 0u;
    s_d_filt = 0.0f;
    s_ms_prev = 0u;
    s_ierr = 0.0f;            /* v3：积分与斜坡状态也清（防上一段带到下一段）*/
    s_steer_out = 0.0f;
}

/* 保留差速：先限制舵量，再让共同速度给舵量让位。 */
static void tr_mix(int base, int steer, int *l, int *r)
{
    if (steer > 100) { steer = 100; }
    if (steer < -100) { steer = -100; }
    int room = 100 - (steer < 0 ? -steer : steer);
    if (base > room) { base = room; }
    if (base < 0) { base = 0; }
    /* 循迹时只允许内轮减速到零；大误差不能命令反转。 */
    if (steer > base) { steer = base; }
    if (steer < -base) { steer = -base; }
    *l = base + steer;
    *r = base - steer;
}



static uint16_t s_fin_push_ms = A01_FIN_PUSH_MS;   /* 冲刺段时长（STOPDIST 可在线改）*/

void Trace_DistCal(float meters)
{
    int16_t l = En_Count(ENC_L), r = En_Count(ENC_R);
    int32_t a = (l < 0 ? -(int32_t)l : (int32_t)l);
    int32_t b = (r < 0 ? -(int32_t)r : (int32_t)r);
    int32_t avg = (a + b) / 2;
    if (avg <= 0 || meters <= 0.0f) {
        printf("# DISTCAL 失败：先 ENCRESET，推车走一段，再调本命令\r\n");
        return;
    }
    s_mm_per_cnt = (meters * 1000.0f) / (float)avg;
    {
        char m3[16], mp[16];
        Bsp_FmtFix(m3, sizeof m3, meters, 3, 0);
        Bsp_FmtFix(mp, sizeof mp, s_mm_per_cnt, 4, 0);
        printf("# DISTCAL: 走了 %s m，计数 L=%d R=%d ⇒ mm/计数 = %s\r\n",
               m3, (int)l, (int)r, mp);
    }
}

/* 兼容保留命令名 `STOPDIST`：现在它设的是【终点判定后的冲刺时长 ms】（0 = 立刻刹）*/
void Trace_SetStopDist(int ms)
{
    if (ms < 0)    { ms = 0; }
    if (ms > 1000) { ms = 1000; }    /* 上限 1 秒，防手滑 */
    s_fin_push_ms = (uint16_t)ms;
    printf("# STOPDIST(now push ms) = %u ms, PWM %d%%\r\n",
           (unsigned)s_fin_push_ms, (int)A01_FIN_PUSH_PWM);
}

void Trace_FinReset(void)
{
    s_fin = 0; s_fin_need = 0;
    tr_reset_ctrl();
    s_lost = 0u;
    s_base_actual = 0;
    /* ⭐ 解除「丢线锁存 + 全白计时」。
       原来 s_lost_stop 只有置位、**没有任何清零点** ⇒ 一次丢线刹停之后，
       下一次按按钮发车也会第一拍就被刹停，只能断电（现场就是"车不动了"）。
       发车入口 Trace_StartRun() 先调本函数 ⇒ 这里清就等于"下一趟自动解除"；
       线上还能用串口命令 FINRESET 直接解。*/
    s_lost_stop = 0u;
    s_white_run = 0u;
    s_launch_fast = 1u;
    s_white_t0  = 0u;
#if A01_LOST_SEARCH
    s_lost_mode = 0u; s_lost_turn = 0; s_lost_active = 0u;
    s_lost_phase = LP_IDLE; s_lost_phase_ms = 0u;
    s_lost_pwm_l = s_lost_pwm_r = 0;
    s_all8_ms = 0u;
    s_lost_cnt = 0u;              /* 新一趟：找回次数清零 */
#endif
    printf("# FINRESET ok\r\n");
}

void Trace_FinStatus(void)
{
    const char *nm[3] = { "0=巡线", "1=走最后一段", "2=已刹车停" };
    char mp[16];
    Bsp_FmtFix(mp, sizeof mp, s_mm_per_cnt, 4, 0);
    printf("# FIN %s | 已扫过线=%u | 发车后 %lums / 允许 %ums | 冲刺 %ums @ %d%%\r\n",
           (s_fin < 3u) ? nm[s_fin] : "?", (unsigned)s_fin_swept,
           (unsigned long)(HAL_GetTick() - s_run_t0), (unsigned)A01_FIN_MIN_MS,
           (unsigned)s_fin_push_ms, (int)A01_FIN_PUSH_PWM);
}

/* 把 s[0..7] 打包成一个字节（bit0=第1路，1=黑）—— 供板载快照使用 */
static uint8_t tr_pack(const int s[8])
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        if (s[i]) { b |= (uint8_t)(1u << i); }
    }
    return b;
}

void Trace_SetDrive(uint8_t on)
{
    if (!on && s_drive && s_fin != 2u) { Mo_Stop(); }
    if (!on) {                     /* 收车/停车：立刻中止正在进行的丢线找回 */
#if A01_LOST_SEARCH
        s_lost_active = 0u; s_lost_phase = LP_IDLE;
        s_lost_pwm_l = s_lost_pwm_r = 0;
#endif
    }
    s_drive = on ? 1u : 0u;
}
void Trace_SetSeek(uint8_t on)
{
    s_seek = on ? 1u : 0u;
#if A01_LOST_SEARCH
    if (on) {                      /* A02 交还控制权 ⇒ 取消 A01 的丢线找回动作 */
        s_lost_active = 0u; s_lost_phase = LP_IDLE;
        s_lost_pwm_l = s_lost_pwm_r = 0;
    }
#endif
}

/* ⭐ 让接下来的一段先用「慢速 A01」跑（ms = 0 ⇒ 用顶部 TR_SLOW_MS）。
   典型调用点：main.c 检测到 A02 把控制权交回的那一拍（避障刚回线）。
   这里顺带把 PID/速度清一遍 ⇒ 不会沿用接管前的满速和舵量。*/
void Trace_BeginSlow(uint16_t ms)
{
    uint32_t d = (ms != 0u) ? (uint32_t)ms : (uint32_t)TR_SLOW_MS;
    /* 任一为 0 ⇒ 功能关闭：**完全不动任何状态**（等价于没加这个功能）*/
    if (d == 0u || TR_SLOW_PWM0 <= 0 || TR_SLOW_PWM1 <= 0) { return; }
    tr_reset_ctrl();
    s_base_actual = 0;                           /* 从 0 重新爬，不沿用接管前的满速 */
    s_slow_t0 = HAL_GetTick();
    s_slow_until_ms = HAL_GetTick() + d;
    Tlm_SendEvent(85, "SLOW A01");
}

/* ⭐ 发车统一入口（按钮 / 以后加命令都调它）：复位精停 + 打开【起步找线】
   为什么必须有 seek：起始线是个 T 头，车摆在它【前】面 ⇒ 发车瞬间就是全白，
   旧逻辑会立刻“丢线停车” ⇒ 车一动不动（2026-10-01 场地实测 ✗）*/
void Trace_StartRun(void)
{
    Trace_FinReset();
    s_seek = 1u;
    s_track_ms = 0u;

    s_wide_until_ms = 0u;
    s_run_t0 = HAL_GetTick();         /* 本趟发车时刻（终点 25s 时间闸）*/
    s_fin_swept = 0u;
    for (int i = 0; i < 8; i++) { s_fin_black_ms[i] = 0u; }
    s_last_line_err = 0.0f;
    s_last_line_err_raw = 0.0f;
    s_risk = 0u; s_clear_frames = 0u;
    s_slow_until_ms = 0u;             /* 新一趟：清掉上一趟残留的慢速窗口 */
    s_slow_t0 = 0u;
}
uint8_t Trace_GetDrive(void)       { return s_drive; }
uint8_t Trace_Online(void)         { return s_online; }

void Trace_Init(void)
{
    s_drive  = 0u;                /* ⚠️ 默认不驱动：第一次一定要让轮子悬空再 `TRACE 1` */
    s_state  = 0;
    s_online = Gr_Ping();
    Tlm_Params()->base_speed = TR_BASE_DEFAULT;  /* $P/GET 与实际试跑默认速度一致 */

    printf("# TRACE 灰度=%s 驱动=%s\r\n",
           s_online ? "在线" : "不在线", s_drive ? "开" : "关(只算不发)");
    if (!s_online) {
        printf("#       灰度不在线 -> 先发模拟数据（保住 P01 链路自检）；每 500ms 自动重探，插好会自己切回真实读数 ✓\r\n");
    }
}

#if A01_LOST_SEARCH
/* ===== §1b 丢线找回：判据与动作 =====
 * 居中判据与 A02 回线同一套（掩码见 tuning.h §3.4）：
 *   第 3~6 路有相邻两路黑，且第 1/2/7/8 路全白。*/
static uint8_t tr_center_ok(uint8_t bits)
{
    uint8_t nib = (uint8_t)((bits & A02_MID_MASK) >> 2);
    if ((nib & (uint8_t)(nib >> 1)) == 0u) { return 0u; }
    if ((bits & A02_OUT_MASK) != 0u)       { return 0u; }
    return 1u;
}

/* 每 10ms 单轮最多变 A02_PWM_STEP（与 A02 回线的旋转手感一致）*/
static int tr_lost_slew(int cur, int tgt)
{
    if (cur < tgt - A02_PWM_STEP) { return cur + A02_PWM_STEP; }
    if (cur > tgt + A02_PWM_STEP) { return cur - A02_PWM_STEP; }
    return tgt;
}

/* 全白第一拍调用：定下"要不要找回 / 往哪边转"，之后不再改 */
static void tr_lost_decide(uint32_t ms)
{
    s_lost_mode = 0u;
    s_lost_turn = 0;
    s_lost_active = 0u;
    s_lost_phase = LP_IDLE;
    s_lost_pwm_l = s_lost_pwm_r = 0;

    if (s_all8_ms != 0u && (uint32_t)(ms - s_all8_ms) <= A01_LOST_TBAR_MS) { return; }  /* 刚过黑条 */
    if (s_last_line_err_raw > A01_LOST_DIR_ERR)       { s_lost_mode = 1u; s_lost_turn =  1; }  /* 线在右 ⇒ 右摆 */
    else if (s_last_line_err_raw < -A01_LOST_DIR_ERR) { s_lost_mode = 1u; s_lost_turn = -1; }  /* 线在左 ⇒ 左摆 */
    /* 其余：|err| 太小，保持保守 */
}

/* 找回状态机：直行 → 刹停 → 原地旋转 → 居中保持 → 交回循迹（期间独占电机）*/
static void tr_lost_run(uint32_t ms, const int s[8])
{
    uint8_t  bits = tr_pack(s);
    uint32_t el   = (uint32_t)(ms - s_lost_phase_ms);

    switch (s_lost_phase) {
    case LP_HOLD:                                  /* ① 直行跨接缝（设 0 则立刻跳过）*/
        if (el < (uint32_t)A01_LOST_HOLD_MS) {
            s_lost_pwm_l = s_lost_pwm_r = TR_ALL_WHITE_PWM;
            Mo_Set(s_lost_pwm_l, s_lost_pwm_r);
            break;
        }
        s_lost_phase = LP_STOPWAIT;
        s_lost_phase_ms = ms;
        s_lost_pwm_l = s_lost_pwm_r = 0;
        Mo_Brake();
        break;

    case LP_STOPWAIT:                              /* ② 刹停，等前进惯性消掉 */
        s_lost_pwm_l = s_lost_pwm_r = 0;
        Mo_Brake();
        if (el >= (uint32_t)A01_LOST_STOP_WAIT_MS) {
            s_lost_phase = LP_ROTATE;
            s_lost_phase_ms = ms;
        }
        break;

    case LP_ROTATE: {                              /* ③ 原地旋转，等线扫回中间 */
        int rl = (s_lost_turn > 0) ? (int)TR_ROT_L_PWM : (int)TR_ROT_R_PWM;
        int rr = (s_lost_turn > 0) ? (int)TR_ROT_R_PWM : (int)TR_ROT_L_PWM;

        if (tr_center_ok(bits)) {                  /* 线已居中 ⇒ 先刹停，进入复核 */
            s_white_run = 0u;   /* 已确认真看到线：全白计时作废，下次全白重新判 */
            s_lost_phase = LP_VERIFY;
            s_lost_phase_ms = ms;
            s_lost_pwm_l = s_lost_pwm_r = 0;
            Mo_Brake();
            break;
        }
        if (el >= (uint32_t)A01_LOST_ROT_TIMEOUT_MS) {   /* 转一圈仍找不到 ⇒ 锁存停车 */
            s_lost_active = 0u;
            s_lost_phase = LP_IDLE;
            s_lost_pwm_l = s_lost_pwm_r = 0;
            s_lost_stop = 1u;
            s_state = 0;
            Mo_Brake();
            Tlm_SendEvent(88, "LOST rotate timeout -> BRAKE");
            Log_Note(bits, 0.0f, 0, 0, 0);
            Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
            return;
        }
        s_lost_pwm_l = tr_lost_slew(s_lost_pwm_l, rl);
        s_lost_pwm_r = tr_lost_slew(s_lost_pwm_r, rr);
        Mo_Set(s_lost_pwm_l, s_lost_pwm_r);
        break;
    }

    case LP_VERIFY:                                /* ④ 居中后保持，中途丢了就回去转 */
        s_lost_pwm_l = s_lost_pwm_r = 0;
        Mo_Brake();
        if (!tr_center_ok(bits)) {
            s_lost_phase = LP_ROTATE;
            s_lost_phase_ms = ms;
        } else if (el >= (uint32_t)A01_LOST_CENTER_HOLD_MS) {
            s_lost_active = 0u;                    /* 交回循迹（下一拍自动进 §2 慢速窗口）*/
            s_lost_phase = LP_IDLE;
            s_lost = 1u;
            /* ⭐ 允许"连续找回"：把全白计时与黑条签名都清掉，
               这样紧接着又全白时会**重新判一次方向**（而不是沿用上一次的方向）。*/
            s_white_run = 0u;
            s_white_t0  = 0u;
            s_all8_ms   = 0u;
            Tlm_SendEvent(88, "LOST recovered -> A01 trace (slow)");
        }
        break;

    default:
        s_lost_active = 0u;
        s_lost_phase = LP_IDLE;
        s_lost_pwm_l = s_lost_pwm_r = 0;
        Mo_Brake();
        break;
    }

    s_lost = 1u;
    s_state = 6;
    Log_Note(bits, 0.0f, s_lost_pwm_l, s_lost_pwm_r, 6);
    Tlm_SendT(ms, s, 0.0f, s_lost_pwm_l, s_lost_pwm_r, 6);
}
#endif /* A01_LOST_SEARCH */

void Trace_Step(uint8_t allow_drive)
{
    uint32_t ms = HAL_GetTick();
    Params  *p  = Tlm_Params();
    int      s[8];
    float    err;

    /* 丢线锁存：一直保持刹车，直到下一次发车（Trace_StartRun 清标志）*/
    if (s_lost_stop) {
        for (int i = 0; i < 8; i++) { s[i] = 0; }
        Mo_Brake();
        Log_Note(0u, 0.0f, 0, 0, 0);
        Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
        s_state = 0;
        return;
    }

    if (s_online) {
        /* ---- 读灰度（bit0 = 第 1 路 … bit7 = 第 8 路）----
           ⚠️ 模块的原始极性是 1 = 白、0 = 黑（2026-09-30 成车实测确认）；
              $T 帧的约定是 1 = 黑 —— 在这里统一，算法只认 s[] 这个约定。 */
        uint8_t d = 0;
        if (!Gr_ReadDigital(&d)) {           /* I2C 读失败：绝不能拿旧数据瞎跑 */
            tr_reset_ctrl(); s_lost = 0u; s_base_actual = 0;
            for (int i = 0; i < 8; i++) { s[i] = 0; }
            if (s_drive && allow_drive) { Mo_Stop(); }
            Log_Note(tr_pack(s), 0.0f, 0, 0, 0);
            Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
            s_state = 0;
            return;
        }
        int sum = 0, cnt = 0;
        for (int i = 0; i < 8; i++) {
            s[i] = ((d >> i) & 1u) ? 0 : 1;  /* 1 = 黑 */
            if (s[i]) { sum += i; cnt++; }   /* 只统计「看到黑」的那几路 */
        }

        /* ---- v3 ② 模拟量求连续误差（ANALOG 1 时；50Hz 读一次，其余拍用缓存）----
           数字灰度只能给 0.125 的台阶（实测 err 中位 0.48 秒才变一次 ✗）；
           模拟量是连续值 ⇒ err 每拍都在动 ⇒ 舵量不再“半秒踹一脚” ✓
           自标定没就绪 / 读失败 ⇒ 自动退回数字量（见 tr_err_from_analog 里的 TR_ANA_SPAN_MIN）*/
        if (s_analog) {
            if ((s_ana_div++ % TR_ANA_DIV) == 0u) {
                uint8_t a[8];
                if (Gr_ReadAnalog(a)) {
                    float e2 = tr_err_from_analog(a, s);
                    if (s_ana_ok) { s_ana_err = e2; }   /* 只覆盖“有效”的（没就绪就不用）*/
                } else {
                    s_ana_ok = 0u;                       /* 读失败：退回数字量 */
                }
            }
            /* 不是采样拍：保留上一次的 s_ana_ok / s_ana_err（缓存）*/
        } else {
            s_ana_ok = 0u;
        }

        /* ============ ① 终点线检测 + 精停状态机（**必须先于“丢线”判断**）============
           ⚠️ 2026-10-01 场地实踩：精停走最后一段时，车冲过终点 T 头后必然进入【全白】，
              而旧的“全白⇒丢线⇒停车”会把这个阶段打断 ⇒ 精停走了 0mm 就停下 ✗
              （日志证据：第 4194 帧全黑触发，第 4202 帧全白被丢线逻辑接管）*/

/* ⭐⭐ 2026-10-01 晚【v6 关键修复】：武装必须【锁存】
   踩的坑（自找的 ✗）：①-1 里“遇到全黑就 s_track_ms = 0”，而 ①-2 紧接着用 s_track_ms 判断是否已武装
   ⇒ **全黑的同一拍里先清零、再拿它判断** ⇒ 判断永远为假 ⇒ **精停永远不会触发** ✗✗
   证据：场地那趟 8 路全黑持续 8 秒（268 帧）也毫无反应，整趟 state 里没有 4 ✓
   修法：用【锁存标志】s_fin_armed（跑够时间置 1，只有新一趟开始时才清 ✓）*/

/* ①-1 在窄线上累计时间：只给"积分项启用门槛"用（不再用于终点判定）*/
if (cnt >= 1 && cnt <= 3) {
    if (s_track_ms < 60000u) { s_track_ms += 10u; }
} else {
    s_track_ms = 0u;   /* 离开窄线后重新累计 */
}

/* ①-1b 用真实时钟算保持期；主循环被超声波读数阻塞时也不会延长盲走。 */
if (cnt >= (int)TR_WIDE_BLACK_N) { s_wide_until_ms = ms + TR_WIDE_HOLD_MS; }
#if A01_LOST_SEARCH
/* 同一拍八路全黑 = 压过横向黑条（T 头/标识）的签名，用于区分终点与脱线 */
if (cnt >= 8) { s_all8_ms = ms; }
#endif

        /* ①-2 终点判定（两段，缺一不可）：
         *   ⓐ A01_FIN_WINDOW_MS 窗口内，**八路分别都黑过一次**（不必同时）
         *      ⇒ 说明整条终点标识刚从阵列下面扫过
         *   ⓑ 紧接着出现**八路全白**（线已冲出阵列）⇒ 认定过终点 ⇒ 立即刹车
         *   + 时间闸：发车满 A01_FIN_MIN_MS（25s）才允许；之前只打 $E 83 不动作
         * ⚠️ 与 A02 回线的"过线全白刹停"是两回事：那个在避障接管期间由 A02 判。
         * ⚠️ 用"实时窗口"而不是锁存：否则起点 T 头会一直被记住，25s 后随便一次
         *    丢线全白就被误判成终点。*/
        {
            for (int i = 0; i < 8; i++) {
                if (s[i]) { s_fin_black_ms[i] = ms; }
            }
            uint8_t swept = 1u;
            for (int i = 0; i < 8; i++) {
                if ((uint32_t)(ms - s_fin_black_ms[i]) > A01_FIN_WINDOW_MS) { swept = 0u; break; }
            }
            s_fin_swept = swept;                       /* 只用于 FINSTAT 显示 */

            if (s_fin == 0u && swept && cnt == 0) {
                if ((uint32_t)(ms - s_run_t0) >= A01_FIN_MIN_MS) {
                    s_fin = 1u;                        /* 进入冲刺段 */
                    s_fin_t0 = ms;
                    Tlm_SendEvent(80, "FINISH LINE");
                } else if ((ms - s_startt_t0) >= 2000u) {
                    s_startt_t0 = ms;
                    Tlm_SendEvent(83, "FIN ignored: <A01_FIN_MIN_MS");
                }
            } else if (s_fin == 0u && cnt >= 8) {
                if ((ms - s_startt_t0) >= 2000u) {
                    s_startt_t0 = ms;
                    Tlm_SendEvent(83, "START T ignored");
                }
            }
        }

        /* ①-3 精停状态机（驱动/超时/刹车都在这收口，并直接 return，
              不让下面的“丢线停车 / 循迹 PID”插手）*/
        if (s_fin != 0u) {
            tr_reset_ctrl(); s_lost = 0u; s_base_actual = 0;


            if (s_fin == 1u) {
                /* 冲刺段：直行 A01_FIN_PUSH_MS，PWM = A01_FIN_PUSH_PWM（左右相同）*/
                if (s_fin_push_ms == 0u ||
                    (uint32_t)(ms - s_fin_t0) >= (uint32_t)s_fin_push_ms) {
                    s_fin = 2u;                              /* 时间到 ⇒ 刹车停 */
                    if (s_drive && allow_drive) { Mo_Brake(); }
                    Trace_SetDrive(0u);                      /* 关发车开关，防继续跑 */
                    Log_Stop();                              /* 停止记录 + 自动存 Flash */
                    Tlm_SendEvent(81, "FINISH STOP");
                    Hmi_Beep(120); HAL_Delay(90); Hmi_Beep(120);   /* 两声=到终点 */
                } else if (s_drive && allow_drive) {
                    Mo_Set(A01_FIN_PUSH_PWM, A01_FIN_PUSH_PWM);   /* 冲刺 */
                }
            } else {
                if (s_drive && allow_drive) { Mo_Brake(); }  /* 状态 2：保持刹车 */
            }
            s_state = 4;                                     /* $T 的 state=4 ⇒ 精停 ✓ */
            /* ⭐ 上报【实际】占空比：状态 2 是刹车 ⇒ 报 0，别再报"打算给的 spd"
               （以前两种状态都报 spd，会让事后分析误以为"命令了却没走"）*/
            {
                int rl = (s_fin == 1u) ? (int)A01_FIN_PUSH_PWM : 0;
                int rr = (s_fin == 1u) ? (int)A01_FIN_PUSH_PWM : 0;
                Log_Note(tr_pack(s), 0.0f, rl, rr, 4);
                Tlm_SendT(ms, s, 0.0f, rl, rr, 4);
            }
            return;
        }

        /* ①-4 找回进行中：接管电机，跳过下面的 ②/PID */
#if A01_LOST_SEARCH
        if (s_lost_active) { tr_lost_run(ms, s); return; }
#endif

        /* ============ ② 八路全白 ============*/
        if (cnt == 0) {
            /* 精停已在上面处理。
             * ① 起步找线（s_seek=1）：正常全白 ⇒ 26/26 直行找线（设计行为）
             * ② 行驶中意外全白（丢线）：先直行 A01_LOST_STOP_MS 抢救，
             *    仍全白 ⇒ Mo_Brake() 刹停并**锁存**（直到下一次发车）
             * ③ 未驱动（TRACE 0 / 避障接管）：只上报，不动电机 */
            tr_reset_ctrl(); s_base_actual = 0;

            if (!s_white_run) {
                s_white_run = 1u; s_white_t0 = ms;
#if A01_LOST_SEARCH
                tr_lost_decide(ms);      /* 本段全白的第一拍：定"要不要找回 / 往哪边转" */
#endif
            }

            if (s_drive && allow_drive && !s_seek) {
                /* 意外全白：能判出方向就停车旋转找线，否则直行后刹停（见 tuning.h §1b）*/
#if A01_LOST_SEARCH
                if (s_lost_mode != 0u) {
                    if (!s_lost_active) {          /* 进入找回：从"直行跨接缝"阶段开始 */
                        s_lost_active   = 1u;
                        s_lost_phase    = LP_HOLD;
                        s_lost_phase_ms = ms;
                        s_lost_pwm_l = s_lost_pwm_r = 0;
                        Tlm_SendEvent(88, (s_lost_turn > 0)
                                          ? "LOST: car LEFT -> rotate RIGHT"
                                          : "LOST: car RIGHT -> rotate LEFT");
                        if (s_lost_cnt < 255u) { s_lost_cnt++; }   /* 每次找回 +1（诊断）*/
                    }
                    tr_lost_run(ms, s);
                    return;
                }
#endif
                if ((A01_LOST_STOP_MS > 0u) &&
                    (uint32_t)(ms - s_white_t0) >= (uint32_t)A01_LOST_STOP_MS) {
                    Mo_Brake();                       /* 直接刹停（不是滑行）*/
                    s_lost_stop = 1u;
                    s_state = 0;
                    Tlm_SendEvent(88, "LOST LINE -> BRAKE (no direction)");
                    Log_Note(tr_pack(s), 0.0f, 0, 0, 0);
                    Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
                    return;
                }
                s_lost = 1u;
                s_state = 6;
                Mo_Set(TR_ALL_WHITE_PWM, TR_ALL_WHITE_PWM);
                Log_Note(tr_pack(s), 0.0f, TR_ALL_WHITE_PWM, TR_ALL_WHITE_PWM, s_state);
                Tlm_SendT(ms, s, 0.0f, TR_ALL_WHITE_PWM, TR_ALL_WHITE_PWM, s_state);
                return;
            }

            if (s_drive && allow_drive && s_seek) {
                /* —— 起步找线：正常全白 —— */
                s_lost = 1u;
                s_state = 5;
                Mo_Set(TR_ALL_WHITE_PWM, TR_ALL_WHITE_PWM);
                Log_Note(tr_pack(s), 0.0f, TR_ALL_WHITE_PWM, TR_ALL_WHITE_PWM, s_state);
                Tlm_SendT(ms, s, 0.0f, TR_ALL_WHITE_PWM, TR_ALL_WHITE_PWM, s_state);
                return;
            }

            s_lost = 0u;
            Log_Note(tr_pack(s), 0.0f, 0, 0, 0);
            Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
            s_state = 0;
            return;
        }
        s_white_run = 0u;            /* 看到黑 ⇒ 连续全白被打断 */
        if ((s_seek && cnt < 8) || s_lost || (s_state != 1 && !s_seek)) {
            tr_reset_ctrl(); s_base_actual = 0;
            s_risk = 2u; s_clear_frames = 0u;
            /* ⭐ 丢线后找回线：先慢速 TR_SLOW_MS，稳住再放满速
               （起步找线 s_seek 不额外限速，保持原有起步手感）*/
            if (s_lost && TR_SLOW_MS > 0u && TR_SLOW_PWM0 > 0 && TR_SLOW_PWM1 > 0) {
                if (s_slow_until_ms == 0u) { Tlm_SendEvent(85, "SLOW A01"); }
                s_slow_t0 = ms;
                s_slow_until_ms = ms + (uint32_t)TR_SLOW_MS;
            }
        }
        if (cnt < 8) { s_seek = 0u; } /* 起点全黑 T 头不代表已找到窄线 */
        s_lost = 0u;                 /* 找回线后重新起速 */
        /* 位置误差 = 黑点的平均位置 − 中心（负 = 线偏左，要往左修）*/
        err = (float)sum / (float)cnt - TR_POS_CENTER;
        if (s_analog && s_ana_ok) { err = s_ana_err; }   /* v3 ②：模拟量优先 */
            s_last_line_err_raw = err;   /* 先存未 clamp 的值（宽黑规则会把 err 置 0）*/
        /* ⭐ v5/v6：宽黑区（T 头 / 标识 / 大黑块）不当线看 —— 走直线，且【出来后继续直行一小段】
           为什么还要“保持”：实测过完 T 头时车常是斜着的，退出黑区那几帧会出现“只剩最边上一路黑”
           ⇒ 被当成 err=±3.5 而猛拐一下 ✗（场地日志 2656~2659 帧：4→3→1 路黑，pwm 一路打到 65/−43）*/
        if (cnt >= (int)TR_WIDE_BLACK_N ||
            (s_wide_until_ms != 0u && (int32_t)(ms - s_wide_until_ms) < 0)) {
            err = 0.0f;
        }
    } else {
        /* ---- 灰度不在线：只做链路自检，**绝不驱动电机** --------------------
           [!][!] 2026-10-01 实踩（当天"车完全不听灰度、左右乱打舵"的真因）：
             ① 原本 `s_online` 只在 Trace_Init() 探一次 ⇒ 开机瞬间 5V 未稳/模块未就绪，
                就【永久】判为离线（开机提示却写着"接上就自动走真实读数"，是假的）；
             ② 更危险的是：模拟数据【照样驱动电机】⇒ 车会去追一条【假线】
                （三角波在左右匀速扫、err 到 ±5.0）⇒ 表现为"不听灰度、左右乱来"。
           现在：离线时每 500ms 重探一次（插好会自动切回真实读数）；
                并把 allow_drive 置 0 ⇒ **假数据永远不许开车**。*/
        if ((ms - s_online_t0) >= 500u) {
            s_online_t0 = ms;
            if (Gr_Ping()) {
                s_online = 1u;
                tr_reset_ctrl(); s_lost = 0u; s_base_actual = 0;
                s_offline_warn = 0u;
                Tlm_SendEvent(92, "GRAY_ONLINE");
                printf("# 灰度上线了 ok，切回真实读数（之前一直是模拟数据）\r\n");
                return;                       /* 下一拍就走真实读数 */
            }
            if (s_offline_warn == 0u || (s_offline_warn % 10u) == 0u) {
                Tlm_SendEvent(93, "GRAY_OFFLINE");
                printf("# [!] 灰度不在线：只发模拟数据、**电机绝不驱动**（查 5V 供电 / 排线 / 安装高度）\r\n");
            }
            s_offline_warn = (uint8_t)(s_offline_warn + 1u);
        }
        allow_drive = 0u;                     /* 假数据永远不许开车 */
        tr_reset_ctrl(); s_lost = 0u; s_base_actual = 0;
#if A01_LOST_SEARCH
        s_lost_active = 0u; s_lost_phase = LP_IDLE;   /* 灰度掉了：立刻中止丢线找回 */
        s_lost_pwm_l = s_lost_pwm_r = 0;
#endif
        if (s_drive) { Mo_Stop(); }           /* 顺手把电机停掉 */

        /* ---- 模拟数据（仅用于验证链路）---- */
        int32_t k   = (int32_t)((ms / 10u) % 200u);
        int32_t e10 = (k < 100) ? (k - 50) : (150 - k);
        int center  = 3 + (int)(e10 / 25);
        if (center < 0) { center = 0; }
        if (center > 7) { center = 7; }
        for (int i = 0; i < 8; i++) { s[i] = (i == center || i == center + 1) ? 1 : 0; }
        err = (float)e10 / 10.0f;
    }

    /* ---- 巡航上限 + 曲率风险：量化误差需连续稳定才升速 ---- */
    int base = p->base_speed;
    if (base <= 0) { base = TR_BASE_DEFAULT; }
    if (base > TR_BASE_MAX) { base = TR_BASE_MAX; }

    /* ---- 慢速 A01 窗口：PWM 从 TR_SLOW_PWM0 线性过渡到 TR_SLOW_PWM1 ----
       只压【共同速度】，差速照常算 ⇒ 该修的方向照修，只是整体慢下来。
       ⚠️ 三道守门，保证永远不会把 base 压成 0（不会停车）：
          ① 只有窗口被翻开时才会走到这里（翻开时已要求两个 PWM 都 >0）
          ② 这里再要求两个 PWM 都 >0、时长 >0
          ③ 插值结果只往下压（当前 base 更小时按 base 走），不会抬到巡航以上 */
    uint8_t in_slow = 0u;                    /* 1 = 处于「慢速 A01」窗口内 */
    if (s_slow_until_ms != 0u) {
        in_slow = 1u;
        if ((int32_t)(s_slow_until_ms - ms) > 0) {
            if (TR_SLOW_PWM0 > 0 && TR_SLOW_PWM1 > 0 && TR_SLOW_MS > 0u) {
                uint32_t off  = ms - s_slow_t0;                  /* 窗口内已走毫秒 */
                int32_t  span = (int32_t)TR_SLOW_PWM1 - (int32_t)TR_SLOW_PWM0;
                int32_t  slow = (int32_t)TR_SLOW_PWM0 +
                                span * (int32_t)off / (int32_t)TR_SLOW_MS;
                if (slow < 1) { slow = 1; }                      /* 双保险：不会到 0 */
                if (slow < base) { base = (int)slow; }
            }
        } else {
            s_slow_until_ms = 0u;               /* 窗口结束：自动回 base_speed */
            Tlm_SendEvent(86, "SLOW A01 end");
        }
    }

    /* ---- P + D（⚠️ D 项必须用【真实 dt】+ 钳位）----
       用假设的固定 10ms 会在“主循环被拖慢”时算出虚假的巨大变化率 ⇒ 车猛打舵 ✗
       （2026-10-01 实踩：I2C2 器件超时把节奏打乱 ⇒ D 项爆表 ⇒ 左右猛一下猛一下）*/
    uint32_t dt_ms = ms - s_ms_prev;
    s_ms_prev = ms;
    if (!s_d_valid || dt_ms == 0u || dt_ms > 50u) {
        s_d_filt = 0.0f;
        s_d_valid = 1u;
        dt_ms = 0u;             /* 重新上线或阻塞太久：这一拍不计算 D */
    }
    float d_rate = dt_ms ? (err - s_err_prev) * 1000.0f / (float)dt_ms : 0.0f;
    s_err_prev = err;
    s_d_filt += TR_D_ALPHA * (d_rate - s_d_filt);
    float ae = err < 0.0f ? -err : err;
    float ad = s_d_filt < 0.0f ? -s_d_filt : s_d_filt;
    uint8_t outer = s[0] || s[7];
    uint8_t risk = (ae >= 2.0f || outer || ad >= 65.0f) ? 2u :
                   (ae >= 0.5f || ad >= 25.0f) ? 1u : 0u;
    /* ⭐⭐ v7（2026-10-01 晚，日志挖出来的关键缺陷）：
       err 在【正常过弯】时会长期为 0（车跟着线转 ✓），于是曲率风险永远判“直道”⇒ 不降速 ✗
       实例：1838 那趟第 1980~1986 帧，err=0、pwm=20/20，而陀螺仪 gz 已经 14→34→44→60 °/s
             ⇒ 车以满速冲进急弯 ⇒ 第 1987 帧直接全白丢线 ✗
       ⇒ 用【陀螺仪偏航率】当弯道信号（它是唯一能看出“正在转弯”的量 ✓）提前降速 ✓ */
    {
        /* ⚠️ 只在【有效 + 新鲜】时才采信陀螺仪：
           Log_GetGz() 只是缓存，读失败时**保留旧值** —— 一个陈旧的偏大值会让 risk
           永久 = 2 ⇒ 车一路按 55% 速度跑，现场完全看不出原因（O-10 那颗 MPU 不稳）。
           失效时等价于"没有陀螺"：退回 err / 最外两路黑 的判据（= 加陀螺之前的行为）。
           Log_GetGyro() 只读结构体，不碰 I2C、不阻塞。*/
        LogGyro g;
        Log_GetGyro(&g);
        if (g.valid && (uint32_t)(HAL_GetTick() - g.stamp_ms) < 100u) {
            float gz_dps = (float)g.raw / 131.0f;   /* ±250°/s 量程 ⇒ 131 LSB/(°/s) */
            float agz = (gz_dps < 0.0f) ? -gz_dps : gz_dps;
            if (agz >= 100.0f)                    { risk = 2u; }
            else if (agz >= 55.0f && risk < 1u)   { risk = 1u; }
        }
    }
    if (risk > s_risk) { s_risk = risk; s_clear_frames = 0u; }
    else if (risk < s_risk) {
        if (++s_clear_frames >= 8u) { s_risk = risk; s_clear_frames = 0u; }
    } else { s_clear_frames = 0u; }
    int target = base;
    if (s_risk == 1u) { target = base * 3 / 4; }
    if (s_risk == 2u) { target = base * 11 / 20; }
    if (target < 8) { target = 8; }
    if (s_launch_fast) {                 /* ⭐ 发车第一拍：直接就位，不要 0→目标 的爬升等待 */
        s_base_actual = target;
        s_launch_fast = 0u;
    }
    if (s_base_actual < target) { s_base_actual += 1; if (s_base_actual > target) { s_base_actual = target; } }
    if (s_base_actual > target) { s_base_actual -= 3; if (s_base_actual < target) { s_base_actual = target; } }
    s_last_line_err = err;
    /* 慢速窗口内可用 A01_SLOW_KP/KI/KD 单独调；窗口外一律用巡航值 */
    float kp_eff = in_slow ? A01_SLOW_KP : p->kp;
    float ki_eff = in_slow ? A01_SLOW_KI : p->ki;
    float kd_eff = in_slow ? A01_SLOW_KD : p->kd;
    float d_term = kd_eff * s_d_filt;
    if (d_term >  12.0f) { d_term =  12.0f; }
    if (d_term < -12.0f) { d_term = -12.0f; }

    /* ================= v3 ③ 小积分（长弧道自动维持差速）=================
       用户诉求：“大弧线不要一段段直线，而是自然差速走出弯” ——
       本质就是 err 持续偏一侧时，舵量应该**稳定维持一个值**，而不是“拽一下、又直一下”。
       ⇒ 加小积分；带抗饱和（|ki·I| 不超过 TR_I_MAX），且起步头 0.3s 不积（防起步瞬间累积）*/

    if (ki_eff > 0.0001f && s_track_ms > TR_I_START_MS) {
        s_ierr += err * (float)(dt_ms ? dt_ms : 10u) * 0.001f;   /* err*秒（ki=0 时根本不积）*/
    }
    float i_term = ki_eff * s_ierr;
    if (i_term >  TR_I_MAX) { i_term =  TR_I_MAX; if (ki_eff > 0.0001f) { s_ierr = TR_I_MAX / ki_eff; } }
    if (i_term < -TR_I_MAX) { i_term = -TR_I_MAX; if (ki_eff > 0.0001f) { s_ierr = -TR_I_MAX / ki_eff; } }

    float steer_f = kp_eff * err + i_term + d_term;

    /* ================= v3 ④ 陀螺仪阻尼（GYROD 1 时才生效）=================
       拿偏航率抵消一部分舵量，抑制摆动；⚠️ 它也可能抵抗掉“需要持续转向”的弯道，
       所以**默认关**、且标为未验证 —— 实车对比后再决定开不开 ✓*/
    if (s_gyrod) {
        float gz_dps = (float)Log_GetGz() / 131.0f;     /* 原始值 → °/s */
        steer_f -= TR_GYROD_K * gz_dps;
    }

    /* ================= v3 ① 舵量斜坡限速 =================
       实测：err 中位 0.48 秒才变一次，一变就是 ~10 个 duty ⇒ “每半秒踹一脚” ✗
       把目标舵量摊成平滑爬升；|err| 越大允许越快（真遇急弯不迟顿）：
           每拍最大变化 = TR_STEER_SLEW × (1 + |err|)     → 2~9 个 duty / 10ms
       注：限的是“舵量”而不是“轮速”，共同速度（含曲率降速）不受影响 ✓*/
    {
        float scale = (float)s_base_actual / 12.0f;      /* v5：坡度随巡航速度缩放（高速允许更快修正）*/
        if (scale < 1.0f) { scale = 1.0f; }
        float slew = TR_STEER_SLEW * (1.0f + ae) * scale;
        float dstep = steer_f - s_steer_out;
        if (dstep >  slew) { dstep =  slew; }
        if (dstep < -slew) { dstep = -slew; }
        s_steer_out += dstep;
    }
    /* 舵量软限幅：小误差仍有差速，大误差渐近于内轮停转，避免突然反转。 */
    float mag = s_steer_out < 0.0f ? -s_steer_out : s_steer_out;
    float steer_safe = mag > 0.0f ? s_steer_out * (float)s_base_actual /
                                   ((float)s_base_actual + mag) : 0.0f;
    int steer = (int)(steer_safe + (steer_safe >= 0.0f ? 0.5f : -0.5f));

    int l, r;
    tr_mix(s_base_actual, steer, &l, &r); /* 线偏左 → 左轮慢、右轮快 */

    s_state = 1;
    if (s_drive && allow_drive) {
        Mo_Set(l, r);         /* Mo_Set 内部会自动夹到 ±100 */
    }

    /* $T 里的 pwm 是"算法打算给的值"；驱动关掉时电机实际不动 */
    Log_Note(tr_pack(s), err, l, r, s_state);     /* 板载快照（跑完用 DUMP 读）*/
    Tlm_SendT(ms, s, err, l, r, s_state);
}
