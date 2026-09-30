/* =============================================================================
 *  app_trace.c —— 循迹算法骨架（A01 熊雅琦）
 *
 *  ★ 你只改这个文件。下面是"能跑起来的起点"，不是最终方案。
 *  ★ 骨架已经做了三件你会用到的事：
 *      1) 读灰度 → 算位置误差 err（−3.5 ~ +3.5，负=线在左边）
 *      2) 单 P 控制 → 算出左右占空比
 *      3) 上报 $T 帧（电脑/曲线图看的就是它）
 *  ★ 你要做的是把它变成"能跑完全赛道"的策略：加 I/D、处理丢线、过弯减速…
 *
 *  参数不用重烧就能改（串口，回车结尾）：
 *      SET kp 8        改比例
 *      SET ki 0.02     改积分
 *      SET kd 1.5      改微分
 *      SET base_speed 45   改基准速度
 *      GET             看当前参数
 * ========================================================================== */
#include "app/app_trace.h"
#include "app/app_telemetry.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"
#include "app/app_log.h"       /* 板载 RAM 快照（跑车时插不了串口，靠它事后读）*/
#include "drv/drv_oled.h"      /* 车载仪表盘 */
#include "drv/drv_hmi.h"       /* 精停蜂鸣两声 */
#include "drv/drv_encoder.h"    /* 终点精停靠编码器计距 */

#include <stdio.h>

#define TR_BASE_DEFAULT   35      /* 基准速度的兜底值。
           ⚠️ 2026-10-01 实测：**15% 占空比时电机卡在死区附近挣扎** ——
              左右轮真实增量只有 2.5/3.8 counts/帧 ⇒ 仅 ~24mm/s（满速 1210mm/s 的 2%）。
              ⇒ base_speed 必须抬到 30 以上，差速才有"劲"可用。 */
#define TR_REV_MIN        25      /* ⭐ 急弯时内侧轮若反转，至少给 25% ——
              否则 −5%~−10% 也在死区里，等于没转（实测 9% 的帧就栽在这）*/
#define TR_POS_CENTER     3.5f    /* 8 路灰度的中心：0..7 的中点是 3.5 */

static uint8_t s_online;          /* 灰度是否在线（不在线就发模拟数据，保住 P01 链路）*/
static uint8_t s_drive;           /* ⚠️ 0 = 只算不发（默认，安全）；1 = 真的驱动电机 */
static int     s_state;           /* 上报用：0 = 丢线停车，1 = 正常巡线，4 = 精停 */

/* ==================== 终点精停（T 头 / 终止线）====================
   规则：车头必须过线，且**车尾投影不得过线**（20 分）
   原理：正常巡线只有 1~2 路看到线；到终止线（横向 T 头）时 **8 路同时全黑**
   ⚠️ 前提：赛道是「白底窄黑线」。若测试面是一大片黑（宽胶带、黑纸），会误触发
   流程：0 巡线 ──8 路全黑连续 N 帧──▶ 1 直行走完最后一段 ──走够 D mm──▶ 2 刹车停+存 Flash */
#define TR_FIN_NEED     3u      /* 连续 N 帧全黑才认定（防抖；30ms ≈ 车走 1~2mm）*/
/* 检到线后再前进多少 mm —— 几何依据（2026-09-30 实测：车头到探头中心 15mm、车长 200mm）：
     检到线时车头在【线前 15mm】、车尾在【线前 215mm】
     前进 D 后：车头在线前 (15−D)，车尾在线前 (215−D)
     要求 车头过线 ⇒ D > 15；  车尾不过线 ⇒ D < 215
   ⇒ 安全窗口 (15, 215)，取中值 80mm：车头过线 65mm、车尾在线前 135mm，两边都有大余量 ✓
   ⇒ 标定 mm/计数 后可以改得更精确（STOPDIST <mm>）*/
#define TR_FIN_MM_DEF   80
#define TR_FIN_SPEED_PCT 0      /* 最后一段速度：0 = 用 base_speed */
/* 未标定 mm/计数 时的兑底：按时间估算车速（mm/s）。
   依据：JGB37-520 12V 330RPM + 7cm 轮 ⇒ 满速约 1210mm/s；
        15% 占空比扣除死区后估计只有 100~200mm/s ⇒ 取 130 保守中值。
   ⚠️ 这个估算误差可能达 2 倍 —— 好在安全窗口很宽（15~215mm），能容忍 ✓ */
#define TR_FIN_MMPS_EST 130

