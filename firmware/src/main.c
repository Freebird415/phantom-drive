/* =============================================================================
 *  main.c —— 主程序：初始化 + 主循环
 *
 *  目录约定（详见 firmware/README.md）：
 *    bsp/  板级（时钟 / µs 计时 / 串口 / 指示灯）
 *    drv/  器件驱动（一个器件一个文件，各自带 bring-up 自检命令）
 *    app/  应用层（遥测帧、参数、命令解析）
 *
 *  $T 帧由 app/app_trace.c 生产（A01 循迹）；避障在 app/app_avoid.c（A02）。
 *  $T 里的 s0..s7 / err / pwm 现在是**真实数据**（灰度在线时）；灰度不在线时
 *  app_trace.c 会自动退化成模拟三角波，保证 P01 链路自检仍可用。
 * ========================================================================== */
#include "bsp/bsp_board.h"
#include "drv/drv_ultrasonic.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_motor.h"
#include "drv/drv_encoder.h"
#include "drv/drv_hmi.h"
#include "drv/drv_mpu6050.h"
#include "app/app_telemetry.h"
#include "app/app_trace.h"          /* A01 循迹（熊雅琦）*/
#include "app/app_avoid.h"          /* A02 避障（毕烨恺）*/
#include "app/app_log.h"            /* 板载 RAM 快照 */

#include <stdio.h>

