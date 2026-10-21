/* =============================================================================
 *  tuning.h —— **全车调参总表（唯一出处）**
 *
 *  要改数值只看这一个文件；下面所有代码文件都只是引用这里的宏，不要再在别处改。
 *
 *  分成四块：
 *    §1 A01 循迹：正常巡航 PID            （kp / ki / kd / base / 上限）
 *    §2 A01 循迹：「回线后慢速窗口」      （时长 / 起止 PWM / 慢速 PID）
 *    §3 A02 避障：触发 · 判线 · 回线动作  （阈值 / 时间 / 旋转 PWM）
 *    §4 A02 轨迹：推出 + 回摆             （两段的时长与左右轮 PWM）
 *
 *  方向约定（很重要）：**右轮比左轮快 ⇒ 车向左转**；左轮比右轮快 ⇒ 车向右转。
 *  单位：*_MS 毫秒；PWM 是占空比 %（-100~100，负数 = 倒转）。
 * ==========================================================================*/
#ifndef APP_TUNING_H
#define APP_TUNING_H

/* ═══════════════════════ §1 A01 正常巡航 PID ═══════════════════════════════ */
#define A01_KP              35.00f   /* 比例：偏一点打多少舵（太大甩头/振荡，太小切弯）*/
#define A01_KI               0.30f   /* 积分：消除长弧道稳态偏差（太小≈纯 PD）*/
#define A01_KD               0.90f   /* 微分：阻尼/提前量（对灰度跳变敏感）*/
#define A01_BASE_SPEED         55    /* 巡航 PWM（%）：这就是"循迹基本速度" */
#define A01_BASE_MAX           90    /* 速度上限：SET 也越不过，运行时把 base 夹到这 */

/* ═══════════════════════ §2 A01「回线后慢速窗口」═══════════════════════════
 * A02 交还控制权（或 A01 自己丢线找回）之后，先在慢速下跑一段，再升到巡航：
 *   窗口内 base 从 A01_SLOW_PWM0 线性升到 A01_SLOW_PWM1，时长 A01_SLOW_MS。
 *   窗口一过，交回 A01_BASE_SPEED。
 * 关掉：A01_SLOW_MS = 0 或任一 PWM = 0。 */
#define A01_SLOW_MS         3500u    /* ① 慢速窗口时长 t（ms）*/
#define A01_SLOW_PWM0         20     /* ② 窗口起点 PWM（%）= "电机初速度" */
#define A01_SLOW_PWM1         45     /* ③ 窗口终点 PWM（%）= "电机末速度" */

/* 窗口内的 PID（默认与巡航相同；想让"慢速循迹"更软/更硬就改这三个）*/
/* ═══════════════════════ §1b A01 丢线兜底（行驶中意外八路全白）═══════════════
 * 只作用于「**A01 正常循迹中**突然八路全白」。
 * 不作用于：起步找线(state=5)、A02 避障/回线全过程、精停冲刺段(state=4)、灰度离线。
 *
 * A01_LOST_SEARCH = 1（默认）：判定方向 → 停车 → **原地旋转找线** → 居中并保持 → 交回循迹
 *    ① 0 ~ A01_LOST_HOLD_MS        直行 TR_ALL_WHITE_PWM(26/26)，跨胶带接缝/磨损
 *    ② 刹停 A01_LOST_STOP_WAIT_MS   消掉前进惯性
 *    ③ 原地旋转（PWM 沿用 A02 回线那套 L=+8 / R=-20），**方向按 err 符号**
 *    ④ 「第 3~6 路相邻两路黑 + 第 1/2/7/8 路全白」= 线已居中 ⇒ 刹停并保持
 *       A01_LOST_CENTER_HOLD_MS 复核 ⇒ 交回 A01 循迹（自动进 §2 慢速窗口）
 *    · 超时 A01_LOST_ROT_TIMEOUT_MS（≈一整圈）仍找不到 ⇒ 刹停 + 锁存（同旧行为）
 *
 * ⚠️⚠️ 方向必须这么理解（参照系容易搞反，务必按这张表现场核对）：
 *      err > 0  ⇒ **车向左冲出去了**（线落在阵列右侧）⇒ 车头要**向右摆** ⇒ L=+8 / R=-20
 *      err < 0  ⇒ **车向右冲出去了**（线落在阵列左侧）⇒ 车头要**向左摆** ⇒ L=-20 / R=+8
 *      |err| ≤ A01_LOST_DIR_ERR ⇒ 判不出方向 ⇒ 走保守路径（不转）
 *
 * ⚠️ 两种"不做旋转"的保守情况（直接沿用旧行为：直行 A01_LOST_STOP_MS 后刹停）：
 *      ① A01_LOST_TBAR_MS 内出现过「同一拍八路全黑」= 刚压过横向黑条
 *         （**终点 T 头 / 赛道标识都在这里，绝不能被当成脱线去转圈**）
 *      ② |err| 太小、判不出线在哪一侧
 *
 * A01_LOST_SEARCH = 0：完全退回旧行为（直行 A01_LOST_STOP_MS 后刹停，**不旋转**）。
 * 锁存释放：按按钮重新发车（Trace_StartRun）、串口 `FINRESET`、或断电。*/
