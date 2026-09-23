/* =============================================================================
 *  app_telemetry.h —— 遥测帧输出 + 电脑命令处理（协议见 docs/遥测协议.md）
 *
 *  $T,ms,s0..s7,err,pwm_l,pwm_r,state     100 Hz   循迹/电机
 *  $P,ms,kp,ki,kd,base_speed              上电一次 参数快照
 *  $E,ms,code,note                        事件触发 打点
 *  $U,ms,l_mm,r_mm                        10 Hz    超声波
 *  命令：PING / GET / SET <名> <值> / US
 * ========================================================================== */
#ifndef APP_TELEMETRY_H
#define APP_TELEMETRY_H

#include <stdint.h>

/* 可调参数（在线改，$P 帧会把“当时用的哪套参数”记进日志） */
typedef struct {
    float kp, ki, kd;
    int   base_speed;
} Params;

Params *Tlm_Params(void);

void Tlm_Init(void);                                  /* 开中断收行 */
void Tlm_Poll(void);                                  /* 主循环调用：取出并执行收到的命令 */

/* 主循环是否自动测超声波（调试时用 AUTO 0 关掉，避免和命令抢节拍） */
uint8_t Tlm_UsAuto(void);

void Tlm_SendParams(void);                            /* $P */

void Tlm_SendT(uint32_t ms, const int s[8], float err,
               int pwm_l, int pwm_r, int state);      /* $T */
void Tlm_SendEvent(uint8_t code, const char *note);   /* $E */
void Tlm_SendU(uint32_t ms, int32_t l_mm, int32_t r_mm);  /* $U */

#endif /* APP_TELEMETRY_H */