int main(void)
{
    HAL_Init();
    Bsp_ClockInit();
    Bsp_TimeInit();          /* DWT：µs 计时，超声波要用 */
    Bsp_LedInit();
    Us_Init();
    Gr_Init();
    Mo_Init();
    En_Init();
    Hmi_Init();
    Mpu_Init();
    Bsp_UartInit();
    Bsp_Uart2Init();         /* ⚠️ BLE 串口（PA2/PA3）—— 漏了这句的话，BT/BTAT/BTSEND
                                会对着【未初始化】的串口发数据，HAL 直接返回错误（静默失败）*/
    Tlm_Init();

    /* 算法初始化放最后：它们要串口打印自检信息 */
    Trace_Init();            /* A01：探测灰度是否在线 */
    Avoid_Init();            /* A02 */
    Log_LoadFlash();         /* ⚠️ 先尝试从 Flash 恢复上次跑车的数据（掉电保留）。
                                 **不要**在这里调 Log_Reset() —— 那会把 Flash 快照抹掉。 */
    printf("# 跑车提示：**长按 PA12 按钮 1 秒** = 发车 + 开始记录；再长按 1 秒 = 停车 + 停止记录；"
           "跑完插回串口发 DUMP\r\n");

    Tlm_SendParams();                                    /* 上电先报一次参数快照 */
    Tlm_SendEvent(97, "BOOT a02-v1-20261010-20cm-trial");

    uint32_t next_t  = HAL_GetTick();   /* $T：100 Hz */
    uint32_t last_us = HAL_GetTick() - 100;  /* 超声波：上一发结束时刻（从完成时刻计时，避免追赶补发） */
    uint32_t next_led = HAL_GetTick();  /* 心跳灯：2 Hz */
    uint8_t  us_idx  = 0;               /* 0 = 下次测左，1 = 下次测右 */
    uint8_t  avoid_on = 0;              /* 上一拍 A02 是否在接管（用于检测“交还 A01”那一刻）*/
    uint32_t beep_off_at = 0u;          /* 非阻塞蜂鸣：到点关断（0 = 不响）*/

    while (1) {
        /* ---- 非阻塞蜂鸣：到点关断 ---- */
        if (beep_off_at != 0u && (int32_t)(HAL_GetTick() - beep_off_at) >= 0) {
            Hmi_BeepOff();
            beep_off_at = 0u;
        }

        /* ---- 电脑命令（在中断里只收行，到这里才执行） ---- */
        Tlm_Poll();

        /* ---- 心跳灯：证明程序在跑（串口不通时靠它判断） ---- */
        if ((int32_t)(HAL_GetTick() - next_led) >= 0) {
            next_led += 500;
            Bsp_LedToggle();
        }

        /* ---- PA12 按钮：长按 ≥1 秒才动作 ----
           ⚠️ 两道安全设计（2026-09-30 成车实测教训：曾出现“一上电车就跑”）：
             ① 上电后先等 2 秒（等电源/电平稳定）
             ② **必须先“看到按钮松开(高)”才武装** ——
                否则若上电瞬间 PA12 就是低的（按钮卡住/线缆干扰），
                1 秒后会自动发车 ✗（这很容易撞人撞东西）*/
        {
            static uint8_t  s_prev = 1u, s_fired = 0u, s_armed = 0u;
            static uint32_t s_t0 = 0u, s_boot = 0u;
            uint8_t key = Hmi_Key();
            if (s_boot == 0u) { s_boot = HAL_GetTick(); if (s_boot == 0u) { s_boot = 1u; } }
            if ((HAL_GetTick() - s_boot) >= 2000u) {          /* ① 上电 2s 后才启用 */
                if (key == 1u) { s_armed = 1u; }               /* ② 见过松开才武装 */
                if (key == 0u && s_prev == 1u) { s_t0 = HAL_GetTick(); s_fired = 0u; }
                if (key == 1u) { s_fired = 0u; }
                if (s_armed && key == 0u && !s_fired &&
                    (HAL_GetTick() - s_t0) >= 1000u) {
                    s_fired = 1u;
                    if (Trace_GetDrive()) {
                        Trace_SetDrive(0u); Avoid_SetDrive(0u); Log_Stop();
                        /* ⭐ 3 短声 = 手动停车（场地没屏幕，靠声音确认按键生效）
                           对照：1 短声=发车 ／ 2 长声=到终点自动停 */
                        Hmi_Beep(90); HAL_Delay(110);
                        Hmi_Beep(90); HAL_Delay(110);
                        Hmi_Beep(90);
                        printf("# BTN(长按1s): 停车 + 停止记录 n=%u\r\n", (unsigned)Log_Count());
                    } else {
                        /* ⚠️ 故意**不** Log_Reset()（同上：防误碰清掉上一趟数据）*/
                        Trace_StartRun();          /* ⭐ 复位终点精停 + 打开起步找线（起始线 T 头在前面 ✓）*/
                        Log_SetBle(1u);            /* ⭐ 顺便开无线回传：跑车时插不了串口，
                                                      长按按钮就把数据推到电脑 ✓ */
                        Log_Start(); Avoid_SetDrive(1u); Trace_SetDrive(1u);
                        Hmi_BeepOn();              /* 1 短声 = 已发车（非阻塞，主循环关断）*/
                        beep_off_at = HAL_GetTick() + 150u;
                        printf("# BTN(长按1s): 开始记录 + 发车 + BLE回传（从 n=%u 接着记）\r\n",
                               (unsigned)Log_Count());
                    }
                }
            }
            s_prev = key;
        }

        /* ---- 超声波：每颗间隔 ≥100ms，左右轮流（两颗不能同时触发）
                注意：要从“上一发结束”计时，不能用累加器 —— 否则一次 30ms 超时
                之后会补发一串，反而把节拍打乱（驱动层的 60ms 守卫会拦下）。
                AUTO 0 可临时关掉，让 US 命令独占节拍 ---- */
        /* 节拍 100ms（2026-10-01 改回）：左右各 5Hz
             ⚠️ 2026-09-23 曾记“100ms ⇒ 左侧掉到 42%”并把节拍放慢到 250ms ✗
             但现在看，那个 42% 很可能是**把失败后 600ms 恢复期的 `-6` 当失败**统计出来的✗
             （`-6` = 驱动自己拦下重复触发，不算故障；而 10/01 台面实测 50 秒零 `-2`/`-1`）
              ⇒ 改回 100ms，**本次实测会重新统计成功率**（看 `-2`/`-1` 而不是看 `-6`）
             若真出现大量 `-2` ⇒ 再改回 200ms 即可（就在这里）*/
/* 节拍 = 每次只测【一颗】，左右交替 ⇒ 单颗频率 = 1/(2×本值)
 *   100ms → 单颗 5.0Hz ｜ **62ms → 8.06Hz（当前）** ｜ 60ms → 8.3Hz ｜ 55ms → 9.1Hz
 * 约束：同一颗两次之间 ≥ US_MIN_GAP_MS(60ms) —— 这里每颗间隔 124ms，满足。
 * ⚠️ 单次读取最坏阻塞 30ms（没回波等超时）⇒ 62ms 节拍下最坏占用 ~48% CPU，
 *    100Hz 控制环可能掉拍；实测看 $T 的 ms 间隔是否仍是 10ms。*/
#define US_PERIOD_MS  62u
        if (Tlm_UsAuto() && (int32_t)(HAL_GetTick() - last_us) >= (int32_t)US_PERIOD_MS) {
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
            Log_BleUsFrame(Us_Get(US_L)->mm, Us_Get(US_R)->mm);  /* 超声波也走蓝牙（4Hz）*/
            last_us = HAL_GetTick();
        }

        /* ---- 100Hz 控制与 $T 遥测 ----
               避障接管时由 A02 独立读灰度、控电机、发 state=2/3；
               循迹暂停，避免其终点/丢线状态机覆盖绕障动作。 */
        if ((int32_t)(HAL_GetTick() - next_t) >= 0) {
            next_t += 10;
            if (Avoid_Mode()) {
                avoid_on = 1u;
                Avoid_Step();
            } else {
                /* ⭐ A02 刚把控制权交回（回线了）：先让 A01 用慢速跑 TR_SLOW_MS，
                   等姿态稳下来再自动放回满速 —— 否则交还第一拍就按满速抢方向 ✗ */
                if (avoid_on) { avoid_on = 0u; Trace_BeginSlow(0u); }
                Trace_Step(1u);
                Avoid_Step();  /* 用新鲜 $U 测量触发接管；下一拍起 A02 负责 $T */
            }
            Log_BleTick();                              /* BLE 实时日志回传（BTL 1 才发）*/
        }
    }
}
