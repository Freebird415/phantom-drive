/* =============================================================================
 *  app_log.c —— 板载快照：RAM 记录 + 内部 Flash 持久化（详见 app_log.h）
 *
 *  Flash 布局（基址 0x0800C000，占最后 16KB = 16 页）：
 *      [0..3]   magic  0x50414E53（"SNAP"）
 *      [4..7]   帧数
 *      [8..11]  帧数据校验和（FNV-1a）
 *      [12..15] 保留（0）
 *      [16..]   帧数据，5 字节/帧，紧排
 * ========================================================================== */
#include "app/app_log.h"
#include "stm32f1xx_hal.h"

#include <stdio.h>
#include <string.h>

#define LOG_FLASH_ADDR   0x0800C000UL   /* ⚠️ 见 app_log.h：固件必须 < 48KB */
#define LOG_FLASH_PAGES  16u
#define LOG_FLASH_MAGIC  0x50414E53UL   /* "SNAP" */
#define LOG_HDR_BYTES    16u

typedef struct {
    uint8_t bits;      /* 8 路灰度：bit0=第1路 … bit7=第8路；1 = 黑 */
    int8_t  err10;     /* err × 10（−8.0~+8.0 ⇒ −80~+80）*/
    int8_t  pwm_l;     /* −100~100 */
    int8_t  pwm_r;
    uint8_t state;
    uint8_t pad;       /* ⚠️ 必须凑成偶数：STM32F1 的 Flash 只能按【半字】写，
                          奇数长度会静默丢掉最后 1 个字节 ⇒ 校验和永远对不上。
                          （2026-09-30 实踩：347 帧×5B=1735B 奇数 ⇒ 数据坏）*/
} LogFrame;
_Static_assert(sizeof(LogFrame) == 6, "LogFrame 必须是 6 字节（Flash 半字写入要求偶数长度）");

static LogFrame s_buf[LOG_MAX_FRAMES];
static uint16_t s_n;
static uint8_t  s_rec;
static uint8_t  s_full;

static int8_t sat_i8(int v)
{
    if (v > 127)  { return 127; }
    if (v < -127) { return -127; }
    return (int8_t)v;
}

static uint32_t log_sum(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;               /* FNV-1a */
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* ------------------------------ RAM 操作 ------------------------------ */
void Log_Start(void)      { s_rec = 1; }

/* 停车：停止记录 + **自动持久化到 Flash**（掉电不丢）*/
void Log_Stop(void)
{
    uint8_t was = s_rec;
    s_rec = 0;
    if (was) { Log_SaveFlash(); }
}
uint8_t Log_IsRecording(void) { return s_rec; }
uint16_t Log_Count(void)  { return s_n; }

void Log_Note(uint8_t bits8, float err, int pwm_l, int pwm_r, int state)
{
    if (!s_rec) { return; }
    if (s_n >= LOG_MAX_FRAMES) {
        /* ⚠️ 写满即停，绝不覆盖 —— 保证已经记下的不会丢 */
        s_rec = 0; s_full = 1;
        printf("# SNAP FULL: 已记满 %u 帧，停止记录（数据保留，停车时会存入 Flash）\r\n",
               (unsigned)LOG_MAX_FRAMES);
        return;
    }
    LogFrame *f = &s_buf[s_n++];
    f->bits  = bits8;
    f->err10 = sat_i8((int)(err * 10.0f + (err >= 0.0f ? 0.5f : -0.5f)));
    f->pwm_l = sat_i8(pwm_l);
    f->pwm_r = sat_i8(pwm_r);
    f->state = (uint8_t)state;
}

/* ------------------------------ Flash ------------------------------ */
/* 写入前先擦 16 页（约 0.5 秒，CPU 会停住 —— 只在停车后调用）
 * ⚠️⚠️ **必须关中断**：STM32F1 编程 Flash 时 CPU 会停住等 BSY，
 *    而 USART1 接收中断 / SysTick 的 ISR **代码就在 Flash 里** ——
 *    不关中断的话，取指与“正在编程”打架，写入会被破坏。
 *    （2026-09-30 实踩：头部 8 个半字写对了，6250 个半字的数据全坏 ✗）*/
static uint8_t log_flash_write(const uint8_t *hdr, const uint8_t *frames, uint32_t nbytes)
{
    FLASH_EraseInitTypeDef er = {0};
    uint32_t bad = 0;
    uint8_t ok = 0;

    __disable_irq();                       /* ⚠️ 全程关中断（约 0.5 秒，此时车已停）*/
    HAL_FLASH_Unlock();

    er.TypeErase   = FLASH_TYPEERASE_PAGES;
    er.PageAddress = LOG_FLASH_ADDR;
    er.NbPages     = LOG_FLASH_PAGES;
    if (HAL_FLASHEx_Erase(&er, &bad) == HAL_OK) {
        ok = 1;
        for (uint32_t i = 0; i < LOG_HDR_BYTES / 2u; i++) {
            uint16_t v; memcpy(&v, hdr + i * 2u, 2u);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                  LOG_FLASH_ADDR + i * 2u, (uint64_t)v) != HAL_OK) { ok = 0; break; }
        }
        for (uint32_t i = 0; ok && i < nbytes / 2u; i++) {
            uint16_t v; memcpy(&v, frames + i * 2u, 2u);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                  LOG_FLASH_ADDR + LOG_HDR_BYTES + i * 2u, (uint64_t)v) != HAL_OK) { ok = 0; break; }
        }
    }
    HAL_FLASH_Lock();
    __enable_irq();

    if (!ok) { return 0; }

    /* 写入后**读回校验**（不再默默错）*/
    if (memcmp((const void *)LOG_FLASH_ADDR, hdr, LOG_HDR_BYTES) != 0) { return 0; }
    if (memcmp((const void *)(LOG_FLASH_ADDR + LOG_HDR_BYTES), frames, nbytes) != 0) { return 0; }
    return 1;
}

