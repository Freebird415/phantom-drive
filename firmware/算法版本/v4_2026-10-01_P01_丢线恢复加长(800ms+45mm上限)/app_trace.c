/* =============================================================================
 *  app_trace.c —— A01 循迹控制（熊雅琦）
 *
 *  数字灰度 → 位置误差 → P+D → 分档速度/差速混控；独立处理起步找线、
 *  普通短时丢线和终点精停，并上报 $T/板载快照。
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
#include "bsp/bsp_board.h"      /* Bsp_FmtFix：不用浮点 printf（省 ~6KB）*/

#include <stdio.h>

#define TR_BASE_DEFAULT   12     /* 与当前 1.5× 默认巡航参数一致的兜底值 */
#define TR_REV_MIN        10     /* 反转落入低占空比死区时的最小幅度 */
#define TR_POS_CENTER     3.5f    /* 8 路灰度的中心：0..7 的中点是 3.5 */

static uint8_t s_online;          /* 灰度是否在线（不在线就发模拟数据，保住 P01 链路）*/
static uint8_t s_drive;           /* ⚠️ 0 = 只算不发（默认，安全）；1 = 真的驱动电机 */
/* 车载仪表盘开关（命令 `DASH 0|1`）。
   ⚠️ 2026-10-01 起【默认关】：OLED 是"锦上添花"的显示件，不该影响主体控制 ——
   它一旦没插好/不亮，I2C2 写帧缓冲会【超时】（每帧最多几百毫秒），
   足以把 10ms 控制循环拖慢 ⇒ D 项算出虚假的巨大变化率 ⇒ 车左右猛打舵 ✗
   ⇒ 要用仪表盘时显式发 `DASH 1`（那时才产生 I2C 流量）。*/
static uint8_t s_dash  = 0u;
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
#define TR_FIN_MMPS_EST 195

/* ⭐ 走最后一段的【超时兜底】（2026-10-01 实车踩到）：
   精停原本只能靠"走够 D mm"退出；一旦车轮不动（堵转/打滑/离地/抵住，
   低速档尤其容易），`s_fin_mm` 永远是 0 ⇒ **它永远不刹车、一直"直行"** ✗
   （实车现象：跑到终点附近没停，人把车拿起来才停）
   ⇒ 加 3 秒硬超时：就算一米没走够，也照样刹车停住 ✓ */
#define TR_FIN_TIMEOUT_MS 3000u

/* ⭐ 2026-10-01 场地实车新增两条（都是当天踩出来的）*/
/* ① 起步找线：起始位置是“车摆在起始线 T 头【前】” ⇒ 发车瞬间八路全 0。
      若按“丢线即停车”，车就永远走不了 ✗（实测：长按后纹丝不动）。
      ⇒ 起步后 s_seek 期间改成【慢速直行找线】，看到线自动转正常循迹；
        超过这个时间还没找到才停车（安全兜底）。*/
#define TR_SEEK_TIMEOUT_MS 3000u
/* ② 终点线“武装”时间：起点的 T 头也是“八路全黑”，会被误判成终点 ✗
      ⇒ 必须先在线上连续跑够这么久，才允许把全黑当终点线。
      （用时间而不用里程：mm/计数 尺度还没定死，时间与尺度无关 ✓）*/
#define TR_FIN_ARM_MS 1500u

