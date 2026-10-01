/* =============================================================================
 *  app_telemetry.c —— 遥测帧输出 + 电脑命令处理
 *  帧格式与命令表见 docs/遥测协议.md（改协议要两边同时改）
 * ========================================================================== */
#include "app_telemetry.h"
#include "bsp/bsp_board.h"
#include "drv/drv_ultrasonic.h"
#include "drv/drv_grayscale.h"
#include "drv/drv_oled.h"
#include "drv/drv_motor.h"
#include "drv/drv_encoder.h"
#include "drv/drv_hmi.h"
#include "drv/drv_mpu6050.h"
#include "app_trace.h"          /* 算法开关命令 TRACE 用 */
#include "app_avoid.h"          /* 算法开关命令 AVOID 用 */
#include "app_log.h"            /* 板载快照命令 SNAP / DUMP 用 */

#include <stdio.h>
#include <string.h>

static Params s_p = { 9.00f, 0.00f, 0.30f, 12 };
/* ↑ 2026-10-01【1.5× 档】：把慢速档（kp6/kd0.2/base8）整体 ×1.5 ⇒ kp9/kd0.3/base12。
   为什么连 kp/kd 一起乘：修正量是叠加在 base 上的舵量，base 提了 1.5 倍，
   转向灵敏度得跟着提，否则同样的弯会欠修正。
   现场想微调：`SET base_speed 10` / `SET kp 8`（断电会丢，定稿要写回默认值）*/
/* ↑ 2026-10-01【慢速档】：base_speed 8（约 45mm/s）、kp 6、kd 0.2
   —— 弯急，先用能转的最低速跑，目标是把黑线稳定压在中间。
   跑车时插不了串口 ⇒ 参数必须做进默认值（`SET` 的值断电就丢）。
   慢速时 steer/base 的比例要跟着降，否则同样 kp 在低速下会"甩头"。*/
/* ↑ 2026-10-01：kp=14、kd=0.3（加 D 项提前预判急弯）、base_speed=35
   （15 太低：实测电机卡在死区，只跑 24mm/s ⇒ 差速没劲）。
   2026-09-30：默认 kp 1.2→10、base_speed 30→15。
   改默认值的理由：跑车时插不了串口，断电后 SET 的参数会丢
   ⇒ 必须把“跑车那套参数”做进固件默认值里。A01/A02 以后可按需改。 */

Params *Tlm_Params(void) { return &s_p; }

/* ==================== 发帧 ==================== */
void Tlm_SendParams(void)
{
    char a[16], b[16], c[16];
    Bsp_FmtFix(a, sizeof a, s_p.kp, 2, 0);
    Bsp_FmtFix(b, sizeof b, s_p.ki, 2, 0);
    Bsp_FmtFix(c, sizeof c, s_p.kd, 2, 0);
    printf("$P,%u,%s,%s,%s,%d\r\n", (unsigned)HAL_GetTick(), a, b, c, s_p.base_speed);
}

void Tlm_SendEvent(uint8_t code, const char *note)
{
    printf("$E,%u,%u,%s\r\n", (unsigned)HAL_GetTick(), (unsigned)code, note);
}

void Tlm_SendT(uint32_t ms, const int s[8], float err, int pwm_l, int pwm_r, int state)
{
    char e[16];
    Bsp_FmtFix(e, sizeof e, err, 1, 0);
    printf("$T,%u,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d\r\n",
           (unsigned)ms,
           s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
           e, pwm_l, pwm_r, state);
}

void Tlm_SendU(uint32_t ms, int32_t l_mm, int32_t r_mm)
{
    printf("$U,%u,%d,%d\r\n", (unsigned)ms, (int)l_mm, (int)r_mm);
}

/* ---- BLE（USART2）回声：手机发什么就回什么，同时打到 USART1 调试口 ---- */
static char    s_bleline[64];
static uint8_t s_blerlen = 0;
static uint8_t s_blebyte;

/* ==================== 收命令 ====================
   重要：中断里**只收行**，命令执行放到主循环（Tlm_Poll）。
   原因：命令处理里的 printf 会和主循环的 printf 抢同一个 UART，
   在中断里跑会互相打断、输出被静默丢掉；而且有些命令要阻塞几十毫秒。 */
static char    s_rxline[64];
static uint8_t s_rxlen = 0;
static uint8_t s_rxbyte;

static char    s_cmd[64];
static volatile uint8_t s_cmd_pending = 0;

