#include "joybus.h"
#include "hal/rmt_types.h"
#include <string.h>

int get_joybus_bytes(rmt_symbol_word_t* received_symbols,
                      size_t len_symbols,
                      uint8_t bytes_buffer[BYTES_LEN(len_symbols)])
{
    uint8_t out_bytes[BYTES_LEN(len_symbols)];

    int byte_i = 0;
    uint8_t current_byte = 0;

    for(int bit_i = 0; bit_i < len_symbols; bit_i++)
    {
        rmt_symbol_word_t current_symbol = received_symbols[bit_i];

        if(JOYBUS_VALID_1(current_symbol.duration0, current_symbol.duration1))
        {
            current_byte |= (1u << (bit_i % 8));
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
        if((bit_i > 0 && bit_i % 7 == 0) || bit_i == len_symbols - 1)
        {
            out_bytes[byte_i] = current_byte;
            current_byte = 0;
            byte_i++;
        }
    }

    memcpy(bytes_buffer, out_bytes, BYTES_LEN(len_symbols));
    return 1;
}
