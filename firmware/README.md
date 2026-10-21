# firmware —— STM32 固件

PlatformIO 工程（`platformio.ini` + `src/`），目标板 `bluepill_f103c8`。

## 目录

```
firmware/
├── platformio.ini        工程配置（含 check_size.py 构建挂钩）
├── check_size.py         编译后体积守门：不得压进快照保留区
├── src/
│   ├── main.c            初始化 + 主循环 + A01/A02 仲裁
│   ├── bsp/              板级（时钟 / µs 计时 / 串口 / 指示灯）
│   ├── drv/              器件驱动（一个器件一个文件）
│   └── app/              应用层
│        ├── tuning.h     ★ 全部可调参数（唯一出处）
│        ├── app_trace.c  A01 循迹 + 脱线兜底 + 终点精停
│        ├── app_avoid.c  A02 避障与回线
│        ├── avoid_path.c A02 固定轨迹（左右镜像共用）
│        ├── app_telemetry.c  遥测帧 + 命令解析
│        └── app_log.c    板载快照（黑匣子）
├── 文档/                 驱动接口说明
├── 测试/                 上位机回归/诊断脚本（不参与编译）
└── 算法快照/             各版本源码归档 + 说明.md
```

## 约定

| 项 | 要求 |
|---|---|
| 引脚 | 以 `docs/引脚分配表.md`（v3.0，已冻结）为准 |
| 遥测帧 | 见 `docs/遥测协议.md`；新字段只能加在行尾 |
| 体积 | 固件必须 < 54KB（快照区占最后 10KB）；`check_size.py` 编译后自动拦截 |
| 分层 | `app → drv → bsp`，不准反向 |
| 参数 | 只改 `src/app/tuning.h`，其它文件不再重复定义数值 |
| 提交 | `main` 始终保持可编译、零 warning |

## 编译 / 烧录

```bash
pio run -d firmware                    # 编译
pio run -d firmware -t upload          # 编译 + ST-Link 烧录
python tools/flash.py                  # 一键：编译 + 多策略重试 + 中文诊断
pio run -d firmware -e uart_isp -t upload --upload-port COMx   # UART(ISP) 备用通道（BOOT0=1）
```

## 看数据

```bash
python tools/btview.py            # 无线日志（网页界面，跑车时用）
python tools/snapshot.py          # 板载快照 → CSV + 曲线（100Hz 全精度）
python tools/track.py logs/<目录>  # 用左右轮增量重建轨迹
```

串口直连（115200）：`$T` 100Hz 遥测、`$U` 测距、`$E` 事件、`$P` 参数；命令表见
[`文档/驱动接口说明.md`](文档/驱动接口说明.md)。

## 固件行为（最终版摘要）

- **A01 循迹**：8 路灰度 → `err = 平均黑位 − 3.5` → PID（kp35/ki0.3/kd0.9）+ 舵量斜坡 + 抗饱和积分 → 差速；巡航 55%，上限 90。
- **曲率风险降速**：`|err|≥0.5` / 最外两路黑 / 陀螺≥55°/s ⇒ ×3/4；`|err|≥2` / 陀螺≥100°/s ⇒ ×11/20；8 帧迟滞。
- **A02 接管**：400mm 预减速（超过 1.5s 无近障自动恢复）、310mm 接管、两段轨迹、回线 v3、3s 冷却。
- **脱线兜底**：`A01_LOST_SEARCH=1` 时旋转找回；`=0` 时直行 200ms 后刹停。
- **终点**：30s 时间闸 + 八路扫过 + 全白 ⇒ 冲刺 100ms@70% ⇒ 刹停。

完整参数表与时序见 [`算法快照/v12_2026-10-21_比赛最终版/说明.md`](算法快照/v12_2026-10-21_比赛最终版/说明.md)。
