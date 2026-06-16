#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_ADC.h"
#include "HW_SPI.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
  #include "FreeRTOS.h"
  #include "task.h"
  #include "usb.h"
  // io/ and dev/ are embedded-only (the io tree pulls in tinyusb), so the
  // encoder, LED and switch drivers are only available on this target.
  #include "IO_AS5048.h"
  #include "IO_SK6805.h"
  #include "DEV_switch.h"
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_GPIO_config_S HW_GPIO_config;
extern const HW_ADC_config_S HW_ADC_config;
extern const HW_SPI_config_S HW_SPI_config;

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
extern const IO_AS5048_config_S IO_AS5048_config;
extern const IO_SK6805_config_S IO_SK6805_config;
extern const DEV_switch_config_S DEV_switch_config;
#endif

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
// stm32g4xx_it.c's TIM6_DAC_IRQHandler references hdac1; the DAC isn't
// integrated, so a zeroed weak handle lets it.o link (the DAC interrupt never
// fires). TODO: remove when a DAC driver lands.
__attribute__((weak)) DAC_HandleTypeDef hdac1;
#endif


void Error_Handler(void)
{
    while (1)
    {
    }
}

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
// FreeRTOS static-allocation memory (configSUPPORT_STATIC_ALLOCATION=1).
// cmsis_os2.c normally provides these; we supply them since we use the native
// FreeRTOS API without the CMSIS-RTOS wrapper.
static StaticTask_t idleTaskTcb;
static StackType_t  idleTaskStack[configMINIMAL_STACK_SIZE];
void vApplicationGetIdleTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    *ppxTcb   = &idleTaskTcb;
    *ppxStack = idleTaskStack;
    *pulSize  = configMINIMAL_STACK_SIZE;
}

static StaticTask_t timerTaskTcb;
static StackType_t  timerTaskStack[configTIMER_TASK_STACK_DEPTH];
void vApplicationGetTimerTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    *ppxTcb   = &timerTaskTcb;
    *ppxStack = timerTaskStack;
    *pulSize  = configTIMER_TASK_STACK_DEPTH;
}

// FreeRTOS task priority hierarchy (higher number preempts lower), defined in
// one place so the ordering is explicit. The encoder sampler is hard
// real-time. The LED refresh briefly blocks (~1.25 ms every 50 ms) but must
// hit its cadence regardless of USB load, so it outranks the USB servicer —
// the added USB latency is negligible. USB is event-driven and blocks when
// idle, so it sits lowest of the three.
#define TASK_PRIO_ENCODER  (configMAX_PRIORITIES - 1U)
#define TASK_PRIO_LED      (configMAX_PRIORITIES - 2U)
#define TASK_PRIO_USB      (configMAX_PRIORITIES - 3U)

// Fixed-rate 1 ms IO task. Home for periodic sensor/actuator run functions
// (encoder sampling now; the control loop will likely move to its own faster
// task later). vTaskDelayUntil gives a drift-free 1 ms cadence regardless of
// how long the body takes. High priority so sampling preempts USB servicing.
static void task_1ms(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1U));

        // hw
        HW_GPIO_run1ms();   // cache input-pin levels before anything reads them

        // io
        IO_AS5048_run1ms();

        // dev
        DEV_switch_run1ms();   // debounce switches off the cached GPIO snapshot

        // app
    }
}

// Low-rate LED task: bring-up modes refreshed every LED_FRAME_PERIOD_MS. Runs
// above the USB task (TASK_PRIO_LED) so its cadence isn't held off by USB
// servicing — otherwise the animation stutters under USB load. A full refresh
// only blocks ~1.25 ms, so the USB latency cost is negligible.
//
// The SK6805 is a continuous timed stream: a task preemption mid-transfer
// (notably by the 1 ms task) underruns the SPI FIFO and corrupts the frame.
// vTaskSuspendAll() blocks task switches across the transmit so the stream
// stays intact; ISRs still run (short enough for the FIFO to ride out), and
// the encoder task just slips one ~1 ms sample. The real fix is DMA (no
// HW_DMA yet); this is the prototype path.
//
// Bring-up LED modes, cycled by the user button. Throwaway scaffolding — to be
// restructured into a dedicated app module later.
typedef enum
{
    LED_MODE_WALK,     // colour pip walking the ring; dial sets signed speed
    LED_MODE_SOLID,    // two-encoder HSV colour picker (motor=hue, dial=sat)
    LED_MODE_ENCODER,  // red pip tracks the motor angle, blue pip the dial
    LED_MODE_OFF,      // all off
    LED_MODE_COUNT,
} ledMode_E;