#define A01_LOST_SEARCH         1    /* 1=停车→原地旋转找线；0=旧行为（直行后刹停）*/
#define A01_LOST_STOP_MS     200u    /* 保守路径：全白持续多久 ⇒ 刹停+锁存（0 = 关闭保护）*/
#define A01_LOST_HOLD_MS     150u    /* SEARCH=1：先直行多久（跨接缝）；设 0 = 立刻停车 */
#define A01_LOST_STOP_WAIT_MS 60u    /* SEARCH=1：刹停后等多久再开始旋转（消惯性）*/
#define A01_LOST_ROT_TIMEOUT_MS 3000u/* SEARCH=1：旋转找线上限（按差分估算 ≈ 一整圈）*/
#define A01_LOST_CENTER_HOLD_MS 100u /* SEARCH=1：线居中后保持多久才交回循迹 */
#define A01_LOST_DIR_ERR     0.5f    /* |err| > 它才认方向；否则算"判不出"（保守刹车）*/
#define A01_LOST_TBAR_MS     300u    /* 这么久内出现过"八路同黑"⇒ 认为刚过黑条，不找回 */

#define A01_SLOW_KP         38.00f   /* 慢速段 kp（取小一点会更柔和，但弯里欠修正）*/
#define A01_SLOW_KI          0.30f   /* 慢速段 ki */
#define A01_SLOW_KD          1.00f   /* 慢速段 kd */


/* ═══════════════════════ §5 A01 终点判定（与 A02 回线无关）═══════════════════
 * 判定顺序（都发生在 A01 循迹期间）：
 *   ① 发车后满 A01_FIN_MIN_MS（25s）才允许检测；之前看到"疑似终点"只打 $E 83
 *   ② A01_FIN_WINDOW_MS 窗口内，**八路分别都黑过一次**（不要求同时）
 *   ③ 之后出现**八路全白**（= 线已冲出阵列）⇒ 认定过终点
 *   ④ 再直行 A01_FIN_PUSH_MS（默认 100ms）、PWM = A01_FIN_PUSH_PWM（默认 70%）
 *      → 然后刹车 + 2 声长鸣 + 存 Flash
 * ⚠️ 与 A02 回线的"过线全白刹停"完全不同：那个在避障接管期间由 A02 判，
 *    这里的全白是 A01 循迹时判。 */
#define A01_FIN_MIN_MS      30000u  /* 发车后多久才允许判终点（替代原"里程 ≥800mm"）*/
#define A01_FIN_WINDOW_MS     300u  /* 窗口内八路都黑过一次就算"压到过终点线" */
#define A01_FIN_PUSH_MS       100u  /* 判到终点后再直行多久（ms）；0 = 立刻刹停 */
#define A01_FIN_PUSH_PWM       70   /* 冲刺段左右轮 PWM（%）*/


