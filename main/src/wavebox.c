#include <stdio.h>
#include <driver/gpio.h>
#include "controller/uni_gamepad.h"

#include "wavebox.h"
#include "freertos/idf_additions.h"
#include "my_platform.h"

// bluepad requirements
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <btstack_stdio_esp32.h>
#include <hci_dump.h>
#include <hci_dump_embedded_stdout.h>
#include <uni.h>

#include "sdkconfig.h"

// pin 18 is the GPIO output
#define PIN_OUT 18
#define PIN_MASK (1ULL << PIN_OUT)

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

void print_controller_state_test(void*);

// extern global var from wavebox.h
// allows access for files including
QueueHandle_t controller_state_queue;

int app_main(void)
{
    controller_state_queue = xQueueCreate(1, sizeof(uni_gamepad_t));

    xTaskCreatePinnedToCore(
        print_controller_state_test,
        "controller_print",
        4096, // ?
        (void*) controller_state_queue,
        5,
        NULL,
        1 // assuming btstack on core 0
    );

    // bluepad stack init

#ifdef CONFIG_ESP_CONSOLE_UART
#ifndef CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE
    btstack_stdio_init();
#endif  // CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE
#endif  // CONFIG_ESP_CONSOLE_UART

    btstack_init();

    // from my_platform
    uni_platform_set_custom(get_my_platform());

    uni_init(0, NULL);

    // does not return
    btstack_run_loop_execute();
    
    return 0;
}

void print_controller_state_test(void* controller_queue)
{
    gpio_config_t io_conf = {};

    io_conf.intr_type = GPIO_INTR_DISABLE; // disable interrupts
    io_conf.mode = GPIO_MODE_OUTPUT; // set as output
    io_conf.pin_bit_mask = PIN_MASK;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    
    // TODO: handle no queue state pushed
    while(1)
    {
        uni_gamepad_t gp;
        xQueueReceive(controller_queue, &gp, portMAX_DELAY); // wait indefinitely

        gpio_set_level(PIN_OUT, !(gp.buttons & BUTTON_A));
    }
}
