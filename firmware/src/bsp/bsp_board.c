/* =============================================================================
 *  bsp_board.c —— 板级支持实现（时钟 / 计时 / 串口 / 指示灯）
 * ========================================================================== */
#include "bsp_board.h"
#include <stdint.h>

/* ==================== 时钟 ==================== */
void Bsp_ClockInit(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
    osc.PLL.PLLMUL          = RCC_PLL_MUL16;           /* 8MHz/2 ×16 = 64MHz */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { while (1) { } }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) { while (1) { } }
}

/* ==================== 时间 ==================== */
/* HAL 依赖 SysTick 做毫秒计时 —— 用 PlatformIO 的 stm32cube 框架时必须自己提供，
   否则中断一跳就进“默认处理器”死循环（现象：灯不闪、串口无输出） */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Bsp_TimeInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t Bsp_Micros(void)
{
    return DWT->CYCCNT / (SystemCoreClock / 1000000u);   /* 64MHz → /64 */
}

/* ==================== 串口 ==================== */
static UART_HandleTypeDef s_huart1;

UART_HandleTypeDef *Bsp_Uart(void) { return &s_huart1; }

/* printf 重定向到 USART1（轮询发送：调试阶段够用且实现最简单） */
int _write(int fd, char *ptr, int len)
{
    (void)fd;
    HAL_UART_Transmit(&s_huart1, (uint8_t *)ptr, (uint16_t)len, 100);
    return len;
}

/* HAL 会调用这个 MspInit —— 注意：**不能加 static**（HAL 头文件里是非静态声明） */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};
    if (huart->Instance != USART1) { return; }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin   = GPIO_PIN_9;                 /* PA9 = USART1_TX */
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin  = GPIO_PIN_10;                 /* PA10 = USART1_RX */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_NVIC_SetPriority(USART1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void Bsp_UartInit(void)
{
    s_huart1.Instance          = USART1;
    s_huart1.Init.BaudRate     = 115200;
    s_huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    s_huart1.Init.StopBits     = UART_STOPBITS_1;
    s_huart1.Init.Parity       = UART_PARITY_NONE;
    s_huart1.Init.Mode         = UART_MODE_TX_RX;
    s_huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    s_huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_huart1) != HAL_OK) { while (1) { } }
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_huart1);
}

/* ==================== 指示灯 ==================== */
void Bsp_LedInit(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Pin   = GPIO_PIN_13;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);     /* 低电平点亮 → 先灭 */
}

void Bsp_LedToggle(void)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}