static uint8_t s_us_auto = 1;      /* 主循环自动测超声波；AUTO 0 可关（调试用） */

uint8_t Tlm_UsAuto(void) { return s_us_auto; }

/* ---- 轻量浮点解析（**不要**用 sscanf("%f")）--------------------------------
   ⚠️ newlib-nano 的 %f 【解析】需要单独链接 `-Wl,-u,_scanf_float`；
      实测把固件从 47.3KB 顶到 53.9KB（超当时的 48KB 红线）⇒ 所以自己解析（几十字节，不拉库）。
      同理**打印**也一律用 Bsp_FmtFix（%f 会拉 ~9KB）。
      同样原因也不能用 atof（拉 ~7KB）。
   支持：前导空格、正负号、整数、小数；缺省 0。返回 1 = 至少解析出一个数字。 */
static int parse_float(const char *s, float *out)
{
    int neg = 0, got = 0;
    float v = 0.0f, frac = 0.1f;
    while (*s == ' ' || *s == '\t') { s++; }
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10.0f + (float)(*s - '0'); got = 1; s++; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            v += (float)(*s - '0') * frac; frac *= 0.1f; got = 1; s++;
        }
    }
    if (!got) { return 0; }
    *out = neg ? -v : v;
    return 1;
}

/* ---- 轻量整数解析（同样是为了不拉 sscanf 库）--------------------
   返回：解析到的数字个数（0 = 一个都没解析到）。
   分隔符不限定是空格：遇到的第一个非数字字符就当下一段的开始。 */
static int parse_int(const char *s, int *out)
{
    int got = 0;
    while (*s != 0) {
        int neg = 0, v = 0, d = 0;
        while (*s == ' ' || *s == '\t' || *s == ',') { s++; }
        if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); d = 1; s++; }
        if (d) { if (out) { *out = neg ? -v : v; } got++; }
        if (*s != 0 && !( (*s >= '0' && *s <= '9') || *s == ' ' || *s == '\t' || *s == ','
                          || *s == '+' || *s == '-' )) { s++; }
        if (out) { out++; }
    }
    return got;
}

