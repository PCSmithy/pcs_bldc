/* Includes */
#include "app_sequencer.h"

#include <array>
#include <atomic>

/* Defines */
#define APP_SEQUENCER_PASS_US (1000U)

/* Typedefs */

class Sequencer
{
    public:
        bool init(const app_sequencer_channelConfig_S * const channelConfig);
        void run1ms();

        bool begin();
        app_sequencer_append_E append(const app_sequencer_setpoint_S * const setpoint);
        bool commit();
        bool getStatus(app_sequencer_status_S * const status) const;

    private:
        app_sequencer_setpoint_S * table(uint32_t half) const;

        app_sequencer_channelConfig_S const * config;

        // Two halves of the storage: the server fills the staged one while the
        // pass plays the running one; a commit flips them on the pass after.
        uint32_t runningHalf;
        uint32_t counts[2];
        bool staging;        // begin done, commit not yet
        bool pending;        // staged table committed, not yet started

        uint32_t next;       // the first unwritten setpoint
        uint32_t elapsed_ms;
        uint16_t faultEpoch; // the motor's fault-latch count at the start
        bool running;
};

typedef struct
{
    std::array<Sequencer, APP_SEQUENCER_CHANNEL_COUNT> channels;
} app_sequencer_data_S;

typedef void (*app_sequencer_writer_T)(app_motorControl_channel_E motor, const app_sequencer_setpoint_S & setpoint);

/* Private Function Definitions */

static void app_sequencer_private_writeVelocity(app_motorControl_channel_E motor, const app_sequencer_setpoint_S & setpoint)
{
    app_motorControl_setVelocity(motor, setpoint.target.value, setpoint.target.rateLimit);
}

static void app_sequencer_private_writeTorque(app_motorControl_channel_E motor, const app_sequencer_setpoint_S & setpoint)
{
    app_motorControl_setTorque(motor, setpoint.target.value, setpoint.target.rateLimit);
}

static void app_sequencer_private_writeStop(app_motorControl_channel_E motor, const app_sequencer_setpoint_S & setpoint)
{
    (void)setpoint;
    app_motorControl_setMode(motor, APP_MOTORCONTROL_MODE_OFF);
}

static void app_sequencer_private_writePosition(app_motorControl_channel_E motor, const app_sequencer_setpoint_S & setpoint)
{
    app_motorControl_setPositionSetpoint(motor, setpoint.move.position_rad, setpoint.move.velocityLimit_radPerSec,
                                         setpoint.move.accelLimit_radPerSec2, setpoint.move.velocityAtPoint_radPerSec);
}

/* Private Data Definitions */
constinit static app_sequencer_data_S app_sequencer_data = {};
constinit static app_sequencer_data_S * const data = &app_sequencer_data;

// One write per kind, in app_sequencer_kind_E order: the speed and torque
// targets (fw~mc_027), the stop a MODE_OFF request (fw~mc_006), the position
// setpoint (fw~mc_030).
static constexpr std::array<app_sequencer_writer_T, APP_SEQUENCER_KIND_COUNT> app_sequencer_writers = {
    app_sequencer_private_writeVelocity,
    app_sequencer_private_writeTorque,
    app_sequencer_private_writeStop,
    app_sequencer_private_writePosition,
};

/* Private Class Function Definitions */

app_sequencer_setpoint_S * Sequencer::table(uint32_t half) const
{
    return &this->config->storage[half * this->config->capacity];
}

/* Public Class Function Definitions */

bool Sequencer::init(const app_sequencer_channelConfig_S * const channelConfig)
{
    bool isValid = true;
    isValid &= (channelConfig->motor < APP_MOTORCONTROL_CHANNEL_COUNT);
    isValid &= (channelConfig->storage != NULL);
    isValid &= (channelConfig->capacity > 0U);
    if (isValid)
    {
        this->config = channelConfig;
        this->runningHalf = 0U;
        this->counts[0] = 0U;
        this->counts[1] = 0U;
        this->staging = false;
        this->pending = false;
        this->next = 0U;
        this->elapsed_ms = 0U;
        this->faultEpoch = 0U;
        this->running = false;
    }
    return isValid;
}