/* ═══════════════════════ §3 A02 避障：触发 · 判线 · 回线动作 ══════════════ */

/* ---- 3.1 总开关 ---- */
#define A02_ENABLE             1    /* 1=启用避障；0=完全关闭（只跑 A01 循迹，测速度用）*/
#define A02_TEST_FIXED_TRACK   0    /* 1=只测固定轨迹（A02 发车即独占、不判线）；0=正常融合 */
#define A02_LINE_STOP_TEST     1    /* 1=回线 v3（过线刹停→旋转摆正→交还 A01）；0=旧流程 */

/* ---- 3.2 障碍触发与预减速 ---- */
#define A02_PREP_MM          400    /* 新鲜读数 ≤400mm：A01 提前降速（只降速、不接管）*/
#define A02_PREP_BASE_PWM     30    /* 预备时的临时巡航 PWM */
#define A02_PREP_HOLD_MS    1500u   /* 预备后连续这么久【没有再看到】≤PREP_MM 的读数 ⇒ 撤销预备、把速度还回去
                                     * （没有它的话，一次"看见但没触发"会让 base 永久停在 PREP_BASE_PWM）*/
#define A02_TRIGGER_MM       310    /* 新鲜读数 ≤310mm：A02 接管 */
#define A02_CENTER_SPLIT_MM   30    /* 两侧都近且差 <30mm ⇒ 当作正中障碍，默认向左避 */
#define A02_COOLDOWN_MS      800u   /* 交还 A01 后多久内不再触发避障（原 3000 太长会漏障）*/
#define A02_US_FRESH_MS      500u   /* 超过 500ms 的旧测距不用（容忍一次失败重试）*/

/* ---- 3.3 判线（判「线回来了」）---- */
#define A02_LEAVE_WHITE_MS   200u   /* 连续全白 ≥200ms = 已离开线（之后才认回线）*/
#define A02_LINE_IGNORE_MS   250u   /* 接管后前 250ms 即使见黑也不判 */
#define A02_LINE_WINDOW_MS   300u   /* 判线滑动窗口 */
#define A02_LINE_GAP_MS       50u   /* 采样中断超过这么久就清零重累计 */
#define A02_LINE_BLACK_COUNT   5u   /* 窗口内要有 5 个【不同通道】黑过 */

/* ---- 3.4 回线 v3 动作（A02_LINE_STOP_TEST=1 生效）----
 * 流程：过线(八路全白)刹停 → 等 A02_STOP_WAIT_MS → 原地旋转 →
 *       判「第 3~6 路里有**两路相邻**全黑 且 第 1/2/7/8 路全白」⇒ 线已居中 ⇒ 刹停；
 *       并连续保持 A02_CENTER_HOLD_MS 才认（中途丢了会回去继续旋转）；
 *       保持成功后等 A02_HANDOFF_WAIT_MS → 交还 A01（走 §2 的慢速窗口）。
 * ⚠️ 旋转 A02_ROT_TIMEOUT_MS 内仍不居中 ⇒ **锁存停车、不交还 A01**
 *    （否则 A01 接过去看到全白会按"丢线找线"直行冲出去）。*/
#define A02_STOP_WAIT_MS     100u   /* 过线刹停 → 开始旋转，等待多久 */
#define A02_ROT_L_PWM           8   /* 旋转：左轮 PWM（从右侧回线时）*/
#define A02_ROT_R_PWM         -20   /* 旋转：右轮 PWM（负数=倒转）*/
#define A02_ROT_TIMEOUT_MS  3000u   /* 旋转找中的兜底超时 */
#define A02_HANDOFF_WAIT_MS  0u   /* 摆正刹停 → 交还 A01，等待多久 */
#define A02_CENTER_HOLD_MS   100u   /* 「居中」要连续保持这么久才敢交还 A01 */
#define A02_MID_MASK        0x3Cu   /* 「中间四路」：第 3~6 路（bit2..bit5）*/
#define A02_OUT_MASK        0xC3u   /* 「必须全白」：第 1/2/7/8 路 */

