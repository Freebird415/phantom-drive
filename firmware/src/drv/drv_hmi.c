/* =============================================================================
 *  drv_hmi.c —— 用户按键(PA12) + 蜂鸣器(PB15)，说明见 drv_hmi.h
 * ========================================================================== */
#include "drv_hmi.h"
#include "bsp/bsp_board.h"
#include <stdio.h>

#define KEY_PORT    GPIOA
#define KEY_PIN     GPIO_PIN_12
#define BEEP_PORT   GPIOB
#define BEEP_PIN    GPIO_PIN_15

void Hmi_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* 按键：输入 + 上拉（核心板另有 1.5k 上拉，双保险）；按下接地 → 读 0 */
    gpio.Pin  = KEY_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KEY_PORT, &gpio);

    /* 蜂鸣器驱动脚：推挽输出，初值低（不响） */
    gpio.Pin   = BEEP_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BEEP_PORT, &gpio);
    HAL_GPIO_WritePin(BEEP_PORT, BEEP_PIN, GPIO_PIN_RESET);
}

uint8_t Hmi_Key(void)
{
    return (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
}

void Hmi_BeepOn(void)  { HAL_GPIO_WritePin(BEEP_PORT, BEEP_PIN, GPIO_PIN_SET); }
void Hmi_BeepOff(void) { HAL_GPIO_WritePin(BEEP_PORT, BEEP_PIN, GPIO_PIN_RESET); }

void Hmi_Beep(uint16_t ms)
{
    Hmi_BeepOn();
    HAL_Delay(ms);
    Hmi_BeepOff();
}

void Hmi_PrintDebug(void)
{
    printf("HMI> 按键=%s (PA12=%d)  蜂鸣器 PB15=%d\r\n",
           Hmi_Key() ? "按下" : "松开",
           (int)HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN),
           (int)HAL_GPIO_ReadPin(BEEP_PORT, BEEP_PIN));
}