static uint8_t  s_fin;            /* 0=巡线 1=走最后一段 2=已停 */
static uint8_t  s_fin_need;       /* 全黑连续帧计数 */
static int32_t  s_fin_mm;         /* 最后一段已走 mm */
static int16_t  s_fin_l0, s_fin_r0;   /* 进入最后一段时的编码器起点 */
static uint32_t s_fin_t0;         /* 未标定时退化成按时间 */
static float    s_mm_per_cnt;     /* 每计数多少 mm（0 = 未标定）*/
static float    s_err_prev;       /* 上一拍 err（算 D 项用）*/
static uint32_t s_ms_prev;        /* 上一拍时刻（D 项要用真实 dt，不能用假设的 10ms）*/


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
    printf("# DISTCAL: 走了 %.3f m，计数 L=%d R=%d ⇒ mm/计数 = %.4f\r\n",
           (double)meters, (int)l, (int)r, (double)s_mm_per_cnt);
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
    printf("# FINRESET ok\r\n");
}

void Trace_FinStatus(void)
{
    const char *nm[3] = { "0=巡线", "1=走最后一段", "2=已刹车停" };
    printf("# FIN %s | 全黑连续=%u/%u | 最后一段已走 %ld/%d mm | mm每计数=%.4f%s\r\n",
           (s_fin < 3u) ? nm[s_fin] : "?", (unsigned)s_fin_need, (unsigned)TR_FIN_NEED,
           (long)s_fin_mm, s_fin_dist, (double)s_mm_per_cnt,
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

void    Trace_SetDrive(uint8_t on) { s_drive = on ? 1u : 0u; }
uint8_t Trace_GetDrive(void)       { return s_drive; }

void Trace_Init(void)
{
    s_drive  = 0u;                /* ⚠️ 默认不驱动：第一次一定要让轮子悬空再 `TRACE 1` */
    s_state  = 0;
    s_online = Gr_Ping();

    printf("# TRACE 灰度=%s 驱动=%s\r\n",
           s_online ? "在线" : "不在线", s_drive ? "开" : "关(只算不发)");
    if (!s_online) {
        printf("#       灰度不在线 -> 先发模拟数据（保住 P01 链路自检）；接上就自动走真实读数\r\n");
    }
}

/* ==================== 车载仪表盘（OLED）====================
   分页刷新：每调用一次只刷 1 页(128B ≈ 3ms) ⇒ 8 次一轮 ≈ 80~160ms 全屏更新
   ⇒ **不卡控制循环**（100kHz 下刷满屏要 100ms，直接卡死 ✗ 所以必须分页）
   调用位置：main 主循环末尾（每 10ms 一次）*/
void Trace_Dash(void)
{
    static uint32_t t_next;
    LogDash d;
    char l[24];

    if (s_drive) { return; }                  /* 跑车时不刷，保护控制循环 */
    if ((int32_t)(HAL_GetTick() - t_next) < 0) { return; }
    t_next = HAL_GetTick() + 500u;            /* 2Hz */

    Log_GetDash(&d);
    float e = (double)d.err10 / 10.0;

    Ol_FbClear();                             /* 整屏清空 + 重画 + 一次刷完（简单=不出错）*/
    const char *mode = (s_state == 4u) ? "STOP" :
                       (s_fin   != 0u) ? "FIN " :
                       (e < -0.8f)     ? "LEFT" :
                       (e >  0.8f)     ? "RGHT" : "GO  ";
    snprintf(l, sizeof l, "%-4s k%g v%d", mode,
             (double)Tlm_Params()->kp, Tlm_Params()->base_speed);
    Ol_Text(0, 0, l);

    snprintf(l, sizeof l, "e%+.1f L%3d R%3d", (double)e, d.pwm_l, d.pwm_r);
    Ol_Text(0, 1, l);

    snprintf(l, sizeof l, "gray %d", (int)d.bits);
    Ol_Text(0, 2, l);

    snprintf(l, sizeof l, "dL%+d dR%+d", d.dl, d.dr);
    Ol_Text(0, 3, l);

    snprintf(l, sizeof l, "gy%+d F%ld/%d", d.gz, (long)s_fin_mm, s_fin_dist);
    Ol_Text(0, 4, l);

    snprintf(l, sizeof l, "N%u %.1fs", (unsigned)d.n, (double)d.n / 100.0);
    Ol_Text(0, 5, l);

    Ol_FbFlush();
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

        if (cnt == 0) {
            /* 全白 = 丢线。最安全的处理是先停；想"循着上次方向找线"就改这里。 */
            if (s_drive && allow_drive) { Mo_Stop(); }
            Log_Note(tr_pack(s), 0.0f, 0, 0, 0);
            Tlm_SendT(ms, s, 0.0f, 0, 0, 0);
            s_state = 0;
            return;
        }

        /* ---- 终止线检测：8 路全黑连续 TR_FIN_NEED 帧 ---- */
        if (cnt >= 8) {
            if (s_fin_need < 250u) { s_fin_need++; }
            if (s_fin_need >= TR_FIN_NEED && s_fin == 0u) {
                s_fin = 1u;                              /* 进入"走最后一段" */
                s_fin_l0 = En_Count(ENC_L);
                s_fin_r0 = En_Count(ENC_R);
                s_fin_mm = 0;
                s_fin_t0 = HAL_GetTick();
                Tlm_SendEvent(80, "FINISH LINE");
            }
        } else {
            s_fin_need = 0u;
        }

        /* ---- 终点精停：走完最后一段 ⇒ 刹车停 + 停止记录（自动存 Flash）---- */
        if (s_fin != 0u) {
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
                if (s_fin_mm >= s_fin_dist) {
                    s_fin = 2u;                              /* 到位 ⇒ 刹车停 */
                    if (s_drive && allow_drive) { Mo_Brake(); }
                    Trace_SetDrive(0u);                      /* 关发车开关，防继续跑 */
                    Log_Stop();                              /* 停止记录 + 自动存 Flash ✓ */
                    Tlm_SendEvent(81, "FINISH STOP");
                    Hmi_Beep(120); HAL_Delay(90); Hmi_Beep(120);   /* 两声=到终点，好辨认 ✓ */
                } else if (s_drive && allow_drive) {
                    Mo_Set(spd, spd);                        /* 直行走完最后一段 */
                }
            } else {
                if (s_drive && allow_drive) { Mo_Brake(); }  /* 状态 2：保持刹车 */
            }
            s_state = 4;                                     /* $T 的 state=4 ⇒ 精停 ✓ */
            Log_Note(tr_pack(s), 0.0f, spd, spd, 4);
            Tlm_SendT(ms, s, 0.0f, spd, spd, 4);
            return;
        }
        /* 位置误差 = 黑点的平均位置 − 中心（负 = 线偏左，要往左修）*/
        err = (float)sum / (float)cnt - TR_POS_CENTER;
    } else {
        /* ---- 灰度没接时的模拟数据（接上后这段永远走不到，可以删）---- */
        int32_t k   = (int32_t)((ms / 10u) % 200u);
        int32_t e10 = (k < 100) ? (k - 50) : (150 - k);
        int center  = 3 + (int)(e10 / 25);
        if (center < 0) { center = 0; }
        if (center > 7) { center = 7; }
        for (int i = 0; i < 8; i++) { s[i] = (i == center || i == center + 1) ? 1 : 0; }
        err = (float)e10 / 10.0f;
    }

    /* ---- 控制器：现在只有单 P ---- */
    int base = p->base_speed;
    if (base <= 0) { base = TR_BASE_DEFAULT; }

    /* ---- P + D（⚠️ D 项必须用【真实 dt】+ 钳位）----
       用假设的固定 10ms 会在“主循环被拖慢”时算出虚假的巨大变化率 ⇒ 车猛打舵 ✗
       （2026-10-01 实踩：OLED 400k 超时把节奏打乱 ⇒ D 项爆表 ⇒ 左右猛一下猛一下）*/
    uint32_t dt_ms = ms - s_ms_prev;
    if (dt_ms == 0u)  { dt_ms = 10u; }
    if (dt_ms > 50u)  { dt_ms = 50u; }        /* 拖慢时钳住，等价于“忽略这一拍” */
    s_ms_prev = ms;
    float d_rate = (err - s_err_prev) * 1000.0f / (float)dt_ms;   /* err / 秒 */
    s_err_prev = err;
    float d_term = p->kd * d_rate;
    if (d_term >  30.0f) { d_term =  30.0f; }  /* ⚠️ 钳位：D 项最多贡献 ±30 */
    if (d_term < -30.0f) { d_term = -30.0f; }
    float steer_f = p->kp * err + d_term;
    int   steer   = (int)(steer_f + (steer_f >= 0.0f ? 0.5f : -0.5f));

    int l = base + steer;     /* 线偏左 → 往左修：左轮慢、右轮快 */
    int r = base - steer;

    /* ⭐ 急弯：内侧轮若被压到“轻微反转”，补足到 TR_REV_MIN ——
       否则 −5%~−10% 也在电机死区里，等于没转（急弯就转不过来）*/
    if (l < 0 && l > -TR_REV_MIN) { l = -TR_REV_MIN; }
    if (r < 0 && r > -TR_REV_MIN) { r = -TR_REV_MIN; }

    s_state = 1;
    if (s_drive && allow_drive) {
        Mo_Set(l, r);         /* Mo_Set 内部会自动夹到 ±100 */
    }

    /* $T 里的 pwm 是"算法打算给的值"；驱动关掉时电机实际不动 */
    Log_Note(tr_pack(s), err, l, r, s_state);     /* 板载快照（跑完用 DUMP 读）*/
    Tlm_SendT(ms, s, err, l, r, s_state);
}