/* ---- 3.5 轨迹测试模式（A02_TEST_FIXED_TRACK=1 生效）---- */
#define A02_TEST_MS         2000u   /* 发车后先直行多久 */
#define A02_TEST_PWM          20    /* 直行时左右 PWM（相同）*/

/* ---- 3.6 旧回线流程（仅 A02_LINE_STOP_TEST=0 生效）---- */
#define A02_ALIGN_A_PWM      -40    /* 摆正：内侧轮（左避时=左轮）*/
#define A02_ALIGN_B_PWM       10    /* 摆正：外侧轮 */
#define A02_ALIGN_MS         300u   /* 摆正时长（含末段降速）*/
#define A02_ALIGN_STEP         5    /* 摆正段每 10ms 最多变 5% */
#define A02_CAPTURE_A_PWM      9    /* 慢弧搜线：左轮（左避时）*/
#define A02_CAPTURE_B_PWM     15    /* 慢弧搜线：右轮 */
#define A02_SEARCH_MS        4500u  /* 慢弧搜线最长时间 */
#define A02_CAPTURE_AFTER_BLACK_MS 500u  /* 见黑后保持慢弧多久再交还 A01 */

/* ---- 3.7 内部常量（一般不用动）---- */
#define A02_PWM_STEP           2    /* 每 10ms 单轮最多变 2%（防突变打滑）*/

/* ═══════════════════════ §4 A02 固定轨迹：推出 + 回摆 ═════════════════════
 * 只写「向左避」（右侧有障碍）这一套；向右避时左右轮自动对调（镜像）。
 * 以左避为例：① 先向左推出（L0/R48）→ ② 再向右回摆（L48→45 / R25→40）。*/
#define AV_OUT_MS            400u   /* ① 推出段时长 */
#define AV_OUT_L_PWM            0   /*    左轮 */
#define AV_OUT_R_PWM           50   /*    右轮（右快 ⇒ 向左推出）*/

#define AV_RETURN_MS        4500u   /* ② 回摆段时长（也是"回线"的时间上限）*/
#define AV_RET_START_L_PWM     48   /*    回摆起点：左轮 */
#define AV_RET_START_R_PWM     22   /*              右轮（左快 ⇒ 向右回摆）*/
#define AV_RET_END_L_PWM       45   /*    回摆终点：左轮 */
#define AV_RET_END_R_PWM       40   /*              右轮（差速收到 5，接近直行）*/

#define AV_PWM_STEP             2   /* 每 10ms 单轮最多变 2% */

/* ═══════════════════════ 编译期守门（别删）═══════════════════════════════ */
#if (A01_BASE_MAX < 1) || (A01_BASE_MAX > 100)
#error "A01_BASE_MAX must be 1..100"
#endif
#if (A01_SLOW_PWM0 != 0) && (A01_SLOW_PWM0 < 8)
#error "A01_SLOW_PWM0 must be 0 (off) or >=8; 1..7 is below the motor dead zone"
#endif
#if (A01_SLOW_PWM1 != 0) && (A01_SLOW_PWM1 < 8)
#error "A01_SLOW_PWM1 must be 0 (off) or >=8; 1..7 is below the motor dead zone"
#endif
#if A02_PREP_MM <= A02_TRIGGER_MM
#error "A02_PREP_MM must exceed A02_TRIGGER_MM"
#endif
#if A01_LOST_SEARCH
#  if (A01_LOST_HOLD_MS >= A01_LOST_ROT_TIMEOUT_MS)
#    error "A01_LOST_HOLD_MS must be < A01_LOST_ROT_TIMEOUT_MS"
#  endif
   /* 注：A01_LOST_DIR_ERR 是浮点，**不能**放进 #if（预处理器只认整数），
      取值只能在 (0, 3.5] 之间，改的时候自己看一眼。*/
#endif
#if (A02_MID_MASK & A02_OUT_MASK) != 0
#  error "A02_MID_MASK / A02_OUT_MASK overlap"
#endif

#endif /* APP_TUNING_H */
