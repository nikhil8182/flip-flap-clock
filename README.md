# Split-Flap Clock

A retro mechanical split-flap display that shows 4 digits — like the departure boards in old airports and train stations. Powered by an ESP32 microcontroller driving 4 stepper motors with Hall-effect sensor feedback.

```
  +-------+  +-------+  +-------+  +-------+
  |       |  |       |  |       |  |       |
  |   1   |  |   2   |  |   3   |  |   4   |
  |       |  |       |  |       |  |       |
  +---+---+  +---+---+  +---+---+  +---+---+
      |          |          |          |
   Motor 0    Motor 1    Motor 2    Motor 3
   (28BYJ)    (28BYJ)    (28BYJ)    (28BYJ)
      |          |          |          |
  ULN2003    ULN2003    ULN2003    ULN2003
      \          |          |          /
       +-------- ESP32 DevKit -------+
```

Each drum has **10 flaps** (digits 0-9) and **10 magnets** (one per digit). A Hall-effect sensor on each drum detects magnets for position tracking.

---

## Hardware Required

| Component | Qty | Notes |
|-----------|-----|-------|
| ESP32 DevKit V1 | 1 | Must be ESP32 (not S2/S3/C3) |
| 28BYJ-48 Stepper Motor | 4 | 5V unipolar stepper |
| ULN2003 Driver Board | 4 | Comes with 28BYJ-48 usually |
| Hall Effect Sensor (A3144 / OH49E) | 4 | Active LOW output |
| Small Magnets | 40 | 10 per drum, one per digit |
| 10k Resistor | 3 | Pull-up for GPIO 34, 36, 39 |
| 5V Power Supply | 1 | At least 1A for 4 motors |
| Split-flap drum assemblies | 4 | 3D printed or custom built |

---

## Wiring Diagram

```
                          ESP32 DevKit
                    +---------------------+
                    |                     |
    Motor 0 (M1)   |  GPIO 18 --- IN1    |   Hall Sensor 0
    ULN2003 Board   |  GPIO 19 --- IN2    |   GPIO 34 --- OUT
                    |  GPIO 21 --- IN3    |   (needs 10k pull-up to 3.3V)
                    |  GPIO 22 --- IN4    |
                    |                     |
    Motor 1 (M2)   |  GPIO 23 --- IN1    |   Hall Sensor 1
    ULN2003 Board   |  GPIO 25 --- IN2    |   GPIO 39 --- OUT
                    |  GPIO 26 --- IN3    |   (needs 10k pull-up to 3.3V)
                    |  GPIO 27 --- IN4    |
                    |                     |
    Motor 2 (M3)   |  GPIO 32 --- IN1    |   Hall Sensor 2
    ULN2003 Board   |  GPIO 33 --- IN2    |   GPIO 36 --- OUT
                    |  GPIO  4 --- IN3    |   (needs 10k pull-up to 3.3V)
                    |  GPIO  5 --- IN4    |
                    |                     |
    Motor 3 (M4)   |  GPIO 12 --- IN1    |   Hall Sensor 3
    ULN2003 Board   |  GPIO 13 --- IN2    |   GPIO 16 --- OUT
                    |  GPIO 14 --- IN3    |   (internal pull-up, no resistor needed)
                    |  GPIO 15 --- IN4    |
                    |                     |
                    +---------------------+
                      |       |       |
                     GND    3.3V     5V
                      |       |       |
                     All    Hall    Motor
                    grounds sensors  power
```

### GPIO Pin Table

| Function | GPIO | Direction | Notes |
|----------|------|-----------|-------|
| Motor 0 IN1 | 18 | Output | |
| Motor 0 IN2 | 19 | Output | |
| Motor 0 IN3 | 21 | Output | |
| Motor 0 IN4 | 22 | Output | |
| Motor 1 IN1 | 23 | Output | |
| Motor 1 IN2 | 25 | Output | |
| Motor 1 IN3 | 26 | Output | |
| Motor 1 IN4 | 27 | Output | |
| Motor 2 IN1 | 32 | Output | |
| Motor 2 IN2 | 33 | Output | |
| Motor 2 IN3 | 4 | Output | |
| Motor 2 IN4 | 5 | Output | |
| Motor 3 IN1 | 12 | Output | Strapping pin - must be LOW at boot |
| Motor 3 IN2 | 13 | Output | |
| Motor 3 IN3 | 14 | Output | |
| Motor 3 IN4 | 15 | Output | |
| Hall 0 | 34 | Input | Input-only, needs external 10k pull-up |
| Hall 1 | 39 | Input | Input-only, needs external 10k pull-up |
| Hall 2 | 36 | Input | Input-only, needs external 10k pull-up |
| Hall 3 | 16 | Input | Has internal pull-up |

### Hall Sensor Wiring

```
    3.3V
     |
    [10k]  <-- pull-up resistor (required for GPIO 34, 36, 39)
     |
     +-------> GPIO pin (ESP32)
     |
   [Hall]  <-- Hall effect sensor output
     |
    GND

    When magnet is near:  GPIO reads LOW  (0)
    When magnet is away:  GPIO reads HIGH (1)
```

