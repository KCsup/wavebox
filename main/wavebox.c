#include <stdio.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// pin 18 is the GPIO output
#define PIN_OUT 18
#define PIN_MASK (1ULL << PIN_OUT)

void app_main(void)
{
    gpio_config_t io_conf = {};

    io_conf.intr_type = GPIO_INTR_DISABLE; // disable interrupts
    io_conf.mode = GPIO_MODE_OUTPUT; // set as output
    io_conf.pin_bit_mask = PIN_MASK;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    int gpio_state = 0;
    while(1)
    {
        gpio_state = !gpio_state;
        gpio_set_level(PIN_OUT, gpio_state);

        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
}
