/* =============================================================================
 *  drv_mpu6050.c —— MPU6050 六轴（I2C2 · **本总线现在只有 MPU6050**）
 *  说明与注意见 drv_mpu6050.h
 * ========================================================================== */
#include "drv_mpu6050.h"
#include "bsp/bsp_board.h"

#include <stdio.h>

#define MPU_ADDR7   0x68
#define MPU_ADDR8   (MPU_ADDR7 << 1)
#define MPU_TO       5u /* HAL transaction timeout, not a whole-loop guarantee */

#define REG_WHO     0x75
#define REG_PWR     0x6B
#define REG_CONFIG  0x1A
#define REG_GYRO    0x1B
#define REG_ACCEL   0x1C
#define REG_DATA    0x3B        /* 之后 14 字节：ax ay az temp gx gy gz */

#define ACC_LSB_PER_G     16384.0f     /* ±2g */
#define GYRO_LSB_PER_DPS  131.0f       /* ±250°/s */

/* I2C2 句柄由本驱动独占（2026-10-10 起 OLED 已放弃，总线上只剩 MPU6050）。
   GPIO/时钟的板级初始化仍在 bsp_board.c 的 HAL_I2C_MspInit 里。*/
static I2C_HandleTypeDef s_i2c2;
static uint8_t s_ready, s_status = MPU_NOT_READY;
static uint32_t s_retry_ms;

static uint8_t mpu_bus_init(void)
{
    s_i2c2.Instance             = I2C2;
    s_i2c2.Init.ClockSpeed      = 100000;   /* 100k 够用（14B 读 ≈1.5ms）；MPU 支持 400k，需要时再提 */
    s_i2c2.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    s_i2c2.Init.OwnAddress1     = 0;
    s_i2c2.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    s_i2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_i2c2.Init.OwnAddress2     = 0;
    s_i2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_i2c2.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    return HAL_I2C_Init(&s_i2c2) == HAL_OK;
}

static uint8_t mpu_rd(uint8_t reg, uint8_t *buf, uint16_t n)
{
    if (HAL_I2C_Mem_Read(&s_i2c2, MPU_ADDR8, reg, I2C_MEMADD_SIZE_8BIT,
                         buf, n, MPU_TO) != HAL_OK) {
        s_status = MPU_IO_ERROR; return 0u;
    }
    return 1u;
}

static uint8_t mpu_wr(uint8_t reg, uint8_t val)
{
    if (HAL_I2C_Mem_Write(&s_i2c2, MPU_ADDR8, reg, I2C_MEMADD_SIZE_8BIT,
                          &val, 1, MPU_TO) != HAL_OK) {
        s_status = MPU_IO_ERROR; return 0u;
    }
    return 1u;
}

uint8_t Mpu_WhoAmI(void)
{
    uint8_t v = 0;
    if (!mpu_rd(REG_WHO, &v, 1)) { return 0; }
    return v;
}

/* Six zero axes are rejected data, not proof of hardware damage. Reinitializing
 * a failed sensor must not block the loop while motors are enabled. */
static uint16_t s_reinit_cnt;      /* 自动重新初始化了几次 */

uint16_t Mpu_ReinitCount(void) { return s_reinit_cnt; }
uint8_t Mpu_Status(void) { return s_status; }

static uint8_t mpu_config_ok(void)
{
    uint8_t pwr, gyro;
    if (!mpu_rd(REG_PWR, &pwr, 1) || !mpu_rd(REG_GYRO, &gyro, 1)) {
        s_status = MPU_IO_ERROR;
        return 0u;
    }
    /* Reject reset/sleep/cycle and disabled temperature/alternate clock setup;
     * reject self-test or non-250dps range. Matches our verified init values. */
    if (pwr != 0u || gyro != 0u) {
        s_status = (pwr & 0x40u) ? MPU_SLEEP : MPU_CONFIG_ERROR;
        return 0u;
    }
    return 1u;
}

