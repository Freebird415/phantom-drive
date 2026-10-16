/* A02 向右避障轨迹：左侧障碍 -> 右弧 -> 左弧，直到入口确认回线。
 * 它是 avoidl.c 的左右镜像：时长、PWM 数值及缓变规则完全相同，
 * 只交换左右探头对应的触发方向和左右电机的命令。
 * 超声判侧与“正中默认左避”在 app_avoid.c；本文件只管电机轨迹。 */
#include "app/app_telemetry.h"
#include "drv/drv_motor.h"

#include <stdint.h>

/* _MS 是毫秒，PWM 是电机占空比命令，不等于实测轮速。 */
#define A02_RIGHT_MS                 380u
#define A02_RIGHT_L_PWM               48  /* 左轮快、右轮慢：先向右偏 */
#define A02_RIGHT_R_PWM               0

/* 反向回摆：左右目标 PWM 随时间由 START 线性变为 END。 */
#define A02_LEFT_MS                 5000u
#define A02_LEFT_START_L_PWM          20
#define A02_LEFT_START_R_PWM          48
#define A02_LEFT_END_L_PWM            40
#define A02_LEFT_END_R_PWM            45

#define A02_PWM_STEP                   2  /* 每 10ms 单轮最多变化 2% */

typedef enum { RIGHT_OFF, RIGHT_OUT, RIGHT_RETURN } RightPhase;
static RightPhase s_phase;
static uint32_t s_phase_ms;
static int s_pwm_l, s_pwm_r;

static int slew(int current, int target)
{
    if (current < target - A02_PWM_STEP) { return current + A02_PWM_STEP; }
    if (current > target + A02_PWM_STEP) { return current - A02_PWM_STEP; }
    return target;
}

/* 3000ms 决定 PWM 渐变斜率，也是入口的回线完成上限。 */
static int return_pwm_at(int start, int end, uint32_t elapsed)
{
    if (elapsed >= A02_LEFT_MS) { return end; }
    int32_t change = (int32_t)(end - start) * (int32_t)elapsed;
    if (change >= 0) { change += (int32_t)(A02_LEFT_MS / 2u); }
    else { change -= (int32_t)(A02_LEFT_MS / 2u); }
    return start + (int)(change / (int32_t)A02_LEFT_MS);
}

void AvoidR_Stop(void)
{
    s_phase = RIGHT_OFF;
    s_phase_ms = 0u;
    s_pwm_l = s_pwm_r = 0;
}

/* 从直行的当前 PWM 接续，避免切换阶段时突然跳变。 */
void AvoidR_Start(uint32_t now, int pwm_l, int pwm_r)
{
    s_phase = RIGHT_OUT;
    s_phase_ms = now;
    s_pwm_l = pwm_l;
    s_pwm_r = pwm_r;
    Tlm_SendEvent(3, "A02 left US -> right");
}

uint8_t AvoidR_Returning(void) { return s_phase == RIGHT_RETURN; }
uint8_t AvoidR_ReturnExpired(uint32_t now)
{
    return s_phase == RIGHT_RETURN && now - s_phase_ms >= A02_LEFT_MS;
}
void AvoidR_SyncPwm(int pwm_l, int pwm_r) { s_pwm_l = pwm_l; s_pwm_r = pwm_r; }

/* 正常完成由 app_avoid.c 判线；超时也由入口锁存停车。 */
uint8_t AvoidR_Step(uint32_t now, int *pwm_l, int *pwm_r)
{
    if (s_phase == RIGHT_OFF) { return 0u; }
    if (s_phase == RIGHT_OUT && now - s_phase_ms >= A02_RIGHT_MS) {
        s_phase = RIGHT_RETURN;
        s_phase_ms = now;
        Tlm_SendEvent(3, "A02 replay left");
    }

    int target_l = A02_RIGHT_L_PWM, target_r = A02_RIGHT_R_PWM;
    if (s_phase == RIGHT_RETURN) {
        uint32_t elapsed = now - s_phase_ms;
        target_l = return_pwm_at(A02_LEFT_START_L_PWM, A02_LEFT_END_L_PWM, elapsed);
        target_r = return_pwm_at(A02_LEFT_START_R_PWM, A02_LEFT_END_R_PWM, elapsed);
    }
    s_pwm_l = slew(s_pwm_l, target_l);
    s_pwm_r = slew(s_pwm_r, target_r);
    *pwm_l = s_pwm_l;
    *pwm_r = s_pwm_r;
    Mo_Set(s_pwm_l, s_pwm_r);
    return 0u;
}
