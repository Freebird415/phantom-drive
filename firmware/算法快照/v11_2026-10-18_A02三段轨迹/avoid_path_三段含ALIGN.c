/* =============================================================================
 *  avoid_path.c —— A02 避障的「固定轨迹」：外偏 → 反向回摆（只控电机）
 *
 *  为什么只有一个文件（2026-10-18 合并）：
 *    原来 avoidl.c / avoidr.c 是两份**完全镜像**的代码（时长、PWM、缓变规则一模一样），
 *    只是左右对调 ⇒ 改一个数要改两处、还容易改漏。现在合成一份：
 *    **只写"向左避"那一套数值，向右避自动镜像**。
 *
 *  三段轨迹（以"向左避"= 右侧有障碍为例）：
 *    ① OUT      AV_OUT_MS：固定差速（左轮 0% / 右轮 48%）→ 先向左偏出去
 *    ② RETURN   AV_RETURN_MS：从 (48,25) 线性渐变到 (45,40) → 反向回摆（向右），
 *              边摆边等回线；到点仍没回线 → 由 app_avoid.c 锁存停车
 *    ③ ALIGN    AV_ALIGN_MS：**再反一次方向（向右避时相反）把车身摆正**，
 *              同样是从 START 线性渐变到 END；默认全 0 = 不启用（行为与以前完全一致）
 *
 *  方向约定：三段的数值都按"向左避"写，向右避时**左右轮自动对调**（见 apply()）。
 *
 *  调参：只改下面这一块。向右避会自动用镜像值（左右轮对调），**不需要改两处**。
 *  ⚠️ PWM 是"电机占空比命令"，不等于实测轮速；_MS 单位毫秒。
 * ==========================================================================*/
#include "app/app_telemetry.h"
#include "drv/drv_motor.h"

#include <stdint.h>

/* ---------------- 调参区（只写"向左避"这一套；向右避时左右轮自动对调）----------------
 * ⚠️ 转向规则（唯一判据，看数字不看名字）：
 *      右轮比左轮快（R > L） ⇒ 车向左转
 *      左轮比右轮快（L > R） ⇒ 车向右转
 *    左轮=0 / 右轮=48 是最猛的左转；左右相等 = 直行。
 *    下面 L/R 就是**字面的左轮/右轮**（都按"向左避"这个场景写）。*/
#define AV_OUT_MS         400u   /* ① 推出段时长 */
#define AV_OUT_L_PWM        0    /*    左轮 0 */
#define AV_OUT_R_PWM       48    /*    右轮 48 → 向左推出 */

#define AV_RETURN_MS     4500u   /* ② 回摆段时长（也是"回线完成"的时间上限）*/
#define AV_RET_START_L_PWM 48    /*    回摆起点：左轮 48 */
#define AV_RET_START_R_PWM 25    /*              右轮 25 → 左快右慢 ⇒ 向右（回摆）*/
#define AV_RET_END_L_PWM   45    /*    回摆终点：左轮 45 */
#define AV_RET_END_R_PWM   40    /*              右轮 40 → 差速 5，接近直行 */

/* ③ 回正段（回摆之后再反一次方向，把车身摆正）
 *    ⭐ 当前：**已停用（全 0）** ⇒ 实际轨迹 = ① 推出 + ② 定时线性回摆（两段）。
 *    能力保留：把下面 5 个值填上（MS>0 且至少 1 个 PWM≠0）即可启用第三段。
 *    📌 快照：2026-10-18 测试过的一套三段参数（左避）——
 *         AV_ALIGN_MS 4000u / START_L 30 / START_R 35
 *         / END_L 34 / END_R 35
 *       详细见 firmware/算法快照/v11_2026-10-18_A02三段轨迹/说明.md */
#define AV_ALIGN_MS            0u   /*    回正持续时长（ms）；0 = 不做回正 */
#define AV_ALIGN_START_L_PWM   0    /*    回正起点：左轮 */
#define AV_ALIGN_START_R_PWM   0    /*              右轮 */
#define AV_ALIGN_END_L_PWM     0    /*    回正终点：左轮 */
#define AV_ALIGN_END_R_PWM     0    /*              右轮 */

#define AV_PWM_STEP            2    /* 每 10ms 单轮最多变化 2%（防突变打滑）*/
/* ------------------------------------------------------------------------- */

typedef enum { PATH_OFF = 0, PATH_OUT, PATH_RETURN, PATH_ALIGN } PathPhase;
static PathPhase s_phase;
static uint8_t   s_dir_left;     /* 1 = 向左避（右侧障碍）；0 = 向右避（左侧障碍）*/
static uint32_t  s_phase_ms;
static int       s_pwm_l, s_pwm_r;

static int slew(int cur, int target)
{
    if (cur < target - AV_PWM_STEP) { return cur + AV_PWM_STEP; }
    if (cur > target + AV_PWM_STEP) { return cur - AV_PWM_STEP; }
    return target;
}

