#include <stdio.h>
#include <driver/gpio.h>
#include "controller/uni_gamepad.h"

#include "wavebox.h"
#include "driver/rmt_types.h"
#include "esp_err.h"
#include "freertos/idf_additions.h"
#include "hal/rmt_types.h"
#include "my_platform.h"

// bluepad requirements
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <btstack_stdio_esp32.h>
#include <hci_dump.h>
#include <hci_dump_embedded_stdout.h>
#include <uni.h>

#include <driver/rmt_rx.h>

#include "portmacro.h"
#include "sdkconfig.h"
#include "soc/clk_tree_defs.h"

// pin 18 is the GPIO output
#define PIN_OUT 18
#define PIN_MASK (1ULL << PIN_OUT)

// TODO: refine GPIO pin candidate
#define JOYBUS_GPIO 17

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

void wavebox_exec_task(void*);

// extern global var from wavebox.h
// allows access for files including
QueueHandle_t controller_state_queue;

int app_main(void)
{
    controller_state_queue = xQueueCreate(1, sizeof(uni_gamepad_t));

    xTaskCreatePinnedToCore(
        wavebox_exec_task,
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

bool joybus_rx_callback(rmt_channel_handle_t rx_chann, const rmt_rx_done_event_data_t* edata, void* user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t receive_queue = (QueueHandle_t) user_ctx;
    // send the received RMT symbols to the parser task
    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
    // return whether any task is woken up
    return high_task_wakeup == pdTRUE;
}

void wavebox_exec_task(void* controller_queue)
{
    // testing LED setup
    // ----
    gpio_config_t io_conf = {};

    io_conf.intr_type = GPIO_INTR_DISABLE; // disable interrupts
    io_conf.mode = GPIO_MODE_OUTPUT; // set as output
    io_conf.pin_bit_mask = PIN_MASK;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    // ----

    // joybus RMT RX channel setup
    // ----
    
    // init RMT RX data queue
    QueueHandle_t rmt_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    
    rmt_channel_handle_t rx_chann = NULL;
    
    rmt_rx_channel_config_t rx_chann_conf = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz tick resolution, so 1 tick = 0.1 us
        .mem_block_symbols = 64, // at least 64?
        // equaling 256 bytes (x4)?
        // TODO: find if this is needed for joybus command parsing (which looks)
        //       to only require 3 bytes of maximum data
        .gpio_num = JOYBUS_GPIO,
        .flags.invert_in = false,
        .flags.with_dma = false
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chann_conf, &rx_chann));

    rmt_rx_event_callbacks_t rx_callbacks = {
        .on_recv_done = joybus_rx_callback
    };

    // pass RX data queue
    rmt_rx_register_event_callbacks(rx_chann, &rx_callbacks, rmt_rx_queue);
    
    ESP_ERROR_CHECK(rmt_enable(rx_chann));
    // ----

    // initiate RMT RX transactions
    rmt_receive_config_t rx_receive_conf = {
        .signal_range_min_ns = 800, // 0.8 us
        .signal_range_max_ns = 4000 // 4 us
    };

    rmt_symbol_word_t raw_rmt_symbols[64];

    
    // TODO: handle no queue state pushed
    while(1)
    {
        // initiate RX transfer
        ESP_ERROR_CHECK(rmt_receive(rx_chann, raw_rmt_symbols, sizeof(raw_rmt_symbols), &rx_receive_conf));
        
        // wait for RMT signal RX
        rmt_rx_done_event_data_t rx_event_data;
        xQueueReceive(rmt_rx_queue, &rx_event_data, portMAX_DELAY);

        // TODO: parse rx event data
        logi("START OF RX WORD:\n");
        for(int i = 0; i < rx_event_data.num_symbols; i++)
            logi("RX %d: dur0 %d, level0 %d | dur1 %d, level1 %d\n", i, rx_event_data.received_symbols[i].duration0, rx_event_data.received_symbols[i].level0, rx_event_data.received_symbols[i].duration1, rx_event_data.received_symbols[i].level1);
        logi("----\n\n");
        
        // must be controller data waiting
        if(!uxQueueMessagesWaiting(controller_queue)) continue;
        
        uni_gamepad_t gp;
        xQueueReceive(controller_queue, &gp, portMAX_DELAY); // wait indefinitely

        gpio_set_level(PIN_OUT, !(gp.buttons & BUTTON_A));
    }
}

