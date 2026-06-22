/* Includes */
#include "app_rgbLedRing.h"

#include "FreeRTOS.h"
#include "task.h"

#include "IO_SK6805.h"
#include "IO_AS5048.h"
#include "DEV_switch.h"

/* Private Data Definitions */

static const app_rgbLedRing_config_S * appConfig = NULL;

// One UI state per configured ring. Sized to the consumer's channel count.
static app_rgbLedRing_state_S ringState[APP_RGBLEDRING_CHANNEL_COUNT];

/* Private Function Definitions — rendering helpers (pure) */

// Brightness (0..1) a pip centred at angle_deg contributes to ledIndex, by
// circular angular distance with a linear falloff out to the pip half-width.
static float32_t pipBrightness(float32_t angle_deg, uint16_t ledIndex, uint16_t ledCount)
{
    const float32_t ledsPerRev = (float32_t)ledCount;
    const float32_t degPerLed  = 360.0f / ledsPerRev;
    const float32_t center     = angle_deg / degPerLed;   // pip centre in LED units

    float32_t dist = (float32_t)ledIndex - center;
    if (dist < 0.0f)                { dist = -dist; }
    if (dist > (ledsPerRev * 0.5f)) { dist = ledsPerRev - dist; }  // wrap the short way

    const float32_t halfWidthLeds = APP_RGBLEDRING_PIP_HALF_WIDTH_DEG / degPerLed;
    float32_t brightness = 0.0f;
    if (dist < halfWidthLeds)
    {
        brightness = 1.0f - (dist / halfWidthLeds);
    }
    return brightness;
}

// HSV -> RGB. h in [0,360) deg, s and v in [0,1]; each output channel is scaled
// to [0, maxLevel].
static void hsvToRgb(float32_t h, float32_t s, float32_t v, uint8_t maxLevel,
                     uint8_t * r, uint8_t * g, uint8_t * b)
{
    const float32_t chroma = v * s;
    const float32_t hp     = h / 60.0f;          // hue sector position, 0..6

    float32_t hpMod2 = hp;
    while (hpMod2 >= 2.0f) { hpMod2 -= 2.0f; }
    float32_t tri = hpMod2 - 1.0f;
    if (tri < 0.0f) { tri = -tri; }
    const float32_t second = chroma * (1.0f - tri);   // the "x" component
    const float32_t base   = v - chroma;              // the "m" offset

    float32_t rp = 0.0f;
    float32_t gp = 0.0f;
    float32_t bp = 0.0f;
    if      (hp < 1.0f) { rp = chroma; gp = second; }
    else if (hp < 2.0f) { rp = second; gp = chroma; }
    else if (hp < 3.0f) { gp = chroma; bp = second; }
    else if (hp < 4.0f) { gp = second; bp = chroma; }
    else if (hp < 5.0f) { rp = second; bp = chroma; }
    else                { rp = chroma; bp = second; }

    const float32_t scale = (float32_t)maxLevel;
    *r = (uint8_t)((rp + base) * scale);
    *g = (uint8_t)((gp + base) * scale);
    *b = (uint8_t)((bp + base) * scale);
}

// Advance one signed walk-head: fold this frame's encoder movement into a
// sticky, clamped speed accumulator (centred on zero) and integrate the
// resulting signed speed into a ring position, wrapped into [0, ledCount).
static void walkAdvance(float32_t * accum, float32_t * pos, float32_t delta, uint16_t ledCount)
{
    *accum += delta;
    if (*accum >  APP_RGBLEDRING_WALK_ACCUM_RANGE_DEG) { *accum =  APP_RGBLEDRING_WALK_ACCUM_RANGE_DEG; }
    if (*accum < -APP_RGBLEDRING_WALK_ACCUM_RANGE_DEG) { *accum = -APP_RGBLEDRING_WALK_ACCUM_RANGE_DEG; }

    const float32_t frac      = *accum / APP_RGBLEDRING_WALK_ACCUM_RANGE_DEG;   // -1..+1
    const float32_t speedDps  = frac * APP_RGBLEDRING_WALK_SPEED_MAX_DPS;       // signed
    const float32_t degPerLed = 360.0f / (float32_t)ledCount;

    *pos += (speedDps / degPerLed) * APP_RGBLEDRING_FRAME_DT_S;                 // signed LED units
    while (*pos >= (float32_t)ledCount) { *pos -= (float32_t)ledCount; }
    while (*pos < 0.0f)                 { *pos += (float32_t)ledCount; }
}

