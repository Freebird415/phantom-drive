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
void    Trace_StartRun(void);           /* ⭐ 发车统一入口：复位精停 + 开启起步找线 */
void    Trace_SetSeek(uint8_t on);      /* 命令 `SEEK 0|1`：手动开关起步找线（调试用）*/
uint8_t Trace_Online(void);             /* 灰度是否在线（1=真实读数；0=模拟数据且不驱动）*/
uint8_t Trace_GetDrive(void);

/* ---- 终点精停（T 头 / 终止线）----
   规则：车头必须过线，且车尾投影不得过线（20 分）
   原理：正常巡线只有 1~2 路看到线；到终止线（横向 T 头）时 **8 路同时全黑**
   ⚠️ 前提：赛道是「白底窄黑线」。若测试面是一大片黑（宽胶带/黑纸），会误触发 */
void    Trace_DistCal(float meters);    /* 标定：ENCRESET → 推车走 meters 米 → 调它 */
void    Trace_SetStopDist(int mm);      /* 设置「检到线后再前进多少 mm」（默认 60）*/
void    Trace_FinStatus(void);          /* FINSTAT：打印精停状态 / 标定值 / 阈值 */
void    Trace_FinReset(void);           /* FINRESET：复位精停状态，方便再跑一趟 */
void    Trace_Dash(void);               /* 车载 OLED 仪表盘（main 主循环末尾调）*/

/* 仪表盘开关（命令 `DASH 0|1`）。
   ⚠️ 仪表盘每 500ms 清屏重画，会把 `OLED` / `OLEDTXT` 的测试页盖掉；
   所以发测试页前先 `DASH 0`（命令处理里已自动关），看完发 `DASH 1` 恢复。*/
void    Trace_DashEnable(uint8_t on);

#endif /* APP_TRACE_H */
