#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "ff.h"
#include "tavernkeep.h"
#include "bartap/bartap.c"


/* example code 
// Data will be copied from src to dst
const char src[] = "Hello, world! (from DMA)";
char dst[count_of(src)];

#include "blink.pio.h"

void blink_pin_forever(PIO pio, uint sm, uint offset, uint pin, uint freq) {
    blink_program_init(pio, sm, offset, pin);
    pio_sm_set_enabled(pio, sm, true);

    printf("Blinking pin %d at %d Hz\n", pin, freq);

    // PIO counter program takes 3 more cycles in total than we pass as
    // input (wait for n + 1; mov; jmp)
    pio->txf[sm] = (125000000 / (2 * freq)) - 3;
}
*/

// initialize spi and associated gpio pins
void init_sd_connection() {
    //reset spi peripheral
    REG(RESETS_BASE + REG_ALIAS_SET_BITS) = SPI_RESET_BITS;

    //unreset spi
    REG(RESETS_BASE + REG_ALIAS_CLR_BITS) = SPI_RESET_BITS;
    while ((~REG(RESETS_BASE + 0x8)) & (SPI_RESET_BITS));

    //set prescaler value to 2
    REG(SPI_BASE + SPI_SSPCPSR_OFFSET + REG_ALIAS_SET_BITS) = 2;

    //set SSPCR0 bits, SCR value 255, SPH and SPO to 0, FRF to 0, DSS to 7
    REG(SPI_BASE + SPI_SSPCR0_OFFSET + REG_ALIAS_SET_BITS) = 0x0000FF07;

    //enable DMA
    REG(SPI_BASE + SPI_SSPDMACR_OFFSET + REG_ALIAS_SET_BITS) = 3;

    //enable SPI
    REG(SPI_BASE + SPI_SSPCR1_OFFSET + REG_ALIAS_SET_BITS) = 2;

    //initialize and set functions to gpio pins to be used
    //reset gpio banks
    REG(RESETS_BASE + REG_ALIAS_SET_BITS) = (1 << RESET_IO_BANK0) | (1 << RESET_PADS_BANK0);
    REG(RESETS_BASE + REG_ALIAS_CLR_BITS) = (1 << RESET_IO_BANK0) | (1 << RESET_PADS_BANK0);
    while ((~REG(RESETS_BASE + 0x8)) & ((1 << RESET_IO_BANK0) | (1 << RESET_PADS_BANK0)));
    
    //turn on input enable, turn off output disable
    REG(PADS_WP_REGISTER) |= 0x40;
    REG(PADS_MOSI_REGISTER) |= 0x40;
    REG(PADS_MISO_REGISTER) |= 0x40;
    REG(PADS_CS_REGISTER) |= 0x40;
    REG(PADS_SCK_REGISTER) |= 0x40;
    
    //set pin functions
    REG(IO_WP_REGISTER) = GPIO_FUNC_SIO;
    REG(IO_MOSI_REGISTER) = GPIO_FUNC_SPI;
    REG(IO_MISO_REGISTER) = GPIO_FUNC_SPI;
    REG(IO_CS_REGISTER) = GPIO_FUNC_SIO;
    REG(IO_SCK_REGISTER) = GPIO_FUNC_SPI;
    
    //clear ISO bits
    REG(PADS_WP_REGISTER) &= 0xFFFFFEFF;
    REG(PADS_MOSI_REGISTER) &= 0xFFFFFEFF;
    REG(PADS_MISO_REGISTER) &= 0xFFFFFEFF;
    REG(PADS_CS_REGISTER) &= 0xFFFFFEFF;
    REG(PADS_SCK_REGISTER) &= 0xFFFFFEFF;
    
    //drive chip select high (active low)
    CS_DISABLE();
}

//deinitialize spi and asociated gpio pins
void deinit_sd_connection() {
    //disable SPI
    REG(SPI_BASE + SPI_SSPCR1_OFFSET + REG_ALIAS_CLR_BITS) = 2;
    //disable DMA
    REG(SPI_BASE + SPI_SSPDMACR_OFFSET + REG_ALIAS_CLR_BITS) = 3;
    //reset spi peripheral
    REG(RESETS_BASE + REG_ALIAS_SET_BITS) = SPI_RESET_BITS;
    //TODO deinitialize gpio pins used
    REG(RESETS_BASE + REG_ALIAS_SET_BITS) = (1 << RESET_IO_BANK0) | (1 << RESET_PADS_BANK0);
}