---

## Getting Started

### macOS

Open Terminal and paste these 4 lines:

```bash
git clone https://github.com/nikhil8182/flip-flap-clock.git
cd flip-flap-clock
chmod +x start flash dev.sh open.sh
./start
```

### Linux (Ubuntu/Debian)

Open Terminal and paste:

```bash
sudo apt update && sudo apt install -y git python3 python3-venv cmake screen
git clone https://github.com/nikhil8182/flip-flap-clock.git
cd flip-flap-clock
chmod +x start flash dev.sh open.sh
./start
```

### Windows

**Option A: WSL (Recommended)**

1. Open PowerShell **as Administrator** and run:
```powershell
wsl --install
```
2. Restart your computer
3. Open **Ubuntu** from Start Menu and paste:
```bash
sudo apt update && sudo apt install -y git python3 python3-venv cmake screen
git clone https://github.com/nikhil8182/flip-flap-clock.git
cd flip-flap-clock
chmod +x start flash dev.sh open.sh
./start
```

**Option B: ESP-IDF Windows Installer (no WSL)**

1. Download and install from: https://dl.espressif.com/dl/esp-idf/
2. Pick version **5.4.1** during install
3. Open **ESP-IDF Command Prompt** from Start Menu
4. Run:
```cmd
git clone https://github.com/nikhil8182/flip-flap-clock.git
cd flip-flap-clock
idf.py build
idf.py flash
idf.py monitor
```

### What `./start` Does

You don't need to install anything manually. The start script handles everything:

1. Checks dependencies (git, python3, cmake, screen) -- installs if missing
2. Downloads and installs ESP-IDF v5.4.1 (~5 min, first time only)
3. Detects your ESP32 USB port automatically
4. Builds the firmware
5. Flashes it to the ESP32
6. Opens the serial monitor

### First Boot

After flashing, the serial monitor will show:

```
+-----------------------------------+
|  What number is showing now?      |
|  Type 4 digits + Enter            |
+-----------------------------------+
>
```

Look at the physical flaps and type what you see (e.g., `0000`). Press Enter. Now you can send commands.

---

## Serial Commands

Connect to the serial monitor at **115200 baud**. Type commands and press Enter.

### Display Digits
```
> 1234          Set display to 1234
> 0000          Set display to 0000
> 9999          Set display to 9999
```

### Test & Calibrate Motors

| Command | Example | What it does |
|---------|---------|--------------|
| `m`N | `m0` | Move motor N by exactly 1 flap |
| `m`N count | `m2 3` | Move motor N by 3 flaps |
| `s`N steps | `s0 200` | Move motor N by raw steps |
| `all` steps | `all 4000` | Move all 4 motors simultaneously |
| `cal` | `cal` | Recalibrate using Hall sensors |
| `st` | `st` | Show Hall sensor status |

### Tune SPF (Steps Per Flap)

SPF is the most important calibration value. Each drum needs its own SPF because of mechanical differences.

```
> spf0 210      Set motor 0 SPF to 210 (auto-saved to flash)
> spf1 195      Set motor 1 SPF to 195
> speed 1500    Set motor speed to 1500 microseconds/step
> reset         Clear all saved data and restart fresh
```

### Auto-Correct Workflow

After every digit command, the system asks what's actually showing:

```
> 5555
  ... motors move ...
Showing? (Enter to skip): 5654
  D0: OK
  D1: wanted 5 got 6 (off +1, moved 6/5 flaps) -> SPF 200=>166
  D2: OK
  D3: wanted 5 got 4 (off -1, moved 4/5 flaps) -> SPF 200=>250
  SPF updated & saved.
```

The system automatically:
1. Calculates how many flaps each motor actually moved
2. Adjusts SPF based on the error
3. Updates the current position to match reality
4. Saves everything to flash memory

Press **Enter** to skip verification when you don't need it.

### SPF Tuning Tips

1. **Use big moves** for calibration (e.g., `0000` then `5555`) — more flaps = more accurate correction
2. **Do 3-4 rounds** — SPF converges quickly after a few corrections
3. **Fine-tune with `m` command** — `m0` tests exactly 1 flap on motor 0
4. SPF typically ranges from **150 to 300** for 28BYJ-48 motors
5. All SPF values are **automatically saved** to flash and survive power cycles

---

## Development Workflow

### Edit-Build-Flash-Test Cycle

```bash
# 1. Edit main/main.c in your editor

# 2. Build and flash
./flash

# 3. Open serial monitor to test
./start

# 4. Repeat
```

### Available Scripts

| Script | What it does |
|--------|--------------|
| `./start` | First-time setup + open serial monitor |
| `./flash` | Build firmware + detect ESP32 + flash |
| `./dev.sh` | Build + Flash + Monitor (all in one) |
| `./open.sh` | Just open serial monitor |

### Serial Monitor Exit

- **screen** (used by ./start): `Ctrl+A` then `K`, then `Y`
- **idf.py monitor**: `Ctrl+]`

---

## How It Works

### Architecture