// Encoder-pip tuning. The ring's 36 LEDs sit 10 deg apart; a pip is spread
// over ~PIP_HALF_WIDTH_DEG each side of the encoder angle with a linear
// falloff, so 2-3 LEDs light and the centre tracks the encoder's sub-degree
// resolution. PIP_MAX_BRIGHTNESS keeps it dim (not eye-searing).
#define PIP_HALF_WIDTH_DEG   18.0f
#define PIP_MAX_BRIGHTNESS   50U

// WALK-mode speed control. The dial sets a SIGNED walk-head speed through a
// sticky, clamped accumulator of dial *movement* (not absolute angle), centred
// on zero: at centre the walk slows to a stop, winding one way runs it forward
// and the other way reverses it, up to +/-WALK_SPEED_MAX_DPS. Winding past a
// bound saturates and discards the excess, so reversing the dial responds
// immediately instead of unwinding. ~DIAL_ACCUM_RANGE_DEG of travel each side
// sweeps stop -> full speed.
#define WALK_SPEED_MAX_DPS    720.0f   // peak speed magnitude, either direction
#define DIAL_ACCUM_RANGE_DEG  340.0f   // dial travel from stop to full speed (per direction)

// Colour-picker saturation. Like the walk speed, the dial drives a bounded
// accumulator of *movement* (not absolute angle), so saturation sweeps
// white<->full smoothly with no harsh jump at the encoder's 0/360 seam.
// ~SAT_ACCUM_RANGE_DEG of dial travel sweeps white -> full saturation.
#define SAT_ACCUM_RANGE_DEG   180.0f

// LED task cadence. The frame dt is derived from the period so the two can't
// drift apart (the walk speed depends on it).
#define LED_FRAME_PERIOD_MS   10U
#define LED_FRAME_DT_S        ((float32_t)LED_FRAME_PERIOD_MS / 1000.0f)

// Brightness (0..1) a pip centred at angle_deg contributes to ledIndex, by
// circular angular distance with a linear falloff out to the pip half-width.
static float32_t ledPipBrightness(float32_t angle_deg, uint16_t ledIndex)
{
    const float32_t ledsPerRev = (float32_t)IO_SK6805_PIXEL_COUNT;
    const float32_t degPerLed  = 360.0f / ledsPerRev;
    const float32_t center     = angle_deg / degPerLed;   // pip centre in LED units

    float32_t dist = (float32_t)ledIndex - center;
    if (dist < 0.0f)               { dist = -dist; }
    if (dist > (ledsPerRev * 0.5f)) { dist = ledsPerRev - dist; }  // wrap the short way

    const float32_t halfWidthLeds = PIP_HALF_WIDTH_DEG / degPerLed;
    float32_t brightness = 0.0f;
    if (dist < halfWidthLeds)
    {
        brightness = 1.0f - (dist / halfWidthLeds);
    }
    return brightness;
}

