#include "led.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TICK_MS     25
#define BLINK_MS    75
#define RAPID_HALF  2 /* ticks per half period while recording: 10 Hz */
#define SLOW_HALF   20 /* ticks per half period while pairing with the TV: 1 Hz */

static volatile led_mode_t s_mode;
static volatile int s_blink_ticks;
static volatile bool s_pairing;

/* A bool Kconfig option set to n is not defined at all */
#ifdef CONFIG_SRR_LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#else
#define LED_ACTIVE_LOW 0
#endif

#if CONFIG_SRR_LED_GPIO >= 0
static void led_write(bool on)
{
    gpio_set_level(CONFIG_SRR_LED_GPIO, on != LED_ACTIVE_LOW);
}

static void led_task(void *param)
{
    unsigned tick = 0;
    for (;;) {
        bool on;
        if (s_mode == LED_RECORDING) {
            on = (tick / RAPID_HALF) % 2 == 0;
        } else if (s_blink_ticks > 0) {
            /* Blink off against a solid session light, on otherwise */
            s_blink_ticks--;
            on = s_mode != LED_SESSION;
        } else if (s_mode == LED_IDLE && s_pairing) {
            on = (tick / SLOW_HALF) % 2 == 0;
        } else {
            on = s_mode == LED_SESSION;
        }
        led_write(on);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}
#endif

void led_init(void)
{
#if CONFIG_SRR_LED_GPIO >= 0
    gpio_reset_pin(CONFIG_SRR_LED_GPIO);
    gpio_set_direction(CONFIG_SRR_LED_GPIO, GPIO_MODE_OUTPUT);
    led_write(false);
    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);
#endif
}

void led_blink(void)
{
    s_blink_ticks = BLINK_MS / TICK_MS;
}

void led_set_mode(led_mode_t mode)
{
    s_mode = mode;
}

void led_set_pairing(bool on)
{
    s_pairing = on;
}
