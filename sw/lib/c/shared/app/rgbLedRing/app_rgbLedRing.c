/* Includes */
#include "app_rgbLedRing.h"

#include "IO_SK6805.h"
#include "IO_AS5048.h"

/* Defines */

// Mode-change confirmation flash, spread across 10 ms ticks: white for the
// first half, off for the second (originally a blocking blink; now cooperative
// so run10ms never stalls the shared task).
#define APP_RGBLEDRING_BLINK_HALF_TICKS  (APP_RGBLEDRING_MODE_BLINK_MS / APP_RGBLEDRING_FRAME_PERIOD_MS)
#define APP_RGBLEDRING_BLINK_TOTAL_TICKS (2U * APP_RGBLEDRING_BLINK_HALF_TICKS)

// SCAFFOLD speed-estimate low-pass factor (0..1) and deg->rad. Temporary — see
// the renderFrame estimate note.
#define APP_RGBLEDRING_SPEED_FILTER_ALPHA (0.2f)
#define APP_RGBLEDRING_RAD_PER_DEG        (3.14159265f / 180.0f)

/* Private Data Definitions */

static const app_rgbLedRing_config_S * appConfig = NULL;

// One UI state per configured ring. Sized to the consumer's channel count.
static app_rgbLedRing_state_S ringState[APP_RGBLEDRING_CHANNEL_COUNT];

// Ticks left in each ring's mode-change flash (0 = not flashing).
static uint8_t blinkTicksLeft[APP_RGBLEDRING_CHANNEL_COUNT];

// Pending cycle-view requests. cycleMode (1 ms user-controls task) only sets
// the flag; run10ms (10 ms task) applies it, so ringState is mutated by a
// single task. volatile single-word access is the cross-task handoff.
static volatile bool cyclePending[APP_RGBLEDRING_CHANNEL_COUNT];

// Encoder references are seeded on the first run10ms, once the 1 ms task has
// taken at least one AS5048 sample, so the first frame's deltas are ~0.
static bool encodersSeeded = false;

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

// Clamp an additive 0..255+ channel sum to a uint8_t.
static uint8_t clampChannel(uint16_t value)
{
    uint16_t clamped = value;
    if (clamped > 255U) { clamped = 255U; }
    return (uint8_t)clamped;
}

// Complement a physical angle into LED-ring space: the ring's LED order runs
// opposite the dial/motor sense, so a physical angle maps to (360 - angle).
static float32_t ringAngle(float32_t deg)
{
    float32_t a = 360.0f - deg;
    while (a >= 360.0f) { a -= 360.0f; }
    while (a < 0.0f)    { a += 360.0f; }
    return a;
}

// Add one pip (an RGB colour faded by pip proximity) into the frame.
static void drawPip(app_rgbLedRing_rgb_S * pixels, uint16_t ledCount, float32_t ringAngle_deg,
                    uint8_t r, uint8_t g, uint8_t b)
{
    for (uint16_t i = 0U; i < ledCount; i++)
    {
        const float32_t bright = pipBrightness(ringAngle_deg, i, ledCount);
        pixels[i].red   = clampChannel((uint16_t)pixels[i].red   + (uint16_t)(bright * (float32_t)r));
        pixels[i].green = clampChannel((uint16_t)pixels[i].green + (uint16_t)(bright * (float32_t)g));
        pixels[i].blue  = clampChannel((uint16_t)pixels[i].blue  + (uint16_t)(bright * (float32_t)b));
    }
}

// Fill the whole ring one colour.
static void fillRing(app_rgbLedRing_rgb_S * pixels, uint16_t ledCount, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint16_t i = 0U; i < ledCount; i++)
    {
        pixels[i].red = r; pixels[i].green = g; pixels[i].blue = b;
    }
}

/* Public Function Definitions — rendering core */

void app_rgbLedRing_renderInit(app_rgbLedRing_state_S * state)
{
    state->mode              = APP_RGBLEDRING_MODE_POSITION;
    state->prevDial          = 0.0f;
    state->prevMotor         = 0.0f;
    state->actualSpeedEst_dps = 0.0f;
}

