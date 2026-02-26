# Split-Flap Display Firmware v12 — Design

## Goal
Clean rewrite of v11 with robustness and hardware safety. Single file. No WiFi.

## Architecture
Same two-task FreeRTOS design (motor_task prio 10, input_task prio 3, queue-based IPC).

## Safety Additions
- Motor timeout: abort if move takes >3x expected steps
- Task watchdog (TWDT) on motor task
- Cooldown between moves (200ms)
- Auto-recalibrate after 2 consecutive failures per drum
- Motor auto-off on idle timeout and after every operation
- Stall detection: abort if 2x expected steps pass without a Hall pulse
- Per-drum error counting with ESP_LOG warnings

## Speed (Slow for Testing)
- RUN_US: 3000 (was 1500)
- CAL_US: 4000 (was 3000)
- RAMP_START_US: 7000 (was 5000)
- RAMP_STEPS: 100 (was 80)

## Pin Config
Unchanged from v11.
