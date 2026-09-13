/*
 * 遥测链路 demo —— P01 反馈链路与工具链
 *
 * 目标：打通「固件定时打印 → 电脑落盘 CSV → 画图 → 改参数重烧」闭环。
 * 现在车上还没有传感器/电机，$T 帧里是模拟数据（三角波）。
 * 接上真实模块后，只需替换 main() 里标注的那一段，帧格式不变。
 *
 * 硬件接线：
 *   ST-Link ：SWDIO→PA13，SWCLK→PA14，GND→GND，3V3→3V3
 *   USB-TTL ：TXD→PA10(RX)，RXD→PA9(TX)，GND→GND（TTL 的电源脚不要接）
 *
 * 协议详见 docs/遥测协议.md
 */
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart1;

/* ==================== 可调参数 ==================== */
typedef struct {
    float kp;
    float ki;
    float kd;
    int   base_speed;
} Params;

static Params g_p = { 1.20f, 0.00f, 0.05f, 30 };

/* ==================== printf → UART1 ==================== */
int _write(int fd, char *ptr, int len)
{
    (void)fd;
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100);
    return len;
}

/* ==================== 时钟：HSI 8MHz → PLL×16 = 64MHz ==================== */
static void SystemClock_Config(void)
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
    osc.PLL.PLLMUL          = RCC_PLL_MUL16;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) { while (1) {} }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) { while (1) {} }
}

/* ==================== UART1（PA9 TX / PA10 RX） ==================== */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};
    if (huart->Instance != USART1) { return; }

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin   = GPIO_PIN_9;              /* TX */
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin  = GPIO_PIN_10;              /* RX */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}

/* HAL 依赖 SysTick 做毫秒计时（HAL_GetTick / HAL_Delay）——
   用 PlatformIO 的 stm32cube 框架时必须自己提供，否则中断一跳就进死循环 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ==================== 帧输出 ==================== */
static void send_params(void)
{
    printf("$P,%u,%.2f,%.2f,%.2f,%d\r\n",
           (unsigned)HAL_GetTick(),
           (double)g_p.kp, (double)g_p.ki, (double)g_p.kd, g_p.base_speed);
}

static void handle_command(char *line)
{
    if (strcmp(line, "PING") == 0) {
        printf("$E,%u,90,PONG\r\n", (unsigned)HAL_GetTick());
        return;
    }
    if (strcmp(line, "GET") == 0) {
        send_params();
        return;
    }
    if (strncmp(line, "SET ", 4) == 0) {
        char  name[16] = {0};
        float val = 0.0f;
        if (sscanf(line + 4, "%15s %f", name, &val) == 2) {
            if      (strcmp(name, "kp") == 0)         { g_p.kp = val; }
            else if (strcmp(name, "ki") == 0)         { g_p.ki = val; }
            else if (strcmp(name, "kd") == 0)         { g_p.kd = val; }
            else if (strcmp(name, "base_speed") == 0) { g_p.base_speed = (int)val; }
            send_params();    /* 改完回一帧参数：日志里就能看到"何时改成了什么" */
            return;
        }
    }
    printf("$E,%u,99,UNKNOWN_CMD\r\n", (unsigned)HAL_GetTick());
}

/* ---- 行接收（中断逐字节） ---- */
static char    s_rxline[64];
static uint8_t s_rxlen = 0;
static uint8_t s_rxbyte;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        char c = (char)s_rxbyte;
        if (c == '\n' || c == '\r') {
            if (s_rxlen > 0) {
                s_rxline[s_rxlen] = '\0';
                handle_command(s_rxline);
                s_rxlen = 0;
            }
        } else if (s_rxlen < sizeof(s_rxline) - 1) {
            s_rxline[s_rxlen++] = c;
        } else {
            s_rxlen = 0;      /* 太长，丢弃 */
        }
        HAL_UART_Receive_IT(&huart1, &s_rxbyte, 1);
    }
}

/* ==================== 心跳灯（PC13，蓝板 LED） ==================== */
static void Heartbeat_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Pin   = GPIO_PIN_13;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);   /* 低电平点亮 */
}

/* ==================== 主循环 ==================== */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    Heartbeat_Init();
    MX_USART1_UART_Init();
    HAL_UART_Receive_IT(&huart1, &s_rxbyte, 1);

    send_params();                 /* 上电先报一次参数快照 */
    printf("$E,%u,97,BOOT\r\n", (unsigned)HAL_GetTick());

    uint32_t next_tick = HAL_GetTick();
    uint32_t next_led  = HAL_GetTick();
    uint32_t frames    = 0;

    while (1) {
        /* 心跳：LED 每秒闪一次（证明程序在跑，串口不通时靠它判断） */
        if ((int32_t)(HAL_GetTick() - next_led) >= 0) {
            next_led += 500;
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        }

        if ((int32_t)(HAL_GetTick() - next_tick) >= 0) {
            next_tick += 10;       /* 100 Hz */

            uint32_t ms = HAL_GetTick();

            /* ======== 模拟数据：接上真实模块后替换这一段 ======== */
            int32_t k      = (int32_t)((ms / 10u) % 200u);      /* 0..199 */
            int32_t err_x10 = (k < 100) ? (k - 50) : (150 - k); /* -50..+49 → 单位 0.1 */

            int s[8];
            int center = 3 + (int)(err_x10 / 25);
            if (center < 0) { center = 0; }
            if (center > 7) { center = 7; }
            for (int i = 0; i < 8; i++) {
                s[i] = (i == center || i == center + 1) ? 1 : 0;
            }

            float   err    = (float)err_x10 / 10.0f;
            int32_t err_i  = err_x10 / 5;                       /* -10..+9 */
            int     pwm_l  = g_p.base_speed + (int)err_i;
            int     pwm_r  = g_p.base_speed - (int)err_i;
            int     state  = 1;                                 /* 1 = 循迹 */
            /* ================================================== */

            printf("$T,%u,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%d,%d,%d\r\n",
                   (unsigned)ms,
                   s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                   (double)err, pwm_l, pwm_r, state);

            frames++;
            if (frames % 500u == 0u) {                          /* 每 5 秒一个演示事件 */
                printf("$E,%u,98,DEMO_EVENT\r\n", (unsigned)ms);
            }
        }
    }
}
