#include "timer_control.h"
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
      .frequency = TIMER_CONTROL_DEFAULT_FREQUENCY_HZ, \
      .duty_cycle = TIMER_CONTROL_DEFAULT_DUTY_CYCLE,  \
      .dead_time = TIMER_CONTROL_DEFAULT_DEAD_TIME_NS, \
  }

timer_channel_t channel_a = DEFAULT_CHANNEL(TIMER_CHANNEL_A);
timer_channel_t channel_b = DEFAULT_CHANNEL(TIMER_CHANNEL_B);
timer_channel_t channel_c = DEFAULT_CHANNEL(TIMER_CHANNEL_C);
timer_channel_t channel_d = DEFAULT_CHANNEL(TIMER_CHANNEL_D);
timer_channel_t channel_e = DEFAULT_CHANNEL(TIMER_CHANNEL_E);

#undef DEFAULT_CHANNEL

volatile bool timer_global_fault_latched = false;

/* channel->number doubles as the HAL timer index (sTimerxRegs[] offset). */
_Static_assert((TIMER_CHANNEL_A == HRTIM_TIMERINDEX_TIMER_A) &&
                   (TIMER_CHANNEL_E == HRTIM_TIMERINDEX_TIMER_E),
               "timer_channel_id_t must match the HAL HRTIM timer indices");

static timer_channel_t *const kChannels[TIMER_CHANNEL_COUNT] = {
    &channel_a, &channel_b, &channel_c, &channel_d, &channel_e,
};
static const uint32_t kTimerId[TIMER_CHANNEL_COUNT] = {
    HRTIM_TIMERID_TIMER_A, HRTIM_TIMERID_TIMER_B, HRTIM_TIMERID_TIMER_C,
    HRTIM_TIMERID_TIMER_D, HRTIM_TIMERID_TIMER_E,
};
static const uint32_t kOutput1[TIMER_CHANNEL_COUNT] = {
    HRTIM_OUTPUT_TA1, HRTIM_OUTPUT_TB1, HRTIM_OUTPUT_TC1, HRTIM_OUTPUT_TD1, HRTIM_OUTPUT_TE1,
};
static const uint32_t kOutput2[TIMER_CHANNEL_COUNT] = {
    HRTIM_OUTPUT_TA2, HRTIM_OUTPUT_TB2, HRTIM_OUTPUT_TC2, HRTIM_OUTPUT_TD2, HRTIM_OUTPUT_TE2,
};
/* Fault N is wired as channel N's own OCP input (FLT1->A ... FLT5->E). */
static const uint32_t kFaultEnable[TIMER_CHANNEL_COUNT] = {
    HRTIM_TIMFAULTENABLE_FAULT1, HRTIM_TIMFAULTENABLE_FAULT2, HRTIM_TIMFAULTENABLE_FAULT3,
    HRTIM_TIMFAULTENABLE_FAULT4, HRTIM_TIMFAULTENABLE_FAULT5,
};
static const uint32_t kFaultIrq[TIMER_CHANNEL_COUNT] = {
    HRTIM_IT_FLT1, HRTIM_IT_FLT2, HRTIM_IT_FLT3, HRTIM_IT_FLT4, HRTIM_IT_FLT5,
};

static uint32_t clamp(uint32_t value, uint32_t min, uint32_t max)
{
  return (value < min) ? min : ((value > max) ? max : value);
}

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
  uint32_t duty = clamp(duty_tenths_pct, TIMER_CONTROL_MIN_DUTY_CYCLE, TIMER_CONTROL_MAX_DUTY_CYCLE);
  return (period_ticks * duty) / 1000U;
}

static bool channel_is_valid(const timer_channel_t *channel)
{
  return (channel != NULL) && (channel->number < TIMER_CHANNEL_COUNT);
}

static void apply_time_base(uint32_t timer_idx, uint32_t period_ticks)
{
  HRTIM_TimeBaseCfgTypeDef pTimeBaseCfg = {0};
  pTimeBaseCfg.Period = period_ticks;
  pTimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  pTimeBaseCfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, timer_idx, &pTimeBaseCfg) != HAL_OK)
  {
    Error_Handler();
  }
}

static void apply_dead_time(uint32_t timer_idx, uint16_t dead_time_ns)
{
  HRTIM_DeadTimeCfgTypeDef pDeadTimeCfg = {0};
  pDeadTimeCfg.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
  pDeadTimeCfg.RisingValue = ns_to_ticks(dead_time_ns);
  pDeadTimeCfg.FallingValue = ns_to_ticks(dead_time_ns);
  if (HAL_HRTIM_DeadTimeConfig(&hhrtim, timer_idx, &pDeadTimeCfg) != HAL_OK)
  {
    Error_Handler();
  }
}

