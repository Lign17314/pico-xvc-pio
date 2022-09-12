#include "pio_xfer.h"
#include "string.h"
pio_xfer_inst_t xfer;
#define TEST_TMS

void pio_tms_set_period(PIO pio, uint sm, uint32_t num)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_put_blocking(pio, sm, (num % 32 == 0 ? 0 : (32 - num % 32)) << 16 | (num - 1));
    pio_sm_exec(pio, sm, pio_encode_pull(false, false));
    pio_sm_exec(pio, sm, pio_encode_out(pio_y, 16));
    pio_sm_exec(pio, sm, pio_encode_out(pio_x, 16));
}
int write_read_nbits(PIO pio, uint sm_data, uint sm_tms, uint32_t *tx_data, uint32_t *tx_tms, uint32_t *rx, uint16_t nbits)
{
    int i = 0;
    pio_tms_set_period(pio, sm_data, nbits);
#ifdef TEST_TMS
    pio_tms_set_period(pio, sm_tms, nbits);
#endif
#if 1
    pio_enable_sm_mask_in_sync(pio, ((1u << sm_data) | (1u << (sm_tms))));

    for (i = 0; i < (nbits + 31) / 32; i++)
    {
#ifdef TEST_TMS
        pio_sm_put_blocking(pio, sm_tms, *(tx_tms + i));
#endif
        pio_sm_put_blocking(pio, sm_data, *(tx_data + i));
#ifdef TEST_TMS
        *(rx + i) = pio_sm_get_blocking(pio, sm_tms);
#endif
        *(rx + i) = pio_sm_get_blocking(pio, sm_data);
    }

    return 0;
#endif
    if (nbits <= 32 * 4) // fifo is 4 * 32 bits
    {
        for (i = 0; i < (nbits + 31) / 32; i++)
        {
#ifdef TEST_TMS
            pio_sm_put_blocking(pio, sm_tms, *(tx_tms + i));
#endif
            pio_sm_put_blocking(pio, sm_data, *(tx_data + i));
        }
        pio_enable_sm_mask_in_sync(pio, ((1u << sm_data) | (1u << (sm_tms))));
        for (i = 0; i < (nbits + 31) / 32; i++)
        {
#ifdef TEST_TMS
            *(rx + i) = pio_sm_get_blocking(pio, sm_tms);
#endif
            *(rx + i) = pio_sm_get_blocking(pio, sm_data);
        }
    }
    else
    {
        for (i = 0; i < 4; i++)
        {
#ifdef TEST_TMS
            pio_sm_put_blocking(pio, sm_tms, *(tx_tms + i));
#endif
            pio_sm_put_blocking(pio, sm_data, *(tx_data + i));
        }
        pio_enable_sm_mask_in_sync(pio, ((1u << sm_data) | (1u << (sm_tms))));
        for (i = 0; i < (nbits + 31) / 32 - 4; i++)
        {
#ifdef TEST_TMS
            *(rx + i) = pio_sm_get_blocking(pio, sm_tms);
#endif
            *(rx + i) = pio_sm_get_blocking(pio, sm_data);
#ifdef TEST_TMS
            pio_sm_put_blocking(pio, sm_tms, *(tx_tms + i + 4));
#endif
            pio_sm_put_blocking(pio, sm_data, *(tx_data + i + 4));
        }
        for (i = (nbits + 31) / 32 - 4; i < (nbits + 31) / 32; i++)
        {
#ifdef TEST_TMS
            *(rx + i) = pio_sm_get_blocking(pio, sm_tms);
#endif
            *(rx + i) = pio_sm_get_blocking(pio, sm_data);
        }
    }
    return 0;
}

