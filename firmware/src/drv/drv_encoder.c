/* =============================================================================
 *  drv_encoder.c —— JGB37-520 编码器 ×2（TIM4 左 / TIM3 右，硬件编码器模式）
 *  说明与注意见 drv_encoder.h
 * ========================================================================== */
#include "drv_encoder.h"
#include "bsp/bsp_board.h"

#include <stdio.h>

static TIM_HandleTypeDef s_tim4;        /* 左（PB6/PB7） */
static TIM_HandleTypeDef s_tim3;        /* 右（PA6/PA7） */
static int16_t s_base_l = 0, s_base_r = 0;   /* 清零基线（累计变化量用） */

static void enc_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* 编码器输入：复用输入 + 上拉（编码器多为开集/推挽，上拉都安全） */
    gpio.Mode  = GPIO_MODE_AF_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;                 /* PA6/PA7 = TIM3 */
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;                 /* PB6/PB7 = TIM4 */
    HAL_GPIO_Init(GPIOB, &gpio);
}

static void enc_tim_init(TIM_HandleTypeDef *h, TIM_TypeDef *inst)
{
    TIM_Encoder_InitTypeDef enc = {0};

    /* ⚠️ 必须显式指定编码器模式！{0} 会让 EncoderMode=0（非法值）→
       定时器退化成用内部时钟计数（表现为计数自己狂跑几千/秒） */
    enc.EncoderMode  = TIM_ENCODERMODE_TI12;      /* 两通道都计，4 倍频 */
    enc.IC1Polarity  = TIM_ICPOLARITY_RISING;
    enc.IC1Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC1Prescaler = TIM_ICPSC_DIV1;
    enc.IC1Filter    = 0;                         /* 计数有毛刺就调大（0~15） */
    enc.IC2Polarity  = TIM_ICPOLARITY_RISING;
    enc.IC2Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC2Prescaler = TIM_ICPSC_DIV1;
    enc.IC2Filter    = 0;

    h->Instance               = inst;
    h->Init.Prescaler         = 0;
    h->Init.CounterMode       = TIM_COUNTERMODE_UP;
    h->Init.Period            = 0xFFFF;                 /* 16 位满量程 */
    h->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    h->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Encoder_Init(h, &enc);                /* 全 0 = TI12 模式、上升沿、无滤波 */
    HAL_TIM_Encoder_Start(h, TIM_CHANNEL_ALL);
    __HAL_TIM_SET_COUNTER(h, 0);
}

void En_Init(void)
{
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM4_CLK_ENABLE();
    enc_gpio_init();
    enc_tim_init(&s_tim3, TIM3);        /* 右 */
    enc_tim_init(&s_tim4, TIM4);        /* 左 */
    printf("# ENCODER init: TIM4=左(PB6/PB7) TIM3=右(PA6/PA7) 编码器模式 TI12\r\n");
}

int16_t En_Count(EncId id)
{
    if (id == ENC_L) { return (int16_t)__HAL_TIM_GET_COUNTER(&s_tim4); }
    return (int16_t)__HAL_TIM_GET_COUNTER(&s_tim3);
}

void En_Reset(void)
{
    __HAL_TIM_SET_COUNTER(&s_tim4, 0);
    __HAL_TIM_SET_COUNTER(&s_tim3, 0);
    s_base_l = s_base_r = 0;
}

void En_PrintDebug(void)
{
    printf("ENC> L=%d R=%d\r\n", (int)En_Count(ENC_L), (int)En_Count(ENC_R));
}
