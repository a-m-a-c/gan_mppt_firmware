#include "pwm.h"
#include "hrtim.h"
#include "main.h"

/* fHRTIM with prescaler DIV1 (see RCC.HRTIMFreq_Value in the .ioc).
 * Becomes 480000000U once the 480 MHz clock tree lands. */
#define HRTIM_KERNEL_CLOCK_HZ 64000000U

#define ALL_TIMERS                                                         \
  (HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B | HRTIM_TIMERID_TIMER_C | \
   HRTIM_TIMERID_TIMER_D | HRTIM_TIMERID_TIMER_E)
#define ALL_OUTPUTS                                                            \
  (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2 | \
   HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2 | HRTIM_OUTPUT_TD1 | HRTIM_OUTPUT_TD2 | \
   HRTIM_OUTPUT_TE1 | HRTIM_OUTPUT_TE2)

#define DEFAULT_CHANNEL(id)                            \
  {                                                    \
      .number = (id),                                  \
      .frequency = PWM_DEFAULT_FREQUENCY_HZ, \
      .duty_cycle = PWM_DEFAULT_DUTY_CYCLE,  \
      .dead_time = PWM_DEFAULT_DEAD_TIME_NS, \
  }

pwm_channel_t channel_a = DEFAULT_CHANNEL(PWM_CHANNEL_A);
pwm_channel_t channel_b = DEFAULT_CHANNEL(PWM_CHANNEL_B);
pwm_channel_t channel_c = DEFAULT_CHANNEL(PWM_CHANNEL_C);
pwm_channel_t channel_d = DEFAULT_CHANNEL(PWM_CHANNEL_D);
pwm_channel_t channel_e = DEFAULT_CHANNEL(PWM_CHANNEL_E);

#undef DEFAULT_CHANNEL

volatile bool pwm_global_fault_latched = false;

/* Fixed hardware wiring for one channel: its HRTIM timer, both outputs, and
 * its dedicated OCP fault line (FLT1->A ... FLT5->E). */
typedef struct
{
  uint32_t timer_index;  /* HRTIM_TIMERINDEX_x - HAL config calls */
  uint32_t timer_id;     /* HRTIM_TIMERID_x - counter start/stop */
  uint32_t output1;      /* carrier output */
  uint32_t output2;      /* dead-time complement output */
  uint32_t fault_enable; /* HRTIM_TIMFAULTENABLE_x */
  uint32_t fault_irq;    /* HRTIM_IT_FLTx */
} channel_hw_t;

static channel_hw_t channel_hw(pwm_channel_id_t channel)
{
  switch (channel)
  {
    case PWM_CHANNEL_A:
      return (channel_hw_t){.timer_index = HRTIM_TIMERINDEX_TIMER_A,
                            .timer_id = HRTIM_TIMERID_TIMER_A,
                            .output1 = HRTIM_OUTPUT_TA1,
                            .output2 = HRTIM_OUTPUT_TA2,
                            .fault_enable = HRTIM_TIMFAULTENABLE_FAULT1,
                            .fault_irq = HRTIM_IT_FLT1};
    case PWM_CHANNEL_B:
      return (channel_hw_t){.timer_index = HRTIM_TIMERINDEX_TIMER_B,
                            .timer_id = HRTIM_TIMERID_TIMER_B,
                            .output1 = HRTIM_OUTPUT_TB1,
                            .output2 = HRTIM_OUTPUT_TB2,
                            .fault_enable = HRTIM_TIMFAULTENABLE_FAULT2,
                            .fault_irq = HRTIM_IT_FLT2};
    case PWM_CHANNEL_C:
      return (channel_hw_t){.timer_index = HRTIM_TIMERINDEX_TIMER_C,
                            .timer_id = HRTIM_TIMERID_TIMER_C,
                            .output1 = HRTIM_OUTPUT_TC1,
                            .output2 = HRTIM_OUTPUT_TC2,
                            .fault_enable = HRTIM_TIMFAULTENABLE_FAULT3,
                            .fault_irq = HRTIM_IT_FLT3};
    case PWM_CHANNEL_D:
      return (channel_hw_t){.timer_index = HRTIM_TIMERINDEX_TIMER_D,
                            .timer_id = HRTIM_TIMERID_TIMER_D,
                            .output1 = HRTIM_OUTPUT_TD1,
                            .output2 = HRTIM_OUTPUT_TD2,
                            .fault_enable = HRTIM_TIMFAULTENABLE_FAULT4,
                            .fault_irq = HRTIM_IT_FLT4};
    case PWM_CHANNEL_E:
      return (channel_hw_t){.timer_index = HRTIM_TIMERINDEX_TIMER_E,
                            .timer_id = HRTIM_TIMERID_TIMER_E,
                            .output1 = HRTIM_OUTPUT_TE1,
                            .output2 = HRTIM_OUTPUT_TE2,
                            .fault_enable = HRTIM_TIMFAULTENABLE_FAULT5,
                            .fault_irq = HRTIM_IT_FLT5};
    default:
      return (channel_hw_t){0};
  }
}

