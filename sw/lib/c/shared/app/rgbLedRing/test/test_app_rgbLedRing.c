#include "app_rgbLedRing.h"
#include "unity.h"

// The board ring; the render core takes the count as a parameter.
#define LED_COUNT  36U

static app_rgbLedRing_state_S state;
static app_rgbLedRing_rgb_S   pixels[LED_COUNT];

void setUp(void)
{
    app_rgbLedRing_renderInit(&state);
    app_rgbLedRing_seedEncoders(&state, 0.0f, 0.0f);
    for (uint16_t i = 0U; i < LED_COUNT; i++)
    {
        pixels[i] = (app_rgbLedRing_rgb_S){ 0U, 0U, 0U };
    }
}

void tearDown(void) {}

// One press = a rising edge of the debounced button (advance), then a release.
static void press(void)
{
    (void)app_rgbLedRing_advanceMode(&state, true);
    (void)app_rgbLedRing_advanceMode(&state, false);
}

/* ---- fw~obs_ring_001: mode selection ---- */

// [test->fw~obs_ring_001~1]
static void test_starts_in_walk_mode(void)
{
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_WALK, state.mode);
}

// [test->fw~obs_ring_001~1]
static void test_button_edge_advances_once_per_press(void)
{
    // Rising edge advances.
    TEST_ASSERT_TRUE(app_rgbLedRing_advanceMode(&state, true));
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_SOLID, state.mode);

    // Held (no new edge) does not advance.
    TEST_ASSERT_FALSE(app_rgbLedRing_advanceMode(&state, true));
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_SOLID, state.mode);

    // Release, then the next press advances again.
    TEST_ASSERT_FALSE(app_rgbLedRing_advanceMode(&state, false));
    TEST_ASSERT_TRUE(app_rgbLedRing_advanceMode(&state, true));
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_SOLID2, state.mode);
}

// [test->fw~obs_ring_001~1]
static void test_mode_cycle_wraps(void)
{
    // WALK -> SOLID -> SOLID2 -> ENCODER -> OFF -> WALK.
    press();
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_SOLID, state.mode);
    press();
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_SOLID2, state.mode);
    press();
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_ENCODER, state.mode);
    press();
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_OFF, state.mode);
    press();
    TEST_ASSERT_EQUAL_INT(APP_RGBLEDRING_MODE_WALK, state.mode);
}

/* ---- fw~obs_ring_002: per-mode rendering ---- */

// [test->fw~obs_ring_002~1]
static void test_off_clears_every_pixel(void)
{
    state.mode = APP_RGBLEDRING_MODE_OFF;
    app_rgbLedRing_renderFrame(&state, 123.0f, 250.0f, pixels, LED_COUNT);
    for (uint16_t i = 0U; i < LED_COUNT; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0U, pixels[i].red);
        TEST_ASSERT_EQUAL_UINT8(0U, pixels[i].green);
        TEST_ASSERT_EQUAL_UINT8(0U, pixels[i].blue);
    }
}

// [test->fw~obs_ring_002~1]
static void test_solid_fills_ring_uniformly(void)
{
    // Defaults: hue 0 (red), full saturation, value scaled to MAX/2 = 25.
    state.mode = APP_RGBLEDRING_MODE_SOLID;
    app_rgbLedRing_renderFrame(&state, 0.0f, 0.0f, pixels, LED_COUNT);

    const uint8_t r = pixels[0].red;
    const uint8_t g = pixels[0].green;
    const uint8_t b = pixels[0].blue;
    TEST_ASSERT_TRUE(r > 0U);     // a colour is shown
    TEST_ASSERT_EQUAL_UINT8(0U, g);
    TEST_ASSERT_EQUAL_UINT8(0U, b);
    for (uint16_t i = 1U; i < LED_COUNT; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(r, pixels[i].red);
        TEST_ASSERT_EQUAL_UINT8(g, pixels[i].green);
        TEST_ASSERT_EQUAL_UINT8(b, pixels[i].blue);
    }
}

// [test->fw~obs_ring_002~1]
static void test_encoder_pip_tracks_each_angle(void)
{
    // Dial pip (SOLID colour, default red) at LED 0; motor pip (SOLID2 colour,
    // default blue) at the opposite side (180 deg -> LED 18).
    state.mode = APP_RGBLEDRING_MODE_ENCODER;
    app_rgbLedRing_renderFrame(&state, 0.0f, 180.0f, pixels, LED_COUNT);

    // LED 0: full dial pip, red, no blue.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS, pixels[0].red);
    TEST_ASSERT_EQUAL_UINT8(0U, pixels[0].blue);

    // LED 18 (180 deg): full motor pip, blue, no red.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)APP_RGBLEDRING_PIP_MAX_BRIGHTNESS, pixels[18].blue);
    TEST_ASSERT_EQUAL_UINT8(0U, pixels[18].red);

    // LED 9 (90 deg): outside both pips' +/-18 deg falloff -> dark.
    TEST_ASSERT_EQUAL_UINT8(0U, pixels[9].red);
    TEST_ASSERT_EQUAL_UINT8(0U, pixels[9].blue);
}

/* ---- fw~obs_ring_003: colour persistence across modes ---- */

// [test->fw~obs_ring_003~1]
static void test_solid_colour_persists_and_only_active_picker_responds(void)
{
    // Pick a colour in SOLID by winding the motor (hue) to 120 deg.
    state.mode = APP_RGBLEDRING_MODE_SOLID;
    app_rgbLedRing_renderFrame(&state, 0.0f, 120.0f, pixels, LED_COUNT);
    const uint8_t pickR = state.pickR;
    const uint8_t pickG = state.pickG;
    const uint8_t pickB = state.pickB;
    TEST_ASSERT_TRUE((pickR + pickG + pickB) > 0U);

    // Leave for OFF and move the encoders there; the SOLID colour must not move.
    state.mode = APP_RGBLEDRING_MODE_OFF;
    app_rgbLedRing_renderFrame(&state, 90.0f, 200.0f, pixels, LED_COUNT);
    TEST_ASSERT_EQUAL_UINT8(pickR, state.pickR);
    TEST_ASSERT_EQUAL_UINT8(pickG, state.pickG);
    TEST_ASSERT_EQUAL_UINT8(pickB, state.pickB);

    // Return to SOLID with no further motor movement: the colour resumes.
    state.mode = APP_RGBLEDRING_MODE_SOLID;
    app_rgbLedRing_renderFrame(&state, 90.0f, 200.0f, pixels, LED_COUNT);
    TEST_ASSERT_EQUAL_UINT8(pickR, state.pickR);
    TEST_ASSERT_EQUAL_UINT8(pickG, state.pickG);
    TEST_ASSERT_EQUAL_UINT8(pickB, state.pickB);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_starts_in_walk_mode);
    RUN_TEST(test_button_edge_advances_once_per_press);
    RUN_TEST(test_mode_cycle_wraps);

    RUN_TEST(test_off_clears_every_pixel);
    RUN_TEST(test_solid_fills_ring_uniformly);
    RUN_TEST(test_encoder_pip_tracks_each_angle);

    RUN_TEST(test_solid_colour_persists_and_only_active_picker_responds);

    return UNITY_END();
}