void channel_timer_init(timer_channel_t *channel)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  /* Clamp and write back, so the struct reflects what was programmed. */
  channel->frequency = clamp(channel->frequency, TIMER_CONTROL_MIN_FREQUENCY_HZ,
                             TIMER_CONTROL_MAX_FREQUENCY_HZ);
  channel->duty_cycle = (uint16_t)clamp(channel->duty_cycle, TIMER_CONTROL_MIN_DUTY_CYCLE,
                                        TIMER_CONTROL_MAX_DUTY_CYCLE);
  channel->dead_time = (uint16_t)clamp(channel->dead_time, TIMER_CONTROL_MIN_DEAD_TIME_NS,
                                       TIMER_CONTROL_MAX_DEAD_TIME_NS);

  uint32_t idx = channel->number;
  uint32_t period_ticks = hz_to_period_ticks(channel->frequency);

  apply_time_base(idx, period_ticks);
  apply_dead_time(idx, channel->dead_time);

  HRTIM_CompareCfgTypeDef pCompareCfg = {0};
  pCompareCfg.CompareValue = duty_to_compare_ticks(period_ticks, channel->duty_cycle);
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim, idx, HRTIM_COMPAREUNIT_1, &pCompareCfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Output 1: set at period start, reset at CMP1 -> duty = CMP1/period.
     Output 2 is generated as its dead-time complement.
     FaultLevel INACTIVE: a fault trip forces the output low. */
  HRTIM_OutputCfgTypeDef pOutputCfg = {0};
  pOutputCfg.SetSource = HRTIM_OUTPUTSET_TIMPER;
  pOutputCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP1;
  pOutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, idx, kOutput1[idx], &pOutputCfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* FaultLevel is per-output: without this, a fault would leave the
     low-side switch uncontrolled. */
  HRTIM_OutputCfgTypeDef pOutput2Cfg = {0};
  pOutput2Cfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, idx, kOutput2[idx], &pOutput2Cfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Enable this channel's own OCP fault line (CubeMX leaves these at NONE). */
  HRTIM_TimerCfgTypeDef pTimerCfg = {0};
  pTimerCfg.FaultEnable = kFaultEnable[idx];
  pTimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_ENABLED;
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim, idx, &pTimerCfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* The trip itself is hardware; the interrupt just tells software about it
     (routed back into timer_control_channel_fault via interrupts.c). */
  __HAL_HRTIM_ENABLE_IT(&hhrtim, kFaultIrq[idx]);

  channel->active = false;
}

void set_timer_duty_cycle(timer_channel_t *channel, uint16_t duty_cycle)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  duty_cycle = (uint16_t)clamp(duty_cycle, TIMER_CONTROL_MIN_DUTY_CYCLE,
                               TIMER_CONTROL_MAX_DUTY_CYCLE);
  uint32_t period_ticks = hz_to_period_ticks(
      clamp(channel->frequency, TIMER_CONTROL_MIN_FREQUENCY_HZ, TIMER_CONTROL_MAX_FREQUENCY_HZ));
  __HAL_HRTIM_SETCOMPARE(&hhrtim, channel->number, HRTIM_COMPAREUNIT_1,
                         duty_to_compare_ticks(period_ticks, duty_cycle));
  channel->duty_cycle = duty_cycle;
}

void set_timer_deadtime(timer_channel_t *channel, uint16_t dead_time)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  dead_time = (uint16_t)clamp(dead_time, TIMER_CONTROL_MIN_DEAD_TIME_NS,
                              TIMER_CONTROL_MAX_DEAD_TIME_NS);
  apply_dead_time(channel->number, dead_time);
  channel->dead_time = dead_time;
}

void set_timer_frequency(timer_channel_t *channel, uint32_t frequency)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  frequency = clamp(frequency, TIMER_CONTROL_MIN_FREQUENCY_HZ, TIMER_CONTROL_MAX_FREQUENCY_HZ);
  uint32_t idx = channel->number;
  uint32_t period_ticks = hz_to_period_ticks(frequency);

  apply_time_base(idx, period_ticks);

  /* Reapply duty% against the new period so a frequency change doesn't
     silently also change duty. */
  __HAL_HRTIM_SETCOMPARE(&hhrtim, idx, HRTIM_COMPAREUNIT_1,
                         duty_to_compare_ticks(period_ticks, channel->duty_cycle));

  channel->frequency = frequency;
}

void start_timer(timer_channel_t *channel)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  uint32_t idx = channel->number;
  if (HAL_HRTIM_WaveformCountStart(&hhrtim, kTimerId[idx]) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_WaveformOutputStart(&hhrtim, kOutput1[idx] | kOutput2[idx]) != HAL_OK)
  {
    Error_Handler();
  }
  channel->active = true;
}

void stop_timer(timer_channel_t *channel)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  uint32_t idx = channel->number;
  if (HAL_HRTIM_WaveformOutputStop(&hhrtim, kOutput1[idx] | kOutput2[idx]) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_HRTIM_WaveformCountStop(&hhrtim, kTimerId[idx]) != HAL_OK)
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
void timer_control_channel_fault(timer_channel_id_t channel)
{
  if (channel < TIMER_CHANNEL_COUNT)
  {
    kChannels[channel]->active = false;
  }
}

void timer_control_global_fault(void)
{
  /* Direct register writes: the HAL stop functions take __HAL_LOCK and would
     silently do nothing if the main loop held the lock. */
  hhrtim.Instance->sCommonRegs.ODISR = ALL_OUTPUTS;        /* all outputs off */
  CLEAR_BIT(hhrtim.Instance->sMasterRegs.MCR, ALL_TIMERS); /* all counters stopped */

  timer_global_fault_latched = true;
  for (uint32_t i = 0U; i < TIMER_CHANNEL_COUNT; i++)
  {
    kChannels[i]->active = false;
  }
}
