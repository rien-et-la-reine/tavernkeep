#include <stdatomic.h>
#include "gpio_irq_hardware_mock.h"
#include "hardware/gpio.h"
#include "pico_mock.h"
#include "sd_card_model.h"
#include "platform/gpio_irq.h"
#include "storage/sd_spi.h"
#include "test_check.h"

static unsigned int input_events;
static void input_handler(void *context, unsigned int gpio, uint32_t events)
{
    REQUIRE(context == &input_events && gpio == 7 && events == GPIO_IRQ_EDGE_FALL);
    input_events++;
}
int main(void)
{
    pico_mock_reset();
    pico_mock_sd_use_chip_select(5);
    REQUIRE(platform_gpio_irq_register(7, GPIO_IRQ_EDGE_FALL, input_handler, &input_events));
    spi_inst_t spi = {0};
    sd_spi_t sd = {0};
    sd_spi_config_t config = {
        .spi = &spi, .baud_rate_hz = 12000000,
        .pin_clock = 2, .pin_controller_out = 3, .pin_controller_in = 4,
        .pin_chip_select = 5, .pin_card_available = 6
    };
    const uint8_t r7[] = {0, 0, 1, 0xaa};
    const uint8_t ocr[] = {0xc0, 0xff, 0x80, 0};
    uint8_t csd[] = {0xfe, 0x40, 0, 0, 0, 0, 0, 0, 0, 0x0f, 0xff, 0, 0, 0, 0, 0, 0, 0, 0};
    /* Real CRC16 over the 16 register bytes: the driver validates it. */
    const uint16_t csd_crc = sd_crc16_ccitt(&csd[1], 16U);
    csd[17] = (uint8_t)(csd_crc >> 8U);
    csd[18] = (uint8_t)(csd_crc & 0xFFU);
    REQUIRE(pico_mock_sd_set_command(0, 1, NULL, 0));
    REQUIRE(pico_mock_sd_set_command(8, 1, r7, sizeof(r7)));
    /* CMD59 (CRC_ON_OFF) is part of every bring-up; unscripted it stalls the R1 poll. */
    REQUIRE(pico_mock_sd_set_command(59, 1, NULL, 0));
    REQUIRE(pico_mock_sd_set_command(55, 1, NULL, 0));
    REQUIRE(pico_mock_sd_set_command(41, 0, NULL, 0));
    REQUIRE(pico_mock_sd_set_command(58, 0, ocr, sizeof(ocr)));
    REQUIRE(pico_mock_sd_set_command(9, 0, csd, sizeof(csd)));
    REQUIRE(sd_spi_configure(&sd, &config) == BLOCK_DEVICE_RESULT_OK);
    block_device_t *device = sd_spi_as_block_device(&sd);
    REQUIRE(block_device_init(device) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(sd.block_count == 4194304);
    REQUIRE(gpio_irq_hardware_mock_callback_set_count() == 1);
    REQUIRE(gpio_irq_hardware_mock_fire(7, GPIO_IRQ_EDGE_FALL));
    REQUIRE(input_events == 1 && !atomic_load(&sd.removal_latched));
    size_t before = pico_mock_spi_transfer_count();
    uint64_t before_time = pico_mock_now_ms();
    REQUIRE(gpio_irq_hardware_mock_fire(6, GPIO_IRQ_EDGE_RISE));
    REQUIRE(atomic_load(&sd.removal_latched));
    REQUIRE(pico_mock_spi_is_initialized() && pico_mock_spi_deinit_count() == 0);
    REQUIRE(pico_mock_spi_transfer_count() == before && pico_mock_now_ms() == before_time);
    REQUIRE(!gpio_irq_hardware_mock_fire(6, GPIO_IRQ_EDGE_RISE));
    REQUIRE(gpio_irq_hardware_mock_fire(7, GPIO_IRQ_EDGE_FALL));
    REQUIRE(input_events == 2);
    uint8_t block[512];
    REQUIRE(block_device_read_blocks(device, 0, block, 1) == BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    REQUIRE(block_device_deinit(device) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(pico_mock_spi_transfer_count() == before && pico_mock_spi_deinit_count() == 1);
    REQUIRE(!gpio_irq_hardware_mock_fire(6, GPIO_IRQ_EDGE_RISE));
    REQUIRE(gpio_irq_hardware_mock_fire(7, GPIO_IRQ_EDGE_FALL));
    /* Reinsertion requires successful fresh initialization and re-arms only SD. */
    REQUIRE(block_device_init(device) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(!atomic_load(&sd.removal_latched));
    REQUIRE(gpio_irq_hardware_mock_callback_set_count() == 1);
    uint8_t payload[515] = {0xfe};
    REQUIRE(pico_mock_sd_set_command(17, 0, payload, sizeof(payload)));
    pico_mock_gpio_irq_fire_after_spi_transfers(20, 6, GPIO_IRQ_EDGE_RISE);
    REQUIRE(block_device_read_blocks(device, 0, block, 1) == BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    REQUIRE(pico_mock_spi_is_initialized() && pico_mock_spi_deinit_count() == 1);
    REQUIRE(block_device_deinit(device) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(pico_mock_spi_deinit_count() == 2);
    REQUIRE(gpio_irq_hardware_mock_fire(7, GPIO_IRQ_EDGE_FALL));
    REQUIRE(input_events == 4);
    REQUIRE(platform_gpio_irq_unregister(7));
    puts("PASS real SD driver and GPIO dispatcher integration");
    return 0;
}
