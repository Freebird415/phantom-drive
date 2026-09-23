/* =============================================================================
 *  drv_grayscale.c —— 感为八路灰度（I2C 模式，硬件 I2C1 重映射到 PB8/PB9）
 *
 *  为什么用 100kHz 而不是 400kHz：面包板 + 杜邦线的总线电容大，
 *  100kHz 更稳（调通后可以再往上试）。
 * ========================================================================== */
#include "drv_grayscale.h"
#include "bsp/bsp_board.h"

#include <stdio.h>

#define GR_ADDR8    (GR_ADDR7 << 1)     /* HAL 要 8bit 地址 */
#define GR_TIMEOUT  20u                 /* ms */

static I2C_HandleTypeDef s_i2c1;

void Gr_Init(void)
{
    /* I2C1 重映射到 PB8/PB9 —— 把默认的 PB6/PB7 让给 TIM4（将来接左编码器） */
    __HAL_AFIO_REMAP_I2C1_ENABLE();

    s_i2c1.Instance             = I2C1;
    s_i2c1.Init.ClockSpeed      = 100000;
    s_i2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    s_i2c1.Init.OwnAddress1     = 0;
    s_i2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    s_i2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_i2c1.Init.OwnAddress2     = 0;
    s_i2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_i2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    (void)HAL_I2C_Init(&s_i2c1);
}

/* 先写 1 字节命令，再读 n 字节（手册 §7.4.2 方法 1 的结构） */
static uint8_t gr_cmd_read(uint8_t cmd, uint8_t *buf, uint8_t n)
{
    if (HAL_I2C_Master_Transmit(&s_i2c1, GR_ADDR8, &cmd, 1, GR_TIMEOUT) != HAL_OK) {
        return 0;
    }
    if (HAL_I2C_Master_Receive(&s_i2c1, GR_ADDR8 | 0x01u, buf, n, GR_TIMEOUT) != HAL_OK) {
        return 0;
    }
    return 1;
}

uint8_t Gr_Ping(void)
{
    uint8_t cmd = 0xAA, r = 0;
    if (!gr_cmd_read(cmd, &r, 1)) { return 0; }
    return (r == 0x66) ? 1u : 0u;
}

uint8_t Gr_ReadDigital(void)
{
    uint8_t cmd = 0xDD, d = 0;
    if (!gr_cmd_read(cmd, &d, 1)) { return 0xFF; }   /* 0xFF = 读失败（8 路全黑也是 0xFF，用 ping 区分） */
    return d;
}

uint8_t Gr_ReadAnalog(uint8_t out[8])
{
    uint8_t cmd = 0xB0;
    return gr_cmd_read(cmd, out, 8);
}

void Gr_PrintDebug(void)
{
    static const char *nm[8] = { "1","2","3","4","5","6","7","8" };
    uint8_t ping = Gr_Ping();
    printf("GR> ping=%s ", ping ? "OK(0x66)" : "FAIL");

    if (!ping) {
        printf("| I2C 没通 —— 查：① 上拉（须 5V，最省事=插感为板载跳帽）"
               " ② SCL/SDA 是否接反（15→PB8, 16→PB9）③ 共地 ④ 模块是否 5V 供电\r\n");
        return;
    }

    uint8_t d = Gr_ReadDigital();
    printf("| dir=0b");
    for (int i = 7; i >= 0; i--) { printf("%d", (d >> i) & 1); }
    printf(" (bit0=第1路, 1=黑)");

    uint8_t a[8] = {0};
    if (Gr_ReadAnalog(a)) {
        printf(" | ana=[");
        for (int i = 0; i < 8; i++) {
            printf("%s%s=%u", (i ? " " : ""), nm[i], (unsigned)a[i]);
        }
        printf("]\r\n");
    } else {
        printf(" | ana=读失败\r\n");
    }
}

void Gr_Scan(void)
{
    printf("GRSCAN> 扫描 I2C1(100kHz) 地址 0x08~0x77 …\r\n");
    uint8_t found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (HAL_I2C_IsDeviceReady(&s_i2c1, (uint16_t)(a << 1), 2, 5) == HAL_OK) {
            printf("GRSCAN>   应答 7bit=0x%02X (8bit 写=0x%02X)\r\n", (unsigned)a, (unsigned)(a << 1));
            found++;
        }
    }
    if (!found) {
        printf("GRSCAN> **没有任何设备应答** → 总线层没通，按顺序查：\r\n");
        printf("GRSCAN>   1) 上拉必须是 5V：跳帽要插在 SCL/SDA 两列（不是 AD0/AD1）\r\n");
        printf("GRSCAN>   2) SCL->PB8 / SDA->PB9 是否接反或没接\r\n");
        printf("GRSCAN>   3) 共地：模块 GND 与蓝板 GND 必须连在一起\r\n");
        printf("GRSCAN>   4) 感为针脚 1=GND、2=+5V（别和亚博的 1/2 顺序搞混）\r\n");
    } else {
        printf("GRSCAN> 共 %u 个应答（感为出厂地址应为 0x4C）\r\n", (unsigned)found);
    }
}