void Log_SaveFlash(void)
{
    if (s_n == 0u) { return; }
    uint8_t hdr[LOG_HDR_BYTES];
    uint32_t nbytes = (uint32_t)s_n * sizeof(LogFrame);   /* 6 的倍数 ⇒ 一定偶数 ✓ */
    uint32_t magic = LOG_FLASH_MAGIC, cnt = s_n, sum = log_sum((const uint8_t *)s_buf, nbytes);
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr + 0, &magic, 4);
    memcpy(hdr + 4, &cnt,   4);
    memcpy(hdr + 8, &sum,   4);

    if (log_flash_write(hdr, (const uint8_t *)s_buf, nbytes)) {
        printf("# SNAP 已存入内部 Flash（%u 帧）—— 掉电不丢 ✓\r\n", (unsigned)s_n);
    } else {
        printf("# SNAP ✗ 存 Flash 失败（数据仍在 RAM，别断电）\r\n");
    }
}

void Log_LoadFlash(void)
{
    const uint32_t *w = (const uint32_t *)LOG_FLASH_ADDR;
    if (w[0] != LOG_FLASH_MAGIC) { return; }                   /* 没有有效快照 */
    uint32_t cnt = w[1], sum = w[2];
    if (cnt == 0u || cnt > LOG_MAX_FRAMES) { return; }
    uint32_t nbytes = cnt * sizeof(LogFrame);
    const uint8_t *src = (const uint8_t *)(LOG_FLASH_ADDR + LOG_HDR_BYTES);
    if (log_sum(src, nbytes) != sum) { return; }               /* 校验失败，忽略 */
    memcpy(s_buf, src, nbytes);
    s_n = (uint16_t)cnt;
    s_rec = 0;
    s_full = 0;
    printf("# SNAP 从 Flash 恢复 %u 帧（上次跑车的数据，掉电保留）\r\n", (unsigned)s_n);
}

/* 清 RAM + 让 Flash 快照失效（把 magic 写 0 —— Flash 只能 1→0，写 0 是可行的）*/
void Log_Reset(void)
{
    s_n = 0; s_rec = 0; s_full = 0;
    __disable_irq();                       /* 同上：写 Flash 必须关中断 */
    HAL_FLASH_Unlock();
    uint64_t zero = 0u;
    (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, LOG_FLASH_ADDR,        zero);
    (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, LOG_FLASH_ADDR + 2u,   zero);
    HAL_FLASH_Lock();
    __enable_irq();
}

void Log_Status(void)
{
    printf("# SNAP rec=%u full=%u n=%u/%u (~%u.%us @100Hz)  RAM=%uB  Flash=%uB @0x%08lX\r\n",
           (unsigned)s_rec, (unsigned)s_full, (unsigned)s_n, (unsigned)LOG_MAX_FRAMES,
           (unsigned)(s_n / 100u), (unsigned)(s_n % 100u),
           (unsigned)(LOG_MAX_FRAMES * 5u), (unsigned)(LOG_FLASH_PAGES * 1024u),
           (unsigned long)LOG_FLASH_ADDR);
}

/* 排查用：直读 Flash 头的原始内容，看存进去的和读出来的是否一致 */
void Log_FlashInfo(void)
{
    const uint32_t *w = (const uint32_t *)LOG_FLASH_ADDR;
    uint32_t cnt = w[1], sum = w[2], calc = 0u;
    if (cnt > 0u && cnt <= LOG_MAX_FRAMES) {
        calc = log_sum((const uint8_t *)(LOG_FLASH_ADDR + LOG_HDR_BYTES),
                       cnt * sizeof(LogFrame));
    }
    printf("# FLASH @0x%08lX  magic=0x%08lX (期望 0x%08lX)  cnt=%lu  sum=0x%08lX  calc=0x%08lX\r\n",
           (unsigned long)LOG_FLASH_ADDR, (unsigned long)w[0], (unsigned long)LOG_FLASH_MAGIC,
           (unsigned long)cnt, (unsigned long)sum, (unsigned long)calc);
    printf("#   → magic %s | cnt %s | sum %s\r\n",
           (w[0] == LOG_FLASH_MAGIC) ? "✓" : "✗",
           (cnt > 0u && cnt <= LOG_MAX_FRAMES) ? "✓" : "✗",
           (sum == calc && sum != 0u) ? "✓" : "✗");
}

void Log_Dump(void)
{
    printf("# DUMP n=%u  (格式同 $T：ms,s0..s7,err,pwm_l,pwm_r,state)\r\n", (unsigned)s_n);
    for (uint16_t i = 0; i < s_n; i++) {
        const LogFrame *f = &s_buf[i];
        printf("$T,%u", (unsigned)((uint32_t)i * 10u));
        for (int b = 0; b < 8; b++) {
            printf(",%d", (int)((f->bits >> b) & 1u));
        }
        printf(",%.1f,%d,%d,%d\r\n",
               (double)f->err10 / 10.0, (int)f->pwm_l, (int)f->pwm_r, (int)f->state);
    }
    printf("# DUMP END n=%u\r\n", (unsigned)s_n);
}