// HSV -> RGB. h in [0,360) deg, s and v in [0,1]; each output channel is scaled
// to [0, maxLevel]. Drives the SOLID-mode two-encoder colour picker
// (motor = hue, dial = saturation, value fixed).
static void ledHsvToRgb(float32_t h, float32_t s, float32_t v, uint8_t maxLevel,
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

static void ledTask(void * params)
{
    (void)params;
    ledMode_E mode       = LED_MODE_WALK;
    bool      prevButton = false;
    float32_t walkPos    = 0.0f;   // walk-head position, LED units
    float32_t dialAccum  = 0.0f;   // signed sticky speed accumulator; 0 = stopped
    float32_t satAccum   = SAT_ACCUM_RANGE_DEG;  // colour-picker saturation; starts full

    // Colour picked in SOLID mode (motor=hue, dial=saturation), reused by the
    // walk pip. Defaults to the brightest red until a colour is dialled in.
    uint8_t pickR = (uint8_t)PIP_MAX_BRIGHTNESS;
    uint8_t pickG = 0U;
    uint8_t pickB = 0U;

    // Seed the dial reference so the first frame's delta is ~0.
    float32_t prevDial = 0.0f;
    (void)IO_AS5048_readAngle(IO_AS5048_CHANNEL_DIAL, NULL, &prevDial);

    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        // Advance to the next mode on each button press (rising edge of the
        // debounced state).
        const bool button = DEV_switch_isActive(DEV_SWITCH_CHANNEL_USER_BUTTON);
        if (button && (!prevButton))
        {
            mode = (ledMode_E)((mode + 1U) % (uint32_t)LED_MODE_COUNT);
        }
        prevButton = button;

        // Track dial movement every frame (wrapped to +/-180 deg) so the value
        // is always current and switching modes never injects a jump.
        float32_t dialDeg = 0.0f;
        (void)IO_AS5048_readAngle(IO_AS5048_CHANNEL_DIAL, NULL, &dialDeg);
        float32_t dialDelta = dialDeg - prevDial;
        if (dialDelta > 180.0f)       { dialDelta -= 360.0f; }
        else if (dialDelta < -180.0f) { dialDelta += 360.0f; }
        prevDial = dialDeg;

        switch (mode)
        {
            case LED_MODE_WALK:
            {
                // Signed, sticky-clamped accumulator of dial travel, centred on
                // zero: at centre the walk slows to a stop, one way runs it
                // forward and the other reverses. At a bound the excess movement
                // is discarded, so reversing the dial responds immediately
                // without unwinding.
                dialAccum += dialDelta;
                if (dialAccum >  DIAL_ACCUM_RANGE_DEG) { dialAccum =  DIAL_ACCUM_RANGE_DEG; }
                if (dialAccum < -DIAL_ACCUM_RANGE_DEG) { dialAccum = -DIAL_ACCUM_RANGE_DEG; }

                const float32_t frac      = dialAccum / DIAL_ACCUM_RANGE_DEG;   // -1..+1
                const float32_t speedDps  = frac * WALK_SPEED_MAX_DPS;          // signed
                const float32_t degPerLed = 360.0f / (float32_t)IO_SK6805_PIXEL_COUNT;

                walkPos += (speedDps / degPerLed) * LED_FRAME_DT_S;            // signed LED units
                while (walkPos >= (float32_t)IO_SK6805_PIXEL_COUNT)
                {
                    walkPos -= (float32_t)IO_SK6805_PIXEL_COUNT;
                }
                while (walkPos < 0.0f)
                {
                    walkPos += (float32_t)IO_SK6805_PIXEL_COUNT;
                }

                // Render the head as a wide pip (same falloff as encoder mode),
                // in the SOLID-picked colour, centred on the fractional walkPos
                // for smooth sub-LED motion.
                const float32_t walkAngle = walkPos * degPerLed;              // LED units -> deg
                IO_SK6805_clear();
                for (uint16_t i = 0U; i < IO_SK6805_PIXEL_COUNT; i++)
                {
                    const float32_t bright = ledPipBrightness(walkAngle, i);
                    IO_SK6805_setPixel(i, (uint8_t)(bright * (float32_t)pickR),
                                          (uint8_t)(bright * (float32_t)pickG),
                                          (uint8_t)(bright * (float32_t)pickB));
                }
                break;
            }

            case LED_MODE_SOLID:
            {
                // Two-encoder colour picker: motor = hue (absolute, circular),
                // dial = saturation via a bounded movement accumulator so it
                // sweeps white<->full smoothly with no 0/360 seam jump.
                satAccum += dialDelta;
                if (satAccum < 0.0f)                { satAccum = 0.0f; }
                if (satAccum > SAT_ACCUM_RANGE_DEG) { satAccum = SAT_ACCUM_RANGE_DEG; }
                const float32_t saturation = satAccum / SAT_ACCUM_RANGE_DEG;   // 0..1

                float32_t motorDeg = 0.0f;
                (void)IO_AS5048_readAngle(IO_AS5048_CHANNEL_MOTOR, NULL, &motorDeg);
                ledHsvToRgb(motorDeg, saturation, 1.0f, (uint8_t)(PIP_MAX_BRIGHTNESS/2U), &pickR, &pickG, &pickB);
                IO_SK6805_setAll(pickR, pickG, pickB);
                break;
            }

            case LED_MODE_ENCODER:
            {
                // Red pip follows the motor encoder, blue pip the dial (read
                // above). Each LED takes red from the motor pip and blue from
                // the dial pip, so where they overlap the colours blend.
                float32_t motorDeg = 0.0f;
                (void)IO_AS5048_readAngle(IO_AS5048_CHANNEL_MOTOR, NULL, &motorDeg);

                for (uint16_t i = 0U; i < IO_SK6805_PIXEL_COUNT; i++)
                {
                    const uint8_t red  = (uint8_t)(ledPipBrightness(motorDeg, i) * (float32_t)PIP_MAX_BRIGHTNESS);
                    const uint8_t blue = (uint8_t)(ledPipBrightness(dialDeg, i)  * (float32_t)PIP_MAX_BRIGHTNESS);
                    IO_SK6805_setPixel(i, red, 0U, blue);
                }
                break;
            }

            case LED_MODE_OFF:
            default:
                IO_SK6805_clear();
                break;
        }

        vTaskSuspendAll();
        (void)IO_SK6805_update();
        (void)xTaskResumeAll();

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(LED_FRAME_PERIOD_MS));
    }
}
#endif

