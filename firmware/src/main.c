/* =============================================================================
 *  main.c —— 主程序：初始化 + 主循环
 *
 *  目录约定（详见 firmware/README.md）：
 *    bsp/  板级（时钟 / µs 计时 / 串口 / 指示灯）
 *    drv/  器件驱动（一个器件一个文件，各自带 bring-up 自检命令）
 *    app/  应用层（遥测帧、参数、命令解析）
 *
 *  当前 bring-up 进度：超声波 ×2 已接真机；灰度 / 电机 / 编码器待接。
 *  $T 里的 s0..s7 / err / pwm 仍是模拟数据，接上灰度后替换标注那一段。
 * ========================================================================== */
#include "bsp/bsp_board.h"
#include "drv/drv_ultrasonic.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_oled.h"
#include "drv/drv_motor.h"
#include "drv/drv_encoder.h"
#include "app/app_telemetry.h"

#include <stdio.h>

int main(void)
{
    HAL_Init();
    Bsp_ClockInit();
    Bsp_TimeInit();          /* DWT：µs 计时，超声波要用 */
    Bsp_LedInit();
    Us_Init();
    Gr_Init();
    Ol_Init();
    Mo_Init();
    En_Init();
    Bsp_UartInit();
    Tlm_Init();

    Tlm_SendParams();                                    /* 上电先报一次参数快照 */
    Tlm_SendEvent(97, "BOOT");

    uint32_t next_t  = HAL_GetTick();   /* $T：100 Hz */
    uint32_t last_us = HAL_GetTick() - 100;  /* 超声波：上一发结束时刻（从完成时刻计时，避免追赶补发） */
    uint32_t next_led = HAL_GetTick();  /* 心跳灯：2 Hz */
    uint32_t frames  = 0;
    uint8_t  us_idx  = 0;               /* 0 = 下次测左，1 = 下次测右 */

    while (1) {
        /* ---- 电脑命令（在中断里只收行，到这里才执行） ---- */
        Tlm_Poll();

        /* ---- 心跳灯：证明程序在跑（串口不通时靠它判断） ---- */
        if ((int32_t)(HAL_GetTick() - next_led) >= 0) {
            next_led += 500;
            Bsp_LedToggle();
        }

        /* ---- 超声波：每颗间隔 ≥100ms，左右轮流（两颗不能同时触发）
                注意：要从“上一发结束”计时，不能用累加器 —— 否则一次 30ms 超时
                之后会补发一串，反而把节拍打乱（驱动层的 60ms 守卫会拦下）。
                AUTO 0 可临时关掉，让 US 命令独占节拍 ---- */
        /* 节拍 250ms：实测（2026-09-23，探头朝天花板/前面有线）
             250ms → 左右各 2Hz，成功率 100%
             100ms → 左右各 5Hz，左侧掉到 42%（失败后的 600ms 恢复期把节拍拦下）
           探头正式装好、对着赛道以后可以把这里调回 100ms 再实测。 */
        if (Tlm_UsAuto() && (int32_t)(HAL_GetTick() - last_us) >= 250) {
            uint8_t mode = Tlm_UsAuto();
            if (mode == 2u) {            /* 只测左 */
                Us_Read(US_L);
            } else if (mode == 3u) {     /* 只测右（隔离实验用） */
                Us_Read(US_R);
            } else {                     /* 左右轮流 */
                Us_Read((UsId)us_idx);
                us_idx ^= 1u;
            }
            Tlm_SendU(HAL_GetTick(), Us_Get(US_L)->mm, Us_Get(US_R)->mm);
            last_us = HAL_GetTick();
        }

        /* ---- $T 遥测：100 Hz ---- */
        if ((int32_t)(HAL_GetTick() - next_t) >= 0) {
            next_t += 10;
            uint32_t ms = HAL_GetTick();

            /* ======== 模拟数据：接上灰度后替换这一段 ======== */
            int32_t k       = (int32_t)((ms / 10u) % 200u);
            int32_t err_x10 = (k < 100) ? (k - 50) : (150 - k);

            int s[8];
            int center = 3 + (int)(err_x10 / 25);
            if (center < 0) { center = 0; }
            if (center > 7) { center = 7; }
            for (int i = 0; i < 8; i++) {
                s[i] = (i == center || i == center + 1) ? 1 : 0;
            }

            float   err   = (float)err_x10 / 10.0f;
            int32_t err_i = err_x10 / 5;
            int     pwm_l = Tlm_Params()->base_speed + (int)err_i;
            int     pwm_r = Tlm_Params()->base_speed - (int)err_i;
            /* ================================================== */

            Tlm_SendT(ms, s, err, pwm_l, pwm_r, 1);

            frames++;
            if (frames % 500u == 0u) {                    /* 每 5 秒一个演示事件 */
                Tlm_SendEvent(98, "DEMO_EVENT");
            }
        }
    }
}
