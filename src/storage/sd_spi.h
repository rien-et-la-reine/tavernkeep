#ifndef TAVERNKEEP_STORAGE_SD_SPI_H
#define TAVERNKEEP_STORAGE_SD_SPI_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "hardware/spi.h"

#include "storage/block_device.h"

typedef struct {
    spi_inst_t *spi;
    uint32_t baud_rate_hz;
    uint8_t pin_clock;
    uint8_t pin_controller_out;
    uint8_t pin_controller_in;
    uint8_t pin_chip_select;

    uint8_t pin_card_available;
    /* Sense of the card-available line. false (the default for a zeroed
     * config): the socket switch closes to ground when a card is present, so
     * low means present and a rising edge is a removal. true: the switch
     * closes to ground when the socket is empty, so high means present and a
     * falling edge is a removal. The line is pulled up in both cases; only the
     * level that means "present" and the edge that means "removed" change. */
    bool card_detect_active_high;
} sd_spi_config_t;

/* Caller-owned state; must be zero-initialized before first sd_spi_configure(). */
typedef struct {
    block_device_t block_device;
    sd_spi_config_t config;
    bool configured;
    bool initialized;
    bool card_type_legacy;
    bool card_type_hcxc;
    uint64_t block_count;
    /* The data rate the SPI peripheral actually runs at once initialized:
     * the nearest rate at or below config.baud_rate_hz that clk_peri divides
     * to. 0 while not initialized. */
    uint32_t bus_rate_hz;
    atomic_bool removal_latched;
    /* The card-detect pin is set up as a pulled-up input. Once it is, the
     * device keeps it - across deinit and failed inits - because reading it is
     * the only way to see an insertion while the device is uninitialized
     * (block_device_is_present). Reconfiguring with a different pin fails
     * with error invalid argument. */
    bool card_detect_claimed;
} sd_spi_t;

//assigns config info to sd_spi_t object as well as claims and configures its card detect pin. blocks for ~100us
block_device_result_t sd_spi_configure(
    sd_spi_t *sd,
    const sd_spi_config_t *config);

block_device_t *sd_spi_as_block_device(sd_spi_t *sd);

//the data rate actually in use (bus_rate_hz above), or 0 while the device is not initialized
uint32_t sd_spi_bus_rate_hz(const sd_spi_t *sd);

#endif
