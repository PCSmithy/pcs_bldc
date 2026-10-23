#include "IO_AS5048.h"
#include "mock_IO_AS5048.h"

static float32_t          mockAngle_rad[IO_AS5048_CHANNEL_COUNT];
static IO_AS5048_status_E mockStatus[IO_AS5048_CHANNEL_COUNT];

void mock_IO_AS5048_reset(void)
{
    for (uint32_t ch = 0U; ch < IO_AS5048_CHANNEL_COUNT; ch++)
    {
        mockAngle_rad[ch] = 0.0f;
        mockStatus[ch] = IO_AS5048_STATUS_OK;
    }
}

void mock_IO_AS5048_setAngle(IO_AS5048_channel_E channel, float32_t angle_rad)
{
    if (channel < IO_AS5048_CHANNEL_COUNT)
    {
        mockAngle_rad[channel] = angle_rad;
    }
}

void mock_IO_AS5048_setStatus(IO_AS5048_channel_E channel, IO_AS5048_status_E status)
{
    if (channel < IO_AS5048_CHANNEL_COUNT)
    {
        mockStatus[channel] = status;
    }
}

/* ---- mocked IO_AS5048 surface ---- */

// app_motorControl reads only the radian output; raw/deg are accepted for
// signature parity and written when non-NULL.
bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg, float32_t * angle_rad)
{
    bool ret = false;
    if (channel < IO_AS5048_CHANNEL_COUNT)
    {
        if (angleRaw != NULL) { *angleRaw = 0U; }
        if (angle_deg != NULL) { *angle_deg = mockAngle_rad[channel] * (180.0f / 3.14159265f); }
        if (angle_rad != NULL) { *angle_rad = mockAngle_rad[channel]; }
        ret = true;
    }
    return ret;
}

bool IO_AS5048_getStatus(IO_AS5048_channel_E channel, IO_AS5048_status_E * const out)
{
    bool ret = false;
    if ((out != NULL) && (channel < IO_AS5048_CHANNEL_COUNT))
    {
        *out = mockStatus[channel];
        ret = true;
    }
    return ret;
}
