/* =============================================================================
 *  bsp_board.h —— 板级支持（时钟 / µs 计时 / 串口 / 指示灯）
 *  firmware 目录规则见 ../README.md：一个外设一个文件，只放“板上资源”的初始化与最薄封装
 * ========================================================================== */
#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "stm32f1xx_hal.h"

/* ---- 时钟：HSI 8MHz → PLL×16 → 64MHz（不依赖外部晶振） ---- */
void Bsp_ClockInit(void);

/* ---- 时间 ----
 * SysTick：HAL 的 1ms 时基（HAL_GetTick / HAL_Delay），中断里只做 HAL_IncTick
 * DWT->CYCCNT：µs 级计时，给超声波回波用（不占定时器与引脚） */
void     Bsp_TimeInit(void);
uint32_t Bsp_Micros(void);

/* ---- 串口：USART1 115200 8N1（PA9 = TX，PA10 = RX），printf 直接可用 ---- */
void Bsp_UartInit(void);
UART_HandleTypeDef *Bsp_Uart(void);

/* ---- 串口2：USART2 115200 8N1（PA2 = TX，PA3 = RX）→ BLE 蓝牙调试通道 ---- */
void Bsp_Uart2Init(void);
UART_HandleTypeDef *Bsp_Uart2(void);
void Bsp_Uart2SetBaud(uint32_t baud);   /* 运行时改 BLE 通道波特率（扫波特率用） */

/* ---- USART2 【非阻塞】发送：环形队列 + 中断发送（BLE 日志流用）----
   为什么要它：9600bps 下 8 字节要 ~8.3ms，而控制周期只有 10ms ——
   阻塞发送会把控制循环吃掉 ⇒ 一律走这个队列。
   ⚠️ 队列满时【丢弃并计数】（Bsp_Uart2TxDrops），绝不阻塞。 */
void     Bsp_Uart2Tx(const uint8_t *d, uint16_t n);
void     Bsp_Uart2TxAbort(void);        /* 清队列 + 停中断发送（AT 类阻塞命令前调）*/
uint16_t Bsp_Uart2TxDrops(void);        /* 累计丢弃字节数（诊断用）*/

/* ---- 定点格式化（**不是硬件**，但必须放在最底层才能给 drv 与 app 共用）----
 * 为什么不直接用 printf("%f")：newlib-nano 的浮点 printf 会拉进 _dtoa_r + _printf_float
 * + 整套双精度软浮点库，实测占 **~6KB Flash**（本项目 Flash 很紧）。
 * 本函数用整数运算自己拼字符串，输出与 printf("%.*f") **格式一致**，不拉库。
 *   dst  : 输出缓冲（建议 ≥16 字节）
 *   n    : 缓冲长度
 *   v    : 要打印的值
 *   dec  : 小数位数（0~4）
 *   plus : 1 = 正数也带 '+'（等价 %+.2f）
 * 例：Bsp_FmtFix(b,n,1.01f,2,0) → "1.01"；Bsp_FmtFix(b,n,-4.3f,1,1) → "-4.3" */
void Bsp_FmtFix(char *dst, int n, float v, int dec, int plus);

/* ---- 指示灯：核心板 PC13（低电平点亮） ---- */
void Bsp_LedInit(void);
void Bsp_LedToggle(void);

#endif /* BSP_BOARD_H */
