#include <stdio.h>
#include <driver/gpio.h>
#include "controller/uni_gamepad.h"

#include "wavebox.h"
#include "esp_err.h"
#include "freertos/idf_additions.h"
#include "hal/rmt_ll.h"
#include "hal/rmt_types.h"
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
    rmt_channel_handle_t rx_chann;
    rmt_channel_handle_t tx_chann;
    rmt_symbol_word_t id_response_payload[25];
    uint32_t filter_reg_value;
    uint32_t idle_reg_value;
} joybus_isr_ctx_t;

volatile int led_state = 0;

IRAM_ATTR void joybus_rx_callback(void* user_ctx)
{

    joybus_isr_ctx_t* isr_ctx = (joybus_isr_ctx_t*) user_ctx;

    rmt_channel_t* raw_rx = (rmt_channel_t*) isr_ctx->rx_chann;
    rmt_dev_t* rx_dev = raw_rx->group->hal.regs;
    int rx_id = raw_rx->channel_id;

    uint32_t status = rmt_ll_rx_get_interrupt_status(rx_dev, rx_id);
    rmt_ll_clear_interrupt_status(rx_dev, status);

    // RX not complete
    if(!(status & RMT_LL_EVENT_RX_DONE(rx_id))) return;
    
    // change ownership of RX for software usage
    rmt_ll_rx_set_mem_owner(rx_dev, rx_id, RMT_LL_MEM_OWNER_SW);
    
    do
    {
        uint32_t num_symbols = rmt_ll_rx_get_memory_writer_offset(rx_dev, rx_id);
    
        // during testing, the stop bytes was always appended to the end of
        // the symbol transactions
        // plus, erronious transactions had single symbols added (with zero
        // durations for both logic leves)
        // SO, the length of symbols will be checked as their reported length
        // minus one, which should give clean multiples of 8 for byte parsin
        if(num_symbols <= 1) break;

        // again, assuming the last bit is the stop bit, so ignoring
        const int joybus_bytes_len = BYTES_LEN(num_symbols - 1); 

        // if not enough bits for full command byte, ignore
        if(joybus_bytes_len < 1) break;

        uint8_t joybus_bytes[joybus_bytes_len];
        // returns false if an invalid joybus bit was received
        if(!get_joybus_bytes(raw_rx->hw_mem_base,
                             num_symbols - 1,
                             joybus_bytes)) break;


        // needed later for TX
        rmt_channel_t *raw_tx = (rmt_channel_t*) isr_ctx->tx_chann;
        int tx_id = raw_tx->channel_id;
        rmt_dev_t *tx_dev = raw_tx->group->hal.regs;
        bool tx_started = false;
    


        switch(joybus_bytes[0]) // must be at least a single byte
        {
            case 0x00: // ID
                // 3 bytes + stop bit = 25 bits
                memcpy(raw_tx->hw_mem_base, isr_ctx->id_response_payload, sizeof(isr_ctx->id_response_payload));


                // after stop bit
                raw_tx->hw_mem_base[25] = (rmt_symbol_word_t){ .duration0 = 0, .level0 = 1, .duration1 = 0, .level1 = 1 };

                rmt_ll_tx_reset_pointer(tx_dev, tx_id);
                rmt_ll_tx_fix_idle_level(tx_dev, tx_id, 1, true); // matches eot
                rmt_ll_tx_start(tx_dev, tx_id);

                tx_started = true;
    
                break;
            case 0x41:
                gpio_set_level(PIN_OUT, !led_state);
                led_state = !led_state;
                break;
            default: // not handled command byte
                break;
        }

        if(tx_started)
        {
            // wait for TX to finish
            while (!(rmt_ll_tx_get_interrupt_status_raw(tx_dev, tx_id) & RMT_LL_EVENT_TX_DONE(tx_id)));
            rmt_ll_clear_interrupt_status(tx_dev, RMT_LL_EVENT_TX_DONE(tx_id));
        }
    } while(0);

    // re-arm immediately, right here, no task hop:

    rmt_ll_rx_reset_pointer(rx_dev, rx_id);
    rmt_ll_rx_set_mem_owner(rx_dev, rx_id, RMT_LL_MEM_OWNER_HW);
    rmt_ll_rx_enable(rx_dev, rx_id, true);
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

    // free TX interrupt
    rmt_channel_t* raw_tx_setup = (rmt_channel_t*) tx_chann;
    ESP_ERROR_CHECK(esp_intr_free(raw_tx_setup->intr));

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


    uint8_t payload[] = {0x09, 0x00, 0x20};

    joybus_isr_ctx_t isr_ctx = {
        .rx_chann = rx_chann,
        .tx_chann = tx_chann,
    };
    
    encode_joybus_bytes(3, payload, isr_ctx.id_response_payload);
    

    // // pass RX data queue
    // rmt_rx_register_event_callbacks(rx_chann, &rx_callbacks, &isr_ctx);
    
    ESP_ERROR_CHECK(rmt_enable(rx_chann));

    rmt_channel_t* raw_rx = (rmt_channel_t*) rx_chann;
    int rx_chann_id = raw_rx->channel_id;
    rmt_dev_t *rmt_dev = raw_rx->group->hal.regs;

    // free internal RX interrupt
    ESP_ERROR_CHECK(esp_intr_free(raw_rx->intr));

    // both from 'rmt_receive' implementation
    // min to filter noise
    const uint32_t filter_reg_value = ((uint64_t)((rmt_rx_channel_t*)rx_chann)->filter_clock_resolution_hz * JOYBUS_MIN_SYMBOL_NS) / 1000000000UL;
    // max to determine end of symbol
    const uint32_t idle_reg_value = ((uint64_t)rx_chann->resolution_hz * JOYBUS_MAX_SYMBOL_NS) / 1000000000UL;

    logi("IDLE VAL: %d\n", idle_reg_value);
    
    
    // TODO: handle no queue state pushed

    intr_handle_t rx_intr_handle = NULL;
    ESP_ERROR_CHECK(esp_intr_alloc_intrstatus(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3, (uint32_t)rmt_ll_get_interrupt_status_reg(rmt_dev), RMT_LL_EVENT_RX_DONE(rx_chann_id), joybus_rx_callback, &isr_ctx, &rx_intr_handle));
    
    rmt_ll_enable_interrupt(rmt_dev, RMT_LL_EVENT_RX_MASK(rx_chann_id), true);        
    
    rmt_ll_rx_reset_pointer(rmt_dev, rx_chann_id);
    rmt_ll_rx_set_mem_owner(rmt_dev, rx_chann_id, RMT_LL_MEM_OWNER_HW);

    rmt_ll_rx_set_filter_thres(rmt_dev, rx_chann_id, filter_reg_value);
    rmt_ll_rx_enable_filter(rmt_dev, rx_chann_id, true);
    rmt_ll_rx_set_idle_thres(rmt_dev, rx_chann_id, idle_reg_value);

    isr_ctx.filter_reg_value = filter_reg_value;
    isr_ctx.idle_reg_value = idle_reg_value;

    rmt_ll_rx_enable(rmt_dev, rx_chann_id, true);

    while(1)
    {
        // initiate RX transfer
        // ESP_ERROR_CHECK(rmt_receive(rx_chann, raw_rmt_symbols, sizeof(raw_rmt_symbols), &rx_receive_conf));

        // callback for polling interrupt status

        
        // wait for RMT signal RX


        // TODO: parse rx event data
        
        // must be controller data waiting
        // TODO: move to RX ISR method
        // if(!uxQueueMessagesWaiting(controller_queue)) continue;
        
        // uni_gamepad_t gp;
        // xQueueReceive(controller_queue, &gp, portMAX_DELAY); // wait indefinitely

        // gpio_set_level(PIN_OUT, !(gp.buttons & BUTTON_A));


        // TODO: implement actual blocking functionality
        // (fetching controller state)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

