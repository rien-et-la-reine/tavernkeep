
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
/*
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
*/

/*----------------------------------------------------------------------/
/ Low level disk I/O module function checker                            /
/-----------------------------------------------------------------------/
/ WARNING: The data on the target drive will be lost!
*/

#include <stdio.h>
#include <string.h>
#include "ff.h"         /* Declarations of sector size */
#include "diskio.h"     /* Declarations of disk functions */



static DWORD pn (       /* Pseudo random number generator */
    DWORD pns   /* 0:Initialize, !0:Read */
)
{
    static DWORD lfsr;
    UINT n;


    if (pns) {
        lfsr = pns;
        for (n = 0; n < 32; n++) pn(0);
    }
    if (lfsr & 1) {
        lfsr >>= 1;
        lfsr ^= 0x80200003;
    } else {
        lfsr >>= 1;
    }
    return lfsr;
}


int test_diskio (
    BYTE pdrv,      /* Physical drive number to be checked (all data on the drive will be lost) */
    UINT ncyc,      /* Number of test cycles */
    DWORD* buff,    /* Pointer to the working buffer */
    UINT sz_buff    /* Size of the working buffer in unit of byte */
)
{
    UINT n, cc, ns;
    DWORD sz_drv, lba, lba2, sz_eblk, pns = 1;
    WORD sz_sect;
    BYTE *pbuff = (BYTE*)buff;
    DSTATUS ds;
    DRESULT dr;


    printf("test_diskio(%u, %u, 0x%08X, 0x%08X)\n", pdrv, ncyc, (UINT)buff, sz_buff);

    if (sz_buff < FF_MAX_SS + 8) {
        printf("Insufficient work area to run the program.\n");
        return 1;
    }

    for (cc = 1; cc <= ncyc; cc++) {
        printf("**** Test cycle %u of %u start ****\n", cc, ncyc);

        printf(" disk_initalize(%u)", pdrv);
        ds = disk_initialize(pdrv);
        if (ds & STA_NOINIT) {
            printf(" - failed.\n");
            return 2;
        } else {
            printf(" - ok.\n");
        }

        printf("**** Get drive size ****\n");
        printf(" disk_ioctl(%u, GET_SECTOR_COUNT, 0x%08X)", pdrv, (UINT)&sz_drv);
        sz_drv = 0;
        dr = disk_ioctl(pdrv, GET_SECTOR_COUNT, &sz_drv);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 3;
        }
        if (sz_drv < 128) {
            printf("Failed: Insufficient drive size to test.\n");
            return 4;
        }
        printf(" Number of sectors on the drive %u is %lu.\n", pdrv, sz_drv);

#if FF_MAX_SS != FF_MIN_SS
        printf("**** Get sector size ****\n");
        printf(" disk_ioctl(%u, GET_SECTOR_SIZE, 0x%X)", pdrv, (UINT)&sz_sect);
        sz_sect = 0;
        dr = disk_ioctl(pdrv, GET_SECTOR_SIZE, &sz_sect);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 5;
        }
        printf(" Size of sector is %u bytes.\n", sz_sect);
#else
        sz_sect = FF_MAX_SS;
