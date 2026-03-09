# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Split-flap display controller firmware (project name: `split_flap_v11`) for **ESP32** using **ESP-IDF v5.4.1**. Drives 4 stepper motors (28BYJ-48 via ULN2003) with Hall-effect sensor feedback to display 4 digits (0-9) on physical split-flap drums. Each drum has 10 flaps and 10 magnets. Currently on **v12**.

## Quick Start (for new developers)

### Prerequisites
- macOS with Homebrew (or Linux)
- ESP32 board connected via USB

### First Time (clone + run)
```bash
git clone <repo-url> "flip flap clock"
cd "flip flap clock"
./start              # Installs ESP-IDF, builds, flashes, opens monitor
```

`./start` automatically:
1. Checks git, python3, cmake, screen are installed
2. Installs ESP-IDF v5.4.1 if not present (~5 min first time)
3. Detects ESP32 serial port
4. Builds + flashes firmware (first time only)
5. Opens serial monitor

### Daily Development
```bash
./flash              # Build + auto-detect ESP32 + flash
./start              # Open serial monitor (or re-setup if needed)
```

**Inside the serial monitor**, type commands (see reference below). Exit: `Ctrl+A` then `K`, then `Y`.

### Other Scripts
```bash
./dev.sh             # Build + Flash + Monitor (all in one)
./dev.sh build       # Build only
./dev.sh mon         # Monitor only
./open.sh            # Just open serial monitor (no build)
```

## Serial Commands Reference

After flashing, open the serial monitor (115200 baud). Available commands:

| Command | Example | What it does |
|---------|---------|--------------|
| **4 digits** | `1234` | Display those digits on the flaps |
| **m**N | `m0` | Test 1 flap on motor N (0-3) |
| **m**N count | `m2 3` | Test 3 flaps on motor N |
| **s**N steps | `s0 200` | Raw step motor N by exact steps |
| **all** steps | `all 4000` | Step all 4 motors simultaneously |
| **spf**N val | `spf0 210` | Set steps-per-flap for motor N (saved) |
| **speed** val | `speed 1500` | Set step delay in microseconds |
| **cal** | `cal` | Recalibrate all drums using Hall sensors |
| **st** | `st` | Show Hall sensor status for all drums |
| **reset** | `reset` | Clear saved data and restart |

### Verification Prompt
After every digit command, the system asks **"Showing?"**. Type what the drums actually show (e.g., `0030`), and it will:
- Report which drums are off and by how much
- **Auto-correct SPF** for each drum based on the error
- Update `cur[]` to actual position so next move is accurate
- Save corrected values to flash (NVS)

Press **Enter** to skip verification.

## Architecture

Single-file firmware in `main/main.c`. Two FreeRTOS tasks communicate via a queue:

- **`motor_task`** (priority 10) -- Owns all stepper motor operations. Receives `cmd_t` messages from the queue. Sets `motor_busy` flag while working.
- **`input_task`** (priority 3) -- Reads UART0 serial input, parses commands, posts `cmd_t` to the queue. Waits for `motor_busy` to clear before accepting next command.

### Motor Control Flow

1. **On boot**: Load position + SPF from NVS flash. If no saved data, ask user what's currently showing.
2. **Move**: Calculate flaps needed, multiply by SPF to get steps, execute with trapezoidal ramp profile.
3. **Verify**: After each move, optionally verify actual position and auto-correct SPF.
4. **Save**: Position and SPF saved to NVS after every move.

### Key Data Structures

```c
cmd_t {
    int  digits[4];     // target digits (for display command)
    bool recal;         // true = recalibrate all
    bool manual_step;   // true = raw step command (s0 200)
    bool all_step;      // true = step all motors (all 4000)
    int  step_motor;    // which motor (0-3)
    int  step_count;    // how many steps
}
```

## GPIO Pin Mapping

