/* =============================================================================
 *  drv_oled.c —— 0.96" SSD1306 OLED（硬件 I2C2：PB10=SCL / PB11=SDA）
 *  说明与约束见 drv_oled.h
 * ========================================================================== */
#include "drv_oled.h"
#include "drv/font_subset.h"
#include "bsp/bsp_board.h"

#include <stdio.h>
#include <string.h>

#define OL_TIMEOUT  50u

static I2C_HandleTypeDef s_i2c2;
static uint8_t s_addr = 0;                  /* 8bit 地址；0 = 未找到 */

/* 帧缓冲：8 页 × 128 列，和 SSD1306 的页寻址一一对应 */
static uint8_t s_fb[8][OLED_W];

/* ---------------- 底层 I2C ---------------- */
static uint8_t ol_wr(uint8_t ctrl, const uint8_t *data, uint16_t n)
{
    static uint8_t buf[OLED_W + 1];
    if (n > OLED_W) { n = OLED_W; }
    buf[0] = ctrl;
    memcpy(&buf[1], data, n);
    return (HAL_I2C_Master_Transmit(&s_i2c2, s_addr, buf, (uint16_t)(n + 1), OL_TIMEOUT) == HAL_OK) ? 1u : 0u;
}
static void ol_c1(uint8_t c) { ol_wr(0x00, &c, 1); }

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
    s_i2c2.Init.ClockSpeed      = 100000;  /* ⚠️ 2026-10-01 实测：改 400k 后 OLED 写入超时，
                                             每次要等 50ms 超时 ⇒ 主循环节奏被打乱 ⇒ 
                                             D 项爆表 ⇒ 车左右猛打、OLED 也不亮 ✗
                                             ⇒ 退回 100k。分页刷新足够用（1 页 ~12ms）。
                                             I2C2 只挂 OLED + MPU6050（都支持 400k），
                                             PCB 走线短 + OLED 板载 4.7k 上拉 ⇒ 稳定 ✓
                                             为什么要提：OLED 刷满屏要传 1024B，
                                             100k 下要 ~100ms，会把控制循环卡死 ✗ */
    s_i2c2.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    s_i2c2.Init.OwnAddress1     = 0;
    s_i2c2.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    s_i2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_i2c2.Init.OwnAddress2     = 0;
    s_i2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_i2c2.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    (void)HAL_I2C_Init(&s_i2c2);
}

I2C_HandleTypeDef *Ol_Bus(void) { return &s_i2c2; }

void Ol_Present(void)
{
    uint8_t a = ol_probe();
    if (a) { printf("OLSCAN> 应答 7bit=0x%02X\r\n", (unsigned)a); }
    else   { printf("OLSCAN> 无应答（查 VCC=3.3V / SCL->PB10 / SDA->PB11 / 共地）\r\n"); }
}

/* ---------------- SSD1306 初始化 ---------------- */
static void ol_init_seq(void)
{
    static const uint8_t seq[] = {
        0xAE,               /* display off */
        0xD5, 0x80,         /* clock div */
        0xA8, 0x3F,         /* multiplex = 64 */
        0xD3, 0x00,         /* display offset */
        0x40,               /* start line 0 */
        0x8D, 0x14,         /* charge pump ON */
        0x20, 0x00,         /* 水平寻址模式 */
        0xA1,               /* segment remap */
        0xC8,               /* COM scan dec */
        0xDA, 0x12,         /* COM pins */
        0x81, 0xCF,         /* contrast */
        0xD9, 0xF1,         /* pre-charge */
        0xDB, 0x40,         /* VCOMH */
        0xA4, 0xA6, 0xAF,
    };
    ol_wr(0x00, seq, (uint16_t)sizeof(seq));
}

/* ---------------- 帧缓冲 ---------------- */
void Ol_FbClear(void) { memset(s_fb, 0, sizeof(s_fb)); }

void Ol_FbFlushPage(int page)
{
    if (page < 0 || page > 7) { return; }
    ol_c1(0x21); ol_c1(0x00); ol_c1(0x7F);              /* 列 0~127 */
    ol_c1(0x22); ol_c1((uint8_t)page); ol_c1((uint8_t)page);  /* 只选这一页 */
    ol_wr(0x40, s_fb[page], OLED_W);
}

void Ol_FbFlush(void)
{
    ol_c1(0x21); ol_c1(0x00); ol_c1(0x7F);              /* 列 0~127 */
    ol_c1(0x22); ol_c1(0x00); ol_c1(0x07);              /* 页 0~7 */
    for (int p = 0; p < 8; p++) { ol_wr(0x40, s_fb[p], OLED_W); }
}

/* 画一个 ASCII（6×8，落在单页里）：返回占宽 6 */
static void fb_ascii(int x, int page, char c)
{
    if (x < 0 || x + 5 >= OLED_W || page < 0 || page > 7) { return; }
    uint8_t idx = (uint8_t)c;
    if (idx < FONT_ASCII_FIRST || idx >= FONT_ASCII_FIRST + FONT_ASCII_COUNT) { idx = '?'; }
    const uint8_t *g = FONT_ASCII[idx - FONT_ASCII_FIRST];
    for (int i = 0; i < 6; i++) { s_fb[page][x + i] = g[i]; }
}

