#include <stdio.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

int app_main(void)
{

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