/* ==================== v3 试验项（2026-10-01 晚）====================
   背景：同一趟日志量出“段落感”——err 中位 **0.48 秒才变一次**，
        一变就是 ~10 个 duty 单位 ⇒ 车是“每半秒踹一脚”，不是平滑修正。
   下面四项针对这个现象（前三项默认开，第四项默认关、需显式打开）：

   ① 舵量斜坡限速：把“跳 10 单位”摊成平滑爬升；|err| 越大允许越快
   ② 模拟量求误差：数字灰度只能给 0.125 的台阶 ⇒ 改用模块模拟量做连续质心
      （自标定：拿数字位当“老师”学黑/白基线；范围不够时**自动退回数字量** ✓）
   ③ 小积分 ki：长弧道上 err 持续偏一侧 ⇒ 舵量缓慢累积 ⇒ 车轮自动维持差速
      （对抗“又直一下、又拽一下”，即用户说的“自然差速走出弯道”）
   ④ 陀螺仪阻尼（默认关、标为未验证）：拿偏航率抵消一部分舵量，抑制摆动；
      它也可能抵抗掉“需要持续转向”的弯道 ⇒ 必须实车对比后再决定 ✓ */
#define TR_STEER_SLEW   2.0f   /* 每拍最大舵量变化（基准；实际 = 本值×(1+|err|)）*/
#define TR_I_MAX       25.0f   /* 积分项最大贡献（duty）—— 防饱和 */
#define TR_I_START_MS  300u    /* 起步头 0.3s 不积分 */
#define TR_ANA_DIV      2u     /* 模拟量每 N 拍读一次（100Hz/2 = 50Hz，省 I2C 时间）*/
#define TR_ANA_SPAN_MIN 30.0f  /* 黑/白基线差小于此值 ⇒ 自标定没就绪，退回数字量 */
#define TR_GYROD_K      0.12f  /* 陀螺仪阻尼系数（duty per °/s），仅 GYROD 1 生效 */

static uint8_t  s_fin;            /* 0=巡线 1=走最后一段 2=已停 */
static uint8_t  s_fin_need;       /* 全黑连续帧计数 */
static int32_t  s_fin_mm;         /* 最后一段已走 mm */
static int16_t  s_fin_l0, s_fin_r0;   /* 进入最后一段时的编码器起点 */
static uint32_t s_fin_t0;         /* 未标定时退化成按时间 */
static uint8_t  s_seek;           /* 1 = 起步找线阶段（发车后还没看到线）*/
static uint32_t s_seek_t0;        /* 起步找线开始时刻 */
static uint16_t s_track_ms;       /* 连续“在线上跑”的毫秒数（终点线武装用）*/
static uint32_t s_startt_t0;      /* 上一次报“经过起始线 T 头”的时刻（限流：2s 最多一条）*/
/* 每计数多少 mm。**出厂推定值 0.115**（2026-10-01 实测反推：
   100% 占空比 = 10506 计数/s；按 JGB37-520 12V 330RPM + ⌀70mm 轮 ⇒ 理论满速 1210mm/s
   ⇒ 1210 / 10506 = 0.115）。误差约 ±10%，对最后一段 80mm 来说 ±8mm —— 安全窗口 (15,215) 完全够。
   ⚠️ 这个值**与车速无关**（物理常数），所以慢速档也适用 ✓
   想要更准：`ENCRESET` → 推车走 1.000m → `DISTCAL 1.0`（会覆盖此值，但**断电会丢**，见下方说明）*/
static float    s_mm_per_cnt = 0.115f;
static uint32_t s_online_t0;      /* 上次重探灰度的时刻（离线时每 500ms 探一次）*/
static uint8_t  s_offline_warn;   /* 离线警告计数（降噪：只在第 1 次和每 5 秒提醒）*/
static float    s_err_prev;       /* 上一拍 err（算 D 项用）*/
static uint32_t s_ms_prev;        /* 上一拍时刻（D 项要用真实 dt，不能用假设的 10ms）*/
static float    s_d_filt;         /* 数字灰度量化跳变的一阶低通，单位 err/s */
static float    s_last_line_err;  /* 正常丢线前最后看到线的方向 */
static int      s_base_actual;    /* 有斜率限制的共同速度 */
static uint32_t s_lost_t0;
static uint8_t  s_d_valid, s_lost, s_risk, s_clear_frames;
#define TR_LOST_TIMEOUT_MS 800u
/* ⭐ v4：丢线恢复从 180ms 拉到 800ms，并加“盲开距离”上限
   依据（2026-10-01 场地实跑）：两次“八路全白”都是**一帧从稳定压线直接变全白**，
   位置固定 ⇒ 那两个点本身就没有线（断口/折痕/阴影）✗。
   原 180ms × ~50mm/s ≈ 9mm ⇒ 跨不过 20mm 的断口 ✗ ⇒ 车停下等人推（=人工干预✗）。
   修：给 800ms，并按编码器限住“盲开总距离”，达到上限立刻停 ✓ */
