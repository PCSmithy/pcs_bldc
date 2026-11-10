/* Includes */
#include "app_sequencer.h"

#include <array>
#include <atomic>

/* Defines */
#define PASS_US (1000U)

/* Typedefs */

class Sequencer
{
    public:
        bool init(const app_sequencer_channelConfig_S * const channelConfig);
        void run1ms();

        bool begin();
        bool append(const app_sequencer_setpoint_S * const setpoint);
        bool commit();
        bool getStatus(app_sequencer_status_S * const status) const;

    private:
        app_sequencer_setpoint_S * table(const uint32_t half) const;
        void write(const app_sequencer_setpoint_S & setpoint) const;

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

/* Private Data Definitions */
constinit static app_sequencer_data_S app_sequencer_data = {};
constinit static app_sequencer_data_S * const data = &app_sequencer_data;

/* Private Class Function Definitions */

app_sequencer_setpoint_S * Sequencer::table(const uint32_t half) const
{
    return &this->config->storage[half * this->config->capacity];
}

// One write per kind (fw~mc_027), the stop a MODE_OFF request (fw~mc_006);
// position adds a case.
void Sequencer::write(const app_sequencer_setpoint_S & setpoint) const
{
    switch (setpoint.kind)
    {
        case APP_SEQUENCER_KIND_VELOCITY:
            app_motorControl_setVelocity(this->config->motor, setpoint.value, setpoint.rateLimit);
            break;

        case APP_SEQUENCER_KIND_TORQUE:
            app_motorControl_setTorque(this->config->motor, setpoint.value, setpoint.rateLimit);
            break;

        case APP_SEQUENCER_KIND_STOP:
            app_motorControl_setMode(this->config->motor, APP_MOTORCONTROL_MODE_OFF);
            break;

        default:
            break;
    }
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

// [impl->fw~ops_001~1] the start is the pass after the commit; each pass writes
// every setpoint whose time is at or under the elapsed time, in list order.
// [impl->fw~ops_002~1] a commit replaces the running sequence from its first
// setpoint; a latched fault drops the unwritten rest.
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
            const uint64_t now_us = static_cast<uint64_t>(this->elapsed_ms) * PASS_US;

            // A latch since the start drops the rest, cleared since or not.
            const bool faulted = (motor.state == APP_MOTORCONTROL_STATE_FAULTED) ||
                                 (motor.faultLatches != this->faultEpoch);
            if (faulted)
            {
                this->next = count;
            }

            while ((this->next < count) && (setpoints[this->next].t_us <= now_us))
            {
                this->write(setpoints[this->next]);
                this->next++;
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

bool Sequencer::append(const app_sequencer_setpoint_S * const setpoint)
{
    bool ret = false;
    if ((this->config != NULL) && (setpoint != NULL) && this->staging &&
        (setpoint->kind < APP_SEQUENCER_KIND_COUNT))
    {
        const uint32_t staged = 1U - this->runningHalf;
        if (this->counts[staged] < this->config->capacity)
        {
            this->table(staged)[this->counts[staged]] = *setpoint;
            this->counts[staged]++;
            ret = true;
        }
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

bool Sequencer::getStatus(app_sequencer_status_S * const status) const
{
    bool ret = false;
    if ((this->config != NULL) && (status != NULL))
    {
        status->running = this->running;
        status->elapsed_ms = this->elapsed_ms;
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

bool app_sequencer_append(app_sequencer_channel_E channel, const app_sequencer_setpoint_S * const setpoint)
{
    bool ret = false;
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