void app_rgbLedRing_seedEncoders(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg)
{
    state->prevDial  = dialDeg;
    state->prevMotor = motorDeg;
}

// [impl->fw~obs_ring_001~1]
void app_rgbLedRing_advanceMode(app_rgbLedRing_state_S * state)
{
    state->mode = (app_rgbLedRing_mode_E)(((uint32_t)state->mode + 1U) % (uint32_t)APP_RGBLEDRING_MODE_COUNT);
}

// Map a signed, full-scale-normalized speed (-1..+1) to a ring angle: zero at
// top, +/-full sweeping +/-SPEEDO_SWEEP_DEG, complemented into the reversed LED
// order (like the position pips) so the sweep matches physical rotation.
static float32_t speedoRingAngle(float32_t norm)
{
    if (norm >  1.0f) { norm =  1.0f; }
    if (norm < -1.0f) { norm = -1.0f; }
    return ringAngle(norm * APP_RGBLEDRING_SPEEDO_SWEEP_DEG);
}

// [impl->fw~obs_ring_002~1]
void app_rgbLedRing_renderFrame(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg,
                                const app_motorControl_snapshot_S * motor, float32_t speedoFullScale_radPerSec,
                                app_rgbLedRing_rgb_S * pixels, uint16_t ledCount)
{
    // Motor movement this frame, wrapped to +/-180 deg.
    float32_t motorDelta = motorDeg - state->prevMotor;
    if (motorDelta > 180.0f)       { motorDelta -= 360.0f; }
    else if (motorDelta < -180.0f) { motorDelta += 360.0f; }
    state->prevMotor = motorDeg;
    state->prevDial  = dialDeg;

    // SCAFFOLD actual-speed estimate: raw encoder rate, low-pass filtered.
    // TEMPORARY — belongs in app_motorControl (or an estimator app) as a
    // properly filtered measured speed; this is a stand-in for the speedometer.
    const float32_t instSpeed_dps = motorDelta / APP_RGBLEDRING_FRAME_DT_S;
    state->actualSpeedEst_dps += APP_RGBLEDRING_SPEED_FILTER_ALPHA * (instSpeed_dps - state->actualSpeedEst_dps);

    const uint8_t lvl = (uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS;
    const app_motorControl_state_E motorState = (motor != NULL) ? motor->state : APP_MOTORCONTROL_STATE_DISABLED;

    fillRing(pixels, ledCount, 0U, 0U, 0U);

    // [impl->fw~mc_009~1] Bridge-state indication: faulted / disabled / running
    // are distinguishable on the ring (running renders the active view below).
    if (motorState == APP_MOTORCONTROL_STATE_FAULTED)
    {
        fillRing(pixels, ledCount, lvl, 0U, 0U);            // red takeover
    }
    else if (motorState == APP_MOTORCONTROL_STATE_DISABLED)
    {
        // stays blank
    }
    else if (state->mode == APP_RGBLEDRING_MODE_SPEEDO)
    {
        const float32_t fullScale = (speedoFullScale_radPerSec > 0.0f) ? speedoFullScale_radPerSec : 1.0f;
        const float32_t setNorm = ((motor != NULL) ? motor->velocitySetpoint_radPerSec : 0.0f) / fullScale;
        const float32_t actNorm = (state->actualSpeedEst_dps * APP_RGBLEDRING_RAD_PER_DEG) / fullScale;
        drawPip(pixels, ledCount, speedoRingAngle(setNorm), lvl, 0U, lvl);   // setpoint = magenta
        drawPip(pixels, ledCount, speedoRingAngle(actNorm), 0U, lvl, 0U);    // actual = green
    }
    else   // APP_RGBLEDRING_MODE_POSITION
    {
        drawPip(pixels, ledCount, ringAngle(motorDeg), 0U, lvl, 0U);   // motor = green
        drawPip(pixels, ledCount, ringAngle(dialDeg),  0U, 0U, lvl);   // dial = blue
    }
}

/* Private Function Definitions — RTOS/IO shell */

// Read a channel's latest cached angle (degrees); 0 if the read fails.
static float32_t readAngleDeg(IO_AS5048_channel_E channel)
{
    float32_t deg = 0.0f;
    (void)IO_AS5048_readAngle(channel, NULL, &deg, NULL);
    return deg;
}

// Transmit a ring's staged framebuffer. The SK6805 stream is DMA-backed (the
// LED bus runs in DMA transfer mode), so the transmit runs off-CPU and needs no
// scheduler guard — a task preemption can't underrun the SPI FIFO.
static void transmit(IO_SK6805_channel_E ledChannel)
{
    (void)IO_SK6805_update(ledChannel);
}

/* Public Function Definitions */

bool app_rgbLedRing_init(const app_rgbLedRing_config_S * const config)
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
            channelsValid &= (cfg->pixelCount > 0U);
            channelsValid &= (cfg->pixelCount <= APP_RGBLEDRING_MAX_PIXELS);
        }

        if (channelsValid)
        {
            appConfig = config;
            encodersSeeded = false;
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                app_rgbLedRing_renderInit(&ringState[ch]);
                blinkTicksLeft[ch] = 0U;
                cyclePending[ch] = false;
            }
            success = true;
        }
    }
    return success;
}