/* 16×16 大号 ASCII（真字模，与汉字同高）：8 字/行、4 行，0.96" 上足够清晰 ✓ */
int Ol_TextBig(int x, int page, const char *s)
{
    int x0 = x;
    if (page < 0 || page + 1 > 7) { return 0; }
    for (; *s; s++, x += 16) {
        if (x + 15 >= OLED_W) { break; }
        uint8_t c = (uint8_t)*s, k = 0, found = 0;
        for (k = 0; k < FONT_BIG_COUNT; k++) {
            if (FONT_BIG_UTF8[k] == c) { found = 1; break; }
        }
        if (!found) { k = 0; }                        /* 无此字 ⇒ 画空格 */
        const uint8_t *g = FONT_ASCII_BIG[k];         /* 32B = 16 列 × 2 字节 */
        for (int i = 0; i < 16; i++) {
            s_fb[page][x + i]     |= g[i * 2];
            s_fb[page + 1][x + i] |= g[i * 2 + 1];
        }
    }
    return x - x0;
}

/* 查汉字字模下标；找不到返回 -1 */
static int cn_index(const uint8_t *u3)
{
    for (int i = 0; i < FONT_CN_COUNT; i++) {
        if (FONT_CN_UTF8[i * 3] == u3[0] && FONT_CN_UTF8[i * 3 + 1] == u3[1] && FONT_CN_UTF8[i * 3 + 2] == u3[2]) {
            return i;
        }
    }
    return -1;
}

/* 画一个汉字（16×16，跨 2 页）：返回占宽 16 */
static void fb_cn(int x, int page, int gi)
{
    if (x < 0 || x + 15 >= OLED_W || page < 0 || page > 6) { return; }
    const uint8_t *g = FONT_CN[gi];                     /* 16 列 × 2 页 = 32 字节 */
    for (int i = 0; i < 16; i++) {
        s_fb[page][x + i]         = g[i];               /* 上半（8 行） */
        s_fb[page + 1][x + i]     = g[16 + i];          /* 下半（8 行） */
    }
}

/* 缺字占位：画一个空心方框（16×16），一眼能看出"这里少个字" */
static void fb_box(int x, int page)
{
    if (x < 0 || x + 15 >= OLED_W || page < 0 || page > 6) { return; }
    s_fb[page][x] = 0xFF; s_fb[page][x + 15] = 0xFF;
    s_fb[page + 1][x] = 0xFF; s_fb[page + 1][x + 15] = 0xFF;
    for (int i = 1; i < 15; i++) { s_fb[page][x + i] |= 0x81; s_fb[page + 1][x + i] |= 0x81; }
}

/* 画 UTF-8 字符串：ASCII 走 6×8，汉字走 16×16（字库里没有的画空心框）
 * 返回缺失字符个数；整串画不下时提前停并返回 -1 */
int Ol_Text(int x, int page, const char *utf8)
{
    const uint8_t *p = (const uint8_t *)utf8;
    int miss = 0;

    while (*p) {
        if (*p < 0x80) {                                /* ASCII */
            if (x + 6 > OLED_W) { return -1; }
            fb_ascii(x, page, (char)*p);
            x += 6;
            p++;
        } else if ((*p & 0xF0) == 0xE0) {               /* 三字节 UTF-8（汉字） */
            if (x + 16 > OLED_W) { return -1; }
            int gi = cn_index(p);
            if (gi >= 0) { fb_cn(x, page, gi); }
            else {
                fb_box(x, page);
                miss++;
                printf("# 字库缺字：UTF-8 %02X %02X %02X → 请加进 tools/生成字库.py 的 CN_CHARS\r\n",
                       p[0], p[1], p[2]);
            }
            x += 16;
            p += 3;
        } else {                                        /* 其它字节：跳过 1 字节 */
            p++;
        }
    }
    return miss;
}

/* ---------------- bring-up ---------------- */
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
    Ol_FbClear();

    /* 四周边框 + 棋盘格（比"点亮全屏"更能验寻址） */
    for (int i = 0; i < OLED_W; i++) { s_fb[0][i] = 0xFF; s_fb[7][i] = 0xFF; }
    for (int p = 0; p < 8; p++) {
        s_fb[p][0] = 0xFF; s_fb[p][OLED_W - 1] = 0xFF;
        if (p & 1) { for (int x = 1; x < OLED_W - 1; x++) { s_fb[p][x] = (uint8_t)((x & 0x08) ? 0xF0 : 0x0F); } }
    }
    Ol_FbFlush();
    printf("OLED> 完成：屏幕应出现【棋盘格 + 四周黑边框】\r\n");
}

void Ol_TextTest(void)
{
    uint8_t a = ol_probe();
    if (!a) { printf("OLED> 没找到（0x3C/0x3D 都无应答）\r\n"); return; }

    ol_init_seq();
    Ol_FbClear();

    int miss = 0;
    miss += Ol_Text(0, 0, "ABC abc 0123 !@#$%&*()");     /* 英文/数字/符号：21 字 × 6px = 126 */
    miss += Ol_Text(0, 2, "中文显示测试");                 /* 汉字 16×16：6 字 × 16px = 96 */
    miss += Ol_Text(0, 4, "状态速度偏差左右");             /* 6 字 = 96px */
    miss += Ol_Text(0, 6, "SilkRoad-Car v3 ok?");         /* 20 字 × 6px = 120 */
    Ol_FbFlush();

    printf("OLED> 测试页已刷：\r\n");
    printf("        第1行 ASCII：ABC abc 0123 !@#$%%&*()\r\n");
    printf("        第2行 汉字  ：中文显示测试\r\n");
    printf("        第3行 汉字  ：状态速度偏差左右\r\n");
    printf("        第4行 ASCII：SilkRoad-Car v3 ok?\r\n");
    if (miss > 0) { printf("OLED> ⚠️ 有 %d 个字不在字库里（画成空心方框）—— 见上面的缺字提示\r\n", miss); }
    else          { printf("OLED> 字库齐全，无缺字\r\n"); }
}