```
  +--------------+     cmd_t queue     +---------------+
  |  input_task  | ------------------> |  motor_task   |
  |  (priority 3)|                     | (priority 10) |
  |              |                     |               |
  | - UART input |                     | - Step motors |
  | - Parse cmds |                     | - Read Hall   |
  | - Verify pos |                     | - Ramp speed  |
  | - Auto-correct                     | - Save to NVS |
  +--------------+                     +---------------+
```

Two FreeRTOS tasks run on the ESP32:

- **input_task**: Reads serial commands, parses them, sends to motor_task via a queue. After each move, asks for verification and auto-corrects SPF.

- **motor_task**: Receives commands, drives stepper motors with trapezoidal speed profile (ramp up, cruise, ramp down). Saves position to NVS flash after every move.

### Motor Control

```
  Speed
  (fast)
    |          _______________
    |         /               \
    |        /    cruise       \
    |       /    (RUN_US)       \
    |      /                     \
    |     /                       \
    |    / ramp up        ramp down\
    |   / (RAMP_START_US)          \
    |  /                            \
    +----+-----+-----+-----+-----+----> Steps
       0     80   (total-80)    total
            RAMP_STEPS
```

The motors use a **trapezoidal speed profile**:
1. **Ramp up**: Start slow (5000us/step), accelerate over 80 steps
2. **Cruise**: Run at full speed (2000us/step)
3. **Ramp down**: Decelerate over last 80 steps to stop smoothly

### Position Tracking

```
  Drum rotation (one full revolution = 10 flaps)

     Magnet   Magnet   Magnet   Magnet
       |        |        |        |
  -----*--------*--------*--------*-------
       0        1        2        3  ...  9
       |<-SPF-->|
       (steps per flap)
```

- Each drum has 10 magnets (one per digit)
- A Hall sensor detects when each magnet passes
- SPF (Steps Per Flap) = how many motor steps between magnets
- Position is tracked by counting: `total_steps = flaps * SPF`
- After each move, position is saved to NVS flash memory

### NVS (Non-Volatile Storage)

The ESP32 saves these values to flash memory (survives power cycles):
- Current position of each drum (`cur[0-3]`)
- Steps-per-flap for each drum (`SPF[0-3]`)

On boot, if saved data exists, it skips the setup prompt and goes straight to ready. Type `reset` to clear saved data and start fresh.

---

## Troubleshooting

### Motor doesn't move
- Check ULN2003 power (5V) and connections
- Try `s0 500` to send raw steps — you should hear the motor
- Check that motor wires are connected to the correct GPIO pins

### Motor moves in wrong direction
- Swap IN1 with IN4 and IN2 with IN3 in the `MP` array in `main/main.c`
- Or physically swap the wires on the ULN2003 board

### Motor overshoots / undershoots digits
- Use the auto-correct: send `5555`, then tell it what's actually showing
- Or manually tune: `spf0 210` (increase if undershooting, decrease if overshooting)
- Use `m0` to test single flaps and fine-tune

### Hall sensor not working
- Type `st` to see sensor status
- Should show `ACTIVE(LOW)` when magnet is near, `idle(HIGH)` when away
- Check wiring and 10k pull-up resistor (required for GPIO 34, 36, 39)

### ESP32 won't boot (stuck in boot loop)
- GPIO 12 is a strapping pin — ensure it's LOW at power-on
- If using GPIO 12 for a motor, disconnect it during flashing

### Serial port not found
- Check USB cable (some cables are charge-only, no data)
- Install USB-to-serial driver if needed:
  - CH340: https://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html
  - CP2102: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

### Can't flash (port busy)
- Close the serial monitor first (`Ctrl+A` then `K` in screen)
- Or the scripts will auto-kill existing connections

---

## Configuration Reference

All constants are in `main/main.c` at the top of the file.

| Constant | Default | Description |
|----------|---------|-------------|
| `N_DIGITS` | 4 | Number of drums |
| `N_FLAPS` | 10 | Digits per drum (0-9) |
| `SPF[4]` | 204,204,224,204 | Steps per flap (tunable at runtime) |
| `RUN_US[4]` | 2000 | Cruise speed in microseconds (lower = faster) |
| `CAL_US[4]` | 3000 | Calibration speed |
| `RAMP_START_US` | 5000 | Starting speed for ramp |
| `RAMP_STEPS` | 80 | Steps to ramp up/down |
| `COOLDOWN_MS` | 50 | Pause between drum moves |
| `MOTOR_IDLE_TIMEOUT_MS` | 5000 | Turn off motors after idle |

---

## Project Structure

```
flip-flap-clock/
  main/
    main.c              All firmware code (single file)
    CMakeLists.txt      IDF component dependencies
  start                 Setup + serial monitor script
  flash                 Build + flash script
  dev.sh                Build + flash + monitor script
  open.sh               Serial monitor only script
  build.sh              Legacy build script
  CMakeLists.txt        Top-level IDF project file
  sdkconfig             ESP-IDF configuration
  sdkconfig.defaults    Default config overrides
  CLAUDE.md             AI assistant instructions
  README.md             This file
```

---

## License

MIT