#define TR_LOST_MAX_MM   45.0f   /* 盲开距离上限（mm）；若还有 mm/计数 就换算成计数，否则用下面固定值 */
#define TR_LOST_MAX_CNT  300     /* 固定兜底：约 34mm @0.115mm/计数 */
#define TR_D_ALPHA 0.20f         /* 10ms 时约 45ms 时间常数；真实 dt 仍参与差分 */
static int32_t  s_lost_cnt;       /* 丢线恢复期间累计的“盲开计数”*/
static int16_t  s_lost_l0, s_lost_r0; /* 丢线那一刻的编码器起点（算盲开距离用）*/

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

void    Trace_SetDrive(uint8_t on) { s_drive = on ? 1u : 0u; }
void    Trace_SetSeek(uint8_t on)  { s_seek = on ? 1u : 0u; s_seek_t0 = HAL_GetTick(); }

/* ⭐ 发车统一入口（按钮 / 以后加命令都调它）：复位精停 + 打开【起步找线】
   为什么必须有 seek：起始线是个 T 头，车摆在它【前】面 ⇒ 发车瞬间就是全白，
   旧逻辑会立刻“丢线停车” ⇒ 车一动不动（2026-10-01 场地实测 ✗）*/
void Trace_StartRun(void)
{
    Trace_FinReset();
    s_seek = 1u;
    s_seek_t0 = HAL_GetTick();
    s_track_ms = 0u;
    s_last_line_err = 0.0f;
    s_risk = 0u; s_clear_frames = 0u;
}
uint8_t Trace_GetDrive(void)       { return s_drive; }
uint8_t Trace_Online(void)         { return s_online; }
void    Trace_DashEnable(uint8_t on) { s_dash = on ? 1u : 0u; }

