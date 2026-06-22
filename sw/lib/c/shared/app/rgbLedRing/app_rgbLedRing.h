#ifndef APP_RGBLEDRING_H
#define APP_RGBLEDRING_H

/* Includes */
#include "lib_types.h"
#include "IO_SK6805.h"               // IO_SK6805_channel_E
#include "IO_AS5048.h"               // IO_AS5048_channel_E
#include "DEV_switch.h"              // DEV_switch_channel_E
#include "app_rgbLedRing_channels.h" // consumer-provided app_rgbLedRing_channel_E

/* Defines */

// Pip tuning. A pip is spread over +/-PIP_HALF_WIDTH_DEG each side of its
// angle with a linear falloff; PIP_MAX_BRIGHTNESS keeps it dim.
#define APP_RGBLEDRING_PIP_HALF_WIDTH_DEG    18.0f
#define APP_RGBLEDRING_PIP_MAX_BRIGHTNESS    50U

// WALK-mode speed control: a signed, sticky, clamped accumulator of encoder
// movement centred on zero. ~ACCUM_RANGE_DEG of travel each side sweeps stop ->
// full speed (+/-SPEED_MAX_DPS).
#define APP_RGBLEDRING_WALK_SPEED_MAX_DPS    2880.0f
#define APP_RGBLEDRING_WALK_ACCUM_RANGE_DEG  340.0f

// Colour-picker saturation: dial movement over ~SAT_ACCUM_RANGE_DEG sweeps
// white -> full.
#define APP_RGBLEDRING_SAT_ACCUM_RANGE_DEG   180.0f

// Frame cadence. The frame dt is derived from the period so the two can't
// drift apart (WALK speed depends on it).
#define APP_RGBLEDRING_FRAME_PERIOD_MS       10U
#define APP_RGBLEDRING_FRAME_DT_S            ((float32_t)APP_RGBLEDRING_FRAME_PERIOD_MS / 1000.0f)

// Mode-change confirmation blink half-period.
#define APP_RGBLEDRING_MODE_BLINK_MS         50U

// Largest ring the UI buffers. Per-ring pixel counts come from the channel
// config; a channel with pixelCount above this is rejected at init.
#define APP_RGBLEDRING_MAX_PIXELS            64U

/* Typedefs */

typedef enum
{
    APP_RGBLEDRING_MODE_WALK,     // two pips walk the ring; dial speeds one, motor the other
    APP_RGBLEDRING_MODE_SOLID,    // two-encoder HSV colour picker (motor=hue, dial=sat)
    APP_RGBLEDRING_MODE_SOLID2,   // second colour picker, same controls, saved separately
    APP_RGBLEDRING_MODE_ENCODER,  // a pip per encoder angle
    APP_RGBLEDRING_MODE_OFF,      // all off
    APP_RGBLEDRING_MODE_COUNT,
} app_rgbLedRing_mode_E;

typedef struct
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} app_rgbLedRing_rgb_S;

// All per-frame UI state for one ring. Held across frames so movement
// integrates and picked colours persist across mode changes.
typedef struct
{
    app_rgbLedRing_mode_E mode;
    bool      prevButton;   // debounced button state last frame (edge detection)

    float32_t walkPos;      // dial-driven walk-head position, LED units
    float32_t walkPos2;     // motor-driven walk-head position, LED units
    float32_t dialAccum;    // signed sticky speed accumulator (dial)
    float32_t motorAccum;   // signed sticky speed accumulator (motor)

    float32_t satAccum;     // SOLID colour-picker saturation accumulator
    float32_t satAccum2;    // SOLID2 colour-picker saturation accumulator
    float32_t hueAccum;     // SOLID colour-picker hue (deg)
    float32_t hueAccum2;    // SOLID2 colour-picker hue (deg)

    uint8_t   pickR, pickG, pickB;     // colour picked in SOLID, reused by walk/encoder
    uint8_t   pick2R, pick2G, pick2B;  // colour picked in SOLID2, reused by walk/encoder

    float32_t prevDial;     // dial angle last frame (deg), for delta tracking
    float32_t prevMotor;    // motor angle last frame (deg), for delta tracking
} app_rgbLedRing_state_S;

// Board wiring for one ring: which LED string it drives, which encoders steer
// it, which button cycles its mode, and how many LEDs it has. All board-specific
// data lives here; the rendering logic is channel-agnostic.
typedef struct
{
    IO_SK6805_channel_E  ledChannel;     // LED string this ring renders to
    IO_AS5048_channel_E  dialChannel;    // dial/knob encoder
    IO_AS5048_channel_E  motorChannel;   // motor encoder
    DEV_switch_channel_E buttonChannel;  // mode-cycle button
    uint16_t             pixelCount;     // LEDs in this ring (1..APP_RGBLEDRING_MAX_PIXELS)
} app_rgbLedRing_channelConfig_S;

typedef struct
{
    const app_rgbLedRing_channelConfig_S * channels;
    size_t numChannels;
} app_rgbLedRing_config_S;

/* Public Function Declarations */

// Validate the config and spawn the ring UI task at taskPriority. The task
// refreshes every ring every APP_RGBLEDRING_FRAME_PERIOD_MS, reading each ring's
// dial + motor encoders and button and rendering its active display mode.
// Returns false if the config is rejected or the task cannot be created. Call
// once from main() before the scheduler starts; the IO drivers it consumes
// (IO_SK6805, IO_AS5048, DEV_switch) must be initialised first.
bool app_rgbLedRing_init(const app_rgbLedRing_config_S * const config, uint32_t taskPriority);

/* Rendering core — pure logic (no RTOS/IO). Driven by the task above; declared
   here so the unit test can exercise it directly. Consumers use _init. */

// Initialise UI state to the bring-up defaults: mode WALK, SOLID red, SOLID2
// blue, pickers at full saturation, walk heads stopped. Seed the encoder
// references separately with app_rgbLedRing_seedEncoders.
void app_rgbLedRing_renderInit(app_rgbLedRing_state_S * state);

// Seed the dial/motor angle references so the first frame's deltas are ~0.
void app_rgbLedRing_seedEncoders(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg);

// Advance to the next mode (WALK -> SOLID -> SOLID2 -> ENCODER -> OFF -> WALK)
// on a rising edge of buttonActive. Returns true on the frame the mode changes.
bool app_rgbLedRing_advanceMode(app_rgbLedRing_state_S * state, bool buttonActive);

// Render the active mode into pixels[0..ledCount) from the current dial/motor
// angles (degrees), updating the per-mode accumulators and movement deltas.
void app_rgbLedRing_renderFrame(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg,
                                app_rgbLedRing_rgb_S * pixels, uint16_t ledCount);

#endif // APP_RGBLEDRING_H
