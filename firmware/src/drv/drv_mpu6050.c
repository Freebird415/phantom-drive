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

/* ⭐ 自愈计数（2026-10-01 实踩：断电/上电时序会让芯片回到【睡眠态】）
   现象：`WHO_AM_I=0x68` ✓、`temp` 还在更新 ✓，但 **acc(三轴) 与 gyro 全 0** ✗
        （因为初始化只在开机调一次，芯片被复位回睡眠后就再没人叫醒它；
         `Mpu_Read` 也不报错，只是扔回一排 0 ⇒ 陀螺仪静默失效）
   对策：**连续多次读到“六轴全 0”就重新初始化一次**。
   判据可靠：活着的加速度计静止时至少有一轴 ≈ ±1g，**六轴全 0 在物理上不可能**。*/
static uint16_t s_zero_run;        /* 连续多少次六轴全 0 */
static uint16_t s_reinit_cnt;      /* 自动重新初始化了几次 */
#define MPU_ZERO_RUN_TRIG  20u     /* 陀螺仪按 ~25Hz 轮询 ⇒ 约 0.8 秒触发一次重试 */

uint16_t Mpu_ReinitCount(void) { return s_reinit_cnt; }

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
    /* ⭐ 回读确认：以前不核对，写失败（上电时序/总线忙）也当成功 ⇒ 症状隐蔽 */
    {
        uint8_t pwr = 0xFF;
        if (mpu_rd(REG_PWR, &pwr, 1) && (pwr & 0x40u)) {
            printf("# MPU6050 **唤醒失败**（PWR_MGMT_1=0x%02X，睡眠位仍=1）——可能上电太早，稍后可重试\r\n",
                   (unsigned)pwr);
            return;
        }
    }
    s_zero_run = 0;
    printf("# MPU6050 init OK (WHO_AM_I=0x%02X)\r\n", (unsigned)Mpu_WhoAmI());
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
    s_zero_run = 0;
    (void)mpu_rd(REG_PWR, &back, 1);  /* ⭐ 回读确认（写失败/总线忙时以前看不出来）*/
    printf("# MPU6050 已复位回上电默认（回读 PWR_MGMT_1=0x%02X）——数据寄存器应归 0，等约 1 秒看自愈\r\n",
           (unsigned)back);
}

uint8_t Mpu_Read(int16_t raw[7])
{
    uint8_t b[14];
    if (!mpu_rd(REG_DATA, b, 14)) { return 0; }
    for (int i = 0; i < 7; i++) {
        raw[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);
    }
    /* ⭐ 自愈：芯片应答但六轴全 0 ⇒ 八成是睡回去了（见文件开头的说明）*/
    if (raw[0] == 0 && raw[1] == 0 && raw[2] == 0 &&
        raw[4] == 0 && raw[5] == 0 && raw[6] == 0) {
        if (++s_zero_run >= MPU_ZERO_RUN_TRIG) {
            s_zero_run = 0;
            s_reinit_cnt++;
            printf("# MPU6050 六轴全 0 OK 自动重新初始化（第 %u 次）\r\n", (unsigned)s_reinit_cnt);
            Mpu_Init();               /* 重新叫醒（Mpu_Init 不再递归调 Mpu_Read）*/
        }
    } else {
        s_zero_run = 0;
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
        printf("MPU> 健康度：自动重新初始化 %u 次%s\r\n",
               (unsigned)Mpu_ReinitCount(),
               (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[4] == 0 && r[5] == 0 && r[6] == 0)
                   ? "  ← **六轴仍全 0：没醒过来**（多等几秒会自动重试；也可发 MPUINIT）" : "");
    }
}
