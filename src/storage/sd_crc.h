#include <stdint.h>
#include <stddef.h>

//CRC7
uint8_t crc_helper_7(const uint8_t *message, size_t length);
//CRC16
uint16_t crc_helper_16(const uint8_t *message, size_t length);
//CRC16 Rolling
bool crc_helper_rolling_16(uint16_t crc, uint8_t data);
