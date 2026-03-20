// SPI Defines
//spi peripheral to use
#define SPI_BASE SPI1_BASE

#if SPI_BASE == SPI0_BASE
    #define SPI_RESET_BITS (1 << RESET_SPI0)
#else
    #define SPI_RESET_BITS (1 << RESET_SPI1)
#endif

//spi pin numbers
#define PIN_MISO 12
#define PIN_CS   13
#define PIN_SCK  14
#define PIN_MOSI 11
//write protect/card detect pin
#define PIN_WP   10
//TODO automate setting the offsets...this might be a really long sequence of if conditionals...
#define IO_WP_REGISTER          (IO_BANK0_BASE + IO_BANK0_GPIO10_CTRL_OFFSET)
#define IO_MOSI_REGISTER        (IO_BANK0_BASE + IO_BANK0_GPIO11_CTRL_OFFSET)
#define IO_MISO_REGISTER        (IO_BANK0_BASE + IO_BANK0_GPIO12_CTRL_OFFSET)
#define IO_CS_REGISTER          (IO_BANK0_BASE + IO_BANK0_GPIO13_CTRL_OFFSET)
#define IO_SCK_REGISTER         (IO_BANK0_BASE + IO_BANK0_GPIO14_CTRL_OFFSET)

#define PADS_WP_REGISTER        (PADS_BANK0_BASE + PADS_BANK0_GPIO10_OFFSET)
#define PADS_MOSI_REGISTER      (PADS_BANK0_BASE + PADS_BANK0_GPIO11_OFFSET)
#define PADS_MISO_REGISTER      (PADS_BANK0_BASE + PADS_BANK0_GPIO12_OFFSET)
#define PADS_CS_REGISTER        (PADS_BANK0_BASE + PADS_BANK0_GPIO13_OFFSET)
#define PADS_SCK_REGISTER       (PADS_BANK0_BASE + PADS_BANK0_GPIO14_OFFSET)

//chip select macros
#define CS_ENABLE()     REG(SIO_BASE + SIO_GPIO_OUT_CLR_OFFSET) = 1 << PIN_CS
#define CS_DISABLE()    REG(SIO_BASE + SIO_GPIO_OUT_SET_OFFSET) = 1 << PIN_CS