int main(void)
{
    // TODO: channelize this into an HW_halCore module (stm32g4 + sim
    // impls) so main.c doesn't need a target-specific include or
    // BUILD_TARGET branch. For now, gate it.
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    HAL_Init();
#endif

    bool initSuccess = true;
    initSuccess &= HW_systemClock_init(&HW_systemClock_config);
    initSuccess &= HW_GPIO_init(&HW_GPIO_config);
    initSuccess &= HW_ADC_init(&HW_ADC_config);
    initSuccess &= HW_SPI_init(&HW_SPI_config);
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    initSuccess &= IO_AS5048_init(&IO_AS5048_config);
    initSuccess &= IO_SK6805_init(&IO_SK6805_config);
    initSuccess &= DEV_switch_init(&DEV_switch_config);
#endif

    if (!initSuccess)
    {
        Error_Handler();
    }

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    // Spawn the 1 ms IO task (drives IO_AS5048_run1ms) and bring up USB CDC
    // (spawns the device task, which reads the latest cached angle and prints
    // it), then hand control to the scheduler. Both allocate their task from
    // the FreeRTOS heap; a failure here (e.g. heap exhaustion) must halt loudly
    // rather than silently drop a task. vTaskStartScheduler() does not return.
    bool tasksCreated = true;
    tasksCreated &= (xTaskCreate(task_1ms, "task_1ms", configMINIMAL_STACK_SIZE * 2U,
                                 NULL, TASK_PRIO_ENCODER, NULL) == pdPASS);
    tasksCreated &= (xTaskCreate(ledTask, "led", configMINIMAL_STACK_SIZE * 2U,
                                 NULL, TASK_PRIO_LED, NULL) == pdPASS);
    tasksCreated &= USB_init(TASK_PRIO_USB);

    if (!tasksCreated)
    {
        Error_Handler();
    }

    vTaskStartScheduler();
#endif

    return 0;
}