// [impl->fw~ops_001~1] from the pass after the commit, each setpoint due by the elapsed time, in list order
// [impl->fw~ops_002~1] a commit replaces the running sequence; a latched fault, or a stop, drops the rest
void Sequencer::run1ms()
{
    if (this->config != NULL)
    {
        app_motorControl_snapshot_S motor = {};
        (void)app_motorControl_getSnapshot(this->config->motor, &motor);

        if (this->pending)
        {
            this->runningHalf = 1U - this->runningHalf;
            this->counts[1U - this->runningHalf] = 0U;   // the half released; no one is in it while pending
            this->pending = false;
            this->next = 0U;
            this->elapsed_ms = 0U;
            this->faultEpoch = motor.faultLatches;
            this->running = (this->counts[this->runningHalf] > 0U);
        }

        if (this->running)
        {
            const app_sequencer_setpoint_S * const setpoints = this->table(this->runningHalf);
            const uint32_t count = this->counts[this->runningHalf];
            const uint64_t now_us = static_cast<uint64_t>(this->elapsed_ms) * APP_SEQUENCER_PASS_US;

            // A latch since the start drops the rest, cleared since or not.
            const bool faulted = (motor.state == APP_MOTORCONTROL_STATE_FAULTED) ||
                                 (motor.faultLatches != this->faultEpoch);
            if (faulted)
            {
                this->next = count;
            }

            while ((this->next < count) && (setpoints[this->next].t_us <= now_us))
            {
                const app_sequencer_setpoint_S & setpoint = setpoints[this->next];
                app_sequencer_writers[setpoint.kind](this->config->motor, setpoint);
                this->next = (setpoint.kind == APP_SEQUENCER_KIND_STOP) ? count : (this->next + 1U);
            }

            this->running = (this->next < count);
            this->elapsed_ms++;
        }
    }
}

bool Sequencer::begin()
{
    bool ret = false;
    // Not while a commit awaits its pass: the half it names is about to flip,
    // and the sequence in it was accepted.
    if ((this->config != NULL) && (!this->pending))
    {
        this->counts[1U - this->runningHalf] = 0U;
        this->staging = true;
        ret = true;
    }
    return ret;
}

app_sequencer_append_E Sequencer::append(const app_sequencer_setpoint_S * const setpoint)
{
    app_sequencer_append_E ret = APP_SEQUENCER_APPEND_INVALID;
    if ((this->config == NULL) || (!this->staging))
    {
        ret = APP_SEQUENCER_APPEND_NOT_STAGING;
    }
    else if ((setpoint != NULL) && (setpoint->kind < APP_SEQUENCER_KIND_COUNT))
    {
        app_sequencer_setpoint_S * const staged = this->table(1U - this->runningHalf);
        uint32_t & count = this->counts[1U - this->runningHalf];
        if (count >= this->config->capacity)
        {
            ret = APP_SEQUENCER_APPEND_FULL;
        }
        else if ((count > 0U) && (setpoint->t_us < staged[count - 1U].t_us))
        {
            ret = APP_SEQUENCER_APPEND_OUT_OF_ORDER;
        }
        else
        {
            staged[count] = *setpoint;
            count++;
            ret = APP_SEQUENCER_APPEND_OK;
        }
    }
    else
    {
        // INVALID stands
    }
    return ret;
}

bool Sequencer::commit()
{
    bool ret = false;
    if ((this->config != NULL) && this->staging)
    {
        this->staging = false;
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the table before the flag
        this->pending = true;
        ret = true;
    }
    return ret;
}

// A committed sequence reports from its commit: running with nothing elapsed
// until the pass that starts it.
bool Sequencer::getStatus(app_sequencer_status_S * const status) const
{
    bool ret = false;
    if ((this->config != NULL) && (status != NULL))
    {
        if (this->pending)
        {
            status->running = (this->counts[1U - this->runningHalf] > 0U);
            status->elapsed_ms = 0U;
        }
        else
        {
            status->running = this->running;
            status->elapsed_ms = this->elapsed_ms;
        }
        ret = true;
    }
    return ret;
}

/* Public Function Definitions */

bool app_sequencer_init(const app_sequencer_config_S * const config)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= APP_SEQUENCER_CHANNEL_COUNT))
    {
        bool configsValid = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            configsValid &= data->channels[channel].init(&config->channels[channel]);
        }
        success = configsValid;
    }
    return success;
}

void app_sequencer_run1ms(void)
{
    for (size_t channel = 0U; channel < APP_SEQUENCER_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].run1ms();
    }
}

bool app_sequencer_begin(app_sequencer_channel_E channel)
{
    bool ret = false;
    if (channel < APP_SEQUENCER_CHANNEL_COUNT)
    {
        ret = data->channels[channel].begin();
    }
    return ret;
}

app_sequencer_append_E app_sequencer_append(app_sequencer_channel_E channel, const app_sequencer_setpoint_S * const setpoint)
{
    app_sequencer_append_E ret = APP_SEQUENCER_APPEND_NOT_STAGING;
    if (channel < APP_SEQUENCER_CHANNEL_COUNT)
    {
        ret = data->channels[channel].append(setpoint);
    }
    return ret;
}

bool app_sequencer_commit(app_sequencer_channel_E channel)
{
    bool ret = false;
    if (channel < APP_SEQUENCER_CHANNEL_COUNT)
    {
        ret = data->channels[channel].commit();
    }
    return ret;
}

bool app_sequencer_getStatus(app_sequencer_channel_E channel, app_sequencer_status_S * const status)
{
    bool ret = false;
    if (channel < APP_SEQUENCER_CHANNEL_COUNT)
    {
        ret = data->channels[channel].getStatus(status);
    }
    return ret;
}
