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
static const int SPF[N_DIGITS] = {
    204,   /* D0 — measured */
    204,   /* D1 — measured */
    224,   /* D2 — estimated (was short by ~1 digit at 204) */
    204,   /* D3 — estimate (Hall dead, uses manual cal) */
};

/* Step delays (microseconds) — SLOW for testing */
static const uint32_t RUN_US[N_DIGITS] = { 3000, 3000, 3000, 3000 };
static const uint32_t CAL_US[N_DIGITS] = { 4000, 4000, 4000, 4000 };

/* Ramp profile */
#define RAMP_STEPS      60
#define RAMP_START_US   6000

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
#define COOLDOWN_MS             200     /* min gap between drum moves  */
#define MOTOR_IDLE_TIMEOUT_MS   5000    /* de-energize if idle         */

/* ============================================================
 * GPIO PINS
 * ============================================================ */
static const int MP[N_DIGITS][4] = {
    { 26, 25, 33, 32 },   /* M0 */
    { 27, 14, 12, 13 },   /* M1 */
    { 15,  2,  4, 16 },   /* M2 */
    { 19, 21, 22, 23 },   /* M3 */
};
static const int HP[N_DIGITS] = { 5, 18, 17, 34 };

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
static volatile bool cal_done        = false;

static QueueHandle_t cmd_queue = NULL;

typedef struct {
    int  digits[N_DIGITS];
    bool recal;
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
    step_idx[d] = (step_idx[d] + 1) & 3;
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

    for (int d = 0; d < N_DIGITS; d++) {
        move_to(d, digits[d]);
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_MS));
    }

    motor_off_all();

    ESP_LOGI(TAG, ">>> Display done");
}

/* ============================================================
 * MOTOR TASK
 * ============================================================ */
static void motor_task(void *pv)
{
    calibrate_all();

    cmd_t cmd;
    while (1) {
        if (xQueueReceive(cmd_queue, &cmd,
                          pdMS_TO_TICKS(MOTOR_IDLE_TIMEOUT_MS)) == pdTRUE) {
            if (cmd.recal)
                calibrate_all();
            else
                display_digits(cmd.digits);

            motor_off_all();
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

    while (!cal_done) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* ---- Initial setup: ask what's currently showing ---- */
    {
        printf("\r\n+-----------------------------------+\r\n");
        printf("|  INITIAL SETUP                    |\r\n");
        printf("|  Look at the drums and type the   |\r\n");
        printf("|  %d digits currently showing.      |\r\n", N_DIGITS);
        printf("|  Then they will reset to %s.    |\r\n",
               N_DIGITS == 2 ? "00" : "0000");
        printf("+-----------------------------------+\r\n> ");
        fflush(stdout);

        /* Read initial position from user */
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
            /* Set current position from user input — this also
             * marks ALL drums as calibrated so move_to() won't
             * try Hall-based calibration (D3 Hall is dead). */
            for (int d = 0; d < N_DIGITS; d++) {
                cur[d] = ibuf[d] - '0';
                calibrated[d] = true;
            }

            printf("Set current = [");
            for (int d = 0; d < N_DIGITS; d++) printf(" %d", cur[d]);
            printf(" ] — moving to zero...\r\n");

            /* Send command to move to all zeros */
            cmd_t cmd = { .recal = false };
            for (int d = 0; d < N_DIGITS; d++)
                cmd.digits[d] = 0;
            xQueueSend(cmd_queue, &cmd, portMAX_DELAY);

            /* Wait for move to finish */
            vTaskDelay(pdMS_TO_TICKS(500));
            while (uxQueueMessagesWaiting(cmd_queue) > 0)
                vTaskDelay(pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(1000));

            printf("Ready!\r\n");
        } else {
            printf("Skipped setup (expected %d digits, got %d)\r\n", N_DIGITS, ipos);
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
        printf("| Enter %d digits + Enter    |\r\n", N_DIGITS);
        printf("| 'cal' = recalibrate       |\r\n");
        printf("| 'st'  = sensor status     |\r\n");
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

        xQueueSend(cmd_queue, &cmd, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(300));
        while (uxQueueMessagesWaiting(cmd_queue) > 0)
            vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Ask user what's actually showing */
        printf("\r\n--- What is ACTUALLY showing? Type %d digits: ", N_DIGITS);
        fflush(stdout);

        char fbuf[16];
        int fpos = 0;
        while (fpos < (int)sizeof(fbuf) - 1) {
            uint8_t fch;
            if (uart_read_bytes(UART_NUM_0, &fch, 1, portMAX_DELAY) <= 0)
                continue;
            if (fch == '\n' || fch == '\r') {
                uart_write_bytes(UART_NUM_0, "\r\n", 2);
                break;
            }
            if (fch == 127 || fch == '\b') {
                if (fpos > 0) { fpos--; uart_write_bytes(UART_NUM_0, "\b \b", 3); }
                continue;
            }
            if (fch >= '0' && fch <= '9') {
                uart_write_bytes(UART_NUM_0, (char *)&fch, 1);
                fbuf[fpos++] = (char)fch;
            }
        }
        fbuf[fpos] = '\0';

        if (fpos == N_DIGITS) {
            printf("  Expected: [");
            for (int i = 0; i < N_DIGITS; i++) printf(" %d", cmd.digits[i]);
            printf(" ]  Actual: [");
            for (int i = 0; i < N_DIGITS; i++) printf(" %c", fbuf[i]);
            printf(" ]  ");
            bool match = true;
            for (int i = 0; i < N_DIGITS; i++) {
                int exp = cmd.digits[i];
                int act = fbuf[i] - '0';
                int diff = (act - exp + N_FLAPS) % N_FLAPS;
                if (diff != 0) {
                    printf("D%d: off by +%d  ", i, diff);
                    match = false;
                }
            }
            if (match) printf("PERFECT!");
            printf("\r\n");
        }
    }
}

/* ============================================================
 * APP MAIN
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "Split-Flap Display v12 starting");

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
