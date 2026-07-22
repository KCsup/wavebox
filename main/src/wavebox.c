#include <stdio.h>
#include <driver/gpio.h>
#include "controller/uni_gamepad.h"

#include "wavebox.h"
#include "esp_err.h"
#include "freertos/idf_additions.h"
#include "hal/rmt_ll.h"
#include "my_platform.h"
#include "joybus.h"

// bluepad requirements
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <btstack_stdio_esp32.h>
#include <uni.h>

#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include "driver/rmt_types.h"
#include "soc/rmt_struct.h"

#include "rmt_private.h"


// pin 18 is the GPIO output
#define PIN_OUT 18
#define PIN_MASK (1ULL << PIN_OUT)

// TODO: refine GPIO pin candidate
#define JOYBUS_GPIO 17

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

void wavebox_exec_task(void*);
void rmt_tx_test(void*);

// extern global var from wavebox.h
// allows access for files including
QueueHandle_t controller_state_queue;

int app_main(void)
{
    controller_state_queue = xQueueCreate(1, sizeof(uni_gamepad_t));

    xTaskCreatePinnedToCore(
        wavebox_exec_task,
        "wavebox_exec",
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

typedef struct {
    rmt_channel_handle_t tx_chann;
    TaskHandle_t wavebox_task
} joybus_isr_ctx_t;


IRAM_ATTR bool joybus_rx_callback(rmt_channel_handle_t rx_chann, const rmt_rx_done_event_data_t* rx_event_data, void* user_ctx)
{
    joybus_isr_ctx_t* isr_ctx = (joybus_isr_ctx_t*) user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;

    
    do
    {
        // during testing, the stop bytes was always appended to the end of
        // the symbol transactions
        // plus, erronious transactions had single symbols added (with zero
        // durations for both logic leves)
        // SO, the length of symbols will be checked as their reported length
        // minus one, which should give clean multiples of 8 for byte parsin
        if(rx_event_data->num_symbols <= 1) break;

        // again, assuming the last bit is the stop bit, so ignoring
        const int joybus_bytes_len = BYTES_LEN(rx_event_data->num_symbols - 1);

        // if not enough bits for full command byte, ignore
        if(joybus_bytes_len < 1) break;

        uint8_t joybus_bytes[joybus_bytes_len];
        // returns false if an invalid joybus bit was received
        if(!get_joybus_bytes(rx_event_data->received_symbols,
                             rx_event_data->num_symbols - 1,
                             joybus_bytes)) break;


        struct rmt_channel_t *raw_tx = (struct rmt_channel_t*) isr_ctx->tx_chann;
        int channel_id = raw_tx->channel_id;
        rmt_dev_t *dev = raw_tx->group->hal.regs;

    
        switch(joybus_bytes[0]) // must be at least a single byte
        {
            case 0x00: // ID
                // 3 bytes + stop bit = 25 bits
                rmt_symbol_word_t id_payload[25];
                uint8_t payload[] = {0x09, 0x00, 0x20};

                encode_joybus_bytes(3, payload, id_payload);
                memcpy(raw_tx->hw_mem_base, id_payload, sizeof(id_payload));


                // after stop bit
                raw_tx->hw_mem_base[25] = (rmt_symbol_word_t){ .duration0 = 0, .level0 = 1, .duration1 = 0, .level1 = 1 };

                rmt_ll_tx_reset_pointer(dev, channel_id);
                rmt_ll_tx_fix_idle_level(dev, channel_id, 1, true); // matches eot
                rmt_ll_tx_start(dev, channel_id);
        
                break;
            case 0x41:
                logi("ORIGIN RECEIVED");
                break;
            default: // not handled command byte
                break;
        }
    }
    while(0);

    vTaskNotifyGiveFromISR(isr_ctx->wavebox_task, &high_task_wakeup);
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
    rmt_channel_handle_t rx_chann = NULL;
    
    rmt_rx_channel_config_t rx_chann_conf = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = JOYBUS_CLK_PRECISION, // 10 MHz tick resolution, so 1 tick = 0.1 us
        .mem_block_symbols = 64, // at least 64?
        // equaling 256 bytes (x4)?
        // TODO: find if this is needed for joybus command parsing (which looks)
        //       to only require 3 bytes of maximum data
        .gpio_num = JOYBUS_GPIO,
        .flags.invert_in = false,
        .flags.with_dma = false,
        .intr_priority = 3
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chann_conf, &rx_chann));

    rmt_rx_event_callbacks_t rx_callbacks = {
        .on_recv_done = joybus_rx_callback
    };
    // 
    // initiate RMT RX transactions
    rmt_receive_config_t rx_receive_conf = {
        .signal_range_min_ns = 800, // 0.8 us
        .signal_range_max_ns = 4000 // 4 us
    };

    rmt_symbol_word_t raw_rmt_symbols[64];


    
    // ----
    // joybus RMT TX channel setup
    // ----

    rmt_channel_handle_t tx_chann = NULL;
    rmt_tx_channel_config_t tx_chann_conf = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = JOYBUS_GPIO,
        .mem_block_symbols = 64, // TODO: find if this is sufficient for needed symbol
        .resolution_hz = JOYBUS_CLK_PRECISION, // 10 MHz
        .trans_queue_depth = 1,
        .flags.invert_out = false,
        .flags.with_dma = false,
        .flags.io_loop_back = true, // TODO: see if this functions with both RX and TX enabled
        .flags.io_od_mode = true,
        .flags.init_level = 1 // ensures line is not pulled to GND
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chann_conf, &tx_chann));

    // no callbacks atm

    
    ESP_ERROR_CHECK(rmt_enable(tx_chann));

    // ----

    // joybys TX encoder
    // rmt_bytes_encoder_config_t rmt_encoder_conf = {
    //     .bit0 = {
    //         .duration0 = JOYBUS_BIT_MIN_TICKS * 3,
    //         .level0 = 0,
    //         .duration1 = JOYBUS_BIT_MIN_TICKS,
    //         .level1 = 1
    //     },
    //     .bit1 = {
    //         .duration0 = JOYBUS_BIT_MIN_TICKS,
    //         .level0 = 0,
    //         .duration1 = JOYBUS_BIT_MIN_TICKS * 3,
    //         .level1 = 1
    //     },
    //     .flags.msb_first = true
    // };

    // rmt_copy_encoder_config_t copy_encoder_conf = {};

    // rmt_encoder_handle_t tx_chann_encoder;
    // ESP_ERROR_CHECK(rmt_new_bytes_encoder(&rmt_encoder_conf, &tx_chann_encoder));

    // TX transmit conf
    // rmt_transmit_config_t rmt_transmit_conf = {
    //     .loop_count = 0,
    //     .flags.queue_nonblocking = true, // for ISR safety
    //     .flags.eot_level = 1 // releases line to be pulled up again
    // };


    joybus_isr_ctx_t isr_ctx = {
        .tx_chann = tx_chann,
        .wavebox_task = xTaskGetCurrentTaskHandle()
        // .tx_chann_id = chann_id
    };
    
    // pass RX data queue
    rmt_rx_register_event_callbacks(rx_chann, &rx_callbacks, &isr_ctx);
    
    struct rmt_channel_t *raw_rx_debug = (struct rmt_channel_t *)rx_chann;
    ESP_ERROR_CHECK(rmt_enable(rx_chann));
    logi("RX fsm right after enable: %d\n", atomic_load(&raw_rx_debug->fsm));
    
    // TODO: handle no queue state pushed
    while(1)
    {
        // initiate RX transfer
        ESP_ERROR_CHECK(rmt_receive(rx_chann, raw_rmt_symbols, sizeof(raw_rmt_symbols), &rx_receive_conf));
        
        // wait for RMT signal RX
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);


        // TODO: parse rx event data
        
        // must be controller data waiting
        // TODO: move to RX ISR method
        // if(!uxQueueMessagesWaiting(controller_queue)) continue;
        
        // uni_gamepad_t gp;
        // xQueueReceive(controller_queue, &gp, portMAX_DELAY); // wait indefinitely

        // gpio_set_level(PIN_OUT, !(gp.buttons & BUTTON_A));
    }
}