void Mpu_Init(void)
{
    s_ready = 0u; s_status = MPU_NOT_READY;
    s_retry_ms = HAL_GetTick();
    uint8_t who = 0, config = 0, accel = 0;
    if (!mpu_bus_init()) { s_status = MPU_IO_ERROR; return; }
    if (!mpu_rd(REG_WHO, &who, 1) || who != MPU_ADDR7 ||
        !mpu_wr(REG_PWR, 0) || !mpu_wr(REG_CONFIG, 3) ||
        !mpu_wr(REG_GYRO, 0) || !mpu_wr(REG_ACCEL, 0) || !mpu_config_ok() ||
        !mpu_rd(REG_CONFIG, &config, 1) || !mpu_rd(REG_ACCEL, &accel, 1) ||
        config != 3u || accel != 0u) {
        if (s_status == MPU_NOT_READY) { s_status = MPU_CONFIG_ERROR; }
        printf("# MPU6050 init FAILED status=%u\r\n", (unsigned)s_status);
        return;
    }
    s_ready = 1u; s_status = MPU_OK;
    printf("# MPU6050 init OK (WHO_AM_I=0x%02X)\r\n", (unsigned)who);
}

void Mpu_Service(uint8_t parked)
{
    /* Never reinitialize/print while driving. IO failure, sleep or zero axes
     * all get bounded-frequency recovery while parked, not just zero axes. */
    if (parked && s_status != MPU_OK && (HAL_GetTick() - s_retry_ms) >= 1000u) {
        s_reinit_cnt++;
        Mpu_Init();
    }
}

/* ⭐ 调试用：故意让 MPU 回到睡眠态（模拟“断电/上电时序”那个故障），
   用来验证 Mpu_Read 的自愈真的会叫 —— 没有这个入口就只能“相信它应该能自愈”。*/
/* ⭐ 调试用：让 MPU **复位回上电默认状态**（DEVICE_RESET 0x80 + SLEEP 0x40），
   这才是“断电/上电”后芯片的真实状态：**数据寄存器归 0**（而单纯置睡眠会保留旧值 ✗）。
   用它验证 Mpu_Read 的自愈真的会叫 —— 没这个入口就只能“相信它应该能自愈”。*/
void Mpu_DebugReset(void)
{
    uint8_t back = 0xFF;
    (void)mpu_wr(REG_PWR, 0xC0);      /* DEVICE_RESET(0x80) + SLEEP(0x40) */
    s_ready = 0u; s_status = MPU_NOT_READY;
    (void)mpu_rd(REG_PWR, &back, 1);  /* ⭐ 回读确认（写失败/总线忙时以前看不出来）*/
    printf("# MPU6050 已复位回上电默认（回读 PWR_MGMT_1=0x%02X）——数据寄存器应归 0，等约 1 秒看自愈\r\n",
           (unsigned)back);
}

uint8_t Mpu_Read(int16_t raw[7])
{
    uint8_t b[14];
    if (!s_ready) { return 0; }
    if (!mpu_config_ok()) { return 0; }
    if (!mpu_rd(REG_DATA, b, 14)) { s_status = MPU_IO_ERROR; return 0; }
    for (int i = 0; i < 7; i++) {
        raw[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);
    }
    /* Zero axes are invalid evidence, not an assertion about hardware damage. */
    if (raw[0] == 0 && raw[1] == 0 && raw[2] == 0 &&
        raw[4] == 0 && raw[5] == 0 && raw[6] == 0) {
        s_status = MPU_ZERO_AXES;
        return 0;
    }
    s_status = MPU_OK;
    return 1;
}

void Mpu_PrintDebug(void)
{
    int16_t r[7];
    printf("MPU> WHO_AM_I=0x%02X\r\n", (unsigned)Mpu_WhoAmI());
    if (!Mpu_Read(r)) {
        printf("MPU> invalid status=%u parked-retries=%u\r\n",
               (unsigned)Mpu_Status(), (unsigned)Mpu_ReinitCount());
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
        printf("MPU> status=%u parked-retries=%u\r\n",
               (unsigned)Mpu_Status(), (unsigned)Mpu_ReinitCount());
    }
}
