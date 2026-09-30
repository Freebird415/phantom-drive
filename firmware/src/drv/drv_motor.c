/* =============================================================================
 *  drv_motor.c —— TB6612FNG 双电机（TIM1_CH1=PA8 / TIM1_CH4=PA11，20kHz）
 *  说明与注意见 drv_motor.h
 * ========================================================================== */
#include "drv_motor.h"
#include "bsp/bsp_board.h"

#include <stdio.h>

#define PWM_ARR     3199u               /* 64MHz / (3199+1) = 20kHz */
#define PWM_MAX     3199u

static TIM_HandleTypeDef s_htim1;
static int s_l = 0, s_r = 0;

static int clamp(int v) { return (v > MOT_LIMIT) ? MOT_LIMIT : (v < -MOT_LIMIT) ? -MOT_LIMIT : v; }

void Mo_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();

    /* PWM 脚：PA8 / PA11 = TIM1_CH1 / CH4，复用推挽 */
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin   = GPIO_PIN_8 | GPIO_PIN_11;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* 方向脚 + STBY：推挽输出，初值全 0（方向 00 = 滑行，电机不会动） */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin   = GPIO_PIN_0 | GPIO_PIN_1;                 /* PA 没有方向脚，占位 */
    gpio.Pin   = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_12 | GPIO_PIN_13, GPIO_PIN_RESET);

    /* TIM1：PSC=0，ARR=3199 → 20kHz；CH1/CH4 PWM 模式 1 */
    s_htim1.Instance               = TIM1;
    s_htim1.Init.Prescaler         = 0;
    s_htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim1.Init.Period            = PWM_ARR;
    s_htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim1.Init.RepetitionCounter = 0;
    s_htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_PWM_Init(&s_htim1);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = 0;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    (void)HAL_TIM_PWM_ConfigChannel(&s_htim1, &oc, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_ConfigChannel(&s_htim1, &oc, TIM_CHANNEL_4);

    HAL_TIM_PWM_Start(&s_htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&s_htim1, TIM_CHANNEL_4);
    __HAL_TIM_SET_COMPARE(&s_htim1, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&s_htim1, TIM_CHANNEL_4, 0);

    /* STBY 拉高使能驱动；跳线可把它改成一直接高（软件挂死也能停/走） */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);

    s_l = s_r = 0;
    printf("# MOTOR init: TIM1 20kHz, STBY=HIGH, 方向=00(coast), PWM=0\r\n");
}

/* 单个轮：方向 + 占空比 */
static void mo_one(int v, uint32_t ch, uint16_t p1, uint16_t p2)
{
    v = clamp(v);
    uint32_t duty = (uint32_t)((v < 0 ? -v : v) * (int)PWM_MAX / 100);

    if (v > 0)      { HAL_GPIO_WritePin(GPIOB, p1, GPIO_PIN_SET);   HAL_GPIO_WritePin(GPIOB, p2, GPIO_PIN_RESET); }
    else if (v < 0) { HAL_GPIO_WritePin(GPIOB, p1, GPIO_PIN_RESET); HAL_GPIO_WritePin(GPIOB, p2, GPIO_PIN_SET);   }
    else            { HAL_GPIO_WritePin(GPIOB, p1, GPIO_PIN_RESET); HAL_GPIO_WritePin(GPIOB, p2, GPIO_PIN_RESET); }

    __HAL_TIM_SET_COMPARE(&s_htim1, ch, duty);
}

void Mo_Set(int l, int r)
{
    /* 方向符号统一在这里乘（决策 E-22：符号唯一出处） */
    s_l = clamp(l * MOT_SIGN_L);
    s_r = clamp(r * MOT_SIGN_R);
    mo_one(s_l, TIM_CHANNEL_1, GPIO_PIN_0, GPIO_PIN_1);       /* 左：AIN1/AIN2 */
    mo_one(s_r, TIM_CHANNEL_4, GPIO_PIN_12, GPIO_PIN_13);     /* 右：BIN1/BIN2 */
}

void Mo_Stop(void) { Mo_Set(0, 0); }

void Mo_Brake(void)
{
    __HAL_TIM_SET_COMPARE(&s_htim1, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&s_htim1, TIM_CHANNEL_4, 0);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_12, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_13, GPIO_PIN_SET);
    s_l = s_r = 0;
}

void Mo_PrintDebug(void)
{
    printf("MOT> L=%d%% R=%d%% | AIN1=%d AIN2=%d BIN1=%d BIN2=%d STBY=%d | CCR1=%u CCR4=%u\r\n",
           s_l, s_r,
           (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0), (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1),
           (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_12), (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_13),
           (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_14),
           (unsigned)__HAL_TIM_GET_COMPARE(&s_htim1, TIM_CHANNEL_1),
           (unsigned)__HAL_TIM_GET_COMPARE(&s_htim1, TIM_CHANNEL_4));
}
