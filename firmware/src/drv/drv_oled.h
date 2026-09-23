/* =============================================================================
 *  drv_oled.h —— 0.96" SSD1306 OLED（I2C2：PB10=SCL / PB11=SDA）
 *
 *  实物信息（背面丝印照片确认，2026-09-23）：
 *    0.96 OLED · 4P 丝印 GND/VCC/SCL/SDA · "IIC ADRESS SELECT 0x78" → 7bit 0x3C
 *    R5/R6/R7/R10 = 472（4.7k）→ **板载已有 I2C 上拉**；面板为"上黄下蓝"双色
 *
 *  ⚠️ 两条硬约束：
 *   1) **VCC 必须 3.3V**：板载上拉拉到 VCC → 3.3V 供电才能让整条 I2C2 保持 3.3V；
 *      接 5V 会把总线拉到 5V，将来挂 MPU6050（3.3V 器件，极限 VDD+0.3）会被打坏。
 *   2) **绝不能挂到 I2C1（灰度那条 5V 上拉总线）** —— SSD1306 会被 5V 打坏。
 *
 *  文字：**字库是子集生成的**（`tools/生成字库.py` → `font_subset.h`）。
 *    · ASCII 6×8（0x20~0x7E）
 *    · 汉字 16×16，**只有 CN_CHARS 里列出的那些字**
 *    · 要显示新汉字 → 改 `tools/生成字库.py` 的 CN_CHARS → 重跑脚本 → 重新编译
 *    · 字库里没有的字会画成"空心方框"，并且串口会打印出来（方便定位缺哪个字）
 * ========================================================================== */
#ifndef DRV_OLED_H
#define DRV_OLED_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#define OLED_W      128
#define OLED_H      64

void    Ol_Init(void);          /* I2C2 初始化 */
I2C_HandleTypeDef *Ol_Bus(void); /* 暴露 I2C2 句柄（MPU6050 等共用同一条总线） */
void    Ol_Present(void);       /* 探测并打印地址（0x3C/0x3D） */
void    Ol_PrintDebug(void);    /* 初始化 + 画棋盘测试图案（bring-up） */
void    Ol_TextTest(void);      /* ★ 中英文混排测试页 + 报告缺失字符 */

/* ---- 帧缓冲（1024 字节 = 8 页 × 128 列）；字符先画进缓冲再一次性刷屏 ---- */
void    Ol_FbClear(void);
void    Ol_FbFlush(void);
int     Ol_Text(int x, int page, const char *utf8);   /* 返回缺失字符个数（-1 = 整串没画） */

#endif /* DRV_OLED_H */
