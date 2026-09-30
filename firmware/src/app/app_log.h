/* =============================================================================
 *  app_log.h —— 板载快照（"黑匣子"）：RAM 记录 + 内部 Flash 持久化
 *
 *  为什么需要它：**跑车的时候插不了 USB-TTL**（线会拖车）。
 *  所以数据先在板子上记着，**跑完插回串口，用 DUMP 读出来**。
 *
 *  两层存储：
 *    ① **RAM**（`LOG_MAX_FRAMES` 帧 × 8B，100Hz ⇒ 十几秒）
 *       跑车过程中只写这里 —— **零阻塞**，不影响控制节奏 ✓
 *    ② **内部 Flash**（最后 16KB，地址 0x0800C000）
 *       停车时（`Log_Stop()`）**自动把 RAM 内容写进去** ⇒ **掉电不丢** ✓
 *       上电时（`Log_LoadFlash()`）自动恢复 ⇒ `DUMP` / `SNAP?` 照常看到 ✓
 *
 *  ⚠️ **固件体积红线**：Flash 最后 16KB（0x0800C000 起）被快照占用
 *     ⇒ **固件必须 < 48KB**（64KB − 16KB）。**实测必看 `firmware.bin` 大小，
 *     别只看 PlatformIO 的百分比**；`firmware/check_size.py` 会在编译后
 *     自动比对，超了直接报错（这类覆盖问题编译器本身不会提醒）。
 *
 *  ⚠️ Flash 擦写会**停住 CPU 约 0.5 秒**（16 页）—— 所以只在"停车后"写，
 *     跑车过程中绝不写 Flash。
 *
 *  帧内容（8 字节）：8 路灰度(1) + err×10(1) + pwm_l(1) + pwm_r(1) + state(1)
 *                    + **dL(1) + dR(1)** + gz(1)
 *
 *  ⭐ 为什么要存 dL/dR（左右轮本帧的编码器增量）：
 *     有了左右轮真实速度就能做【航位推算】：
 *         车心速度 v = (vL+vR)/2      ⇒ 积分得走过的距离
 *         角速度   ω = (vR−vL)/轮距   ⇒ 积分得车头朝向
 *     两者一合 = **画出小车实际轨迹 = 轨道形状** ✓
 *     只存 pwm 是不行的 —— 那是“命令值”，不是车轮真实速度（打滑/堆转看不出来）
 * ========================================================================== */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

#define LOG_MAX_FRAMES  1600u   /* 1600 × 8B = 12.8KB；100Hz ⇒ 约 16 秒 */

void     Log_Reset(void);                        /* 清 RAM + 使 Flash 快照失效 */
void     Log_Start(void);                        /* 开始记录（只写 RAM）*/
void     Log_Stop(void);                         /* 停止记录 + **自动存入 Flash** */
uint8_t  Log_IsRecording(void);
uint16_t Log_Count(void);

void     Log_SaveFlash(void);                    /* 手动存（Log_Stop 已自动调）*/
void     Log_LoadFlash(void);                    /* 上电时从 Flash 恢复（main 里调）*/

/* 每拍调用（由 app_trace.c 里的 Trace_Step 调）
   bits8：bit0=第1路 … bit7=第8路；1 = 黑 */
void     Log_Note(uint8_t bits8, float err, int pwm_l, int pwm_r, int state);

/* 最近一帧（给 OLED 仪表盘看）*/
typedef struct {
    uint8_t bits; int err10, pwm_l, pwm_r, state;
    int dl, dr, gz; uint16_t n;
} LogDash;
void     Log_GetDash(LogDash *d);

void     Log_Status(void);                       /* SNAP? 打印状态 */
void     Log_FlashInfo(void);                     /* SNAPFLASH：直读 Flash 头，排查用 */
void     Log_Dump(void);                         /* DUMP：以 $T 帧格式吐出所有数据 */

#endif /* APP_LOG_H */