static uint32_t clamp(uint32_t value, uint32_t min, uint32_t max)
{
  return (value < min) ? min : ((value > max) ? max : value);
}

/* Truncates the period, so actual frequency lands at or just above requested. */
static uint32_t hz_to_period_ticks(uint32_t frequency_hz)
{
  return HRTIM_KERNEL_CLOCK_HZ / frequency_hz;
}

/* Rounds up: actual dead time must never be shorter than requested. */
static uint32_t ns_to_ticks(uint16_t nanoseconds)
{
  /* uint64_t: at 480 MHz, ns * fHRTIM overflows uint32_t */
  uint64_t ticks = (((uint64_t)nanoseconds * HRTIM_KERNEL_CLOCK_HZ) + 999999999ULL) / 1000000000ULL;
  return (ticks == 0U) ? 1U : (uint32_t)ticks;
}

static uint32_t duty_to_compare_ticks(uint32_t period_ticks, uint16_t duty_tenths_pct)
{
  uint32_t duty = clamp(duty_tenths_pct, PWM_MIN_DUTY_CYCLE, PWM_MAX_DUTY_CYCLE);
  return (period_ticks * duty) / PWM_MAX_DUTY_CYCLE;
}

static bool channel_is_valid(const pwm_channel_t *channel)
{
  return (channel != NULL) && (channel->number < PWM_CHANNEL_COUNT);
}

void pwm_init(pwm_channel_t *channel)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  /* Never reconfigure live outputs: a running channel is stopped first. */
  if (channel->active)
  {
    pwm_stop(channel);
  }

  /* Time base, duty and dead time go through the public setters, which clamp
     the requested values and write what was programmed back into the struct. */
  pwm_set_frequency(channel, channel->frequency);
  pwm_set_duty_cycle(channel, channel->duty_cycle);
  pwm_set_dead_time(channel, channel->dead_time);

  channel_hw_t hw = channel_hw(channel->number);

  /* Output 1: set at period start, reset at CMP1 -> duty = CMP1/period.
     Output 2 is generated as its dead-time complement.
     FaultLevel INACTIVE: a fault trip forces the output low. */
  HRTIM_OutputCfgTypeDef output1_cfg = {0};
  output1_cfg.SetSource = HRTIM_OUTPUTSET_TIMPER;
  output1_cfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP1;
  output1_cfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, hw.timer_index, hw.output1, &output1_cfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* FaultLevel is per-output: without this, a fault would leave the
     low-side switch uncontrolled. */
  HRTIM_OutputCfgTypeDef output2_cfg = {0};
  output2_cfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, hw.timer_index, hw.output2, &output2_cfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Enable this channel's own OCP fault line (CubeMX leaves these at NONE). */
  HRTIM_TimerCfgTypeDef timer_cfg = {0};
  timer_cfg.FaultEnable = hw.fault_enable;
  timer_cfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_ENABLED;
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim, hw.timer_index, &timer_cfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* The trip itself is hardware; the interrupt just tells software about it
     (routed back into pwm_channel_fault via interrupts.c). */
  __HAL_HRTIM_ENABLE_IT(&hhrtim, hw.fault_irq);

  channel->active = false;
  channel->ocp_fault = false;
}

void pwm_set_duty_cycle(pwm_channel_t *channel, uint16_t duty_cycle)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  duty_cycle = (uint16_t)clamp(duty_cycle, PWM_MIN_DUTY_CYCLE,
                               PWM_MAX_DUTY_CYCLE);
  uint32_t timer_index = channel_hw(channel->number).timer_index;
  uint32_t period_ticks = hz_to_period_ticks(
      clamp(channel->frequency, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ));
  __HAL_HRTIM_SETCOMPARE(&hhrtim, timer_index, HRTIM_COMPAREUNIT_1,
                         duty_to_compare_ticks(period_ticks, duty_cycle));
  channel->duty_cycle = duty_cycle;
}

