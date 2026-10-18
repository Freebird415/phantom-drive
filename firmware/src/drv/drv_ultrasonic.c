/* =============================================================================
 *  drv_ultrasonic.c —— HC-SR04 ×2 驱动（阻塞轮询版，bring-up 用）
 *
 *  时序：Trig 拉低 2µs → 拉高 10µs → 拉低；模块发 8 个 40kHz 脉冲；
 *        Echo 变高，宽度 = 往返时间 → 距离 mm = width_us × 10 / 58
 *
 *  已知取舍：等回波是阻塞式，最坏一次阻塞 US_TIMEOUT_US(30ms)。
 *  调用方把节奏放慢（每 100ms 一颗）就不会拖垮主循环；
 *  正式版（P02）应改成非阻塞状态机（触发/等待分到不同 tick）。
 * ========================================================================== */
#include "drv_ultrasonic.h"
#include "bsp/bsp_board.h"

#include <stdio.h>

#define US_TIMEOUT_US   30000u      /* 30ms ≈ 5m；超过判定没收到回波 */
#define US_MIN_US       100u        /* <100µs 不合理（约 1.7cm） */
#define US_MIN_MM       20
#define US_MAX_MM       4000

static UsDev s_us[US_COUNT] = {
    { GPIOA, GPIO_PIN_4, GPIOA, GPIO_PIN_5, "L", -1, 0, 0, 0 },   /* 0 = 左 */
    { GPIOA, GPIO_PIN_1, GPIOB, GPIO_PIN_5, "R", -1, 0, 0, 0 },   /* 1 = 右 */
};

void Us_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Trig ×2：推挽输出，默认低 */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin   = GPIO_PIN_4;                 /* PA4 */
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin   = GPIO_PIN_1;                 /* PA1 */
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4 | GPIO_PIN_1, GPIO_PIN_RESET);

    /* Echo ×2：输入 + 下拉。
       下拉的意义：模块没接/线掉了时读到的是一条确定性的低电平（→ 报 -1），
       而不是悬空乱跳，省掉一轮“到底是线的问题还是探头的问题”的排查。 */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Pin  = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOA, &gpio);             /* PA5 = 左 Echo */
    HAL_GPIO_Init(GPIOB, &gpio);             /* PB5 = 右 Echo */
}

UsDev *Us_Get(UsId id)
{
    return &s_us[id];
}

int32_t Us_Read(UsId id)
{
    UsDev *d = &s_us[id];
    uint32_t t;

    /* -1) 节拍守卫：不管是主循环还是串口命令来调，都不允许 60ms 内触发两次。
            （两个源头各测各的时候会撞在一起，是“假 -2”的真正原因） */
    uint32_t gap = d->last_ok ? US_MIN_GAP_MS : US_RECOVER_MS;   /* 上次没测到 → 等久一点让它缓过来 */
    if ((HAL_GetTick() - d->stamp_ms) < gap) {
        d->mm = US_TOO_SOON;
        return d->mm;
    }

    d->stamp_ms = HAL_GetTick();
    d->width_us = 0;

    /* 0) 总线必须先空闲：上一次的 200ms 长脉冲可能还没走完。
          这时不阻塞等待（那会卡住主循环 200ms），直接跳过本轮，下一轮自愈。 */
    if (HAL_GPIO_ReadPin(d->echo_port, d->echo_pin) == GPIO_PIN_SET) {
        d->mm = US_BUSY;
            /* 不置 last_ok=0：这不是"测不到"，只是模块还在吐上次的长脉冲；
               置 0 会触发失败退避，把一次 -2 放大成"瞎一秒"（2026-10-18 实测）*/
        return d->mm;
    }

    /* 1) 10µs 触发脉冲 */
    HAL_GPIO_WritePin(d->trig_port, d->trig_pin, GPIO_PIN_SET);
    t = Bsp_Micros();
    while ((Bsp_Micros() - t) < 10u) { }
    HAL_GPIO_WritePin(d->trig_port, d->trig_pin, GPIO_PIN_RESET);

    /* 2) 等回波上升沿 */
    t = Bsp_Micros();
    while (HAL_GPIO_ReadPin(d->echo_port, d->echo_pin) == GPIO_PIN_RESET) {
        if ((Bsp_Micros() - t) > US_TIMEOUT_US) {
            d->mm = US_NO_RISE;
            d->last_ok = 0;
            return d->mm;
        }
    }
    uint32_t t_up = Bsp_Micros();

    /* 3) 量高电平宽度 */
    while (HAL_GPIO_ReadPin(d->echo_port, d->echo_pin) == GPIO_PIN_SET) {
        if ((Bsp_Micros() - t_up) > US_TIMEOUT_US) {
            d->width_us = Bsp_Micros() - t_up;
            d->mm = US_TOO_LONG;
            d->last_ok = 0;
            return d->mm;
        }
    }
    uint32_t width = Bsp_Micros() - t_up;
    d->width_us = width;

    if (width < US_MIN_US) {                 /* 窄脉冲：多办是干扰 */
        d->mm = US_TOO_SHORT;
        d->last_ok = 0;
        return d->mm;
    }

    /* 声速 340 m/s → 往返：mm = µs × 0.343 / 2 ≈ µs × 10 / 58 */
    int32_t mm = (int32_t)((width * 10u) / 58u);
    if (mm < US_MIN_MM || mm > US_MAX_MM) {
        d->mm = US_OUT_RANGE;
        d->last_ok = 0;
        return d->mm;
    }
    d->mm = mm;
    d->last_ok = 1;
    return d->mm;
}

void Us_PrintDebug(UsId id)
{
    UsDev *d = &s_us[id];
    const char *why = "";
    switch (d->mm) {
        case US_NO_RISE:   why = "没等到回波(查5V供电/Trig-Echo是否接反)"; break;
        case US_TOO_LONG:  why = "超量程或没回波(HC-SR04 此时会输约200ms高)"; break;
        case US_BUSY:      why = "上一发还没结束(本轮跳过)"; break;
        case US_TOO_SHORT: why = "窄脉冲(干扰)"; break;
        case US_OUT_RANGE: why = "换算后超 2~400cm"; break;
        case US_TOO_SOON:  why = "触发太快(刚测过)，驱动已拦下"; break;
        default:           why = "正常"; break;
    }
    printf("US[%s] t=%ums  %dmm  width=%uus  %s\r\n",
           d->name, (unsigned)d->stamp_ms, (int)d->mm,
           (unsigned)d->width_us, why);
}
