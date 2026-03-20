/**
 * ============================================================
 * Split-Flap Display — v12 PRODUCTION
 * ============================================================
 * HOW TO USE:
 *   Flash -> open serial monitor 115200 baud
 *   Type 4 digits + Enter  ->  e.g.  1234
 *   Type  cal  to recalibrate all drums
 *   Type  st   for sensor status
 *
 * HARDWARE: 10 magnets per drum (one per digit).
 *   1 magnet detection = 1 digit forward.
 *   Detection uses EDGE detection (idle→active transition)
 *   with step-count debounce to reject noise.
 *
 * SPEED: Currently SLOW for testing.
 *   Decrease RUN_US toward 1500 for production.
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"

static const char *TAG = "splitflap";

/* ============================================================
 * CONFIGURATION
 * ============================================================ */
#define N_DIGITS    4
#define N_FLAPS     10

/*
 * Steps per flap — MEASURED from magnet gap data.
 * Used for ramp speed profile and timeout calculations.
 * D0: measured 1 gap of ~182 (need more data, using 200)
 * D1: measured gaps 218,203,192,203 → avg 204
 * D2/D3: not yet measured, estimate ~200
 */
static int SPF[N_DIGITS] = {
    204,   /* D0 */
    204,   /* D1 */
    224,   /* D2 */
    204,   /* D3 */
};

/* Step delays (microseconds) */
static uint32_t RUN_US[N_DIGITS] = { 1000, 1000, 1000, 1000 };
static uint32_t CAL_US[N_DIGITS] = { 2000, 2000, 2000, 2000 };

/* Ramp profile */
#define RAMP_STEPS      40
#define RAMP_START_US   3000

/*
 * MIN_IDLE: minimum consecutive idle reads before a rising edge
 * counts as a real magnet. Filters noise without depending on SPF.
 * 5 steps @ 3000us = 15ms — enough to reject flicker.
 */
#define MIN_IDLE                5

/* ---- Safety limits ---- */
#define MOVE_TIMEOUT_FACTOR     3       /* abort if >3x expected steps */
#define CAL_MAX_STEPS_FACTOR    20      /* max SPF * factor for cal    */
#define MAX_CONSECUTIVE_ERRS    2       /* auto-recal after N failures */
#define COOLDOWN_MS             50      /* min gap between drum moves  */
#define MOTOR_IDLE_TIMEOUT_MS   5000    /* de-energize if idle         */

/* ============================================================
 * GPIO PINS
 * ============================================================ */
static const int MP[N_DIGITS][4] = {
    { 18, 19, 21, 22 },   /* M1 */
    { 23, 25, 26, 27 },   /* M2 */
    { 32, 33,  4,  5 },   /* M3 */
    { 12, 13, 14, 15 },   /* M4 */
};
static const int HP[N_DIGITS] = { 34, 39, 36, 16 };

/* ============================================================
 * TWO-PHASE FULL-STEP SEQUENCE (max torque)
 * ============================================================ */
static const uint8_t SEQ[4][4] = {
    { 1, 1, 0, 0 },
    { 0, 1, 1, 0 },
    { 0, 0, 1, 1 },
    { 1, 0, 0, 1 },
};

/* ============================================================
 * STATE
 * ============================================================ */
static int      cur[N_DIGITS]        = {0};
static bool     calibrated[N_DIGITS] = {false};
static int      step_idx[N_DIGITS]   = {0};
static int      err_count[N_DIGITS]  = {0};
static volatile bool cal_done        = true;
static volatile bool motor_busy      = false;

static QueueHandle_t cmd_queue = NULL;

/* ============================================================
 * NVS — SAVE / LOAD POSITION & SPF
 * ============================================================ */
static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("splitflap", NVS_READWRITE, &h) != ESP_OK) return;
    for (int d = 0; d < N_DIGITS; d++) {
        char key[8];
        snprintf(key, sizeof(key), "cur%d", d);
        nvs_set_i32(h, key, cur[d]);
        snprintf(key, sizeof(key), "spf%d", d);
        nvs_set_i32(h, key, SPF[d]);
    }
    nvs_set_u8(h, "valid", 1);
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("splitflap", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t valid = 0;
    nvs_get_u8(h, "valid", &valid);
    if (!valid) { nvs_close(h); return false; }
    for (int d = 0; d < N_DIGITS; d++) {
        char key[8];
        int32_t val;
        snprintf(key, sizeof(key), "cur%d", d);
        if (nvs_get_i32(h, key, &val) == ESP_OK) cur[d] = val;
        snprintf(key, sizeof(key), "spf%d", d);
        if (nvs_get_i32(h, key, &val) == ESP_OK) SPF[d] = val;
    }
    nvs_close(h);
    return true;
}