void pwm_set_dead_time(pwm_channel_t *channel, uint16_t dead_time)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  dead_time = (uint16_t)clamp(dead_time, PWM_MIN_DEAD_TIME_NS,
                              PWM_MAX_DEAD_TIME_NS);
  uint32_t timer_index = channel_hw(channel->number).timer_index;

  HRTIM_DeadTimeCfgTypeDef dead_time_cfg = {0};
  dead_time_cfg.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
  dead_time_cfg.RisingValue = ns_to_ticks(dead_time);
  dead_time_cfg.FallingValue = ns_to_ticks(dead_time);
  if (HAL_HRTIM_DeadTimeConfig(&hhrtim, timer_index, &dead_time_cfg) != HAL_OK)
  {
    Error_Handler();
  }

  channel->dead_time = dead_time;
}

void pwm_set_frequency(pwm_channel_t *channel, uint32_t frequency)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  frequency = clamp(frequency, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ);
  uint32_t timer_index = channel_hw(channel->number).timer_index;
  uint32_t period_ticks = hz_to_period_ticks(frequency);

  HRTIM_TimeBaseCfgTypeDef time_base_cfg = {0};
  time_base_cfg.Period = period_ticks;
  time_base_cfg.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  time_base_cfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, timer_index, &time_base_cfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Reapply duty% against the new period so a frequency change doesn't
     silently also change duty. */
  __HAL_HRTIM_SETCOMPARE(&hhrtim, timer_index, HRTIM_COMPAREUNIT_1,
                         duty_to_compare_ticks(period_ticks, channel->duty_cycle));

  channel->frequency = frequency;
}

void pwm_start(pwm_channel_t *channel)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  channel_hw_t hw = channel_hw(channel->number);
  if (HAL_HRTIM_WaveformCountStart(&hhrtim, hw.timer_id) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_WaveformOutputStart(&hhrtim, hw.output1 | hw.output2) != HAL_OK)
  {
    Error_Handler();
  }
  channel->active = true;
  channel->ocp_fault = false; /* restarting re-arms the channel */
}

void pwm_stop(pwm_channel_t *channel)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  channel_hw_t hw = channel_hw(channel->number);
  if (HAL_HRTIM_WaveformOutputStop(&hhrtim, hw.output1 | hw.output2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_WaveformCountStop(&hhrtim, hw.timer_id) != HAL_OK)
  {
    Error_Handler();
  }
  channel->active = false;
}

/* --- Global fault ---------------------------------------------------------
 * Per-channel OCP (FLT1-5) is pure hardware: the HRTIM fault state forces
 * both outputs low and stays latched until the fault flag is cleared.
 * The global OVP comparator has no hardware force-off path, so its EXTI
 * edge is routed here (see interrupts.c) to latch every channel off. */

/* ISR context. Hardware has already forced the channel's outputs low and
   latched the fault; this just makes the software state agree. */
void pwm_channel_fault(pwm_channel_id_t channel)
{
  pwm_channel_t *ch;
  switch (channel)
  {
    case PWM_CHANNEL_A: ch = &channel_a; break;
    case PWM_CHANNEL_B: ch = &channel_b; break;
    case PWM_CHANNEL_C: ch = &channel_c; break;
    case PWM_CHANNEL_D: ch = &channel_d; break;
    case PWM_CHANNEL_E: ch = &channel_e; break;
    default: return;
  }
  ch->active = false;
  ch->ocp_fault = true;
}

void pwm_global_fault(void)
{
  /* Direct register writes: the HAL stop functions take __HAL_LOCK and would
     silently do nothing if the main loop held the lock. */
  hhrtim.Instance->sCommonRegs.ODISR = ALL_OUTPUTS;        /* all outputs off */
  CLEAR_BIT(hhrtim.Instance->sMasterRegs.MCR, ALL_TIMERS); /* all counters stopped */

  pwm_global_fault_latched = true;
  channel_a.active = false;
  channel_b.active = false;
  channel_c.active = false;
  channel_d.active = false;
  channel_e.active = false;
}
