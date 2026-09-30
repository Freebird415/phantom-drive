/* =============================================================================
 *  app_log.h —— 板载快照（"黑匣子"）：RAM 记录 + 内部 Flash 持久化
 *
 *  为什么需要它：**跑车的时候插不了 USB-TTL**（线会拖车）。
 *  所以数据先在板子上记着，**跑完插回串口，用 DUMP 读出来**。
 *
 *  两层存储：
 *    ① **RAM**（2500 帧 × 5B = 12.5KB，100Hz ⇒ 约 25 秒）
 *       跑车过程中只写这里 —— **零阻塞**，不影响控制节奏 ✓
 *    ② **内部 Flash**（最后 16KB，地址 0x0800C000）
 *       停车时（`Log_Stop()`）**自动把 RAM 内容写进去** ⇒ **掉电不丢** ✓
 *       上电时（`Log_LoadFlash()`）自动恢复 ⇒ `DUMP` / `SNAP?` 照常看到 ✓
 *
 *  ⚠️ **固件体积红线**：Flash 最后 16KB（0x0800C000 起）被快照占用
 *     ⇒ **固件必须 < 48KB**（64KB − 16KB）。当前约 42.4KB，余量 ~5.6KB。
 *     每次编译后请看 PlatformIO 报的 Flash 百分比：**超过 75% 就要警惕**。
 *
 *  ⚠️ Flash 擦写会**停住 CPU 约 0.5 秒**（16 页）—— 所以只在"停车后"写，
 *     跑车过程中绝不写 Flash。
 *
 *  帧内容（5 字节）：8 路灰度压成 1 字节 + err(×10) + pwm_l + pwm_r + state
 * ========================================================================== */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

#define LOG_MAX_FRAMES  2000u   /* 2000 × 6B = 12KB；100Hz ⇒ 约 20 秒（A4 短跑绰绰有余）*/

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

void     Log_Status(void);                       /* SNAP? 打印状态 */
void     Log_FlashInfo(void);                     /* SNAPFLASH：直读 Flash 头，排查用 */
void     Log_Dump(void);                         /* DUMP：以 $T 帧格式吐出所有数据 */

#endif /* APP_LOG_H */
