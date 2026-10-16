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
#define TR_BASE_DEFAULT    22      /* A01 上电默认基准 PWM */
#define TR_BASE_MAX        22      /* 正常循迹共同速度上限，SET 也不能越过 */
#define TR_ALL_WHITE_PWM   26      /* 灰度正常但八路全白：左右轮各自的直行 PWM */
#define TR_POS_CENTER      3.5f    /* 8 路探头编号 0..7 的中心，不是车速 */
#define TR_WIDE_BLACK_N    5u      /* 同时 >=5 路黑按宽黑区处理，不用质心拐弯 */
#define TR_WIDE_HOLD_MS    80u     /* 离开宽黑区后保持直行的时间 */

/* 终点：全黑连续帧 + 窄线武装时间 + 最小里程三道条件。 */
#define TR_FIN_NEED        3u      /* 连续全黑帧数；100Hz 下 3 帧约 30ms */
#define TR_FIN_ARM_MS      1500u   /* 在线上累计时间，够了才允许识别终点 */
#define TR_FIN_MIN_MM      800     /* 名义累计里程门槛；未精标，不能当精确测距 */
#define TR_FIN_MM_DEF      80      /* 检出终点后再走的毫米数；STOPDIST 可临时改 */
#define TR_FIN_SPEED_PCT   0       /* 最后一段 PWM；0 表示沿用 base_speed */
#define TR_FIN_MMPS_EST    195     /* 编码器尺度无效时的退化速度估计 mm/s */
#define TR_FIN_TIMEOUT_MS  3000u   /* 最后一段即使无编码器进度也必须刹车 */
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
static uint8_t  s_fin_need;       /* 全黑连续帧计数 */
static int32_t  s_fin_mm;         /* 最后一段已走 mm */
static int16_t  s_fin_l0, s_fin_r0;   /* 进入最后一段时的编码器起点 */
static uint32_t s_fin_t0;         /* 未标定时退化成按时间 */
static uint8_t  s_seek;           /* 1 = 起步找线阶段（发车后还没看到线）*/
static uint16_t s_track_ms;       /* 连续“在线上跑”的毫秒数（终点线武装用）*/
static uint8_t  s_fin_armed;      /* ⭐ 锁存：一旦在线上跑够时间就置 1（不再被黑条清零）*/
static int32_t  s_odo_cnt;        /* ⭐ v8：累计里程（编码器计数，绝对值）—— 给精停放“最少跑够多远”的门槛 */
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
static int      s_base_actual;    /* 有斜率限制的共同速度 */
static uint8_t  s_d_valid, s_lost, s_risk, s_clear_frames;

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


static int      s_fin_dist = TR_FIN_MM_DEF;

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

void Trace_SetStopDist(int mm)
{
    if (mm < 0)   { mm = 0; }
    if (mm > 300) { mm = 300; }      /* 车长 200mm ⇒ 上限 300 兜底 */
    s_fin_dist = mm;
    printf("# STOPDIST = %d mm\r\n", s_fin_dist);
}

void Trace_FinReset(void)
{
    s_fin = 0; s_fin_need = 0; s_fin_mm = 0;
    tr_reset_ctrl();
    s_lost = 0u;
    s_base_actual = 0;
    printf("# FINRESET ok\r\n");
}

void Trace_FinStatus(void)
{
    const char *nm[3] = { "0=巡线", "1=走最后一段", "2=已刹车停" };
    char mp[16];
    Bsp_FmtFix(mp, sizeof mp, s_mm_per_cnt, 4, 0);
    printf("# FIN %s | 全黑连续=%u/%u | 最后一段已走 %ld/%d mm | mm每计数=%s%s\r\n",
           (s_fin < 3u) ? nm[s_fin] : "?", (unsigned)s_fin_need, (unsigned)TR_FIN_NEED,
           (long)s_fin_mm, s_fin_dist, mp,
           (s_mm_per_cnt > 0.0f) ? "" : " (未标定→按时间估算，不准)");
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
    s_drive = on ? 1u : 0u;
}
void    Trace_SetSeek(uint8_t on)  { s_seek = on ? 1u : 0u; }