// Clamp an additive 0..255+ channel sum to a uint8_t.
static uint8_t clampChannel(uint16_t value)
{
    uint16_t clamped = value;
    if (clamped > 255U) { clamped = 255U; }
    return (uint8_t)clamped;
}

/* Public Function Definitions — rendering core */

void app_rgbLedRing_renderInit(app_rgbLedRing_state_S * state)
{
    state->mode       = APP_RGBLEDRING_MODE_WALK;
    state->prevButton = false;

    state->walkPos    = 0.0f;
    state->walkPos2   = 0.0f;
    state->dialAccum  = 0.0f;
    state->motorAccum = 0.0f;

    state->satAccum   = APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG;  // start full
    state->satAccum2  = APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG;
    state->hueAccum   = 0.0f;                                // start red
    state->hueAccum2  = 240.0f;                              // start blue

    state->pickR  = (uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS;  // default red
    state->pickG  = 0U;
    state->pickB  = 0U;
    state->pick2R = 0U;
    state->pick2G = 0U;
    state->pick2B = (uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS;  // default blue

    state->prevDial  = 0.0f;
    state->prevMotor = 0.0f;
}

void app_rgbLedRing_seedEncoders(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg)
{
    state->prevDial  = dialDeg;
    state->prevMotor = motorDeg;
}

// [impl->fw~obs_ring_001~1]
bool app_rgbLedRing_advanceMode(app_rgbLedRing_state_S * state, bool buttonActive)
{
    bool changed = false;
    if (buttonActive && (!state->prevButton))
    {
        state->mode = (app_rgbLedRing_mode_E)(((uint32_t)state->mode + 1U) % (uint32_t)APP_RGBLEDRING_MODE_COUNT);
        changed = true;
    }
    state->prevButton = buttonActive;
    return changed;
}

// [impl->fw~obs_ring_002~1]
void app_rgbLedRing_renderFrame(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg,
                                app_rgbLedRing_rgb_S * pixels, uint16_t ledCount)
{
    // Track dial and motor movement every frame (wrapped to +/-180 deg) so the
    // deltas are always current and switching modes never injects a jump.
    float32_t dialDelta = dialDeg - state->prevDial;
    if (dialDelta > 180.0f)       { dialDelta -= 360.0f; }
    else if (dialDelta < -180.0f) { dialDelta += 360.0f; }
    state->prevDial = dialDeg;

    float32_t motorDelta = motorDeg - state->prevMotor;
    if (motorDelta > 180.0f)       { motorDelta -= 360.0f; }
    else if (motorDelta < -180.0f) { motorDelta += 360.0f; }
    state->prevMotor = motorDeg;

    switch (state->mode)
    {
        case APP_RGBLEDRING_MODE_WALK:
        {
            // Two signed walk-heads: dial drives the first, motor the second.
            walkAdvance(&state->dialAccum,  &state->walkPos,  dialDelta,  ledCount);
            walkAdvance(&state->motorAccum, &state->walkPos2, motorDelta, ledCount);

            const float32_t degPerLed  = 360.0f / (float32_t)ledCount;
            const float32_t walkAngle  = state->walkPos  * degPerLed;
            const float32_t walkAngle2 = state->walkPos2 * degPerLed;

            for (uint16_t i = 0U; i < ledCount; i++)
            {
                const float32_t bright1 = pipBrightness(walkAngle,  i, ledCount);
                const float32_t bright2 = pipBrightness(walkAngle2, i, ledCount);

                pixels[i].red   = clampChannel((uint16_t)(bright1 * (float32_t)state->pickR) + (uint16_t)(bright2 * (float32_t)state->pick2R));
                pixels[i].green = clampChannel((uint16_t)(bright1 * (float32_t)state->pickG) + (uint16_t)(bright2 * (float32_t)state->pick2G));
                pixels[i].blue  = clampChannel((uint16_t)(bright1 * (float32_t)state->pickB) + (uint16_t)(bright2 * (float32_t)state->pick2B));
            }
            break;
        }

        case APP_RGBLEDRING_MODE_SOLID:
        {
            // [impl->fw~obs_ring_003~1] The picker accumulators live in the
            // persistent state and only the active mode updates them, so a
            // picked colour survives switching to another mode and back.
            state->hueAccum += motorDelta;
            while (state->hueAccum >= 360.0f) { state->hueAccum -= 360.0f; }
            while (state->hueAccum < 0.0f)    { state->hueAccum += 360.0f; }

            state->satAccum += dialDelta;
            if (state->satAccum < 0.0f)                              { state->satAccum = 0.0f; }
            if (state->satAccum > APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG) { state->satAccum = APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG; }
            const float32_t saturation = state->satAccum / APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG;

            hsvToRgb(state->hueAccum, saturation, 1.0f, (uint8_t)(APP_RGBLEDRING_PIP_MAX_BRIGHTNESS / 2U),
                     &state->pickR, &state->pickG, &state->pickB);

            for (uint16_t i = 0U; i < ledCount; i++)
            {
                pixels[i].red   = state->pickR;
                pixels[i].green = state->pickG;
                pixels[i].blue  = state->pickB;
            }
            break;
        }

        case APP_RGBLEDRING_MODE_SOLID2:
        {
            // [impl->fw~obs_ring_003~1] Second picker, identical controls,
            // independent persistent colour.
            state->hueAccum2 += motorDelta;
            while (state->hueAccum2 >= 360.0f) { state->hueAccum2 -= 360.0f; }
            while (state->hueAccum2 < 0.0f)    { state->hueAccum2 += 360.0f; }

            state->satAccum2 += dialDelta;
            if (state->satAccum2 < 0.0f)                              { state->satAccum2 = 0.0f; }
            if (state->satAccum2 > APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG) { state->satAccum2 = APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG; }
            const float32_t saturation = state->satAccum2 / APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG;

            hsvToRgb(state->hueAccum2, saturation, 1.0f, (uint8_t)(APP_RGBLEDRING_PIP_MAX_BRIGHTNESS / 2U),
                     &state->pick2R, &state->pick2G, &state->pick2B);

            for (uint16_t i = 0U; i < ledCount; i++)
            {
                pixels[i].red   = state->pick2R;
                pixels[i].green = state->pick2G;
                pixels[i].blue  = state->pick2B;
            }
            break;
        }

        case APP_RGBLEDRING_MODE_ENCODER:
        {
            // One pip per encoder: dial in the SOLID colour, motor in SOLID2.
            for (uint16_t i = 0U; i < ledCount; i++)
            {
                const float32_t brightDial  = pipBrightness(dialDeg,  i, ledCount);
                const float32_t brightMotor = pipBrightness(motorDeg, i, ledCount);

                pixels[i].red   = clampChannel((uint16_t)(brightDial * (float32_t)state->pickR) + (uint16_t)(brightMotor * (float32_t)state->pick2R));
                pixels[i].green = clampChannel((uint16_t)(brightDial * (float32_t)state->pickG) + (uint16_t)(brightMotor * (float32_t)state->pick2G));
                pixels[i].blue  = clampChannel((uint16_t)(brightDial * (float32_t)state->pickB) + (uint16_t)(brightMotor * (float32_t)state->pick2B));
            }
            break;
        }

        case APP_RGBLEDRING_MODE_OFF:
        default:
        {
            for (uint16_t i = 0U; i < ledCount; i++)
            {
                pixels[i].red   = 0U;
                pixels[i].green = 0U;
                pixels[i].blue  = 0U;
            }
            break;
        }
    }
}

/* Private Function Definitions — RTOS/IO shell */

// Flash a ring white (MODE_BLINK_MS on/off) as a mode-change confirmation.
// Blocks the task for the flash; other tasks keep running. Each push is wrapped
// in vTaskSuspendAll() so the SK6805 stream isn't preempted mid-transfer (the
// real fix is DMA; this is the prototype path).
static void blinkModeChange(IO_SK6805_channel_E ledChannel)
{
    const uint8_t white = (uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS / 10U;
    for (uint8_t flash = 0U; flash < 1U; flash++)
    {
        IO_SK6805_setAll(ledChannel, white, white, white);
        vTaskSuspendAll();
        (void)IO_SK6805_update(ledChannel);
        (void)xTaskResumeAll();
        vTaskDelay(pdMS_TO_TICKS(APP_RGBLEDRING_MODE_BLINK_MS));

        IO_SK6805_clear(ledChannel);
        vTaskSuspendAll();
        (void)IO_SK6805_update(ledChannel);
        (void)xTaskResumeAll();
        vTaskDelay(pdMS_TO_TICKS(APP_RGBLEDRING_MODE_BLINK_MS));
    }
}

// Read a channel's latest cached angle (degrees); 0 if the read fails.
static float32_t readAngleDeg(IO_AS5048_channel_E channel)
{
    float32_t deg = 0.0f;
    (void)IO_AS5048_readAngle(channel, NULL, &deg);
    return deg;
}

// [impl->fw~obs_ring_001~1]
static void app_rgbLedRing_task(void * params)
{
    (void)params;

    // Seed each ring's encoder references so the first frame's deltas are ~0.
    for (size_t ch = 0U; ch < appConfig->numChannels; ch++)
    {
        const app_rgbLedRing_channelConfig_S * const cfg = &appConfig->channels[ch];
        app_rgbLedRing_seedEncoders(&ringState[ch], readAngleDeg(cfg->dialChannel), readAngleDeg(cfg->motorChannel));
    }

    app_rgbLedRing_rgb_S pixels[APP_RGBLEDRING_MAX_PIXELS];

    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        for (size_t ch = 0U; ch < appConfig->numChannels; ch++)
        {
            const app_rgbLedRing_channelConfig_S * const cfg = &appConfig->channels[ch];

            // Advance this ring's mode on a button press (rising edge), confirmed
            // by a white flash.
            const bool button = DEV_switch_isActive(cfg->buttonChannel);
            if (app_rgbLedRing_advanceMode(&ringState[ch], button))
            {
                blinkModeChange(cfg->ledChannel);
                // Re-anchor the cadence: the blink blocked past several frames.
                lastWake = xTaskGetTickCount();
            }

            const float32_t dialDeg  = readAngleDeg(cfg->dialChannel);
            const float32_t motorDeg = readAngleDeg(cfg->motorChannel);

            app_rgbLedRing_renderFrame(&ringState[ch], dialDeg, motorDeg, pixels, cfg->pixelCount);

            for (uint16_t i = 0U; i < cfg->pixelCount; i++)
            {
                IO_SK6805_setPixel(cfg->ledChannel, i, pixels[i].red, pixels[i].green, pixels[i].blue);
            }

            // The SK6805 is a continuous timed stream: a preemption mid-transfer
            // underruns the SPI FIFO and corrupts the frame. Suspend task switches
            // across the transmit so the stream stays intact (DMA removes this).
            vTaskSuspendAll();
            (void)IO_SK6805_update(cfg->ledChannel);
            (void)xTaskResumeAll();
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(APP_RGBLEDRING_FRAME_PERIOD_MS));
    }
}

/* Public Function Definitions — lifecycle */

bool app_rgbLedRing_init(const app_rgbLedRing_config_S * const config, uint32_t taskPriority)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= APP_RGBLEDRING_CHANNEL_COUNT))
    {
        bool channelsValid = true;
        for (size_t ch = 0U; ch < config->numChannels; ch++)
        {
            const app_rgbLedRing_channelConfig_S * const cfg = &config->channels[ch];
            channelsValid &= (cfg->ledChannel < IO_SK6805_CHANNEL_COUNT);
            channelsValid &= (cfg->dialChannel < IO_AS5048_CHANNEL_COUNT);
            channelsValid &= (cfg->motorChannel < IO_AS5048_CHANNEL_COUNT);
            channelsValid &= (cfg->buttonChannel < DEV_SWITCH_CHANNEL_COUNT);
            channelsValid &= (cfg->pixelCount > 0U);
            channelsValid &= (cfg->pixelCount <= APP_RGBLEDRING_MAX_PIXELS);
        }

        if (channelsValid)
        {
            appConfig = config;
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                app_rgbLedRing_renderInit(&ringState[ch]);
            }
            success = (xTaskCreate(app_rgbLedRing_task, "led", configMINIMAL_STACK_SIZE * 2U,
                                   NULL, (UBaseType_t)taskPriority, NULL) == pdPASS);
        }
    }
    return success;
}
