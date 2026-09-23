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

/* ---- 指示灯：核心板 PC13（低电平点亮） ---- */
void Bsp_LedInit(void);
void Bsp_LedToggle(void);

#endif /* BSP_BOARD_H */
