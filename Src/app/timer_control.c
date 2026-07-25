#include "timer_control.h"
#include "main.h"

extern HRTIM_HandleTypeDef hhrtim;

/* fHRTIM at HRTIM_PRESCALERRATIO_DIV1 (fHRCK = fHRTIM), per RCC.HRTIMFreq_Value
 * in the .ioc. Update this if the clock tree changes - e.g. once the 480 MHz
 * CubeMX clock-tree change lands, this becomes 480000000U. */
#define HRTIM_KERNEL_CLOCK_HZ 64000000U

#define DEFAULT_CHANNEL(id)                          \
  {                                                   \
      .number = (id),                                \
      .frequency = TIMER_CONTROL_DEFAULT_FREQUENCY_HZ, \
      .duty_cycle = TIMER_CONTROL_DEFAULT_DUTY_CYCLE, \
      .dead_time = TIMER_CONTROL_DEFAULT_DEAD_TIME_NS, \
  }

timer_channel_t channel_a = DEFAULT_CHANNEL(TIMER_CHANNEL_A);
timer_channel_t channel_b = DEFAULT_CHANNEL(TIMER_CHANNEL_B);
timer_channel_t channel_c = DEFAULT_CHANNEL(TIMER_CHANNEL_C);
timer_channel_t channel_d = DEFAULT_CHANNEL(TIMER_CHANNEL_D);
timer_channel_t channel_e = DEFAULT_CHANNEL(TIMER_CHANNEL_E);

#undef DEFAULT_CHANNEL

static const uint32_t kTimerIndex[TIMER_CHANNEL_COUNT] = {
    HRTIM_TIMERINDEX_TIMER_A, HRTIM_TIMERINDEX_TIMER_B, HRTIM_TIMERINDEX_TIMER_C,
    HRTIM_TIMERINDEX_TIMER_D, HRTIM_TIMERINDEX_TIMER_E,
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

static uint32_t hz_to_period_ticks(uint32_t frequency_hz)
{
  return HRTIM_KERNEL_CLOCK_HZ / frequency_hz;
}

static uint32_t ns_to_ticks(uint16_t nanoseconds)
{
  /* uint64_t intermediate: at 480 MHz, ns * fHRTIM overflows uint32_t */
  uint64_t ticks = ((uint64_t)nanoseconds * HRTIM_KERNEL_CLOCK_HZ + 500000000ULL) / 1000000000ULL;
  return (ticks == 0U) ? 1U : (uint32_t)ticks;
}

static uint32_t duty_to_compare_ticks(uint32_t period_ticks, uint16_t duty_tenths_pct)
{
  if (duty_tenths_pct > 1000U)
  {
    duty_tenths_pct = 1000U;
  }
  return (period_ticks * duty_tenths_pct) / 1000U;
}

static bool channel_is_valid(const timer_channel_t *channel)
{
  return (channel != NULL) && (channel->number < TIMER_CHANNEL_COUNT);
}

void channel_timer_init(timer_channel_t *channel)
{
  if (!channel_is_valid(channel) || (channel->frequency == 0U))
  {
    return;
  }

  HRTIM_TimeBaseCfgTypeDef pTimeBaseCfg = {0};
  HRTIM_CompareCfgTypeDef pCompareCfg = {0};
  HRTIM_OutputCfgTypeDef pOutputCfg = {0};
  HRTIM_DeadTimeCfgTypeDef pDeadTimeCfg = {0};
  HRTIM_TimerCfgTypeDef pTimerCfg = {0};
  uint32_t idx = channel->number;

  uint32_t period_ticks = hz_to_period_ticks(channel->frequency);

  pTimeBaseCfg.Period = period_ticks;
  pTimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  pTimeBaseCfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, kTimerIndex[idx], &pTimeBaseCfg) != HAL_OK)
  {
    Error_Handler();
  }

  pCompareCfg.CompareValue = duty_to_compare_ticks(period_ticks, channel->duty_cycle);
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim, kTimerIndex[idx], HRTIM_COMPAREUNIT_1, &pCompareCfg) != HAL_OK)
  {
    Error_Handler();
  }

  pDeadTimeCfg.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
  pDeadTimeCfg.RisingValue = ns_to_ticks(channel->dead_time);
  pDeadTimeCfg.FallingValue = ns_to_ticks(channel->dead_time);
  if (HAL_HRTIM_DeadTimeConfig(&hhrtim, kTimerIndex[idx], &pDeadTimeCfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Output 1 drives the carrier: active from period-start, inactive at CMP1
     match -> duty = CMP1/Period. Output 2 is generated automatically as
     output 1's dead-time-delayed complement. */
  pOutputCfg.SetSource = HRTIM_OUTPUTSET_TIMPER;
  pOutputCfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP1;
  pOutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, kTimerIndex[idx], kOutput1[idx], &pOutputCfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* FaultLevel is a per-output field - output 2 needs it set explicitly too,
     or a fault trip leaves the low-side switch unaffected. */
  HRTIM_OutputCfgTypeDef pOutput2Cfg = {0};
  pOutput2Cfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, kTimerIndex[idx], kOutput2[idx], &pOutput2Cfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Route this channel's own OCP fault input so it actually cuts the timer -
     the CubeMX-generated init leaves every timer's FaultEnable at NONE. */
  pTimerCfg.FaultEnable = kFaultEnable[idx];
  pTimerCfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_ENABLED;
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim, kTimerIndex[idx], &pTimerCfg) != HAL_OK)
  {
    Error_Handler();
  }

  channel->active = false;
}

void set_timer_duty_cycle(timer_channel_t *channel, uint16_t duty_cycle)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  uint32_t period_ticks = hz_to_period_ticks(channel->frequency);
  uint32_t compare_ticks = duty_to_compare_ticks(period_ticks, duty_cycle);
  __HAL_HRTIM_SETCOMPARE(&hhrtim, kTimerIndex[channel->number], HRTIM_COMPAREUNIT_1, compare_ticks);
  channel->duty_cycle = duty_cycle;
}

void set_timer_deadtime(timer_channel_t *channel, uint16_t dead_time)
{
  if (!channel_is_valid(channel))
  {
    return;
  }

  HRTIM_DeadTimeCfgTypeDef pDeadTimeCfg = {0};
  pDeadTimeCfg.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
  pDeadTimeCfg.RisingValue = ns_to_ticks(dead_time);
  pDeadTimeCfg.FallingValue = ns_to_ticks(dead_time);
  if (HAL_HRTIM_DeadTimeConfig(&hhrtim, kTimerIndex[channel->number], &pDeadTimeCfg) != HAL_OK)
  {
    Error_Handler();
  }
  channel->dead_time = dead_time;
}

void set_timer_frequency(timer_channel_t *channel, uint32_t frequency)
{
  if (!channel_is_valid(channel) || (frequency == 0U))
  {
    return;
  }

  uint32_t idx = channel->number;
  uint32_t period_ticks = hz_to_period_ticks(frequency);

  HRTIM_TimeBaseCfgTypeDef pTimeBaseCfg = {0};
  pTimeBaseCfg.Period = period_ticks;
  pTimeBaseCfg.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  pTimeBaseCfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, kTimerIndex[idx], &pTimeBaseCfg) != HAL_OK)
  {
    Error_Handler();
  }

  /* Period just changed - reapply the last commanded duty% against the new
     period so a frequency change doesn't silently also change duty. */
  uint32_t compare_ticks = duty_to_compare_ticks(period_ticks, channel->duty_cycle);
  __HAL_HRTIM_SETCOMPARE(&hhrtim, kTimerIndex[idx], HRTIM_COMPAREUNIT_1, compare_ticks);

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