// [impl->fw~obs_ring_001~1]
void app_rgbLedRing_cycleMode(app_rgbLedRing_channel_E channel)
{
    if ((appConfig != NULL) && (channel < appConfig->numChannels))
    {
        cyclePending[channel] = true;
    }
}

// [impl->fw~obs_ring_001~1]
void app_rgbLedRing_run10ms(void)
{
    if (appConfig == NULL)
    {
        return;   // not initialised
    }

    if (!encodersSeeded)
    {
        for (size_t ch = 0U; ch < appConfig->numChannels; ch++)
        {
            const app_rgbLedRing_channelConfig_S * const cfg = &appConfig->channels[ch];
            app_rgbLedRing_seedEncoders(&ringState[ch], readAngleDeg(cfg->dialChannel), readAngleDeg(cfg->motorChannel));
        }
        encodersSeeded = true;
    }

    app_rgbLedRing_rgb_S pixels[APP_RGBLEDRING_MAX_PIXELS];

    for (size_t ch = 0U; ch < appConfig->numChannels; ch++)
    {
        const app_rgbLedRing_channelConfig_S * const cfg = &appConfig->channels[ch];

        // Apply a pending cycle-view request in this (the rendering) task.
        if (cyclePending[ch])
        {
            cyclePending[ch] = false;
            app_rgbLedRing_advanceMode(&ringState[ch]);
            blinkTicksLeft[ch] = APP_RGBLEDRING_BLINK_TOTAL_TICKS;
        }

        if (blinkTicksLeft[ch] > 0U)
        {
            // Cooperative mode-change flash: white for the first half, off for
            // the second. Encoder deltas are held (the mode isn't rendered)
            // until the flash finishes, matching the original blocking blink.
            const uint8_t white = (uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS / 10U;
            if (blinkTicksLeft[ch] > APP_RGBLEDRING_BLINK_HALF_TICKS)
            {
                IO_SK6805_setAll(cfg->ledChannel, white, white, white);
            }
            else
            {
                IO_SK6805_clear(cfg->ledChannel);
            }
            blinkTicksLeft[ch]--;
        }
        else
        {
            const float32_t dialDeg  = readAngleDeg(cfg->dialChannel);
            const float32_t motorDeg = readAngleDeg(cfg->motorChannel);

            app_motorControl_snapshot_S motor = { 0 };
            (void)app_motorControl_getSnapshot(cfg->motorControlChannel, &motor);

            app_rgbLedRing_renderFrame(&ringState[ch], dialDeg, motorDeg, &motor,
                                       cfg->speedoFullScale_radPerSec, pixels, cfg->pixelCount);

            for (uint16_t i = 0U; i < cfg->pixelCount; i++)
            {
                IO_SK6805_setPixel(cfg->ledChannel, i, pixels[i].red, pixels[i].green, pixels[i].blue);
            }
        }

        transmit(cfg->ledChannel);
    }
}
