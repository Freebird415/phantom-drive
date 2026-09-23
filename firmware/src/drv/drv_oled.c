/* =============================================================================
 *  drv_oled.c —— 0.96" SSD1306 OLED（硬件 I2C2：PB10=SCL / PB11=SDA）
 *  说明见 drv_oled.h
 * ========================================================================== */
#include "drv_oled.h"
#include "bsp/bsp_board.h"
#include <stdio.h>
#include <string.h>

#define OL_TIMEOUT  50u

static I2C_HandleTypeDef s_i2c2;
static uint8_t s_addr = 0;              /* 8bit 地址；0 = 未找到 */

/* SSD1306 写：控制字节 0x00=命令 / 0x40=数据 */
static uint8_t ol_write(uint8_t ctrl, const uint8_t *data, uint16_t n)
{
    static uint8_t buf[OLED_W + 1];     /* 1 控制字节 + 最多 128 数据字节 */
    if (n > OLED_W) { n = OLED_W; }
    buf[0] = ctrl;
    memcpy(&buf[1], data, n);
    return (HAL_I2C_Master_Transmit(&s_i2c2, s_addr, buf, (uint16_t)(n + 1), OL_TIMEOUT) == HAL_OK) ? 1u : 0u;
}

static void ol_cmd1(uint8_t c) { ol_write(0x00, &c, 1); }

static void ol_cmd2(uint8_t c, uint8_t v) { uint8_t b[2] = { c, v }; ol_write(0x00, b, 2); }

static uint8_t ol_probe(void)
{
    static const uint8_t cand[2] = { 0x3C, 0x3D };      /* 7bit */
    for (int i = 0; i < 2; i++) {
        uint16_t a8 = (uint16_t)(cand[i] << 1);
        if (HAL_I2C_IsDeviceReady(&s_i2c2, a8, 2, 10) == HAL_OK) { s_addr = a8; return cand[i]; }
    }
    s_addr = 0;
    return 0;
}

void Ol_Init(void)
{
    s_i2c2.Instance             = I2C2;
    s_i2c2.Init.ClockSpeed      = 100000;    /* 面包板 + 杜邦线，先稳（100kHz 够点屏） */
    s_i2c2.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    s_i2c2.Init.OwnAddress1     = 0;
    s_i2c2.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    s_i2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_i2c2.Init.OwnAddress2     = 0;
    s_i2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_i2c2.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    (void)HAL_I2C_Init(&s_i2c2);
}

uint8_t Ol_Present(void) { (void)ol_probe(); return s_addr ? (uint8_t)(s_addr >> 1) : 0u; }  /* 每次真探一次 */

static void ol_init_seq(void)
{
    static const uint8_t seq[] = {
        0xAE,               /* display off */
        0xD5, 0x80,         /* clock div */
        0xA8, 0x3F,         /* multiplex = 64 */
        0xD3, 0x00,         /* display offset */
        0x40,               /* start line 0 */
        0x8D, 0x14,         /* charge pump ON（模块自带升压，必须开）*/
        0x20, 0x00,         /* memory mode = horizontal */
        0xA1,               /* segment remap（左右方向）*/
        0xC8,               /* COM scan dec（上下方向）*/
        0xDA, 0x12,         /* COM pins */
        0x81, 0xCF,         /* contrast */
        0xD9, 0xF1,         /* pre-charge */
        0xDB, 0x40,         /* VCOMH */
        0xA4,               /* resume（显示 RAM 内容）*/
        0xA6,               /* normal（非反色）*/
        0xAF,               /* display on */
    };
    ol_write(0x00, seq, (uint16_t)sizeof(seq));
}

void Ol_Clear(void)
{
    static uint8_t zero[OLED_W];
    ol_cmd1(0x21); ol_cmd1(0x00); ol_cmd1(0x7F);    /* 列 0~127 */
    ol_cmd1(0x22); ol_cmd1(0x00); ol_cmd1(0x07);    /* 页 0~7 */
    for (int page = 0; page < 8; page++) { ol_write(0x40, zero, OLED_W); }
}

void Ol_TestPattern(void)
{
    static uint8_t row[OLED_W];
    ol_cmd1(0x21); ol_cmd1(0x00); ol_cmd1(0x7F);
    ol_cmd1(0x22); ol_cmd1(0x00); ol_cmd1(0x07);
    for (int page = 0; page < 8; page++) {
        for (int x = 0; x < OLED_W; x++) {
            uint8_t v = 0x00;
            if (page == 0 || page == 7) { v = 0xFF; }                 /* 上下边框 */
            else if (x == 0 || x == OLED_W - 1) { v = 0xFF; }         /* 左右边框 */
            else if ((page & 1) == 1) { v = (uint8_t)((x & 0x08) ? 0xF0 : 0x0F); }  /* 棋盘 */
            row[x] = v;
        }
        ol_write(0x40, row, OLED_W);
    }
}

void Ol_PrintDebug(void)
{
    uint8_t a = ol_probe();
    if (!a) {
        printf("OLED> **没找到**（试了 0x3C/0x3D）—— 查：① VCC 必须 3.3V ② SCL->PB10 / SDA->PB11 "
               "③ 共地 ④ 有没有插到灰度那条 5V 总线上\r\n");
        return;
    }
    printf("OLED> 找到 7bit=0x%02X，初始化 + 画测试图案…\r\n", (unsigned)a);
    ol_init_seq();
    Ol_Clear();
    Ol_TestPattern();
    printf("OLED> 完成：屏幕应出现【棋盘格 + 四周黑边框】。"
           "若全黑/全亮/花屏，回一句我改参数\r\n");
}
