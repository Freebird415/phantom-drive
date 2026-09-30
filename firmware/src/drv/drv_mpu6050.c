/* =============================================================================
 *  drv_mpu6050.c —— MPU6050 六轴（I2C2，与 OLED 共用总线）
 *  说明与注意见 drv_mpu6050.h
 * ========================================================================== */
#include "drv_mpu6050.h"
#include "drv/drv_oled.h"       /* 借用它的 I2C2 句柄（同一条总线只能有一个句柄） */
#include "bsp/bsp_board.h"

#include <stdio.h>

#define MPU_ADDR7   0x68
#define MPU_ADDR8   (MPU_ADDR7 << 1)
#define MPU_TO      50u

#define REG_WHO     0x75
#define REG_PWR     0x6B
#define REG_CONFIG  0x1A
#define REG_GYRO    0x1B
#define REG_ACCEL   0x1C
#define REG_DATA    0x3B        /* 之后 14 字节：ax ay az temp gx gy gz */

#define ACC_LSB_PER_G     16384.0f     /* ±2g */
#define GYRO_LSB_PER_DPS  131.0f       /* ±250°/s */

static uint8_t mpu_rd(uint8_t reg, uint8_t *buf, uint16_t n)
{
    return (HAL_I2C_Mem_Read(Ol_Bus(), MPU_ADDR8, reg, I2C_MEMADD_SIZE_8BIT,
                             buf, n, MPU_TO) == HAL_OK) ? 1u : 0u;
}

static uint8_t mpu_wr(uint8_t reg, uint8_t val)
{
    return (HAL_I2C_Mem_Write(Ol_Bus(), MPU_ADDR8, reg, I2C_MEMADD_SIZE_8BIT,
                              &val, 1, MPU_TO) == HAL_OK) ? 1u : 0u;
}

uint8_t Mpu_WhoAmI(void)
{
    uint8_t v = 0;
    if (!mpu_rd(REG_WHO, &v, 1)) { return 0; }
    return v;
}

void Mpu_Init(void)
{
    if (Mpu_WhoAmI() == 0) {
        printf("# MPU6050 未应答（查 VCC=3V3 / SCL->PB10 / SDA->PB11 / AD0->GND / 共地）\r\n");
        return;
    }
    (void)mpu_wr(REG_PWR,    0x00);     /* 解除睡眠 */
    (void)mpu_wr(REG_CONFIG, 0x03);     /* DLPF ≈44Hz */
    (void)mpu_wr(REG_GYRO,   0x00);     /* ±250°/s */
    (void)mpu_wr(REG_ACCEL,  0x00);     /* ±2g */
    printf("# MPU6050 init OK (WHO_AM_I=0x%02X)\r\n", (unsigned)Mpu_WhoAmI());
}

uint8_t Mpu_Read(int16_t raw[7])
{
    uint8_t b[14];
    if (!mpu_rd(REG_DATA, b, 14)) { return 0; }
    for (int i = 0; i < 7; i++) {
        raw[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);
    }
    return 1;
}

void Mpu_PrintDebug(void)
{
    int16_t r[7];
    printf("MPU> WHO_AM_I=0x%02X\r\n", (unsigned)Mpu_WhoAmI());
    if (!Mpu_Read(r)) {
        printf("MPU> **读不到数据**（设备没应答；查接线与 AD0 接地）\r\n");
        return;
    }
    {
        char ax[16], ay[16], az[16], gx[16], gy[16], gz[16], tp[16];
        Bsp_FmtFix(ax, sizeof ax, r[0] / ACC_LSB_PER_G, 2, 1);
        Bsp_FmtFix(ay, sizeof ay, r[1] / ACC_LSB_PER_G, 2, 1);
        Bsp_FmtFix(az, sizeof az, r[2] / ACC_LSB_PER_G, 2, 1);
        Bsp_FmtFix(gx, sizeof gx, r[4] / GYRO_LSB_PER_DPS, 1, 1);
        Bsp_FmtFix(gy, sizeof gy, r[5] / GYRO_LSB_PER_DPS, 1, 1);
        Bsp_FmtFix(gz, sizeof gz, r[6] / GYRO_LSB_PER_DPS, 1, 1);
        Bsp_FmtFix(tp, sizeof tp, r[3] / 340.0f + 36.53f, 1, 0);
        printf("MPU> acc(g) x=%s y=%s z=%s | gyro(dps) x=%s y=%s z=%s | temp=%sC\r\n",
               ax, ay, az, gx, gy, gz, tp);
    }
}