int pio_xfer_rw(uint32_t *tx_data, uint32_t *tx_tms, uint32_t *tdi, int nbits)
{
#ifdef USE_PIO
    return write_read_nbits(xfer.pio, xfer.sm_data, xfer.sm_tms, tx_data, tx_tms, tdi, nbits);
#else
    tdi = gpio_xfer(nbits, tx_tms);
    return 0;
#endif
}
int pio_xfer_init()
{
#ifdef USE_PIO
    xfer.pio = pio0;
    xfer.sm_data = 0;
    xfer.sm_tms = 1;
    xfer.tck_pin = PIN_SCK;
    xfer.tdi_pin = PIN_TDI;
    xfer.tdo_pin = PIN_TDO;
    xfer.tms_pin = PIN_TMS;

    float clkdiv = PIO_CLKDIV; // 1 MHz @ 125 clk_sys
    uint tdata_prog_offs = pio_add_program(xfer.pio, &tdata_program);
    pio_tdata_init(xfer.pio, xfer.sm_tms, tdata_prog_offs, clkdiv, 7, PIN_TMS, 8);
    // pio_tms_init(xfer.pio, xfer.sm_tms, tdata_prog_offs, clkdiv, PIN_TMS, PIN_SCK);
    pio_tdata_init(xfer.pio, xfer.sm_data, tdata_prog_offs, clkdiv, PIN_SCK, PIN_TDI, PIN_TDO);

    uint8_t *data = malloc(9);
    uint8_t *data1 = malloc(9);
    data[3] = 0x18;
    data1[3] = 0x33;
    uint16_t i = 4;
    do
    {
        pio_xfer_rw(data1, data1, data, i++);
        if (i == 10)
            i = 4;
        sleep_ms(1000);
    } while (0);
    free(data1);
    free(data);
    return 0;
#else
    gpio_xfer_init();
#endif
}

void gpio_xfer_init(void)
{
    gpio_set_dir(PIN_TMS, GPIO_OUT);
    gpio_set_dir(PIN_TDI, GPIO_OUT);
    gpio_set_dir(PIN_TDO, GPIO_IN);
    gpio_set_dir(PIN_SCK, GPIO_OUT);
    gpio_put(PIN_TMS, 0);
    gpio_put(PIN_SCK, 0);
    gpio_put(PIN_TDI, 0);
    gpio_get(PIN_TDO);
}
void jtag_write(uint8_t sck, uint8_t tms, uint8_t tdi)
{
    gpio_put(PIN_TMS, tms);
    gpio_put(PIN_SCK, sck);
    gpio_put(PIN_TDI, tdi);
    for (uint32_t i = 0; i < 1000; i++);
}
uint8_t jtag_read(void)
{
    return gpio_get(PIN_TDO) & 1;
}
uint32_t jtag_xfer(int nbits, uint32_t tms, uint32_t tdi)
{
    uint32_t tdo = 0;
    for (int i = 0; i < nbits; i++)
    {
        jtag_write(0, tms & 1, tdi & 1);
        tdo |= jtag_read() << i;
        jtag_write(1, tms & 1, tdi & 1);
        tms >>= 1;
        tdi >>= 1;
    }
    return tdo;
}
uint8_t *gpio_xfer(int len, uint8_t *buffer)
{
    int nr_bytes = (len + 7) / 8;
    int bytesLeft = nr_bytes;
    int bitsLeft = len;
    int byteIndex = 0;
    uint32_t tdi, tms, tdo;
    uint8_t *result = calloc(nr_bytes, 1);
    while (bytesLeft > 0)
    {
        tms = 0;
        tdi = 0;
        tdo = 0;
        if (bytesLeft >= 4)
        {
            memcpy(&tms, &buffer[byteIndex], 4);
            memcpy(&tdi, &buffer[byteIndex + nr_bytes], 4);
            tdo = jtag_xfer(32, tms, tdi);
            memcpy(&result[byteIndex], &tdo, 4);
            bytesLeft -= 4;
            bitsLeft -= 32;
            byteIndex += 4;
        }
        else
        {
            memcpy(&tms, &buffer[byteIndex], bytesLeft);
            memcpy(&tdi, &buffer[byteIndex + nr_bytes], bytesLeft);
            tdo = jtag_xfer(bitsLeft, tms, tdi);
            memcpy(&result[byteIndex], &tdo, bytesLeft);
            bytesLeft = 0;
            break;
        }
    }
}