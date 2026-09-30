/* =============================================================================
 *  app_trace.h —— 循迹算法（A01 熊雅琦）
 *
 *  ★★★ 这是你的文件。整个工程里你只需要改这一个 .c 文件。★★★
 *
 *  只有两个函数会被主程序调用：
 *      Trace_Init()                 上电调用一次
 *      Trace_Step(allow_drive)      主循环每 10ms 调一次（100Hz）
 *
 *  可以用的驱动函数（详细说明见 firmware/驱动接口说明.md，就这几个够用了）：
 *      Gr_ReadDigital(&d) 读灰度数字量：成功返回 1、8 位写进 d（bit0=第1路…bit7=第8路）
 *                         模块原始极性是 **1=白 / 0=黑**（实测确认）；
 *                         骨架已经替你翻好了，你手上的 s[i] 一律是 **1=黑**
 *      Gr_ReadAnalog()    读 8 路模拟量（0~255），想做加权/自适应时用
 *      Mo_Set(l, r)       给左右轮占空比 −100~100（负=反转）
 *      Mo_Stop()          停（滑行）
 *      En_Count(id)       读编码器计数（ENC_L / ENC_R）
 *      Tlm_SendT(...)     上报一帧 $T（没调它，电脑那边就看不到你这拍的数据）
 *      Tlm_Params()       取在线可调参数 {kp, ki, kd, base_speed}
 *
 *  ⚠️ 安全：s_drive 默认 **关**（只算不发，电机不动）。想让它真的驱动轮子，
 *     串口发 `TRACE 1`（或把 app_trace.c 里的 s_drive 初值改成 1）。
 *     第一次一定要**把车架起来、轮子悬空**再开。
 * ========================================================================== */
#ifndef APP_TRACE_H
#define APP_TRACE_H

#include <stdint.h>

void    Trace_Init(void);

/* allow_drive = 0 → 只计算、不发电机命令（避障接管时主程序会这样传） */
void    Trace_Step(uint8_t allow_drive);

void    Trace_SetDrive(uint8_t on);     /* 命令 `TRACE 0|1` 用 */
uint8_t Trace_GetDrive(void);

#endif /* APP_TRACE_H */
