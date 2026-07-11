#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_SK6805.h"               // IO_SK6805_channel_E
#include "IO_AS5048.h"               // IO_AS5048_channel_E
#include "app_motorControl.h"        // motor channel + snapshot (state, setpoint)
#include "app_rgbLedRing_channels.h" // consumer-provided app_rgbLedRing_channel_E

/* Defines */

// Pip tuning: a pip spreads +/-PIP_HALF_WIDTH_DEG with a linear falloff;
// PIP_MAX_BRIGHTNESS keeps it dim.
#define APP_RGBLEDRING_PIP_HALF_WIDTH_DEG    18.0f
#define APP_RGBLEDRING_PIP_MAX_BRIGHTNESS    50U

// Speedometer sweep: zero sits at top-of-ring, +/-full speed sweeps this far
// each way, leaving a gap at the bottom (car-cluster style).
#define APP_RGBLEDRING_SPEEDO_SWEEP_DEG      150.0f

// Frame cadence. dt derives from the period so a per-frame speed integral
// can't drift from the real rate.
#define APP_RGBLEDRING_FRAME_PERIOD_MS       10U
#define APP_RGBLEDRING_FRAME_DT_S            ((float32_t)APP_RGBLEDRING_FRAME_PERIOD_MS / 1000.0f)

// Mode-change confirmation blink half-period.
#define APP_RGBLEDRING_MODE_BLINK_MS         50U

// Largest ring the UI buffers; a channel with pixelCount above this is rejected.
#define APP_RGBLEDRING_MAX_PIXELS            64U

/* Typedefs */

// Cycleable views while the motor runs. When the motor is off or faulted the
// ring shows a state display (blank / red) instead, so those are not modes.
typedef enum
{
    APP_RGBLEDRING_MODE_POSITION,   // pips at the motor + dial angles
    APP_RGBLEDRING_MODE_SPEEDO,     // setpoint + actual speed needles
    APP_RGBLEDRING_MODE_COUNT,
} app_rgbLedRing_mode_E;

typedef struct
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} app_rgbLedRing_rgb_S;

// Per-ring UI state, held across frames so movement deltas and the scaffold
// speed estimate integrate smoothly.
typedef struct
{
    app_rgbLedRing_mode_E mode;            // active view while the motor runs
    float32_t prevDial;                    // last dial angle (deg), for deltas
    float32_t prevMotor;                   // last motor angle (deg), for deltas + speed
    float32_t actualSpeedEst_dps;          // SCAFFOLD motor-speed estimate (see renderFrame)
} app_rgbLedRing_state_S;

// Board wiring for one ring: the LED string, the dial/motor encoders that steer
// it, the motor-control channel whose state it reflects, and the speed that maps
// to full speedometer deflection.
typedef struct
{
    IO_SK6805_channel_E        ledChannel;
    IO_AS5048_channel_E        dialChannel;
    IO_AS5048_channel_E        motorChannel;               // motor position encoder
    app_motorControl_channel_E motorControlChannel;        // motor whose state/setpoint it shows
    float32_t                  speedoFullScale_radPerSec;  // speed at full needle deflection
    uint16_t                   pixelCount;                 // 1..APP_RGBLEDRING_MAX_PIXELS
} app_rgbLedRing_channelConfig_S;

typedef struct
{
    const app_rgbLedRing_channelConfig_S * channels;
    size_t numChannels;
} app_rgbLedRing_config_S;

/* Public Function Declarations */

// Validate the config and initialise each configured ring's UI state. Returns
// false if the config is rejected. Call once from main() before the scheduler
// starts; the IO drivers it consumes (IO_SK6805, IO_AS5048, dev_switch) must be
// initialised first. Drive the UI by calling app_rgbLedRing_run10ms() every
// 10 ms after a successful init.
bool app_rgbLedRing_init(const app_rgbLedRing_config_S * const config);

// Service every configured ring: read its encoders, render its active display
// mode, and transmit the frame. Call every 10 ms (e.g. from the common 10 ms
// task), matching APP_RGBLEDRING_FRAME_PERIOD_MS. A no-op until a successful
// app_rgbLedRing_init.
void app_rgbLedRing_run10ms(void);

// Advance a ring's display mode one step, flashing the ring white on the
// change. The button no longer cycles the ring directly; the user-controls
// gesture invokes this instead (fw~obs_ring_001). No-op for an out-of-range
// channel or before init.
void app_rgbLedRing_cycleMode(app_rgbLedRing_channel_E channel);

/* Rendering core — pure logic (no RTOS/IO). Driven by run10ms; declared here so
   the unit test can exercise it directly. Consumers use _init + _run10ms. */

// Initialise UI state to defaults: mode POSITION, stopped. Seed the encoder
// references separately with app_rgbLedRing_seedEncoders.
void app_rgbLedRing_renderInit(app_rgbLedRing_state_S * state);

// Seed the dial/motor angle references so the first frame's deltas are ~0.
void app_rgbLedRing_seedEncoders(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg);

// Advance the render mode one step (POSITION <-> SPEEDO). Pure core;
// app_rgbLedRing_cycleMode wraps it with the ring flash.
void app_rgbLedRing_advanceMode(app_rgbLedRing_state_S * state);

// Render one frame into pixels[0..ledCount) from the dial/motor angles (deg) and
// the motor snapshot: a faulted motor fills red, an off/disabled motor blanks,
// otherwise the active mode renders. speedoFullScale maps setpoint/actual speed
// to full needle deflection.
void app_rgbLedRing_renderFrame(app_rgbLedRing_state_S * state, float32_t dialDeg, float32_t motorDeg,
                                const app_motorControl_snapshot_S * motor, float32_t speedoFullScale_radPerSec,
                                app_rgbLedRing_rgb_S * pixels, uint16_t ledCount);