#endif

        printf("**** Get block size ****\n");
        printf(" disk_ioctl(%u, GET_BLOCK_SIZE, 0x%X)", pdrv, (UINT)&sz_eblk);
        sz_eblk = 0;
        dr = disk_ioctl(pdrv, GET_BLOCK_SIZE, &sz_eblk);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
        }
        if (dr == RES_OK || sz_eblk >= 2) {
            printf(" Size of the erase block is %lu sectors.\n", sz_eblk);
        } else {
            printf(" Size of the erase block is unknown.\n");
        }

        /* Single sector write test */
        printf("**** Single sector write test ****\n");
        lba = 0;
        for (n = 0, pn(pns); n < sz_sect; n++) pbuff[n] = (BYTE)pn(0);
        printf(" disk_write(%u, 0x%X, %lu, 1)", pdrv, (UINT)pbuff, lba);
        dr = disk_write(pdrv, pbuff, lba, 1);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 6;
        }
        printf(" disk_ioctl(%u, CTRL_SYNC, NULL)", pdrv);
        dr = disk_ioctl(pdrv, CTRL_SYNC, 0);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 7;
        }
        memset(pbuff, 0, sz_sect);
        printf(" disk_read(%u, 0x%X, %lu, 1)", pdrv, (UINT)pbuff, lba);
        dr = disk_read(pdrv, pbuff, lba, 1);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 8;
        }
        for (n = 0, pn(pns); n < sz_sect && pbuff[n] == (BYTE)pn(0); n++) ;
        if (n == sz_sect) {
            printf(" Read data matched.\n");
        } else {
            printf(" Read data differs from the data written.\n");
            return 10;
        }
        pns++;

        printf("**** Multiple sector write test ****\n");
        lba = 5; ns = sz_buff / sz_sect;
        if (ns > 4) ns = 4;
        if (ns > 1) {
            for (n = 0, pn(pns); n < (UINT)(sz_sect * ns); n++) pbuff[n] = (BYTE)pn(0);
            printf(" disk_write(%u, 0x%X, %lu, %u)", pdrv, (UINT)pbuff, lba, ns);
            dr = disk_write(pdrv, pbuff, lba, ns);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 11;
            }
            printf(" disk_ioctl(%u, CTRL_SYNC, NULL)", pdrv);
            dr = disk_ioctl(pdrv, CTRL_SYNC, 0);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 12;
            }
            memset(pbuff, 0, sz_sect * ns);
            printf(" disk_read(%u, 0x%X, %lu, %u)", pdrv, (UINT)pbuff, lba, ns);
            dr = disk_read(pdrv, pbuff, lba, ns);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 13;
            }
            for (n = 0, pn(pns); n < (UINT)(sz_sect * ns) && pbuff[n] == (BYTE)pn(0); n++) ;
            if (n == (UINT)(sz_sect * ns)) {
                printf(" Read data matched.\n");
            } else {
                printf(" Read data differs from the data written.\n");
                return 14;
            }
        } else {
            printf(" Test skipped.\n");
        }
        pns++;

        printf("**** Single sector write test (unaligned buffer address) ****\n");
        lba = 5;
        for (n = 0, pn(pns); n < sz_sect; n++) pbuff[n+3] = (BYTE)pn(0);
        printf(" disk_write(%u, 0x%X, %lu, 1)", pdrv, (UINT)(pbuff+3), lba);
        dr = disk_write(pdrv, pbuff+3, lba, 1);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 15;
        }
        printf(" disk_ioctl(%u, CTRL_SYNC, NULL)", pdrv);
        dr = disk_ioctl(pdrv, CTRL_SYNC, 0);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 16;
        }
        memset(pbuff+5, 0, sz_sect);
        printf(" disk_read(%u, 0x%X, %lu, 1)", pdrv, (UINT)(pbuff+5), lba);
        dr = disk_read(pdrv, pbuff+5, lba, 1);
        if (dr == RES_OK) {
            printf(" - ok.\n");
        } else {
            printf(" - failed.\n");
            return 17;
        }
        for (n = 0, pn(pns); n < sz_sect && pbuff[n+5] == (BYTE)pn(0); n++) ;
        if (n == sz_sect) {
            printf(" Read data matched.\n");
        } else {
            printf(" Read data differs from the data written.\n");
            return 18;
        }
        pns++;

        printf("**** 4GB barrier test ****\n");
        if (sz_drv >= 128 + 0x80000000 / (sz_sect / 2)) {
            lba = 6; lba2 = lba + 0x80000000 / (sz_sect / 2);
            for (n = 0, pn(pns); n < (UINT)(sz_sect * 2); n++) pbuff[n] = (BYTE)pn(0);
            printf(" disk_write(%u, 0x%X, %lu, 1)", pdrv, (UINT)pbuff, lba);
            dr = disk_write(pdrv, pbuff, lba, 1);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 19;
            }
            printf(" disk_write(%u, 0x%X, %lu, 1)", pdrv, (UINT)(pbuff+sz_sect), lba2);
            dr = disk_write(pdrv, pbuff+sz_sect, lba2, 1);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 20;
            }
            printf(" disk_ioctl(%u, CTRL_SYNC, NULL)", pdrv);
            dr = disk_ioctl(pdrv, CTRL_SYNC, 0);
            if (dr == RES_OK) {
            printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 21;
            }
            memset(pbuff, 0, sz_sect * 2);
            printf(" disk_read(%u, 0x%X, %lu, 1)", pdrv, (UINT)pbuff, lba);
            dr = disk_read(pdrv, pbuff, lba, 1);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 22;
            }
            printf(" disk_read(%u, 0x%X, %lu, 1)", pdrv, (UINT)(pbuff+sz_sect), lba2);
            dr = disk_read(pdrv, pbuff+sz_sect, lba2, 1);
            if (dr == RES_OK) {
                printf(" - ok.\n");
            } else {
                printf(" - failed.\n");
                return 23;
            }
            for (n = 0, pn(pns); pbuff[n] == (BYTE)pn(0) && n < (UINT)(sz_sect * 2); n++) ;
            if (n == (UINT)(sz_sect * 2)) {
                printf(" Read data matched.\n");
            } else {
                printf(" Read data differs from the data written.\n");
                return 24;
            }
        } else {
            printf(" Test skipped.\n");
        }
        pns++;

        printf("**** Test cycle %u of %u completed ****\n\n", cc, ncyc);
    }

    return 0;
}



int main ()
{
    stdio_init_all();

    //enable spi peripheral, configure pins
    init_sd_connection();
    
    int rc;
    DWORD buff[FF_MAX_SS];  /* Working buffer (4 sector in size) */
    
    printf("testing\n");
    /* Check function/compatibility of the physical drive #0 */
    rc = test_diskio(0, 3, buff, sizeof buff);

    if (rc) {
        printf("Sorry the function/compatibility test failed. (rc=%d)\nFatFs will not work with this disk driver.\n", rc);
    } else {
        printf("Congratulations! The disk driver works well.\n");
    }

    return rc;
}