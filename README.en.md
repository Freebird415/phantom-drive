<h1 align="center">Phantom Drive</h1>

<p align="center"><b>An autonomous line-following robot car with dynamic obstacle avoidance</b><br>
Three-wheel differential drive · fully autonomous racing · built from scratch</p>

<p align="center">
  <img src="docs/images/car_pcb.jpg" width="620" alt="Phantom Drive PCB">
</p>

<p align="center">
  <a href="README.md">中文</a> ｜ <a href="README.en.md"><b>English</b></a>
</p>

<p align="center">
  <img alt="award" src="https://img.shields.io/badge/2026_Engineering_Training-2nd_Prize-dc143c?style=flat-square">
  <img alt="mcu" src="https://img.shields.io/badge/STM32F103C8T6-64MHz-blue?style=flat-square">
  <img alt="toolchain" src="https://img.shields.io/badge/PlatformIO-arm--none--eabi-green?style=flat-square">
  <img alt="language" src="https://img.shields.io/badge/C-C11-lightgrey?style=flat-square">
</p>

---

## What is this

A three-wheel robot car built **entirely from scratch**: STM32F103C8T6, a custom PCB, an 8-channel grayscale sensor array and two ultrasonic rangefinders. It follows a black line around a 6.2 m × 2 m track, avoids randomly placed obstacles, and stops by itself at the finish line.

Course project for *Engineering Innovation Training* at South China University of Technology — **2nd prize**.

## Highlights

| | |
|---|---|
| **Line following** | 8-channel grayscale → position error + PID + steering slew limit; curvature-aware speed reduction (error / edge channels / gyro yaw rate) |
| **Obstacle avoidance** | Dual HC-SR04: slow down at 400 mm, take over at 310 mm; fixed two-phase detour, exit as soon as the line reappears |
| **Line re-acquisition** | Crossing the line → full brake → rotate in place → line centred → hold → hand back to the follower (through a slow-start window) |
| **Lost-line recovery** | Unexpected all-white while following: decide the side → stop → rotate to search → centre and resume; vetoed right after a crossbar / finish T |
| **Finish detection** | 30 s arming gate + all 8 channels swept + all-white ⇒ 100 ms sprint at 70 % ⇒ brake + two long beeps + snapshot |

## Hardware

STM32F103C8T6 (64 MHz, HSI) · 8-channel grayscale sensor (I²C1) · HC-SR04 ×2 · MPU6050 (I²C2) · TB6612FNG · JGB37-520 gear motors ×2 + caster · single 12 V Li-ion pack · custom PCB v2.1 (EasyEDA) with a laser-cut and 3D-printed chassis.

## Quick start

```bash
git clone https://github.com/Freebird415/phantom-drive.git
cd phantom-drive

pio run -d firmware                  # build
pio run -d firmware -t upload        # flash (ST-Link)
python tools/flash.py                # build + flash with retries and diagnostics
python tools/btview.py               # wireless log UI (live data while driving)
```

All tunables live in a single place: [`firmware/src/app/tuning.h`](firmware/src/app/tuning.h).

## Repository layout

| Folder | Contents |
|---|---|
| [`firmware/`](firmware/) | Firmware (PlatformIO) · driver API reference · per-version algorithm archive |
| [`hardware/`](hardware/) | EasyEDA project · electrical design (netlist, wiring, self-check script) · BOM |
| [`mech/`](mech/) | Mechanical design and fabrication evidence (STEP / STL / DXF / photos) |
| [`tools/`](tools/) | Host tools: wireless logging, telemetry parsing, on-board snapshot reader, trajectory reconstruction, flashing |
| [`docs/`](docs/) | Technical conventions · telemetry protocol · pin assignment (Chinese) |
| [`项目记录/`](项目记录/) | Development log · lessons learned · decision register · version index (Chinese) |
| [`logs/`](logs/) | Per-run summaries (one 3–6 KB markdown each) |
| [`参考资料/`](参考资料/) | Official competition rules · device manuals · hardware photos |

## Design notes

- **Layered**: `app → drv → bsp`, one-way only; one file per device driver.
- **Observable**: 100 Hz `$T` telemetry (channels, error, PWM, state) + `$E` event codes + an on-board snapshot (1200 frames in RAM, flushed to flash on stop, survives power loss) + live BLE logs.
- **Safe by default**: drive enable is off after flashing (`TRACE 0` / `AVOID 0` = compute but never spin the motors); the car never drives if the grayscale bus is offline; firmware size is gate-checked at build time (< 54 KB).
- **Single source of truth for parameters**: everything tunable lives in `tuning.h`.

## Team

Peiyang Li (lead: repo & workflow, telemetry tooling, drivers) · Zhenglin Wang (components, PCB & electrical) · Yaqi Xiong (line following) · Xincheng Li (mechanical design & fabrication) · Yekai Bi (obstacle avoidance, presentation)

## Citation

Course project of *Engineering Innovation Training*, South China University of Technology. Please credit the source when reusing.