```
Motor 0 (M1):  IN1=GPIO18  IN2=GPIO19  IN3=GPIO21  IN4=GPIO22
Motor 1 (M2):  IN1=GPIO23  IN2=GPIO25  IN3=GPIO26  IN4=GPIO27
Motor 2 (M3):  IN1=GPIO32  IN2=GPIO33  IN3=GPIO4   IN4=GPIO5
Motor 3 (M4):  IN1=GPIO12  IN2=GPIO13  IN3=GPIO14  IN4=GPIO15

Hall Sensors:   D0=GPIO34   D1=GPIO39   D2=GPIO36   D3=GPIO16
```

**Important GPIO notes:**
- GPIOs 34, 36, 39 are **input-only** on ESP32 -- no internal pull-up. Need **external 10k pull-up to 3.3V**.
- GPIO 16 has internal pull-up (configured in code).
- GPIO 12 is a **strapping pin** -- must be LOW at boot or ESP32 won't start.

## Tuning Constants

All in `main/main.c` top section. SPF and RUN_US can be changed at runtime via serial commands.

| Constant | Default | Range | Purpose |
|----------|---------|-------|---------|
| `RUN_US[4]` | 2000 | 1000-5000 | Cruise step delay in microseconds (lower = faster) |
| `CAL_US[4]` | 3000 | 2000-5000 | Calibration speed (slower for reliability) |
| `RAMP_START_US` | 5000 | -- | Initial ramp delay (slow start) |
| `RAMP_STEPS` | 80 | -- | How many steps to ramp up/down |
| `SPF[4]` | 204,204,224,204 | 50-500 | Steps per flap (CRITICAL -- must be measured per drum) |
| `COOLDOWN_MS` | 50 | -- | Gap between consecutive drum moves |
| `MOTOR_IDLE_TIMEOUT_MS` | 5000 | -- | De-energize motors after this idle time |

### How to Tune SPF (most important calibration)

1. Type `m0` -- motor 0 should move exactly 1 flap
2. If it overshoots (moved 2 flaps): `spf0 100` (decrease SPF)
3. If it undershoots (moved half a flap): `spf0 300` (increase SPF)
4. Repeat `m0` until it lands exactly 1 flap
5. Do the same for `m1`, `m2`, `m3`
6. SPF values are auto-saved to flash

Or use the auto-correct: type `1111`, then when asked "Showing?", type what you actually see. SPF adjusts automatically.

### How to Tune Speed

1. Type `speed 2000` (default)
2. Try `speed 1500` -- if motors work, go faster
3. Try `speed 1200` -- if motors skip/stall, go back to 1500
4. Find the sweet spot per your hardware

## NVS Storage

Position (`cur[4]`) and SPF values are persisted in ESP32's NVS (Non-Volatile Storage) flash. Survives power cycles -- no need to re-enter position on every boot. Type `reset` to clear.

## Hardware Notes

- **Stepper motors**: 28BYJ-48 driven by ULN2003 boards
- **Step sequence**: Two-phase full-step (2 coils at a time for max torque)
- **Motor direction**: Reversed in firmware (`step_idx + 3` instead of `+1`)
- **Hall sensors**: Active LOW with magnets on drum. `hall_active()` = `gpio_get_level() == 0`
- **If a motor rotates wrong way**: swap IN1<->IN4 and IN2<->IN3 in the `MP` array for that motor
- **If digits land wrong**: use the auto-correct verification or manually adjust SPF
- **Target MCU**: ESP32 (not S2/S3/C3)

## IDF Component Dependencies

Defined in `main/CMakeLists.txt`: `esp_driver_gpio`, `esp_driver_uart`, `esp_timer`, `esp_system`, `nvs_flash`.

## File Structure

```
flip flap clock/
  main/
    main.c              # All firmware code (single file)
    CMakeLists.txt      # Component dependencies
  start                 # First time setup + serial monitor (run this first)
  flash                 # Build + auto-detect + flash ESP32
  dev.sh                # Build + Flash + Monitor (all in one)
  open.sh               # Open serial monitor only
  build.sh              # Legacy build script
  sdkconfig             # ESP-IDF configuration
  sdkconfig.defaults    # Default config overrides
  CMakeLists.txt        # Top-level project CMake
  CLAUDE.md             # This file
```
