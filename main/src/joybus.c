#include "joybus.h"
#include "hal/rmt_types.h"

IRAM_ATTR int get_joybus_bytes(rmt_symbol_word_t* received_symbols,
                      size_t len_symbols,
                      uint8_t bytes_buffer[BYTES_LEN(len_symbols)])
{
    int byte_i = 0;
    uint8_t current_byte = 0;

    for(int bit_i = 0; bit_i < len_symbols; bit_i++)
    {
        rmt_symbol_word_t current_symbol = received_symbols[bit_i];

        if(JOYBUS_VALID_1(current_symbol.duration0, current_symbol.duration1))
        {
            current_byte |= (1u << (7 - (bit_i % 8)));
        }
        else if(!JOYBUS_VALID_0(current_symbol.duration0, current_symbol.duration1))
        {
            // zeros do not need to be written since it is the default bit
            // position for the written byte
            // this checks if the bit is not a valid joybus bit
            return 0;
        }

        // if not the first bit and at the end of the current byte, or no
        // more bits to write
        if((bit_i > 0 && bit_i % 8 == 7) || bit_i == len_symbols - 1)
        {
            bytes_buffer[byte_i] = current_byte;
            current_byte = 0;
            byte_i++;
        }
    }

    return 1;
}

IRAM_ATTR void encode_joybus_bytes(int len_bytes,
                         uint8_t in_bytes[len_bytes],
                         rmt_symbol_word_t symbols_buffer[(len_bytes * 8) + 1])
{
    for(int byte_i = 0; byte_i < len_bytes; byte_i++)
    {
        uint8_t current_byte = in_bytes[byte_i];
        for(int bit_i = 0; bit_i < 8; bit_i++)
        {
            // reversing bit shift to achieve MSB first
            if(current_byte & (1u << (7 - bit_i))) // high bit
                symbols_buffer[bit_i + (8 * byte_i)] = (rmt_symbol_word_t) {
                    .duration0 = JOYBUS_BIT_IDEAL_TICKS,
                    .level0 = 0,
                    .duration1 = JOYBUS_BIT_IDEAL_TICKS * 3,
                    .level1 = 1
                };
            else // low bit
                symbols_buffer[bit_i + (8 * byte_i)] = (rmt_symbol_word_t) {
                    .duration0 = JOYBUS_BIT_IDEAL_TICKS * 3,
                    .level0 = 0,
                    .duration1 = JOYBUS_BIT_IDEAL_TICKS,
                    .level1 = 1
                };
        }
    }

    // controller stop bit
    symbols_buffer[len_bytes * 8] = (rmt_symbol_word_t) {
        .duration0 = JOYBUS_BIT_IDEAL_TICKS * 2,
        .level0 = 0,
        .duration1 = JOYBUS_BIT_IDEAL_TICKS * 2,
        .level1 = 1
    };
}