void Trace_Init(void)
{
    s_drive  = 0u;                /* ⚠️ 默认不驱动：第一次一定要让轮子悬空再 `TRACE 1` */
    s_state  = 0;
    s_online = Gr_Ping();

    printf("# TRACE 灰度=%s 驱动=%s\r\n",
           s_online ? "在线" : "不在线", s_drive ? "开" : "关(只算不发)");
    if (!s_online) {
        printf("#       灰度不在线 -> 先发模拟数据（保住 P01 链路自检）；每 500ms 自动重探，插好会自己切回真实读数 ✓\r\n");
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

    if (s_drive || !s_dash) { return; }       /* 跑车时不刷（护控制循环）；DASH 0 时也不刷 */
    if ((int32_t)(HAL_GetTick() - t_next) < 0) { return; }
    t_next = HAL_GetTick() + 500u;            /* 2Hz */

    Log_GetDash(&d);
    float e = (float)d.err10 * 0.1f;
    char kp[16], ee[16], tt[16];
    Bsp_FmtFix(kp, sizeof kp, Tlm_Params()->kp, 2, 0);
    Bsp_FmtFix(ee, sizeof ee, e, 1, 1);          /* 带正负号，等价 %+.1f */
    Bsp_FmtFix(tt, sizeof tt, (float)d.n * 0.01f, 1, 0);   /* n/100 秒 */

    Ol_FbClear();                             /* 整屏清空 + 重画 + 一次刷完（简单=不出错）*/
    const char *mode = (!s_online)     ? "NOGR" :   /* ⭐ 灰度不在线：一眼看出（此时绝不驱动）*/
                       (s_state == 4u) ? "STOP" :
                       (s_fin   != 0u) ? "FIN " :
                       (e < -0.8f)     ? "LEFT" :
                       (e >  0.8f)     ? "RGHT" : "GO  ";
    snprintf(l, sizeof l, "%-4s k%s v%d", mode, kp, Tlm_Params()->base_speed);
    Ol_Text(0, 0, l);

    snprintf(l, sizeof l, "e%s L%3d R%3d", ee, d.pwm_l, d.pwm_r);
    Ol_Text(0, 1, l);

    snprintf(l, sizeof l, "gray %d", (int)d.bits);
    Ol_Text(0, 2, l);

    snprintf(l, sizeof l, "dL%+d dR%+d", d.dl, d.dr);
    Ol_Text(0, 3, l);

    snprintf(l, sizeof l, "gy%+d F%ld/%d", d.gz, (long)s_fin_mm, s_fin_dist);
    Ol_Text(0, 4, l);

    snprintf(l, sizeof l, "N%u %ss", (unsigned)d.n, tt);
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

        /* ①-1 “终点线武装”：刚过完起始 T 头后要等一会儿，才允许把全黑当终点，
                否则**起点的 T 头也会被当成终点** ✗（这是当天第二个致命问题）*/
        if (cnt >= 8) {
            s_track_ms = 0u;                       /* 全黑期间不计数 */
        } else if (cnt > 0 && s_track_ms < (TR_FIN_ARM_MS + 200u)) {
            s_track_ms += 10u;                     /* 本函数 100Hz ⇒ 每拍 10ms */
        }

        /* ①-2 终止线检测：8 路全黑连续 TR_FIN_NEED 帧，且已武装 */
        if (cnt >= 8) {
            if (s_fin_need < 250u) { s_fin_need++; }
            if (s_fin_need >= TR_FIN_NEED && s_fin == 0u) {
                if (s_track_ms >= TR_FIN_ARM_MS) {
                    s_fin = 1u;                              /* 进入"走最后一段" */
                    s_fin_l0 = En_Count(ENC_L);
                    s_fin_r0 = En_Count(ENC_R);
                    s_fin_mm = 0;
                    s_fin_t0 = HAL_GetTick();
                    Tlm_SendEvent(80, "FINISH LINE");
                } else {
                    /* 全黑但没武装 ⇒ 这是起始线 T 头，开过去。
                       ⚠️ 2026-10-01 实踩：原本每拍（100Hz）都发一条事件，
                          车停在起点 T 头上时把串口刷爆（实测日志里满屏 $E 83）✗
                          ⇒ 限流：2 秒最多一条 ✓ */
                    if ((ms - s_startt_t0) >= 2000u) {
                        s_startt_t0 = ms;
                        Tlm_SendEvent(83, "START T ignored");
                    }
                }
            }
        } else {
            s_fin_need = 0u;
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

        /* ============ ② 丢线 / 起步找线 ============*/
        if (cnt == 0) {
            /* 全白 = 丢线（精停阶段在上面已经 return，不会走到这里 ✓）
               ⭐ 起步找线：起始线是 T 头，车摆在它前面 ⇒ 发车瞬间全白。
                  直接停车的话车就永远走不了 ✗ ⇒ s_seek 期间慢速直行去找线；
                  超时还没找到才停车（安全兜底）。*/
            if (s_drive && allow_drive) {
                if (s_seek && (ms - s_seek_t0) < TR_SEEK_TIMEOUT_MS) {
                    tr_reset_ctrl(); s_base_actual = 0;
                    int sp = p->base_speed;
                    if (sp <= 0) { sp = TR_BASE_DEFAULT; }
                    Mo_Set(sp, sp);                      /* 慢速直行去找线 */
                    Log_Note(tr_pack(s), 0.0f, sp, sp, 5);
                    Tlm_SendT(ms, s, 0.0f, sp, sp, 5);
                    s_state = 5;                         /* 5 = 起步找线（新状态码）*/
                    return;
                }
                if (!s_seek && (s_state == 1 || s_lost)) {
                    if (!s_lost) {
                        s_lost = 1u; s_lost_t0 = ms; s_lost_cnt = 0;
                        s_lost_l0 = En_Count(ENC_L); s_lost_r0 = En_Count(ENC_R);   /* v4：盲开起点 */
                        tr_reset_ctrl(); s_base_actual = 0;
                    }
                    /* 盲开上限：优先用 mm/计数 换算，没标定就用固定计数 */
                    int32_t cap = TR_LOST_MAX_CNT;
                    if (s_mm_per_cnt > 0.01f) { cap = (int32_t)(TR_LOST_MAX_MM / s_mm_per_cnt); }
                    {
                        int32_t dc = (int32_t)(En_Count(ENC_L) - s_lost_l0) + (int32_t)(En_Count(ENC_R) - s_lost_r0);
                        if (dc < 0) { dc = -dc; }
                        dc /= 2;
                        s_lost_cnt = dc;
                    }
                    if ((ms - s_lost_t0) < TR_LOST_TIMEOUT_MS && s_lost_cnt < cap) {
                        int sp = p->base_speed;
                        if (sp <= 0) { sp = TR_BASE_DEFAULT; }
                        sp = sp / 2;
                        if (sp < 8) { sp = 8; }
                        if (sp > 16) { sp = 16; }
                        int turn = (s_last_line_err < -0.25f) ? -8 :
                                   (s_last_line_err > 0.25f) ? 8 : 0;
                        int l, r;
                        tr_mix(sp, turn, &l, &r);
                        Mo_Set(l, r);
                        Log_Note(tr_pack(s), 0.0f, l, r, 6);
                        Tlm_SendT(ms, s, 0.0f, l, r, 6);
                        s_state = 6; /* 6 = 丢线找线（v4 新增，便于事后分析区分“找线”和“停着”）*/
                        return;
                    }
                }
                Mo_Stop();                               /* 找线超时 / 平时丢线 ⇒ 停车等待 */
                s_seek = 0u;
            }
            tr_reset_ctrl(); s_lost = 0u; s_base_actual = 0;
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
        s_lost_cnt = 0;              /* v4：顺便清盲开计数 */
        /* 位置误差 = 黑点的平均位置 − 中心（负 = 线偏左，要往左修）*/
        err = (float)sum / (float)cnt - TR_POS_CENTER;
        if (s_analog && s_ana_ok) { err = s_ana_err; }   /* v3 ②：模拟量优先 */
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
    if (base > 100) { base = 100; }

    /* ---- P + D（⚠️ D 项必须用【真实 dt】+ 钳位）----
       用假设的固定 10ms 会在“主循环被拖慢”时算出虚假的巨大变化率 ⇒ 车猛打舵 ✗
       （2026-10-01 实踩：OLED 400k 超时把节奏打乱 ⇒ D 项爆表 ⇒ 左右猛一下猛一下）*/
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
                   (ae >= 1.0f || ad >= 25.0f) ? 1u : 0u;
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
    if (d_term >  30.0f) { d_term =  30.0f; }  /* ⚠️ 钳位：D 项最多贡献 ±30 */
    if (d_term < -30.0f) { d_term = -30.0f; }

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
        float slew = TR_STEER_SLEW * (1.0f + ae);
        float dstep = steer_f - s_steer_out;
        if (dstep >  slew) { dstep =  slew; }
        if (dstep < -slew) { dstep = -slew; }
        s_steer_out += dstep;
    }
    int   steer   = (int)(s_steer_out + (s_steer_out >= 0.0f ? 0.5f : -0.5f));

    int l, r;
    tr_mix(s_base_actual, steer, &l, &r); /* 线偏左 → 左轮慢、右轮快 */

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