static void handle_command(char *line)
{
    /* ---- 连通性 / 参数 ---- */
    if (strcmp(line, "PING") == 0) { Tlm_SendEvent(90, "PONG"); return; }
    if (strcmp(line, "GET") == 0)  { Tlm_SendParams(); return; }

    /* ---- 算法发车开关（A01/A02）----
       ⚠️ 两个都默认关（只算不发）。开之前先把车架起来、轮子悬空！ */
    if (strncmp(line, "TRACE ", 6) == 0) {
        Trace_SetDrive(line[6] == '1');
        printf("# TRACE drive=%u  灰度=%s\r\n", (unsigned)Trace_GetDrive(),
               Trace_Online() ? "在线" : "**不在线(模拟数据/不驱动)**");
        return;
    }
    if (strncmp(line, "AVOID ", 6) == 0) {
        Avoid_SetDrive(line[6] == '1');
        printf("# AVOID drive=%u\r\n", (unsigned)Avoid_GetDrive());
        return;
    }

    /* ---- 板载 RAM 快照（跑车时插不了串口，跑完用 DUMP 读）----
       ⚠️ RAM 断电就丢：必须先 “SNAP 0”（或按 PA12 按钮）再关总开关 */
    if (strcmp(line, "SNAP 1") == 0)   { Log_Reset(); Log_Start(); Log_Status(); return; }
    if (strcmp(line, "SNAP 0") == 0)   { Log_Stop(); Log_Status(); return; }
    if (strcmp(line, "SNAPRESET") == 0){ Log_Reset(); Log_Status(); return; }
    /* BLE 实时日志回传（跑车时插不了串口，用无线把 8B 帧发回电脑）*/
    if (strncmp(line, "BTL ", 4) == 0) {
        Log_SetBle(line[4] == '1');
        printf("# BTL=%u  (%s)  4B同步头+6x8B帧=52B/次, 每180ms一次, 实测送达~7.7帧/s  drop=%u\r\n",
               (unsigned)Log_Ble(), Log_Ble() ? "开：数据从 BLE 推给电脑" : "关",
               (unsigned)Log_BleDrop());
        return;
    }
    if (strcmp(line, "SNAP") == 0 || strcmp(line, "SNAP?") == 0) { Log_Status(); return; }
    if (strcmp(line, "DUMP") == 0)     { Log_Dump(); return; }
    if (strcmp(line, "SNAPFLASH") == 0){ Log_FlashInfo(); return; }

    /* ---- 终点精停（T 头）----
       ⚠️ 参数解析用 parse_float（不用 sscanf "%f"，也不 atof）—— 见文件上方注释 */
    if (strncmp(line, "DISTCAL ", 8) == 0) {
        float m = 0.0f;
        if (parse_float(line + 8, &m)) { Trace_DistCal(m); }
        else { printf("# 用法：DISTCAL <米>   例：ENCRESET → 推车走 1m → DISTCAL 1.0\r\n"); }
        return;
    }
    if (strncmp(line, "STOPDIST ", 9) == 0) {
        int d = 0;
        if (parse_int(line + 9, &d) == 1) { Trace_SetStopDist(d); }
        return;
    }
    if (strcmp(line, "FINSTAT") == 0)       { Trace_FinStatus(); return; }
    if (strcmp(line, "FINRESET") == 0)      { Trace_FinReset(); return; }
    if (strncmp(line, "SEEK ", 5) == 0)   { Trace_SetSeek(line[5] == '1'); Tlm_SendEvent(95, line[5]=='1' ? "SEEK ON" : "SEEK OFF"); return; }   /* 调试：起步找线 */

    /* ---- 超声波 ---- */
    if (line[0] == 'U' && line[1] == 'S') {
        int id = -1, n = 1;
        if (line[2] == ' ') { (void)parse_int(line + 3, &id); }
        if (id < -1 || id > 1) { id = -1; }
        if (n < 1) { n = 1; }
        if (n > 30) { n = 30; }
        printf("# US id=%d n=%d auto=%u\r\n", id, n, (unsigned)s_us_auto);
        HAL_Delay(80);
        for (int k = 0; k < n; k++) {
            if (id != 1) { Us_Read(US_L); Us_PrintDebug(US_L); }
            if (id != 0) { HAL_Delay(70); Us_Read(US_R); Us_PrintDebug(US_R); }
            if (k + 1 < n) { HAL_Delay(100); }
        }
        Tlm_SendU(HAL_GetTick(), Us_Get(US_L)->mm, Us_Get(US_R)->mm);
        return;
    }
    if (strncmp(line, "AUTO ", 5) == 0) {
        char c = line[5];
        s_us_auto = (c == '0') ? 0u : (c == '2') ? 2u : (c == '3') ? 3u : 1u;
        printf("# AUTO=%u\r\n", (unsigned)s_us_auto);
        return;
    }

    /* ---- MPU6050（诊断用） ---- */
    if (strcmp(line, "MPU") == 0) { Mpu_PrintDebug(); return; }
    /* ⭐ 手动重新初始化 MPU6050（芯片断电/上电后可能回到睡眠态；固件也会自动重试）*/
    if (strcmp(line, "MPUINIT") == 0) { Mpu_Init(); Tlm_SendEvent(94, "MPU REINIT"); return; }
    if (strcmp(line, "MPURESET") == 0) { Mpu_DebugReset(); return; }   /* 调试：模拟断电复位，验证自愈 */

    /* ---- BLE 波特率扫描：BTBAUD <n> 改速率；BTAT 发 AT 看回什么 ---- */
    if (strncmp(line, "BTBAUD ", 7) == 0) {
        int b = 0;
        if (parse_int(line + 7, &b) == 1 && b >= 1200 && b <= 921600) {
            Bsp_Uart2SetBaud((uint32_t)b);
            printf("# BLE baud -> %d\r\n", b);
            HAL_UART_Receive_IT(Bsp_Uart2(), &s_blebyte, 1);
        }
        return;
    }
    if (strcmp(line, "BTAT") == 0) {
        /* HC-08 手册原文：AT 指令「不用回车换行」；但部分兼容品接受 CRLF
           -> 两种形式各试一次，谁有回包就用谁（回来的字节原样打印，CR 用 | 标出） */
        /* 实测（2026-09-30，HC-06 丝印的 BLE 克隆）：**必须 CRLF 结尾**，
           无结尾或只给 \r 都不回包 -> CRLF 放第一优先 */
        static const uint8_t form[2][4] = { { 'A', 'T', 13, 10 }, { 'A', 'T', 0, 0 } };
        static const uint8_t flen[2]    = { 4u, 2u };
        char out[40];
        Bsp_Uart2TxAbort();                      /* 阻塞式发送前清空日志队列，避免抢 UART */
        HAL_UART_AbortReceive_IT(Bsp_Uart2());
        for (int f = 0; f < 2; f++) {
            uint8_t c; int n = 0;
            HAL_UART_Transmit(Bsp_Uart2(), (uint8_t *)form[f], flen[f], 100);
            while (n < 30) {
                if (HAL_UART_Receive(Bsp_Uart2(), &c, 1, 120) != HAL_OK) { break; }
                if (c >= 32 && c < 127) { out[n++] = (char)c; }
                else if (c == 13) { out[n++] = '|'; }
            }
            out[n] = 0;
            printf("# BTAT form=%u len=%u n=%d <%s>\r\n",
                   (unsigned)f, (unsigned)flen[f], n, out);
            if (n > 0) { break; }
        }
        HAL_UART_Receive_IT(Bsp_Uart2(), &s_blebyte, 1);
        return;
    }

    /* ---- 向 BLE 口发一条任意文本（自动补 CRLF —— 实测该模块必须 CRLF）----
       例：BTSEND AT+VERSION   /   BTSEND AT+BAUD  */
    if (strncmp(line, "BTSEND ", 7) == 0) {
        char out[40]; int n = 0; uint8_t c;
        Bsp_Uart2TxAbort();                      /* 阻塞式发送前清空日志队列，避免抢 UART */
        HAL_UART_AbortReceive_IT(Bsp_Uart2());
        HAL_UART_Transmit(Bsp_Uart2(), (uint8_t *)(line + 7),
                          (uint16_t)strlen(line + 7), 200);
        { uint8_t eol[2] = { 13, 10 };
          HAL_UART_Transmit(Bsp_Uart2(), eol, 2, 100); }
        while (n < 38) {
            if (HAL_UART_Receive(Bsp_Uart2(), &c, 1, 200) != HAL_OK) { break; }
            if (c >= 32 && c < 127) { out[n++] = (char)c; }
            else if (c == 13) { out[n++] = '|'; }
        }
        out[n] = 0;
        printf("# BTSEND <%s> -> n=%d <%s>\r\n", line + 7, n, out);
        HAL_UART_Receive_IT(Bsp_Uart2(), &s_blebyte, 1);
        return;
    }

    /* ---- BLE：主动发一句给手机 ---- */
    if (strncmp(line, "BT ", 3) == 0) {
        printf("# BLE> %s\r\n", line + 3);
        HAL_UART_Transmit(Bsp_Uart2(), (uint8_t *)(line + 3), (uint16_t)strlen(line + 3), 100);
        HAL_UART_Transmit(Bsp_Uart2(), (uint8_t *)"\r\n", 2, 100);
        return;
    }

    /* ---- 人机：按键 + 蜂鸣器 ---- */
    if (strcmp(line, "KEY") == 0)  { Hmi_PrintDebug(); return; }
    if (strncmp(line, "BEEP", 4) == 0) {
        int ms = 200;
        if (line[4] == ' ') { (void)parse_int(line + 5, &ms); }
        if (ms < 1) { ms = 1; } if (ms > 3000) { ms = 3000; }
        printf("HMI> 蜂鸣 %d ms%s\r\n", ms, Hmi_Mute() ? "（静音中，不会响）" : "");
        Hmi_Beep((uint16_t)ms);
        return;
    }
    /* 静音开关：旁边有人休息/台面反复调试时用（只拦 Hmi_Beep，不影响 BeepOn/Off）*/
    if (strncmp(line, "MUTE ", 5) == 0) {
        Hmi_SetMute(line[5] == '1');
        printf("# MUTE=%u  %s\r\n", (unsigned)Hmi_Mute(), Hmi_Mute() ? "(蜂鸣器已静音)" : "(恢复)");
        return;
    }

    /* ---- 编码器引脚原始电平（诊断：转轮子时看 C1/C2 到底有没有跳变）----
       C1/C2 是开漏输出，静态时可能是高或低；关键是“转的时候会不会变” */
    if (strcmp(line, "ENCPIN") == 0) {
        printf("# ENCPIN L:PB6=%d PB7=%d | R:PA6=%d PA7=%d\r\n",
               (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6),
               (int)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7),
               (int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6),
               (int)HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_7));
        return;
    }

    /* ---- 编码器 ---- */
    if (strcmp(line, "ENCRESET") == 0) { En_Reset(); En_PrintDebug(); return; }
    if (strcmp(line, "ENC") == 0) { En_PrintDebug(); return; }

    /* ---- 电机 (TB6612) ---- */
    if (strncmp(line, "MOT", 3) == 0) {              /* MOT <左 %> <右 %> ；不带参数=报当前状态 */
        int v[2] = {0, 0};
        if (line[3] == ' ' && parse_int(line + 4, v) == 2) {
            Mo_Set(v[0], v[1]);
        }
        Mo_PrintDebug();
        return;
    }
    if (strcmp(line, "STOP") == 0) { Mo_Stop(); Mo_PrintDebug(); return; }
    if (strcmp(line, "BRAKE") == 0) { Mo_Brake(); Mo_PrintDebug(); return; }

    /* ---- 灰度 (I2C1) ---- */
    if (strcmp(line, "GR") == 0) { Gr_PrintDebug(); return; }
    if (strcmp(line, "GRSCAN") == 0) { Gr_Scan(); return; }

    /* ---- OLED (I2C2) ---- */
    if (strcmp(line, "OLSCAN") == 0) {
        Ol_Present();
        return;
    }
    if (strcmp(line, "OLED") == 0) {
        Trace_DashEnable(0u);      /* 先停仪表盘，否则测试图案 0.5s 就被盖掉 */
        Ol_PrintDebug();
        printf("# 仪表盘已暂停，看完发 DASH 1 恢复\r\n");
        return;
    }
    if (strcmp(line, "OLEDTXT") == 0) {
        Trace_DashEnable(0u);
        Ol_TextTest();
        printf("# 仪表盘已暂停，看完发 DASH 1 恢复\r\n");
        return;
    }
    if (strncmp(line, "DASH ", 5) == 0) {
        Trace_DashEnable(line[5] == '1');
        printf("# DASH=%c\r\n", line[5] == '1' ? '1' : '0');
        return;
    }

    /* ---- 在线调参 ---- */
    if (strncmp(line, "SET ", 4) == 0) {
        char name[16] = {0};
        float val = 0.0f;
        const char *p = line + 4;
        int i = 0;
        while (p[i] != 0 && p[i] != ' ' && i < (int)sizeof(name) - 1) { name[i] = p[i]; i++; }
        name[i] = 0;
        if (i > 0 && parse_float(p + i, &val)) {
            if      (strcmp(name, "kp") == 0)         { s_p.kp = val; }
            else if (strcmp(name, "ki") == 0)         { s_p.ki = val; }
            else if (strcmp(name, "kd") == 0)         { s_p.kd = val; }
            else if (strcmp(name, "base_speed") == 0) { s_p.base_speed = (int)val; }
            Tlm_SendParams();
            return;
        }
    }

    Tlm_SendEvent(99, "UNKNOWN_CMD");
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {          /* BLE：原样回声 + 打到调试口 */
        char c = (char)s_blebyte;
        Bsp_Uart2Tx(&s_blebyte, 1);           /* 非阻塞回声（别拖主循环）*/
        if (c == '\n' || c == '\r') {
            if (s_blerlen > 0) { s_bleline[s_blerlen] = 0; printf("# BLE< %s\r\n", s_bleline); s_blerlen = 0; }
        } else if (s_blerlen < sizeof(s_bleline) - 1) {
            s_bleline[s_blerlen++] = c;
        } else { s_blerlen = 0; }
        HAL_UART_Receive_IT(Bsp_Uart2(), &s_blebyte, 1);
        return;
    }
    if (huart->Instance == USART1) {
        char c = (char)s_rxbyte;
        if (c == '\n' || c == '\r') {
            if (s_rxlen > 0) {
                s_rxline[s_rxlen] = '\0';
                if (!s_cmd_pending) {
                    for (uint8_t i = 0; i <= s_rxlen; i++) { s_cmd[i] = s_rxline[i]; }
                    s_cmd_pending = 1;
                }
                s_rxlen = 0;
            }
        } else if (s_rxlen < sizeof(s_rxline) - 1) {
            s_rxline[s_rxlen++] = c;
        } else {
            s_rxlen = 0;
        }
        HAL_UART_Receive_IT(Bsp_Uart(), &s_rxbyte, 1);
    }
}

void Tlm_Poll(void)
{
    if (!s_cmd_pending) { return; }
    char line[64];
    for (uint8_t i = 0; i < sizeof(line); i++) { line[i] = s_cmd[i]; }
    s_cmd_pending = 0;
    handle_command(line);
}

void Tlm_Init(void)
{
    HAL_UART_Receive_IT(Bsp_Uart(), &s_rxbyte, 1);
    HAL_UART_Receive_IT(Bsp_Uart2(), &s_blebyte, 1);   /* BLE 通道也开收 */
}