typedef struct {
    int  digits[N_DIGITS];
    bool recal;
    bool manual_step;
    bool all_step;
    int  step_motor;
    int  step_count;
} cmd_t;

/* ============================================================
 * MOTOR — LOW LEVEL
 * ============================================================ */
static void motor_off(int d)
{
    for (int p = 0; p < 4; p++)
        gpio_set_level(MP[d][p], 0);
}

static void motor_off_all(void)
{
    for (int d = 0; d < N_DIGITS; d++)
        motor_off(d);
}

static void motor_step(int d, uint32_t delay_us)
{
    step_idx[d] = (step_idx[d] + 3) & 3;  /* +3 = reverse direction */
    const uint8_t *s = SEQ[step_idx[d]];
    for (int p = 0; p < 4; p++)
        gpio_set_level(MP[d][p], s[p]);
    esp_rom_delay_us(delay_us);
}

/* Trapezoidal speed profile */
static uint32_t ramp_speed(int step_num, int total, uint32_t cruise_us)
{
    int r = (RAMP_STEPS < total / 2) ? RAMP_STEPS : (total / 2);
    if (r < 1) r = 1;

    if (step_num < r)
        return RAMP_START_US -
               (uint32_t)((RAMP_START_US - cruise_us) * step_num / r);

    if (step_num >= total - r) {
        int remaining = total - step_num;
        if (remaining < 1) remaining = 1;
        return RAMP_START_US -
               (uint32_t)((RAMP_START_US - cruise_us) * remaining / r);
    }

    return cruise_us;
}

/* ============================================================
 * HALL SENSOR — EDGE DETECTION
 *
 * Instead of checking absolute level, we track transitions:
 *   idle→active (rising edge) = entering a magnet = 1 flap
 *
 * Step-count cooldown after each edge prevents noise from
 * being double-counted. This eliminates the need for
 * clear_sensor entirely.
 * ============================================================ */
static bool hall_active(int d)
{
    return gpio_get_level(HP[d]) == 0;   /* active LOW */
}

/* ============================================================
 * CALIBRATION — IDLE-COUNT EDGE DETECTION
 *
 * Rotate until we see MIN_IDLE consecutive idle reads followed
 * by an active read. That transition = home (digit 0).
 * ============================================================ */
static bool calibrate_drum(int d)
{
    int limit = SPF[d] * CAL_MAX_STEPS_FACTOR;
    int idle_count = 0;

    ESP_LOGI(TAG, "[D%d] Calibrating (Hall=GPIO%d, initial=%s)",
             d, HP[d], hall_active(d) ? "ACTIVE" : "idle");

    for (int s = 0; s < limit; s++) {
        motor_step(d, CAL_US[d]);

        if (s % 200 == 0 && s > 0)
            vTaskDelay(1);

        if (!hall_active(d)) {
            idle_count++;
        } else {
            /* Active — was this preceded by enough idle steps? */
            if (idle_count >= MIN_IDLE) {
                /* Settle into the magnet zone */
                for (int j = 0; j < 30; j++)
                    motor_step(d, CAL_US[d]);
                motor_off(d);
                cur[d]        = 0;
                calibrated[d] = true;
                err_count[d]  = 0;
                ESP_LOGI(TAG, "[D%d] Calibrated at step %d (idle_run=%d)",
                         d, s, idle_count);
                vTaskDelay(pdMS_TO_TICKS(200));
                return true;
            }
            idle_count = 0;
        }
    }

    motor_off(d);
    ESP_LOGE(TAG, "[D%d] Cal FAILED — no edge in %d steps", d, limit);
    return false;
}

