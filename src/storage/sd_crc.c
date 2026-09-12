#include <stdbool.h>

#include "storage/sd_crc.h"

//CRC7
uint8_t crc_helper_7(const uint8_t *message, size_t length) {
    //empty message check:
    if (length == 0) {
        return 0;
    }
    //G(x) = x^7 + x^3 + 1 -> mask: 0x09 -> bitshift left by one to align to top of byte = 0x12
    uint8_t mask = 0x12;
    //create eval register
    uint8_t eval = 0;
    //feedback bool to gate whether XOR operation occurs
    bool feedback;
    //byte number of next bit
    size_t next_byte = 0;
    //bit number of next bit
    uint8_t next_bit = 7;
    //repeat:
    do {
        feedback = ((eval >> 7) & 1) ^ ((message[next_byte] >> next_bit) & 1);
        //shift working register
        eval = eval << 1;
        //bitwise xor with mask if feedback is true
        if (feedback) {
            eval = eval ^ mask;
        }
        //decrement next_bit, if less than 0, roll it back to 7 and increment next_byte
        if (--next_bit > 7) { 
            next_bit = 7;
            next_byte++;
        }
    } while (next_byte < length);
    //CRC value (return raw CRC value, processing for transmission should happen in context, not here)
    return eval >> 1;
}

//CRC16
uint16_t crc_helper_16(const uint8_t *message, size_t length) {
    //empty message check:
    if (length == 0) {
        return 0;
    }
    //G(x) = x^16 + x^12 + x^5 + 1 -> mask: 0x1021
    uint16_t mask = 0x1021;
    //create eval register
    uint16_t eval = 0;
    //feedback bool to gate whether XOR operation occurs
    bool feedback;
    //byte number of next bit
    size_t next_byte = 0;
    //bit number of next bit
    uint8_t next_bit = 7;
    //repeat:
    do {
        feedback = ((eval >> 15) & 1) ^ ((message[next_byte] >> next_bit) & 1);
        //shift working register
        eval = eval << 1;
        //bitwise xor with mask if feedback is true
        if (feedback) {
            eval = eval ^ mask;
        }
        //decrement next_bit, if less than 0, roll it back to 7 and increment next_byte
        if (--next_bit > 7) { 
            next_bit = 7;
            next_byte++;
        }
    } while (next_byte < length);
    //CRC value (return raw CRC value, processing for transmission should happen in context, not here)
    return eval;
}

//CRC16 Rolling
bool crc_helper_rolling_16(uint16_t crc, uint8_t data) {
    //G(x) = x^16 + x^12 + x^5 + 1 -> mask: 0x1021
    uint16_t mask = 0x1021;
    //feedback bool to gate whether XOR operation occurs
    bool feedback;
    //bit number of next bit
    uint8_t next_bit = 7;

    do {
        //calculate feedback from existing crc topbit and incoming data bit, then decrement next_bit
        feedback = ((crc >> 15) & 1) ^ ((data >> next_bit--) & 1);
        //shift working register
        crc = crc << 1;
        //if feedback is true, bitwise XOR with mask
        if (feedback) {
            crc ^= mask;
        }
    //repeat for each bit of data byte
    } while (next_bit >= 0);
    //return new rolling crc value
    return crc;
}