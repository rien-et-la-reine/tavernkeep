#ifndef TAVERNKEEP_TEST_FAKE_HARDWARE_SPI_H
#define TAVERNKEEP_TEST_FAKE_HARDWARE_SPI_H

#include <stddef.h>
#include <stdint.h>

typedef struct spi_inst {
    unsigned int id;
} spi_inst_t;

/* The SDK's instance names. The fake keys nothing on the pointer; these exist
 * so production code that says `spi0` compiles against the fake. */
extern spi_inst_t pico_mock_spi0_instance;
extern spi_inst_t pico_mock_spi1_instance;
#define spi0 (&pico_mock_spi0_instance)
#define spi1 (&pico_mock_spi1_instance)

unsigned int spi_init(spi_inst_t *spi, unsigned int baudrate);
void spi_deinit(spi_inst_t *spi);
unsigned int spi_set_baudrate(spi_inst_t *spi, unsigned int baudrate);
int spi_write_read_blocking(
    spi_inst_t *spi,
    const uint8_t *source,
    uint8_t *destination,
    size_t length);

#endif

