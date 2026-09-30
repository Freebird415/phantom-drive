/* =============================================================================
 *  bsp_board.c —— 板级支持实现（时钟 / 计时 / 串口 / 指示灯）
 * ========================================================================== */
#include "bsp_board.h"
#include <stdint.h>
#include <stdio.h>

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

    if (huart->Instance == USART2) {          /* BLE：PA2=TX / PA3=RX */
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_USART2_CLK_ENABLE();
        gpio.Pin = GPIO_PIN_2; gpio.Mode = GPIO_MODE_AF_PP; gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &gpio);
        gpio.Pin = GPIO_PIN_3; gpio.Mode = GPIO_MODE_INPUT; gpio.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(GPIOA, &gpio);
        HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
        return;
    }

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

/* ==================== I2C 引脚（HAL 只允许一个 MspInit，所以两条总线都放这里） ==================== */
void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    GPIO_InitTypeDef gpio = {0};

    if (hi2c->Instance == I2C1) {                 /* 灰度：PB8=SCL / PB9=SDA（重映射），上拉 5V */
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_I2C1_CLK_ENABLE();
        gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
        gpio.Mode = GPIO_MODE_AF_OD;
        gpio.Pull = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOB, &gpio);
    } else if (hi2c->Instance == I2C2) {          /* OLED/MPU6050：PB10=SCL / PB11=SDA，上拉 3.3V */
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_I2C2_CLK_ENABLE();
        gpio.Pin = GPIO_PIN_10 | GPIO_PIN_11;
        gpio.Mode = GPIO_MODE_AF_OD;
        gpio.Pull = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOB, &gpio);
    }
}

/* ==================== USART2（BLE 蓝牙调试通道） ==================== */
static UART_HandleTypeDef s_huart2;

UART_HandleTypeDef *Bsp_Uart2(void) { return &s_huart2; }

void Bsp_Uart2Init(void)
{
    s_huart2.Instance          = USART2;
    s_huart2.Init.BaudRate     = 9600;    /* ⚠️ HC-06 / AT-09 类 BLE 模块默认 9600（不是 115200）*/
    s_huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    s_huart2.Init.StopBits     = UART_STOPBITS_1;
    s_huart2.Init.Parity       = UART_PARITY_NONE;
    s_huart2.Init.Mode         = UART_MODE_TX_RX;
    s_huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    s_huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_huart2) != HAL_OK) { while (1) { } }
}

void Bsp_Uart2SetBaud(uint32_t baud)
{
    s_huart2.Init.BaudRate = baud;
    (void)HAL_UART_Init(&s_huart2);
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_huart2);
}

/* ---- 定点格式化（实现见 bsp_board.h 的说明）----
   全程整数运算：把 v×10^dec 四舍五入成整数，再自己拼 ".dd" 尾巴。
   好处：不链接 newlib 的浮点 printf（_dtoa_r + _printf_float + 双精度库 ≈ 6KB）。*/
void Bsp_FmtFix(char *dst, int n, float v, int dec, int plus)
{
    static const int32_t SC[5] = { 1, 10, 100, 1000, 10000 };
    int32_t scale, q, a, ip, fp;
    const char *sign;

    if (dec < 0) { dec = 0; }
    if (dec > 4) { dec = 4; }
    scale = SC[dec];

    {   /* float → 整数（四舍五入；只需单精度乘加，不拉双精度库）*/
        float x = v * (float)scale;
        q = (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
    }
    a = (q < 0) ? -q : q;
    sign = (q < 0) ? "-" : (plus ? "+" : "");

    if (dec == 0) {
        snprintf(dst, (size_t)n, "%s%ld", sign, (long)a);
    } else {
        ip = a / scale;
        fp = a % scale;
        snprintf(dst, (size_t)n, "%s%ld.%0*ld", sign, (long)ip, dec, (long)fp);
    }
}
