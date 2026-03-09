/**
 * ============================================================
 * Motor Test — Single Split-Flap Digit
 * ============================================================
 * Wiring:
 *   IN1 = GPIO 27
 *   IN2 = GPIO 26
 *   IN3 = GPIO 25
 *   IN4 = GPIO 33
 *
 * SPF = 200 steps per flap (measured)
 * 10 flaps (digits 0-9), forward only
 *
 * Flow:
 *   1. Ask what digit is currently showing
 *   2. Move to 0
 *   3. Accept single digit input, move to it
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "motor_test";

/* GPIO pins: IN1=27, IN2=26, IN3=25, IN4=33 */
static const int PINS[4] = { 27, 26, 25, 33 };

#define SPF         200
#define N_FLAPS     10
#define SPEED_US    2000
#define RAMP_STEPS  40
#define RAMP_START  6000

/* Two-phase full-step sequence (max torque) */
static const uint8_t SEQ[4][4] = {
    { 1, 1, 0, 0 },
    { 0, 1, 1, 0 },
    { 0, 0, 1, 1 },
    { 1, 0, 0, 1 },
};

static int step_idx = 0;
static int cur_digit = 0;

static void motor_off(void)
{
    for (int p = 0; p < 4; p++)
        gpio_set_level(PINS[p], 0);
}

static void motor_step_fwd(uint32_t delay_us)
{
    step_idx = (step_idx - 1) & 3;   /* reversed for gear */
    const uint8_t *s = SEQ[step_idx];
    for (int p = 0; p < 4; p++)
        gpio_set_level(PINS[p], s[p]);
    esp_rom_delay_us(delay_us);
}

/* Trapezoidal speed ramp */
static uint32_t ramp_speed(int s, int total)
{
    int r = (RAMP_STEPS < total / 2) ? RAMP_STEPS : (total / 2);
    if (r < 1) r = 1;

    if (s < r)
        return RAMP_START - (uint32_t)((RAMP_START - SPEED_US) * s / r);
    if (s >= total - r) {
        int rem = total - s;
        if (rem < 1) rem = 1;
        return RAMP_START - (uint32_t)((RAMP_START - SPEED_US) * rem / r);
    }
    return SPEED_US;
}

static void move_to(int target)
{
    int flaps = (target - cur_digit + N_FLAPS) % N_FLAPS;
    if (flaps == 0) {
        printf("Already at %d\r\n", target);
        return;
    }

    int total_steps = flaps * SPF;
    printf("Moving %d -> %d (%d flaps, %d steps)\r\n",
           cur_digit, target, flaps, total_steps);

    for (int s = 0; s < total_steps; s++) {
        motor_step_fwd(ramp_speed(s, total_steps));
        if (s % 200 == 0 && s > 0)
            vTaskDelay(1);
    }

    motor_off();
    cur_digit = target;
    printf("Done -> %d\r\n", target);
}

static void gpio_init_all(void)
{
    for (int p = 0; p < 4; p++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << PINS[p],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(PINS[p], 0);
    }
}

/* Read a single char from UART with echo */
static char read_char_echo(void)
{
    while (1) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) <= 0)
            continue;
        if (ch == '\n' || ch == '\r') {
            uart_write_bytes(UART_NUM_0, "\r\n", 2);
            return '\r';
        }
        if (ch == 127 || ch == '\b')
            return '\b';
        if (ch >= 32 && ch < 127) {
            uart_write_bytes(UART_NUM_0, (char *)&ch, 1);
            return (char)ch;
        }
    }
}

/* Read a line from UART with echo + backspace */
static int read_line(char *buf, int maxlen)
{
    int pos = 0;
    while (pos < maxlen - 1) {
        char ch = read_char_echo();
        if (ch == '\r') break;
        if (ch == '\b') {
            if (pos > 0) { pos--; uart_write_bytes(UART_NUM_0, "\b \b", 3); }
            continue;
        }
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return pos;
}

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

    /* ---- Initial setup ---- */
    printf("\r\n+-----------------------------------+\r\n");
    printf("|  INITIAL SETUP                    |\r\n");
    printf("|  What digit is currently showing? |\r\n");
    printf("|  Type one digit (0-9):            |\r\n");
    printf("+-----------------------------------+\r\n> ");
    fflush(stdout);

    /* Get current digit */
    while (1) {
        char buf[8];
        int len = read_line(buf, sizeof(buf));
        if (len == 1 && buf[0] >= '0' && buf[0] <= '9') {
            cur_digit = buf[0] - '0';
            printf("Current = %d, moving to 0...\r\n", cur_digit);
            move_to(0);
            vTaskDelay(pdMS_TO_TICKS(500));
            printf("Ready!\r\n");
            break;
        }
        printf("Enter one digit 0-9\r\n> ");
        fflush(stdout);
    }

    /* ---- Normal operation ---- */
    while (1) {
        printf("\r\n+---------------------------+\r\n");
        printf("| Now showing: %d            |\r\n", cur_digit);
        printf("| Type digit (0-9) + Enter  |\r\n");
        printf("+---------------------------+\r\n> ");
        fflush(stdout);

        char buf[16];
        int len = read_line(buf, sizeof(buf));

        if (len == 0) continue;

        /* Single digit 0-9 */
        if (len == 1 && buf[0] >= '0' && buf[0] <= '9') {
            int target = buf[0] - '0';
            move_to(target);
            vTaskDelay(pdMS_TO_TICKS(300));

            /* Feedback */
            printf("--- What is ACTUALLY showing? (0-9): ");
            fflush(stdout);
            char fb[8];
            int flen = read_line(fb, sizeof(fb));
            if (flen == 1 && fb[0] >= '0' && fb[0] <= '9') {
                int actual = fb[0] - '0';
                int diff = (actual - target + N_FLAPS) % N_FLAPS;
                if (diff == 0)
                    printf("PERFECT!\r\n");
                else
                    printf("Off by +%d (expected %d, got %d)\r\n", diff, target, actual);
                /* Update cur to what's actually showing */
                cur_digit = actual;
            }
        } else {
            printf("Enter one digit 0-9\r\n");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Split-Flap Single Digit Test — SPF=%d", SPF);

    gpio_init_all();
    motor_off();

    printf("\r\nPins: IN1=GPIO27, IN2=GPIO26, IN3=GPIO25, IN4=GPIO33\r\n");
    printf("SPF=%d, 10 flaps, forward only\r\n", SPF);

    xTaskCreate(input_task, "input", 4096, NULL, 3, NULL);
}