/* 目标 PWM 随 elapsed 从 start 线性变到 end（四舍五入到整 duty）。dur 是总时长。*/
static int lerp_pwm(int start, int end, uint32_t elapsed, uint32_t dur)
{
    if (dur == 0u || elapsed >= dur) { return end; }
    int32_t change = (int32_t)(end - start) * (int32_t)elapsed;
    if (change >= 0) { change += (int32_t)(dur / 2u); }
    else { change -= (int32_t)(dur / 2u); }
    return start + (int)(change / (int32_t)dur);
}

/* 回正段是否启用（默认全 0 ⇒ 不启用，行为与"没有这一段"完全一致）*/
static uint8_t align_enabled(void)
{
    return (uint8_t)(AV_ALIGN_MS > 0u &&
                     (AV_ALIGN_START_L_PWM || AV_ALIGN_START_R_PWM ||
                      AV_ALIGN_END_L_PWM   || AV_ALIGN_END_R_PWM));
}

/* 传进来的 l_left / r_left 是【向左避】时的字面左右轮；向右避时左右对调。*/
static void apply(int l_left, int r_left)
{
    int l = s_dir_left ? l_left : r_left;
    int r = s_dir_left ? r_left : l_left;
    s_pwm_l = slew(s_pwm_l, l);
    s_pwm_r = slew(s_pwm_r, r);
    Mo_Set(s_pwm_l, s_pwm_r);
}

void Path_Stop(void)
{
    s_phase = PATH_OFF;
    s_phase_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
}

/* 从直行的当前 PWM 接续（避免切换阶段时速度突变）。dir_left=1 → 向左避。*/
void Path_Start(uint8_t dir_left, uint32_t now, int pwm_l, int pwm_r)
{
    s_phase = PATH_OUT;
    s_phase_ms = now;
    s_dir_left = dir_left ? 1u : 0u;
    s_pwm_l = pwm_l;
    s_pwm_r = pwm_r;
    Tlm_SendEvent(3, dir_left ? "A02 right US -> left" : "A02 left US -> right");
}

uint8_t Path_Returning(void) { return s_phase == PATH_RETURN; }

/* 回摆到期：只有在"没有回正段"时才算失败（否则交给回正段继续跑）。*/
uint8_t Path_ReturnExpired(uint32_t now)
{
    if (s_phase != PATH_RETURN) { return 0u; }
    if ((uint32_t)(now - s_phase_ms) < AV_RETURN_MS) { return 0u; }
    return align_enabled() ? 0u : 1u;
}

/* 回正到期：回正跑完仍未回线 ⇒ 上层的超时停车接手。*/
uint8_t Path_AlignExpired(uint32_t now)
{
    return s_phase == PATH_ALIGN && (uint32_t)(now - s_phase_ms) >= AV_ALIGN_MS;
}

/* 正常完成由 app_avoid.c 判线；超时也由入口锁存停车。*/
void Path_Step(uint32_t now, int *pwm_l, int *pwm_r)
{
    if (s_phase == PATH_OFF) { return; }

    if (s_phase == PATH_OUT && (uint32_t)(now - s_phase_ms) >= AV_OUT_MS) {
        s_phase = PATH_RETURN;
        s_phase_ms = now;
        Tlm_SendEvent(3, s_dir_left ? "A02 replay right" : "A02 replay left");
    }
    /* 回摆跑完：有回正段就接上，没有就停在 RETURN（让上层按"回摆超时"处理）*/
    if (s_phase == PATH_RETURN && (uint32_t)(now - s_phase_ms) >= AV_RETURN_MS &&
        align_enabled()) {
        s_phase = PATH_ALIGN;
        s_phase_ms = now;
        Tlm_SendEvent(3, s_dir_left ? "A02 align body (left)" : "A02 align body (right)");
    }

    if (s_phase == PATH_OUT) {
        apply(AV_OUT_L_PWM, AV_OUT_R_PWM);
    } else if (s_phase == PATH_RETURN) {
        uint32_t elapsed = (uint32_t)(now - s_phase_ms);
        apply(lerp_pwm(AV_RET_START_L_PWM, AV_RET_END_L_PWM, elapsed, AV_RETURN_MS),
              lerp_pwm(AV_RET_START_R_PWM, AV_RET_END_R_PWM, elapsed, AV_RETURN_MS));
    } else {
        /* 回正段：同样按"向左避"写 L/R，右避自动镜像 */
        uint32_t elapsed = (uint32_t)(now - s_phase_ms);
        apply(lerp_pwm(AV_ALIGN_START_L_PWM, AV_ALIGN_END_L_PWM, elapsed, AV_ALIGN_MS),
              lerp_pwm(AV_ALIGN_START_R_PWM, AV_ALIGN_END_R_PWM, elapsed, AV_ALIGN_MS));
    }

    if (pwm_l) { *pwm_l = s_pwm_l; }
    if (pwm_r) { *pwm_r = s_pwm_r; }
}
