/* =============================================================================
 *  app_avoid.h —— 避障与回线（A02 毕烨恺）
 *
 *  ★★★ 这是你的文件。整个工程里你只需要改这一个 .c 文件。★★★
 *
 *  会被主程序调用的函数：
 *      Avoid_Init()        上电调用一次
 *      Avoid_Step()        主循环 10ms 节拍调用；内部只用新测距累计确认
 *      Avoid_Mode()        返回 1 = 避障接管或故障刹停，主循环暂停循迹
 *
 *  可以用的驱动函数（详细说明见 firmware/驱动接口说明.md）：
 *      Us_Get(id)->mm      最近一次超声波读数：>0 是 mm；**<=0 是状态码**（见下）
 *      Us_Read(id)         主动测一次（一般不用，主循环已经在测了）
 *      Mo_Set(l, r)        给左右轮占空比 −100~100
 *      Mo_Stop() / Mo_Brake()
 *      Tlm_SendEvent(c, s) 打一个 $E 事件点（日志里一眼看到"这里避障了"）
 *
 *  ⚠️ 读超声波**必须先判状态码**：mm <= 0 表示"没测到/超量程"，不是"很近"！
 *       US_NO_RISE(−1) 没回波 ｜ US_TOO_LONG(−2) 超量程 ｜ US_BUSY(−3)
 *       US_TOO_SHORT(−4) ｜ US_OUT_RANGE(−5) ｜ US_TOO_SOON(−6) 触发太密
 *
 *  ⚠️ 安全：上电驱动默认关；长按发车时主循环自动使能，停车时关闭。
 *     `AVOID 1` 只使能策略，仍须循迹发车开关打开才允许接管。
 * ========================================================================== */
#ifndef APP_AVOID_H
#define APP_AVOID_H

#include <stdint.h>

void    Avoid_Init(void);
void    Avoid_Step(void);

/* 1 = 避障接管中（会持续一段时间，不是只一拍）；循迹见到 1 就让位 */
uint8_t Avoid_Mode(void);

void    Avoid_SetDrive(uint8_t on);     /* 命令 `AVOID 0|1` 用 */
uint8_t Avoid_GetDrive(void);

#endif /* APP_AVOID_H */
