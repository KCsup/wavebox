#ifndef _JOYBUS_H
#define _JOYBUS_H

#include "esp_attr.h"
#include "hal/rmt_types.h"
#include "stdio.h"
#include <stdint.h>

// 10 MHz -> 0.1 us = 1 tick
#define JOYBUS_CLK_PRECISION (10 * 1000 * 1000)

// minimum 1 us for valid bit
#define JOYBUS_BIT_MIN_TICKS 10
// maximum 1.4 us for valid bit
#define JOYBUS_BIT_MAX_TICKS 14

#define JOYBUS_BIT_IDEAL_TICKS 12

#define JOYBUS_MIN_SYMBOL_NS 800
#define JOYBUS_MAX_SYMBOL_NS 4000

// TODO: URGENT, find why diving length by 8 does not function
#define BYTES_LEN(len) ((len) / 8)

// low 3 bits, high 1 bit
#define JOYBUS_VALID_0(low_dur, high_dur) \
    (low_dur >= JOYBUS_BIT_MIN_TICKS * 3 && low_dur <= JOYBUS_BIT_MAX_TICKS * 3 \
    && high_dur >= JOYBUS_BIT_MIN_TICKS && high_dur <= JOYBUS_BIT_MAX_TICKS)

#define JOYBUS_VALID_1(low_dur, high_dur) \
    (low_dur >= JOYBUS_BIT_MIN_TICKS && low_dur <= JOYBUS_BIT_MAX_TICKS && \
    high_dur >= JOYBUS_BIT_MIN_TICKS * 3 && high_dur <= JOYBUS_BIT_MAX_TICKS * 3)

// writes the blocked command bytes from an RX symbol to the given buffer
// returns if all bytes were within the tick contraints for a valid bit
// un-filtered, relying on the caller for correct symbols length
IRAM_ATTR int get_joybus_bytes(rmt_symbol_word_t* received_symbols,
                      size_t len_symbols,
                      uint8_t bytes_buffer[BYTES_LEN(len_symbols)]);

// encodes joybus bytes from MSB down
IRAM_ATTR void encode_joybus_bytes(int len_bytes,
                         uint8_t in_bytes[len_bytes],
                         rmt_symbol_word_t symbols_buffer[(len_bytes * 8) + 1]);


#endif