//increase clock speed, call post card initialization
void speed_high() {
    //disable SPI
    REG(SPI_BASE + SPI_SSPCR1_OFFSET + REG_ALIAS_CLR_BITS) = 2;
    //SCR to 3
    REG(SPI_BASE + SPI_SSPCR0_OFFSET + REG_ALIAS_CLR_BITS) = 0x0000FC00;
    //enable SPI
    REG(SPI_BASE + SPI_SSPCR1_OFFSET + REG_ALIAS_SET_BITS) = 2;
}

//decrease clock speed down to initialization level
void speed_low() {
    //disable SPI
    REG(SPI_BASE + SPI_SSPCR1_OFFSET + REG_ALIAS_CLR_BITS) = 2;
    //SCR to 255
    REG(SPI_BASE + SPI_SSPCR0_OFFSET + REG_ALIAS_SET_BITS) = 0x0000FF00;
    //enable SPI
    REG(SPI_BASE + SPI_SSPCR1_OFFSET + REG_ALIAS_SET_BITS) = 2;
}

int main()
{
    stdio_init_all();

    //enable spi peripheral, configure pins
    init_sd_connection();
    //enable peripheral power (must do this before interacting with the card at all...duh)
    //TODO
    //check write protect pin (doubles as card detect), if wp/no card detected, notify user to check card, do not proceed with init sequence
    //TODO
    //small delay to give card time to power up
    busy_wait_ms(1); //TODO replace with non sdk method
    //80 clock cycles for synchro
    for(uint8_t i = 0; i < 10; i++) { spi_transfer(0xFF); }
    //deassert chip select again
    CS_DISABLE();
    spi_transfer(0xFF);
    
    //fatfs disk initialize
    disk_initialize(0);

    //increase spi clock speed once card is initialized
    speed_high();

/* example code 
    // Get a free channel, panic() if there are none
    int chan = dma_claim_unused_channel(true);
    
    // 8 bit transfers. Both read and write address increment after each
    // transfer (each pointing to a location in src or dst respectively).
    // No DREQ is selected, so the DMA transfers as fast as it can.
    
    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    
    dma_channel_configure(
        chan,          // Channel to be configured
        &c,            // The configuration we just created
        dst,           // The initial write address
        src,           // The initial read address
        count_of(src), // Number of transfers; in this case each is 1 byte.
        true           // Start immediately.
    );
    
    // We could choose to go and do something else whilst the DMA is doing its
    // thing. In this case the processor has nothing else to do, so we just
    // wait for the DMA to finish.
    dma_channel_wait_for_finish_blocking(chan);
    
    // The DMA has now copied our text from the transmit buffer (src) to the
    // receive buffer (dst), so we can print it out from there.
    puts(dst);

    // PIO Blinking example
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &blink_program);
    printf("Loaded program at %d\n", offset);
    
    #ifdef PICO_DEFAULT_LED_PIN
    blink_pin_forever(pio, 0, offset, PICO_DEFAULT_LED_PIN, 3);
    #else
    blink_pin_forever(pio, 0, offset, 6, 3);
    #endif
    // For more pio examples see https://github.com/raspberrypi/pico-examples/tree/master/pio
*/

    REG(RESETS_BASE + REG_ALIAS_CLR_BITS) = (1 << RESET_IO_BANK0) | (1 << RESET_PADS_BANK0);
    while ((~REG(RESETS_BASE + 0x8)) & ((1 << RESET_IO_BANK0) | (1 << RESET_PADS_BANK0)));

    REG(IO_BANK0_BASE + IO_BANK0_GPIO25_CTRL_OFFSET) = GPIO_FUNC_SIO;

    REG(PADS_BANK0_BASE + PADS_BANK0_GPIO25_OFFSET + REG_ALIAS_CLR_BITS) = PADS_BANK0_GPIO0_ISO_BITS;
    REG(SIO_BASE + SIO_GPIO_OE_SET_OFFSET) = 1 << 25;

    for (;;) {
        REG(SIO_BASE + SIO_GPIO_OUT_XOR_OFFSET) = 1 << 25;
        for (unsigned int i = 0; i != 1000000; i++);
    }

    while (true) {

    }
}
