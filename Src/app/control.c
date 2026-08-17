/**
  ******************************************************************************
  * @file    control.c
  * @author  Angus Macdonald
  * @brief   Desired converter state, shared by every command source.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "control.h"

#include "main.h"

/* Pending work, one bit per thing a request can change. A bitmask rather than
   a state machine per field, because the set is what matters: control_service()
   applies every bit for a channel inside one stop/start bracket, so a batch of
   requests costs a single output interruption however many fields it touched. */
#define CONTROL_PEND_INIT  (1U << 0)
#define CONTROL_PEND_FREQ  (1U << 1)
#define CONTROL_PEND_DUTY  (1U << 2)
#define CONTROL_PEND_DEAD  (1U << 3)
#define CONTROL_PEND_CLEAR (1U << 4) /* clear this channel's latched OCP */
#define CONTROL_PEND_RUN   (1U << 5) /* start or stop, per `run`         */

/* Bits whose setters stop a running channel to reconfigure it. */
#define CONTROL_PEND_DISRUPTIVE (CONTROL_PEND_INIT | CONTROL_PEND_FREQ | CONTROL_PEND_DEAD)

typedef struct
{
  uint32_t frequency_hz;
  uint16_t duty_tenths;
  uint16_t dead_time_ns;
  bool run;
  volatile uint32_t pending;
  control_source_t last_source;
  uint32_t apply_failures;
} control_channel_t;

static control_channel_t control_channels[CHANNEL_COUNT];

/* OVP is global in the driver, so its clear is global here too. */
static volatile bool control_clear_ovp_pending;

/* The same PRIMASK idiom pwm.c uses. Held only around the read-and-clear of a
   pending mask - never across a pwm_* call, which takes its own critical
   sections and can reach Error_Handler() (which disables interrupts and spins
   forever). ~60-85 ns at 480 MHz; both OCP and OVP force the outputs off in
   silicon before their ISRs run, so delaying those ISRs delays bookkeeping,
   not protection. */
static uint32_t enter_critical(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  __DMB();
  return primask;
}

static void exit_critical(uint32_t primask)
{
  __DMB();
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static control_channel_t *channel_slot(uint32_t channel)
{
  if ((uint32_t)channel >= CHANNEL_COUNT)
  {
    return NULL;
  }
  return &control_channels[(uint32_t)channel];
}

/* Records one request. The value is written before the bit is set so a reader
   that sees the bit always sees the value that goes with it. */
static void post(control_channel_t *slot, uint32_t bit, control_source_t src)
{
  uint32_t primask = enter_critical();

  slot->pending |= bit;
  slot->last_source = src;

  exit_critical(primask);
}

void control_init(void)
{
  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++)
  {
    control_channels[i].pending = 0U;
    control_channels[i].last_source = CONTROL_SRC_NONE;
    control_channels[i].apply_failures = 0U;
    control_channels[i].run = false;
  }
  control_clear_ovp_pending = false;
}

bool control_request_frequency(uint32_t channel, uint32_t hz, control_source_t src)
{
  control_channel_t *slot = channel_slot(channel);

  if ((slot == NULL) || (hz < PWM_MIN_FREQUENCY_HZ) || (hz > PWM_MAX_FREQUENCY_HZ))
  {
    return false;
  }

  slot->frequency_hz = hz;
  post(slot, CONTROL_PEND_FREQ, src);
  return true;
}

bool control_request_duty(uint32_t channel, uint16_t tenths, control_source_t src)
{
  control_channel_t *slot = channel_slot(channel);

  if ((slot == NULL) || (tenths < PWM_MIN_DUTY_CYCLE) || (tenths > PWM_MAX_DUTY_CYCLE))
  {
    return false;
  }

  slot->duty_tenths = tenths;
  post(slot, CONTROL_PEND_DUTY, src);
  return true;
}

bool control_request_dead_time(uint32_t channel, uint16_t ns, control_source_t src)
{
  control_channel_t *slot = channel_slot(channel);

  if ((slot == NULL) || (ns < PWM_MIN_DEAD_TIME_NS) || (ns > PWM_MAX_DEAD_TIME_NS))
  {
    return false;
  }

  slot->dead_time_ns = ns;
  post(slot, CONTROL_PEND_DEAD, src);
  return true;
}

bool control_request_init(uint32_t channel, control_source_t src)
{
  control_channel_t *slot = channel_slot(channel);

  if (slot == NULL)
  {
    return false;
  }

  /* pwm_init() leaves the channel stopped, so a pending start would be a lie. */
  slot->run = false;
  post(slot, CONTROL_PEND_INIT, src);
  return true;
}