static void calibrate_all(void)
{
    ESP_LOGI(TAG, "=== Calibrating all drums ===");
    cal_done = false;
    for (int d = 0; d < N_DIGITS; d++) {
        calibrated[d] = false;
        calibrate_drum(d);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    cal_done = true;
    for (int d = 0; d < N_DIGITS; d++)
        ESP_LOGI(TAG, "  D%d: %s", d, calibrated[d] ? "OK" : "FAIL");
    ESP_LOGI(TAG, "=== Calibration done ===");
}

/* ============================================================
 * MOVE TO TARGET FLAP — PURE STEP COUNTING
 *
 * Hall sensor is unreliable for per-flap counting (magnets
 * have varying active zones, some idle gaps < 5 steps).
 * So we use step counting: flaps * SPF steps forward.
 * Hall sensor is only used for calibration (find home).
 * ============================================================ */
static bool move_to(int d, int target)
{
    if (!calibrated[d]) {
        ESP_LOGW(TAG, "[D%d] Not calibrated — calibrating", d);
        if (!calibrate_drum(d)) return false;
    }

    int flaps = (target - cur[d] + N_FLAPS) % N_FLAPS;
    if (flaps == 0) {
        return true;
    }

    int total_steps = flaps * SPF[d];

    ESP_LOGI(TAG, "[D%d] %d -> %d (%d flaps, %d steps)",
             d, cur[d], target, flaps, total_steps);

    for (int s = 0; s < total_steps; s++) {
        motor_step(d, ramp_speed(s, total_steps, RUN_US[d]));

        if (s % 200 == 0 && s > 0)
            vTaskDelay(1);
    }

    motor_off(d);
    cur[d] = target;
    ESP_LOGI(TAG, "[D%d] Done -> %d (%d steps)", d, target, total_steps);
    return true;
}

/* ============================================================
 * DISPLAY 4 DIGITS
 * ============================================================ */
static void display_digits(const int digits[N_DIGITS])
{
    ESP_LOGI(TAG, ">>> Target [");
    for (int d = 0; d < N_DIGITS; d++) ESP_LOGI(TAG, " %d", digits[d]);
    ESP_LOGI(TAG, "] from [");
    for (int d = 0; d < N_DIGITS; d++) ESP_LOGI(TAG, " %d", cur[d]);
    ESP_LOGI(TAG, "]");

    /* Calculate steps needed for each motor */
    int total[N_DIGITS];
    int max_steps = 0;
    for (int d = 0; d < N_DIGITS; d++) {
        if (!calibrated[d]) {
            ESP_LOGW(TAG, "[D%d] Not calibrated — calibrating", d);
            calibrate_drum(d);
        }
        int flaps = (digits[d] - cur[d] + N_FLAPS) % N_FLAPS;
        total[d] = flaps * SPF[d];
        if (total[d] > max_steps) max_steps = total[d];
        if (flaps > 0)
            ESP_LOGI(TAG, "[D%d] %d -> %d (%d flaps, %d steps)",
                     d, cur[d], digits[d], flaps, total[d]);
    }

    /* Step all motors simultaneously */
    for (int s = 0; s < max_steps; s++) {
        for (int d = 0; d < N_DIGITS; d++) {
            if (s < total[d])
                motor_step(d, ramp_speed(s, total[d], RUN_US[d]));
        }
        if (s % 200 == 0 && s > 0) vTaskDelay(1);
    }

    motor_off_all();
    for (int d = 0; d < N_DIGITS; d++)
        cur[d] = digits[d];
    nvs_save();

    ESP_LOGI(TAG, ">>> Display done");
}

/* ============================================================
 * MOTOR TASK
 * ============================================================ */
static void motor_task(void *pv)
{
    cmd_t cmd;
    while (1) {
        if (xQueueReceive(cmd_queue, &cmd,
                          pdMS_TO_TICKS(MOTOR_IDLE_TIMEOUT_MS)) == pdTRUE) {
            motor_busy = true;
            if (cmd.recal)
                calibrate_all();
            else if (cmd.all_step) {
                int steps = cmd.step_count;
                ESP_LOGI(TAG, "All motors: %d steps", steps);
                for (int s = 0; s < steps; s++) {
                    for (int d = 0; d < N_DIGITS; d++)
                        motor_step(d, RUN_US[d] / N_DIGITS);
                    if (s % 200 == 0 && s > 0) vTaskDelay(1);
                }
                motor_off_all();
                ESP_LOGI(TAG, "All motors done");
            } else if (cmd.manual_step) {
                int d = cmd.step_motor;
                int steps = cmd.step_count;
                ESP_LOGI(TAG, "[D%d] Manual %d steps", d, steps);
                for (int s = 0; s < steps; s++) {
                    motor_step(d, RUN_US[d]);
                    if (s % 200 == 0 && s > 0) vTaskDelay(1);
                }
                motor_off(d);
                ESP_LOGI(TAG, "[D%d] Manual done", d);
            } else
                display_digits(cmd.digits);
            motor_off_all();
            motor_busy = false;
        } else {
            motor_off_all();
        }
    }
}

/* ============================================================
 * GPIO INIT
 * ============================================================ */
static void gpio_init_all(void)
{
    for (int d = 0; d < N_DIGITS; d++) {
        for (int p = 0; p < 4; p++) {
            gpio_config_t out_cfg = {
                .pin_bit_mask = 1ULL << MP[d][p],
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&out_cfg);
            gpio_set_level(MP[d][p], 0);
        }

        gpio_config_t hall_cfg = {
            .pin_bit_mask = 1ULL << HP[d],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&hall_cfg);
    }
}

/* ============================================================
 * INPUT TASK
 * ============================================================ */
static void input_task(void *pv)
{
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);

    vTaskDelay(pdMS_TO_TICKS(500));  /* let boot logs flush */

    /* ---- Initial setup: load SPF from NVS, always ask current position ---- */
    nvs_load();  /* restore SPF values only */

    printf("\r\n+-----------------------------------+\r\n");
    printf("|  What number is showing now?      |\r\n");
    printf("|  Type %d digits + Enter            |\r\n", N_DIGITS);
    printf("+-----------------------------------+\r\n> ");
    fflush(stdout);

    char ibuf[16];
    int ipos = 0;
    while (ipos < (int)sizeof(ibuf) - 1) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) <= 0)
            continue;
        if (ch == '\n' || ch == '\r') {
            uart_write_bytes(UART_NUM_0, "\r\n", 2);
            break;
        }
        if (ch == 127 || ch == '\b') {
            if (ipos > 0) { ipos--; uart_write_bytes(UART_NUM_0, "\b \b", 3); }
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            uart_write_bytes(UART_NUM_0, (char *)&ch, 1);
            ibuf[ipos++] = (char)ch;
        }
    }
    ibuf[ipos] = '\0';

    if (ipos == N_DIGITS) {
        for (int d = 0; d < N_DIGITS; d++) {
            cur[d] = ibuf[d] - '0';
            calibrated[d] = true;
        }
        printf("Current = [");
        for (int d = 0; d < N_DIGITS; d++) printf(" %d", cur[d]);
        printf(" ]  SPF: [");
        for (int d = 0; d < N_DIGITS; d++) printf(" %d", SPF[d]);
        printf(" ]\r\n");

        /* Auto-move to 0000 */
        bool need_move = false;
        for (int d = 0; d < N_DIGITS; d++)
            if (cur[d] != 0) { need_move = true; break; }

        if (need_move) {
            int before[N_DIGITS];
            for (int d = 0; d < N_DIGITS; d++) before[d] = cur[d];

            printf("Moving to 0000...\r\n");
            cmd_t cmd = { .recal = false };
            for (int d = 0; d < N_DIGITS; d++) cmd.digits[d] = 0;
            xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
            while (motor_busy)
                vTaskDelay(pdMS_TO_TICKS(100));

            /* Verify after move to zero */
            printf("\r\nShowing? (Enter to skip): ");
            fflush(stdout);
            char vbuf[16];
            int vpos = 0;
            while (vpos < (int)sizeof(vbuf) - 1) {
                uint8_t vch;
                if (uart_read_bytes(UART_NUM_0, &vch, 1, portMAX_DELAY) <= 0)
                    continue;
                if (vch == '\n' || vch == '\r') {
                    uart_write_bytes(UART_NUM_0, "\r\n", 2);
                    break;
                }
                if (vch == 127 || vch == '\b') {
                    if (vpos > 0) { vpos--; uart_write_bytes(UART_NUM_0, "\b \b", 3); }
                    continue;
                }
                if (vch >= '0' && vch <= '9') {
                    uart_write_bytes(UART_NUM_0, (char *)&vch, 1);
                    vbuf[vpos++] = (char)vch;
                }
            }
            vbuf[vpos] = '\0';

            if (vpos == N_DIGITS) {
                bool perfect = true;
                for (int d = 0; d < N_DIGITS; d++) {
                    int actual = vbuf[d] - '0';
                    if (actual == 0) {
                        printf("  D%d: OK\r\n", d);
                        continue;
                    }
                    perfect = false;
                    int flaps_intended = (0 - before[d] + N_FLAPS) % N_FLAPS;
                    int off = (actual - 0 + N_FLAPS) % N_FLAPS;
                    if (off > N_FLAPS / 2) off -= N_FLAPS;
                    int flaps_actual = flaps_intended + off;
                    printf("  D%d: wanted 0 got %d (off %+d, moved %d/%d flaps)",
                           d, actual, off, flaps_actual, flaps_intended);
                    if (flaps_intended >= 2 && flaps_actual >= 2) {
                        int old_spf = SPF[d];
                        int new_spf = (flaps_intended * old_spf) / flaps_actual;
                        if (new_spf < 150) new_spf = 150;
                        if (new_spf > 400) new_spf = 400;
                        if (new_spf != old_spf) {
                            SPF[d] = new_spf;
                            printf(" -> SPF %d=>%d", old_spf, new_spf);
                        }
                    }
                    printf("\r\n");
                    cur[d] = actual;
                }
                if (perfect) {
                    printf("  PERFECT! Ready!\r\n");
                } else {
                    nvs_save();
                    printf("  SPF updated & saved.\r\n");
                }
            } else {
                printf("Reset to 0000. Ready!\r\n");
            }
        } else {
            printf("Already at 0000. Ready!\r\n");
        }
    } else {
        printf("Skipped (expected %d digits, got %d). Assuming 0000.\r\n", N_DIGITS, ipos);
        for (int d = 0; d < N_DIGITS; d++) {
            cur[d] = 0;
            calibrated[d] = true;
        }
    }

    /* ---- Normal operation loop ---- */
    while (1) {
        printf("\r\n+---------------------------+\r\n");
        printf("| Now: [");
        for (int d = 0; d < N_DIGITS; d++) printf(" %d", cur[d]);
        printf(" ]");
        for (int d = 0; d < 18 - N_DIGITS * 2; d++) printf(" ");
        printf("|\r\n");
        printf("| %d digits = display        |\r\n", N_DIGITS);
        printf("| m0 = test 1 flap motor 0  |\r\n");
        printf("| m0 3 = test 3 flaps       |\r\n");
        printf("| spf0 210 = set SPF        |\r\n");
        printf("| speed 1500 = set speed    |\r\n");
        printf("| s0 200 = raw steps        |\r\n");
        printf("| all = all motors at once  |\r\n");
        printf("| obo = one by one          |\r\n");
        printf("| cal / st                  |\r\n");
        printf("+---------------------------+\r\n> ");
        fflush(stdout);

        /* Read line with echo */
        char buf[32];
        int pos = 0;
        while (pos < (int)sizeof(buf) - 1) {
            uint8_t ch;
            if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) <= 0)
                continue;

            if (ch == '\n' || ch == '\r') {
                uart_write_bytes(UART_NUM_0, "\r\n", 2);
                break;
            }
            if (ch == 127 || ch == '\b') {
                if (pos > 0) {
                    pos--;
                    uart_write_bytes(UART_NUM_0, "\b \b", 3);
                }
                continue;
            }
            if (ch >= 32 && ch < 127) {
                uart_write_bytes(UART_NUM_0, (char *)&ch, 1);
                buf[pos++] = (char)ch;
            }
        }
        buf[pos] = '\0';

        /* Strip whitespace */
        char input[16];
        int len = 0;
        for (int i = 0; i < pos && len < 15; i++) {
            if (!isspace((unsigned char)buf[i]))
                input[len++] = buf[i];
        }
        input[len] = '\0';

        /* 'st' / 'status' */
        if (strcasecmp(input, "status") == 0 || strcasecmp(input, "st") == 0) {
            printf("Hall sensors:\r\n");
            for (int d = 0; d < N_DIGITS; d++) {
                printf("  D%d GPIO%d: %s  cal=%s  cur=%d  errs=%d\r\n",
                       d, HP[d],
                       hall_active(d) ? "ACTIVE(LOW)" : "idle(HIGH)",
                       calibrated[d] ? "YES" : "NO",
                       cur[d], err_count[d]);
            }
            continue;
        }

        /* 'all' = full rotation on ALL motors simultaneously */
        if (strncasecmp(buf, "all", 3) == 0) {
            int max_steps = 0;
            for (int d = 0; d < N_DIGITS; d++) {
                int s = N_FLAPS * SPF[d];
                if (s > max_steps) max_steps = s;
            }
            printf("All motors: full rotation (%d steps)...\r\n", max_steps);
            cmd_t cmd = { .all_step = true, .step_count = max_steps };
            xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
            while (motor_busy)
                vTaskDelay(pdMS_TO_TICKS(100));
            printf("All done!\r\n");
            continue;
        }

        /* 'obo' = full rotation on each motor, one by one */
        if (strncasecmp(buf, "obo", 3) == 0) {
            printf("One by one: full rotation each motor...\r\n");
            for (int d = 0; d < N_DIGITS; d++) {
                int steps = N_FLAPS * SPF[d];
                printf("  M%d: %d flaps, %d steps...\r\n", d, N_FLAPS, steps);
                cmd_t cmd = { .manual_step = true, .step_motor = d, .step_count = steps };
                xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
                while (motor_busy)
                    vTaskDelay(pdMS_TO_TICKS(100));
                vTaskDelay(pdMS_TO_TICKS(COOLDOWN_MS));
            }
            printf("All done!\r\n");
            continue;
        }

        /* 'm0' = test 1 flap on motor 0 */
        if ((buf[0] == 'm' || buf[0] == 'M') && buf[1] >= '0' && buf[1] <= '3' &&
            (buf[2] == '\0' || buf[2] == ' ')) {
            int motor = buf[1] - '0';
            int flaps = atoi(buf + 2);
            if (flaps <= 0) flaps = 1;
            if (flaps > 10) flaps = 10;
            int steps = flaps * SPF[motor];
            printf("M%d: %d flap(s) = %d steps (SPF=%d)\r\n", motor, flaps, steps, SPF[motor]);
            cmd_t cmd = { .manual_step = true, .step_motor = motor, .step_count = steps };
            xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
            while (motor_busy)
                vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 'spf0 210' = set SPF for motor 0 to 210 */
        if (strncasecmp(buf, "spf", 3) == 0 && buf[3] >= '0' && buf[3] <= '3') {
            int motor = buf[3] - '0';
            int val = atoi(buf + 4);
            if (val > 50 && val < 500) {
                SPF[motor] = val;
                nvs_save();
                printf("SPF[%d] = %d (saved)\r\n", motor, val);
            } else {
                printf("SPF must be 50-500 (got %d)\r\n", val);
            }
            continue;
        }

        /* 'speed 1500' = set RUN_US for all motors */
        if (strncasecmp(buf, "speed", 5) == 0) {
            int val = atoi(buf + 5);
            if (val >= 1000 && val <= 5000) {
                for (int d = 0; d < N_DIGITS; d++) RUN_US[d] = val;
                printf("RUN_US = %d for all motors\r\n", val);
            } else {
                printf("Speed must be 1000-5000 us (got %d)\r\n", val);
            }
            continue;
        }

        /* 's0 200' = manual step motor 0 by 200 steps */
        if ((buf[0] == 's' || buf[0] == 'S') && buf[1] >= '0' && buf[1] <= '3') {
            int motor = buf[1] - '0';
            int steps = atoi(buf + 2);
            if (steps <= 0) steps = 1;
            if (steps > 5000) steps = 5000;
            printf("Stepping M%d by %d steps...\r\n", motor, steps);
            cmd_t cmd = { .manual_step = true, .step_motor = motor, .step_count = steps };
            xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
            while (motor_busy)
                vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 'reset' = clear NVS, re-ask position */
        if (strcasecmp(input, "reset") == 0) {
            nvs_handle_t h;
            if (nvs_open("splitflap", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
            printf("NVS cleared. Restarting...\r\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        /* 'cal' */
        if (strcasecmp(input, "cal") == 0) {
            cmd_t cmd = { .recal = true };
            xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
            while (!cal_done) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }

        /* Validate exactly 4 digits */
        if (len != N_DIGITS) {
            if (len > 0)
                ESP_LOGW(TAG, "Need %d digits, got %d: '%s'", N_DIGITS, len, input);
            continue;
        }

        cmd_t cmd = { .recal = false };
        bool valid = true;
        for (int i = 0; i < N_DIGITS; i++) {
            if (input[i] < '0' || input[i] > '9') {
                ESP_LOGW(TAG, "'%c' is not a digit 0-9", input[i]);
                valid = false;
                break;
            }
            cmd.digits[i] = input[i] - '0';
        }
        if (!valid) continue;

        /* Save pre-move positions */
        int before[N_DIGITS];
        for (int d = 0; d < N_DIGITS; d++) before[d] = cur[d];

        xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
        while (motor_busy)
            vTaskDelay(pdMS_TO_TICKS(100));

        /* --- Verification: what's actually showing? --- */
        printf("\r\nShowing? (Enter to skip): ");
        fflush(stdout);

        char vbuf[16];
        int vpos = 0;
        while (vpos < (int)sizeof(vbuf) - 1) {
            uint8_t vch;
            if (uart_read_bytes(UART_NUM_0, &vch, 1, portMAX_DELAY) <= 0)
                continue;
            if (vch == '\n' || vch == '\r') {
                uart_write_bytes(UART_NUM_0, "\r\n", 2);
                break;
            }
            if (vch == 127 || vch == '\b') {
                if (vpos > 0) { vpos--; uart_write_bytes(UART_NUM_0, "\b \b", 3); }
                continue;
            }
            if (vch >= '0' && vch <= '9') {
                uart_write_bytes(UART_NUM_0, (char *)&vch, 1);
                vbuf[vpos++] = (char)vch;
            }
        }
        vbuf[vpos] = '\0';

        if (vpos == N_DIGITS) {
            bool perfect = true;
            for (int d = 0; d < N_DIGITS; d++) {
                int expected = cmd.digits[d];
                int actual   = vbuf[d] - '0';
                int flaps_intended = (expected - before[d] + N_FLAPS) % N_FLAPS;

                if (actual == expected) {
                    printf("  D%d: OK\r\n", d);
                    continue;
                }
                perfect = false;

                int off = (actual - expected + N_FLAPS) % N_FLAPS;
                if (off > N_FLAPS / 2) off -= N_FLAPS;
                /* actual flaps = intended + overshoot (handles wrap-around) */
                int flaps_actual = flaps_intended + off;

                printf("  D%d: wanted %d got %d (off %+d, moved %d/%d flaps)",
                       d, expected, actual, off, flaps_actual, flaps_intended);

                /* Auto-correct SPF if we moved enough flaps for reliable calc */
                if (flaps_intended >= 2 && flaps_actual >= 2) {
                    int old_spf = SPF[d];
                    int new_spf = (flaps_intended * old_spf) / flaps_actual;
                    if (new_spf < 150) new_spf = 150;  /* floor */
                    if (new_spf > 400) new_spf = 400;  /* ceiling */
                    if (new_spf != old_spf) {
                        SPF[d] = new_spf;
                        printf(" -> SPF %d=>%d", old_spf, new_spf);
                    }
                } else {
                    printf(" (skip auto-correct: too few flaps)");
                }
                printf("\r\n");

                /* Update cur to actual so next move is correct */
                cur[d] = actual;
            }

            if (perfect) {
                printf("  PERFECT!\r\n");
            } else {
                nvs_save();
                printf("  SPF updated & saved. cur=[");
                for (int d = 0; d < N_DIGITS; d++) printf(" %d", cur[d]);
                printf(" ] SPF=[");
                for (int d = 0; d < N_DIGITS; d++) printf(" %d", SPF[d]);
                printf(" ]\r\n");
            }
        }
    }
}

/* ============================================================
 * APP MAIN
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "Split-Flap Display v12 starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_init_all();
    motor_off_all();

    ESP_LOGI(TAG, "Hall sensors: D0=GPIO%d(%s) D1=GPIO%d(%s) D2=GPIO%d(%s) D3=GPIO%d(%s)",
             HP[0], hall_active(0) ? "ACTIVE" : "idle",
             HP[1], hall_active(1) ? "ACTIVE" : "idle",
             HP[2], hall_active(2) ? "ACTIVE" : "idle",
             HP[3], hall_active(3) ? "ACTIVE" : "idle");

    cmd_queue = xQueueCreate(4, sizeof(cmd_t));

    xTaskCreate(motor_task, "motor", 8192, NULL, 10, NULL);
    xTaskCreate(input_task, "input", 4096, NULL,  3, NULL);

    ESP_LOGI(TAG, "Tasks launched");
}
