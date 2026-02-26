# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Split-flap display controller firmware (project name: `split_flap_v11`) for **ESP32** using **ESP-IDF v5.4.1**. Drives 4 stepper motors with Hall-effect sensor feedback to display 4 digits (0-9) on physical split-flap drums. Currently on **v12** (production rewrite with safety features).

## Build & Flash Commands

Requires ESP-IDF toolchain. Source the IDF environment first (`source $IDF_PATH/export.sh` or use the VS Code ESP-IDF extension).

```bash
idf.py build              # Build firmware
idf.py flash              # Flash to ESP32 (default port)
idf.py flash -p /dev/...  # Flash to specific serial port
idf.py monitor            # Serial monitor at 115200 baud
idf.py flash monitor      # Flash then immediately monitor
idf.py menuconfig         # SDK configuration editor
idf.py fullclean          # Clean build directory completely
```

A Dev Container (`.devcontainer/`) is available with the `espressif/idf` Docker image for reproducible builds.

## Architecture

Single-file firmware in `main/main.c`. Two FreeRTOS tasks communicate via a queue:

- **`motor_task`** (priority 10) — Owns all stepper motor operations. Receives `cmd_t` messages from the queue. Handles calibration and digit movement. Registered with task watchdog.
- **`input_task`** (priority 3) — Reads UART0 serial input, parses commands (`4 digits` or `cal`), posts `cmd_t` to the queue. Waits for calibration to complete before accepting input.

### Motor Control Flow

1. **Calibration**: `clear_sensor()` -> rotate until Hall pulse -> stop on magnet (position = 0)
2. **Move**: `clear_sensor()` (step past magnet from previous stop) -> count Hall pulses until target -> stop on target magnet
3. **Ramp**: Trapezoidal speed profile — starts at `RAMP_START_US`, accelerates to `RUN_US` cruise, decelerates at end

### Safety Features (v12)

- **Task watchdog** (TWDT) on motor_task with periodic feeding during long operations
- **Stall detection**: aborts move if no Hall pulse for 2x expected step interval
- **Auto-recalibrate**: after `MAX_CONSECUTIVE_ERRS` (2) failures per drum
- **Motor idle timeout**: de-energizes coils after `MOTOR_IDLE_TIMEOUT_MS` (5s)
- **Cooldown**: `COOLDOWN_MS` (200ms) gap between consecutive drum moves
- `clear_sensor()` returns bool — hardware fault detection on sensor clear failure

### Key Tuning Constants

All in `main/main.c` top section. Currently set **slow for testing** — decrease `RUN_US` toward 1500 for production speed.

| Constant | Current (test) | Production | Purpose |
|----------|---------------|------------|---------|
| `RUN_US[4]` | 3000 | ~1500 | Cruise step delay (μs) |
| `CAL_US[4]` | 4000 | ~3000 | Calibration step delay (μs) |
| `RAMP_START_US` | 7000 | ~5000 | Initial ramp delay (μs) |
| `RAMP_STEPS` | 100 | ~80 | Ramp length in steps |
| `SPF[4]` | 339,339,324,313 | — | Steps per flap (measured) |
| `DEBOUNCE_US` | 80000 | — | Hall debounce (80ms) |
| `STEP_PAST` | 15 | — | Steps to clear magnet (< SPF/2) |

### GPIO Pin Mapping

- Motors: `MP[4][4]` — 4 motors x 4 stepper pins (ULN2003/28BYJ-48 style)
- Hall sensors: `HP[4]` — GPIOs 5, 18, 17, 34 (active LOW with internal pull-up)

## Hardware Notes

- Stepper sequence: two-phase full-step (max torque) — `SEQ[4][4]`
- Hall sensors are active LOW — `hall_active()` checks `gpio_get_level() == 0`
- If a motor rotates backward: swap IN1<->IN4 and IN2<->IN3 in the `MP` array
- If digits land wrong: adjust that drum's `SPF` value (+/-20 per flap off)
- Target: ESP32 (not S2/S3/C3). OpenOCD config uses `esp32_devkitj_v1.cfg`

## IDF Component Dependencies

Defined in `main/CMakeLists.txt`: `esp_driver_gpio`, `esp_driver_uart`, `esp_timer`, `esp_system`. Note: `esp_task_wdt.h` is provided by `esp_system`, not a separate component.
