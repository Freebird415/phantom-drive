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

#include <stdio.h>
#include <string.h>

static Params s_p = { 1.20f, 0.00f, 0.05f, 30 };

Params *Tlm_Params(void) { return &s_p; }

/* ==================== 发帧 ==================== */
void Tlm_SendParams(void)
{
    printf("$P,%u,%.2f,%.2f,%.2f,%d\r\n",
           (unsigned)HAL_GetTick(),
           (double)s_p.kp, (double)s_p.ki, (double)s_p.kd, s_p.base_speed);
}

void Tlm_SendEvent(uint8_t code, const char *note)
{
    printf("$E,%u,%u,%s\r\n", (unsigned)HAL_GetTick(), (unsigned)code, note);
}

void Tlm_SendT(uint32_t ms, const int s[8], float err, int pwm_l, int pwm_r, int state)
{
    printf("$T,%u,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%d,%d,%d\r\n",
           (unsigned)ms,
           s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
           (double)err, pwm_l, pwm_r, state);
}

void Tlm_SendU(uint32_t ms, int32_t l_mm, int32_t r_mm)
{
    printf("$U,%u,%d,%d\r\n", (unsigned)ms, (int)l_mm, (int)r_mm);
}

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

static void handle_command(char *line)
{
    /* ---- 连通性 / 参数 ---- */
    if (strcmp(line, "PING") == 0) { Tlm_SendEvent(90, "PONG"); return; }
    if (strcmp(line, "GET") == 0)  { Tlm_SendParams(); return; }

    /* ---- 超声波 ---- */
    if (line[0] == 'U' && line[1] == 'S') {
        int id = -1, n = 1;
        if (line[2] == ' ') { (void)sscanf(line + 3, "%d %d", &id, &n); }
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

    /* ---- 编码器 ---- */
    if (strcmp(line, "ENCRESET") == 0) { En_Reset(); En_PrintDebug(); return; }
    if (strcmp(line, "ENC") == 0) { En_PrintDebug(); return; }

    /* ---- 电机 (TB6612) ---- */
    if (strncmp(line, "MOT", 3) == 0) {              /* MOT <左 %> <右 %> ；不带参数=报当前状态 */
        int l = 0, r = 0;
        if (line[3] == ' ' && sscanf(line + 4, "%d %d", &l, &r) == 2) {
            Mo_Set(l, r);
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
    if (strcmp(line, "OLED") == 0) { Ol_PrintDebug(); return; }
    if (strcmp(line, "OLEDTXT") == 0) { Ol_TextTest(); return; }

    /* ---- 在线调参 ---- */
    if (strncmp(line, "SET ", 4) == 0) {
        char  name[16] = {0};
        float val = 0.0f;
        if (sscanf(line + 4, "%15s %f", name, &val) == 2) {
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
}
