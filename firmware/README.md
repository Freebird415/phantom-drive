# firmware —— STM32 固件

## 现在有什么

- **遥测链路 demo**（P01）：`platformio.ini` + `src/main.c`
  - 100 Hz 通过串口打印遥测帧，支持在线调参命令
  - 目的：打通「固件 → 电脑落盘 → 画图 → 改参数重烧」闭环
- 车上还没有传感器/电机，`$T` 帧里是**模拟数据**（三角波）；
  接上真实模块后**只需替换 `main()` 里标注的那一段**，帧格式不变

## 怎么用（在仓库根目录执行）

```bash
python tools/run.py --note "第一次测试" --seconds 20        # 编译+烧录+采集+出图
python tools/run.py --note "..." --seconds 20 --no-flash   # 只采集
pio run -d firmware -t upload                              # 只编译+烧录
cd firmware && pio device monitor                          # 直接看串口（Ctrl+C 退出）
```

## 硬件接线

| 用途 | 接线 |
|---|---|
| 烧录（ST-Link） | SWDIO→PA13，SWCLK→PA14，GND→GND，3V3→3V3 |
| 串口（USB-TTL） | TTL TXD→**PA10**（RX），TTL RXD→**PA9**（TX），GND→GND（TTL 的电源脚不要接） |

时钟：HSI 8 MHz → PLL → 64 MHz（不依赖外部晶振，开发板差异不影响）

## 约定

- 遥测帧格式见 `docs/遥测协议.md`——**改动需双方确认**（C2、C3 按它写代码）
- `main` 分支保持可编译；接口冻结后只加功能不改接口
- 相关任务：P01 · P02 · A01 · A02
