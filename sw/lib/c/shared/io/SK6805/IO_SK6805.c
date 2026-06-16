/* Includes */
#include "IO_SK6805.h"
#include "HW_SPI.h"

/* Defines */

// 6-SPI-bit code per LED bit (MSB first): high then low.
//   '0' -> 0b110000 (T0H = 2 bits, T0L = 4 bits)
//   '1' -> 0b111000 (T1H = 3 bits, T1L = 3 bits)
#define SK6805_CODE_0  0x30U
#define SK6805_CODE_1  0x38U

#define SK6805_CODE_BITS  6U

/* Typedefs */

typedef struct
{
    const IO_SK6805_config_S * config;

    uint8_t frame[IO_SK6805_PIXEL_COUNT][SK6805_COLORS_PER_PIXEL]; // GRB
    uint8_t txBuf[IO_SK6805_TXBUF_BYTES];
} IO_SK6805_data_S;

/* Private Data Definitions */

static IO_SK6805_data_S IO_SK6805_data;
static IO_SK6805_data_S * const data = &IO_SK6805_data;

/* Private Function Declarations */

static void IO_SK6805_private_expandByte(uint8_t value, uint8_t * out);

/* Private Function Definitions */

// Expand one 8-bit colour value into 6 SPI bytes (48 bits, MSB first): each
// input bit becomes a 6-bit SK6805 code.
static void IO_SK6805_private_expandByte(uint8_t value, uint8_t * out)
{
    uint64_t acc = 0U;
    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
        const uint8_t inBit = (uint8_t)((value >> (7U - bit)) & 0x01U);
        const uint8_t code  = (inBit != 0U) ? SK6805_CODE_1 : SK6805_CODE_0;
        acc = (acc << SK6805_CODE_BITS) | (uint64_t)code;
    }

    for (uint8_t b = 0U; b < SK6805_SPI_BYTES_PER_COLOR; b++)
    {
        out[b] = (uint8_t)((acc >> (40U - (b * 8U))) & 0xFFU);
    }
}

/* Public Function Definitions */

bool IO_SK6805_init(const IO_SK6805_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        // Framebuffer and the trailing reset region start zeroed (.bss), so
        // the string defaults to off and the reset gap is always low.
        data->config = config;
        ret = (config->spiChannel < HW_SPI_CHANNEL_COUNT);
    }
    return ret;
}

void IO_SK6805_setPixel(uint16_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (index < IO_SK6805_PIXEL_COUNT)
    {
        data->frame[index][0] = green; // SK6805 wire order is GRB
        data->frame[index][1] = red;
        data->frame[index][2] = blue;
    }
}

void IO_SK6805_setAll(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint16_t index = 0U; index < IO_SK6805_PIXEL_COUNT; index++)
    {
        IO_SK6805_setPixel(index, red, green, blue);
    }
}

void IO_SK6805_clear(void)
{
    IO_SK6805_setAll(0U, 0U, 0U);
}

bool IO_SK6805_update(void)
{
    bool ret = false;
    if (data->config != NULL)
    {
        size_t outIdx = 0U;
        for (uint16_t px = 0U; px < IO_SK6805_PIXEL_COUNT; px++)
        {
            for (uint8_t colour = 0U; colour < SK6805_COLORS_PER_PIXEL; colour++)
            {
                IO_SK6805_private_expandByte(data->frame[px][colour], &data->txBuf[outIdx]);
                outIdx += SK6805_SPI_BYTES_PER_COLOR;
            }
        }

        // On boards with an inverting level shifter, complement the data so the
        // wire sees the correct codes. Either way drive the trailing reset gap
        // to the wire-low level (MOSI high when inverting) to latch the frame.
        if (data->config->invert)
        {
            for (size_t i = 0U; i < outIdx; i++)
            {
                data->txBuf[i] = (uint8_t)(~data->txBuf[i]);
            }
        }
        const uint8_t resetByte = (data->config->invert) ? 0xFFU : 0x00U;
        for (size_t i = outIdx; i < sizeof(data->txBuf); i++)
        {
            data->txBuf[i] = resetByte;
        }

        ret = HW_SPI_transmit(data->config->spiChannel, data->txBuf, sizeof(data->txBuf));
    }
    return ret;
}
