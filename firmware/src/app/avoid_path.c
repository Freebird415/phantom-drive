/* =============================================================================
 *  avoid_path.c —— A02 固定轨迹：① 推出 → ② 反向回摆（只管电机）
 *
 *  · 左右两个方向共用这一份实现：数值按「向左避」写，向右避时左右轮自动对调。
 *  · 参数（AV_*）全部在 app/tuning.h §4。
 *  · 两段跑完仍没回线 → app_avoid.c 按回线失败处理（锁存停车）。
 *  · 历史：第三段「回正 ALIGN」已移除，源码留档在
 *    firmware/算法快照/v11_2026-10-18_A02三段轨迹/
 * ==========================================================================*/
#include "app/app_telemetry.h"
#include "drv/drv_motor.h"

#include <stdint.h>

#include "app/tuning.h"      /* ⭐ 轨迹参数（AV_*）也在那里 */

/* 轨迹参数（AV_*）已移到 app/tuning.h */

typedef enum { PATH_OFF = 0, PATH_OUT, PATH_RETURN } PathPhase;
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

/* 回摆到期：回摆时间用完 ⇒ 由上层按"回线失败"处理（锁存停车）。*/
uint8_t Path_ReturnExpired(uint32_t now)
{
    return s_phase == PATH_RETURN && (uint32_t)(now - s_phase_ms) >= AV_RETURN_MS;
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
    if (s_phase == PATH_OUT) {
        apply(AV_OUT_L_PWM, AV_OUT_R_PWM);
    } else {
        uint32_t elapsed = (uint32_t)(now - s_phase_ms);
        apply(lerp_pwm(AV_RET_START_L_PWM, AV_RET_END_L_PWM, elapsed, AV_RETURN_MS),
              lerp_pwm(AV_RET_START_R_PWM, AV_RET_END_R_PWM, elapsed, AV_RETURN_MS));
    }

    if (pwm_l) { *pwm_l = s_pwm_l; }
    if (pwm_r) { *pwm_r = s_pwm_r; }
}
