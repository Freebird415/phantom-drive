/* =============================================================================
 *  avoid_path.h —— A02 固定轨迹引擎接口（实现在 avoid_path.c）
 *
 *  用法：路径只在「避障接管」期间跑，参数全在 app/tuning.h §4。
 *    Path_Start(dir_left, now, pwm_l, pwm_r)  开始：从他当前 PWM 接续，先「推出」
 *    Path_Step(now, &l, &r)                   每 10ms 调一次：算这一段的目标并下发电机命令
 *    Path_ReturnExpired(now)                  回摆段跑完 ⇒ 1（上层按"回线失败"处理）
 *    Path_Stop()                              立即停止轨迹（不发电机命令）
 *
 *  dir_left：1 = 向左避（右侧有障碍）｜0 = 向右避（左侧有障碍，自动左右镜像）
 * ==========================================================================*/
#ifndef APP_AVOID_PATH_H
#define APP_AVOID_PATH_H

#include <stdint.h>

void    Path_Start(uint8_t dir_left, uint32_t now, int pwm_l, int pwm_r);
void    Path_Step(uint32_t now, int *pwm_l, int *pwm_r);
void    Path_Stop(void);
uint8_t Path_ReturnExpired(uint32_t now);
uint8_t Path_Returning(void);

#endif /* APP_AVOID_PATH_H */