bool control_request_run(uint32_t channel, bool run, control_source_t src)
{
  control_channel_t *slot = channel_slot(channel);

  if (slot == NULL)
  {
    return false;
  }

  /* Refuse a start the driver would refuse anyway - faulted, still running, or
     never initialised. Reading op_state is a pure read, so asking costs nothing
     and lets the caller answer its host now rather than after the apply. This
     is advisory: a fault landing in the microseconds before control_service()
     runs still turns the start into an apply_failure, and the config report
     that follows tells the truth either way. */
  if (run && (channel_by_id(channel)->pwm.op_state != PWM_STATE_STOPPED))
  {
    return false;
  }

  slot->run = run;
  post(slot, CONTROL_PEND_RUN, src);
  return true;
}

bool control_request_clear_ocp(uint32_t channel, control_source_t src)
{
  control_channel_t *slot = channel_slot(channel);

  if (slot == NULL)
  {
    return false;
  }

  post(slot, CONTROL_PEND_CLEAR, src);
  return true;
}

bool control_request_clear_ovp(control_source_t src)
{
  uint32_t primask = enter_critical();

  (void)src; /* OVP is global; there is no per-channel slot to record it in. */
  control_clear_ovp_pending = true;

  exit_critical(primask);
  return true;
}

/* Takes the pending bits and the values they refer to in one critical section.
   Reading the mask, then the values, then clearing would lose a request that
   arrived in between. */
static uint32_t take_pending(control_channel_t *slot, control_channel_t *snapshot)
{
  uint32_t primask = enter_critical();
  uint32_t bits = slot->pending;

  *snapshot = *slot;
  slot->pending = 0U;

  exit_critical(primask);
  return bits;
}

static void start_channel(control_channel_t *slot, uint32_t channel)
{
  if (!pwm_start(channel))
  {
    slot->apply_failures++;
  }
}

bool control_service(void)
{
  bool applied = false;

  /* Before any channel: a latched OVP makes every pwm_start() refuse, so
     "clear ovp" must land first for a clear-then-start pair to work in one
     pass. */
  if (control_clear_ovp_pending)
  {
    uint32_t primask = enter_critical();
    control_clear_ovp_pending = false;
    exit_critical(primask);

    (void)pwm_clear_OVP_fault();
    applied = true;
  }

  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++)
  {
    control_channel_t *slot = &control_channels[i];
    control_channel_t snapshot;
    uint32_t bits;

    if (slot->pending == 0U)
    {
      continue;
    }

    bits = take_pending(slot, &snapshot);
    if (bits == 0U)
    {
      continue;
    }
    applied = true;

    /* One stop up front rather than one per setter. pwm_set_frequency() and
       pwm_set_dead_time() each capture "was it running" and restart what they
       stopped; stopping first makes that capture false, so the whole batch
       costs exactly one stop and one start. */
    if ((bits & CONTROL_PEND_DISRUPTIVE) != 0U)
    {
      pwm_stop(i);
    }

    if ((bits & CONTROL_PEND_INIT) != 0U)
    {
      pwm_init(i);
    }
    if ((bits & CONTROL_PEND_FREQ) != 0U)
    {
      pwm_set_frequency(i, snapshot.frequency_hz);
    }
    if ((bits & CONTROL_PEND_DUTY) != 0U)
    {
      (void)pwm_set_duty_cycle(i, snapshot.duty_tenths);
    }
    if ((bits & CONTROL_PEND_DEAD) != 0U)
    {
      pwm_set_dead_time(i, snapshot.dead_time_ns);
    }

    /* Before the run bit, so "clear 1" and "start 1" in one pass works. */
    if ((bits & CONTROL_PEND_CLEAR) != 0U)
    {
      (void)pwm_clear_OCP_fault(i);
    }

    if ((bits & CONTROL_PEND_RUN) != 0U)
    {
      if (snapshot.run)
      {
        start_channel(slot, i);
      }
      else
      {
        pwm_stop(i);
      }
    }
    else if (snapshot.run && ((bits & CONTROL_PEND_DISRUPTIVE) != 0U))
    {
      /* Reconfigured a channel that was meant to be running: put it back. */
      start_channel(slot, i);
    }
  }

  return applied;
}

control_source_t control_last_source(uint32_t channel)
{
  const control_channel_t *slot = channel_slot(channel);

  return (slot != NULL) ? slot->last_source : CONTROL_SRC_NONE;
}

uint32_t control_apply_failures(uint32_t channel)
{
  const control_channel_t *slot = channel_slot(channel);

  return (slot != NULL) ? slot->apply_failures : 0U;
}