/* ⭐ 发车统一入口（按钮 / 以后加命令都调它）：复位精停 + 打开【起步找线】
   为什么必须有 seek：起始线是个 T 头，车摆在它【前】面 ⇒ 发车瞬间就是全白，
   旧逻辑会立刻“丢线停车” ⇒ 车一动不动（2026-10-01 场地实测 ✗）*/
void Trace_StartRun(void)
{
    Trace_FinReset();
    s_seek = 1u;
    s_track_ms = 0u;
    s_fin_armed = 0u;                 /* v6：新一趟重新武装（锁存清 0）*/
    s_wide_until_ms = 0u;
    s_odo_cnt = 0;                    /* v8：里程清零（每趟重新计）*/
    s_last_line_err = 0.0f;
    s_risk = 0u; s_clear_frames = 0u;
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

void Trace_Step(uint8_t allow_drive)
{
    uint32_t ms = HAL_GetTick();
    Params  *p  = Tlm_Params();
    int      s[8];
    float    err;

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

/* ①-1 “终点线武装”（锁存式）：
      黑条期间不累计；但只要累计够过，就把锁存置 1 —— **不可被后续黑条清掉** ✓
   ⭐ v8（2026-10-01 桌面实踩）：武装条件必须限定在【窄线】上 ✗
      事故：车在灰色地板上被推着滚 → 灰度看到 1~7 路黑（当成“跟线”）→ 攒够 1.5s ⇒ 武装 ✗
            → 再遇全黑（灰地板/阴影）⇒ **误判成终点线** ⇒ 进精停、车就地停住，看起来“卡住”✗
      依据：真赛道实测巡线时黑的是 **1~2 路**（43% / 47%），**3 路及以上只占 0.1%**
      ⇒ 只在 `1 <= cnt <= 3` 时累计；宽黑（4~8 路）不算“在跟线”✓ */
if (!s_fin_armed) {
    if (cnt >= 1 && cnt <= 3) {
        if ((s_track_ms += 10u) >= TR_FIN_ARM_MS) { s_fin_armed = 1u; }
    } else {
        s_track_ms = 0u;   /* 离开窄线后重新累计；已武装则保持锁存 */
    }
}

/* ①-1c 累计里程（只累计数，不累 mm —— 免得 0.115mm 被取整成 0 ✗）
   ⚠️ 为什么需要：桌面推几把只有几百 mm，也会触发精停（2026-10-01 实踩）✗
   ✓ 有了它：要有 TR_FIN_MIN_MM（默认 800mm）的里程才可能认终点
      ⇒ 桌面推车（几百 mm）永远进不了精停 ✓，也顺手挡住“中途误放的黑块”✓
   ⚠️ mm/计数是**名义值**（0.115mm，未精标，±20~40%）⇒ 门槛只当“米级尺子”用 ✓ */
{
    static int16_t odo_l0, odo_r0;
    static uint8_t odo_init;
    int16_t cl = En_Count(ENC_L), cr = En_Count(ENC_R);
    if (!odo_init) { odo_l0 = cl; odo_r0 = cr; odo_init = 1u; }
    int32_t dc = ((int32_t)(cl - odo_l0) + (int32_t)(cr - odo_r0)) / 2;
    if (dc < 0) { dc = -dc; }            /* 前后推都算里程（绝对值）*/
    s_odo_cnt += dc;
    odo_l0 = cl; odo_r0 = cr;
}

/* ①-1b 用真实时钟算保持期；主循环被超声波读数阻塞时也不会延长盲走。 */
if (cnt >= (int)TR_WIDE_BLACK_N) { s_wide_until_ms = ms + TR_WIDE_HOLD_MS; }

        /* ①-2 终止线检测：8 路全黑连续 TR_FIN_NEED 帧，且【已武装】+【里程够】
             ⭐ v8：里程门槛（TR_FIN_MIN_MM）是桌面误判的第二道闸 ✓ */
        {
            float mmpc = (s_mm_per_cnt > 0.0f) ? s_mm_per_cnt : 0.115f;
            int32_t odo_mm = (int32_t)((float)s_odo_cnt * mmpc);
            if (cnt >= 8) {
                if (s_fin_need < 250u) { s_fin_need++; }
                if (s_fin_need >= TR_FIN_NEED && s_fin == 0u) {
                    if (s_fin_armed && odo_mm >= TR_FIN_MIN_MM) {
                        s_fin = 1u;                              /* 进入"走最后一段" */
                        s_fin_l0 = En_Count(ENC_L);
                        s_fin_r0 = En_Count(ENC_R);
                        s_fin_mm = 0;
                        s_fin_t0 = HAL_GetTick();
                        Tlm_SendEvent(80, "FINISH LINE");
                    } else if ((ms - s_startt_t0) >= 2000u) {
                        /* 全黑但不认（事件跟流：2 秒最多一条，避免刷屏 ✗） */
                        s_startt_t0 = ms;
                        if (!s_fin_armed) {
                            Tlm_SendEvent(83, "START T ignored");
                        } else {
                            Tlm_SendEvent(84, "FIN gated: too short");   /* 里程不够 ⇒ 不当终点 ✓ */
                        }
                    }
                }
            } else {
                s_fin_need = 0u;
            }
        }

        /* ①-3 精停状态机（驱动/超时/刹车都在这收口，并直接 return，
              不让下面的“丢线停车 / 循迹 PID”插手）*/
        if (s_fin != 0u) {
            tr_reset_ctrl(); s_lost = 0u; s_base_actual = 0;
            int spd = p->base_speed;
            if (spd <= 0) { spd = TR_BASE_DEFAULT; }
            if (TR_FIN_SPEED_PCT > 0) { spd = TR_FIN_SPEED_PCT; }

            if (s_fin == 1u) {
                int16_t cl = En_Count(ENC_L), cr = En_Count(ENC_R);
                int32_t dc = ((int32_t)(cl - s_fin_l0) + (int32_t)(cr - s_fin_r0)) / 2;
                if (dc < 0) { dc = -dc; }
                if (s_mm_per_cnt > 0.0f) {
                    s_fin_mm = (int32_t)((float)dc * s_mm_per_cnt);
                } else {
                    /* 未标定：退化成按时间估（TR_FIN_MMPS_EST，不准，但安全窗口很宽）*/
                    s_fin_mm = (int32_t)((HAL_GetTick() - s_fin_t0) * TR_FIN_MMPS_EST / 1000u);
                }
                uint8_t timeout = (uint8_t)((ms - s_fin_t0) >= TR_FIN_TIMEOUT_MS);
                if (s_fin_mm >= s_fin_dist || timeout) {
                    s_fin = 2u;                              /* 到位（或超时）⇒ 刹车停 */
                    if (s_drive && allow_drive) { Mo_Brake(); }
                    Trace_SetDrive(0u);                      /* 关发车开关，防继续跑 */
                    Log_Stop();                              /* 停止记录 + 自动存 Flash ✓ */
                    Tlm_SendEvent(timeout ? 82 : 81, timeout ? "FINISH TIMEOUT" : "FINISH STOP");
                    Hmi_Beep(120); HAL_Delay(90); Hmi_Beep(120);   /* 两声=到终点，好辨认 ✓ */
                } else if (s_drive && allow_drive) {
                    Mo_Set(spd, spd);                        /* 直行走完最后一段 */
                }
            } else {
                if (s_drive && allow_drive) { Mo_Brake(); }  /* 状态 2：保持刹车 */
            }
            s_state = 4;                                     /* $T 的 state=4 ⇒ 精停 ✓ */
            /* ⭐ 上报【实际】占空比：状态 2 是刹车 ⇒ 报 0，别再报"打算给的 spd"
               （以前两种状态都报 spd，会让事后分析误以为"命令了却没走"）*/
            {
                int rl = (s_fin == 1u) ? spd : 0;
                int rr = (s_fin == 1u) ? spd : 0;
                Log_Note(tr_pack(s), 0.0f, rl, rr, 4);
                Tlm_SendT(ms, s, 0.0f, rl, rr, 4);
            }
            return;
        }

        /* ============ ② 全白直行 / 起步找线 ============*/
        if (cnt == 0) {
            /* 精停已在上面处理；这里只处理灰度读取成功后的八路全白。
             * 起步仍报 state=5，其余丢线报 state=6；见黑后原 PID 重新起速。
             * 按用户本轮要求持续 26/26 直行，不再按旧时限/里程停车。 */
            tr_reset_ctrl(); s_base_actual = 0;
            if (s_drive && allow_drive) {
                s_lost = 1u;
                s_state = s_seek ? 5 : 6;
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
        if ((s_seek && cnt < 8) || s_lost || (s_state != 1 && !s_seek)) {
            tr_reset_ctrl(); s_base_actual = 0;
            s_risk = 2u; s_clear_frames = 0u;
        }
        if (cnt < 8) { s_seek = 0u; } /* 起点全黑 T 头不代表已找到窄线 */
        s_lost = 0u;                 /* 找回线后重新起速 */
        /* 位置误差 = 黑点的平均位置 − 中心（负 = 线偏左，要往左修）*/
        err = (float)sum / (float)cnt - TR_POS_CENTER;
        if (s_analog && s_ana_ok) { err = s_ana_err; }   /* v3 ②：模拟量优先 */
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
        int16_t gz_raw = Log_GetGz();
        float gz_dps = (float)gz_raw / 131.0f;      /* ±250°/s 量程 ⇒ 131 LSB/(°/s) */
        float agz = (gz_dps < 0.0f) ? -gz_dps : gz_dps;
        if (agz >= 100.0f)                    { risk = 2u; }
        else if (agz >= 55.0f && risk < 1u)   { risk = 1u; }
    }
    if (risk > s_risk) { s_risk = risk; s_clear_frames = 0u; }
    else if (risk < s_risk) {
        if (++s_clear_frames >= 8u) { s_risk = risk; s_clear_frames = 0u; }
    } else { s_clear_frames = 0u; }
    int target = base;
    if (s_risk == 1u) { target = base * 3 / 4; }
    if (s_risk == 2u) { target = base * 11 / 20; }
    if (target < 8) { target = 8; }
    if (s_base_actual < target) { s_base_actual += 1; if (s_base_actual > target) { s_base_actual = target; } }
    if (s_base_actual > target) { s_base_actual -= 3; if (s_base_actual < target) { s_base_actual = target; } }
    s_last_line_err = err;
    float d_term = p->kd * s_d_filt;
    if (d_term >  12.0f) { d_term =  12.0f; }
    if (d_term < -12.0f) { d_term = -12.0f; }

    /* ================= v3 ③ 小积分（长弧道自动维持差速）=================
       用户诉求：“大弧线不要一段段直线，而是自然差速走出弯” ——
       本质就是 err 持续偏一侧时，舵量应该**稳定维持一个值**，而不是“拽一下、又直一下”。
       ⇒ 加小积分；带抗饱和（|ki·I| 不超过 TR_I_MAX），且起步头 0.3s 不积（防起步瞬间累积）*/

    if (p->ki > 0.0001f && s_track_ms > TR_I_START_MS) {
        s_ierr += err * (float)(dt_ms ? dt_ms : 10u) * 0.001f;   /* err*秒（ki=0 时根本不积）*/
    }
    float i_term = p->ki * s_ierr;
    if (i_term >  TR_I_MAX) { i_term =  TR_I_MAX; if (p->ki > 0.0001f) { s_ierr = TR_I_MAX / p->ki; } }
    if (i_term < -TR_I_MAX) { i_term = -TR_I_MAX; if (p->ki > 0.0001f) { s_ierr = -TR_I_MAX / p->ki; } }

    float steer_f = p->kp * err + i_term + d_term;

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
