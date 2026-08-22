#include "pwm.h"
#include "hrtim.h"
#include "main.h"

/* HRTIM kernel clock. Not a tunable - it follows the PLL chain set up in
   SystemClock_Config(): HSE 8 MHz -> PLLM 1 -> PLLN 120 -> VCO 960 MHz ->
   PLLP 2 -> 480 MHz SYSCLK, with RCC_HRTIM1CLK_CPUCLK selecting the CPU clock
   rather than the APB2 timer clock. That gives 2.0833 ns per tick. Every
   period and dead-time conversion below is derived from it, so it must be
   revisited if the PLL chain changes. See .agents/hardware.md. */
#define PWM_KERNEL_CLOCK_HZ 480000000U

#define ALL_TIMERS                                                         \
  (HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B | HRTIM_TIMERID_TIMER_C | \
   HRTIM_TIMERID_TIMER_D | HRTIM_TIMERID_TIMER_E)
#define ALL_OUTPUTS                                                            \
  (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2 | \
   HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2 | HRTIM_OUTPUT_TD1 | HRTIM_OUTPUT_TD2 | \
   HRTIM_OUTPUT_TE1 | HRTIM_OUTPUT_TE2)

typedef struct {
  channel_t *data;
  uint32_t timer_index;
  uint32_t timer_id;
  uint32_t output1;
  uint32_t output2;
  uint32_t timer_update;
  uint32_t fault_enable;
  uint32_t fault_irq;
  uint32_t fault_flag;
  GPIO_TypeDef *fault_port;
  uint16_t fault_pin;
} channel_hw_t;

static const channel_hw_t hw_a = {
    .data = &chan_a,
    .timer_index = HRTIM_TIMERINDEX_TIMER_A,
    .timer_id = HRTIM_TIMERID_TIMER_A,
    .output1 = HRTIM_OUTPUT_TA1,
    .output2 = HRTIM_OUTPUT_TA2,
    .timer_update = HRTIM_TIMERUPDATE_A,
    .fault_enable = HRTIM_TIMFAULTENABLE_FAULT1,
    .fault_irq = HRTIM_IT_FLT1,
    .fault_flag = HRTIM_FLAG_FLT1,
    .fault_port = GPIOA,
    .fault_pin = GPIO_PIN_15,
};

static const channel_hw_t hw_b = {
    .data = &chan_b,
    .timer_index = HRTIM_TIMERINDEX_TIMER_B,
    .timer_id = HRTIM_TIMERID_TIMER_B,
    .output1 = HRTIM_OUTPUT_TB1,
    .output2 = HRTIM_OUTPUT_TB2,
    .timer_update = HRTIM_TIMERUPDATE_B,
    .fault_enable = HRTIM_TIMFAULTENABLE_FAULT2,
    .fault_irq = HRTIM_IT_FLT2,
    .fault_flag = HRTIM_FLAG_FLT2,
    .fault_port = GPIOC,
    .fault_pin = GPIO_PIN_11,
};

static const channel_hw_t hw_c = {
    .data = &chan_c,
    .timer_index = HRTIM_TIMERINDEX_TIMER_C,
    .timer_id = HRTIM_TIMERID_TIMER_C,
    .output1 = HRTIM_OUTPUT_TC1,
    .output2 = HRTIM_OUTPUT_TC2,
    .timer_update = HRTIM_TIMERUPDATE_C,
    .fault_enable = HRTIM_TIMFAULTENABLE_FAULT3,
    .fault_irq = HRTIM_IT_FLT3,
    .fault_flag = HRTIM_FLAG_FLT3,
    .fault_port = GPIOD,
    .fault_pin = GPIO_PIN_4,
};

static const channel_hw_t hw_d = {
    .data = &chan_d,
    .timer_index = HRTIM_TIMERINDEX_TIMER_D,
    .timer_id = HRTIM_TIMERID_TIMER_D,
    .output1 = HRTIM_OUTPUT_TD1,
    .output2 = HRTIM_OUTPUT_TD2,
    .timer_update = HRTIM_TIMERUPDATE_D,
    .fault_enable = HRTIM_TIMFAULTENABLE_FAULT4,
    .fault_irq = HRTIM_IT_FLT4,
    .fault_flag = HRTIM_FLAG_FLT4,
    .fault_port = GPIOB,
    .fault_pin = GPIO_PIN_3,
};

static const channel_hw_t hw_e = {
    .data = &chan_e,
    .timer_index = HRTIM_TIMERINDEX_TIMER_E,
    .timer_id = HRTIM_TIMERID_TIMER_E,
    .output1 = HRTIM_OUTPUT_TE1,
    .output2 = HRTIM_OUTPUT_TE2,
    .timer_update = HRTIM_TIMERUPDATE_E,
    .fault_enable = HRTIM_TIMFAULTENABLE_FAULT5,
    .fault_irq = HRTIM_IT_FLT5,
    .fault_flag = HRTIM_FLAG_FLT5,
    .fault_port = GPIOG,
    .fault_pin = GPIO_PIN_10,
};

// Channel hardware put in array for easy access
static const channel_hw_t *const channel_hardware[CHANNEL_COUNT] = {
    [CHANNEL_A] = &hw_a,
    [CHANNEL_B] = &hw_b,
    [CHANNEL_C] = &hw_c,
    [CHANNEL_D] = &hw_d,
    [CHANNEL_E] = &hw_e,
};

// Access channel hardware values using channel number.
static const channel_hw_t *channel_hw(uint32_t channel) {
  return channel_hardware[channel];
}

// Controls and updates the status of pwm in each channel.
static void set_op_state(channel_t *ch, pwm_state_t op_state) {
  ch->pwm.op_state = op_state;
  ch->pwm.seq++;
}


static uint32_t clamp(uint32_t value, uint32_t min, uint32_t max) {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}

#define PWM_MIN_COMPARE_TICKS 3U

static uint32_t enter_critical(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  __DMB();
  return primask;
}

static void exit_critical(uint32_t primask) {
  __DMB();
  if (primask == 0U) {
    __enable_irq();
  }
}

static bool ovp_is_active(void) {
  return HAL_GPIO_ReadPin(OVP_GPIO_Port, OVP_Pin) == GPIO_PIN_SET;
}

static bool ovp_is_pending(void) {
  return __HAL_GPIO_EXTI_GET_IT(OVP_Pin) != 0U;
}

static bool ocp_is_active(const channel_hw_t *hw) {
  return HAL_GPIO_ReadPin(hw->fault_port, hw->fault_pin) == GPIO_PIN_SET;
}

static bool ocp_is_pending(const channel_hw_t *hw) {
  return __HAL_HRTIM_GET_FLAG(&hhrtim, hw->fault_flag) != RESET;
}

/* Direct register writes, not HAL: the HAL stop calls take __HAL_LOCK and
   would silently no-op if the handle were busy - unacceptable for a fault
   stop that must always disable the outputs. */
static void channel_stop_hw(const channel_hw_t *hw) {
  SET_BIT(hhrtim.Instance->sCommonRegs.ODISR, hw->output1 | hw->output2);
  CLEAR_BIT(hhrtim.Instance->sMasterRegs.MCR, hw->timer_id);
}

static void all_channels_stop_hw(void) {
  SET_BIT(hhrtim.Instance->sCommonRegs.ODISR, ALL_OUTPUTS);
  CLEAR_BIT(hhrtim.Instance->sMasterRegs.MCR, ALL_TIMERS);
}

static void latch_OCP_fault(const channel_hw_t *hw) {
  channel_t *ch = hw->data;

  channel_stop_hw(hw);
  ch->pwm.ocp_latched = true;
  if (ch->pwm.op_state != PWM_STATE_UNINITIALIZED) {
    set_op_state(ch, PWM_STATE_FAULTED);
  }
}

static void latch_OVP_fault(void) {
  all_channels_stop_hw();
  sys.ovp_latched = true;
  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++) {
    channel_t *ch = channel_hardware[i]->data;
    if (ch->pwm.op_state != PWM_STATE_UNINITIALIZED) {
      set_op_state(ch, PWM_STATE_FAULTED);
    }
  }
}

static bool latch_present_faults(const channel_hw_t *hw) {
  bool ovp = sys.ovp_latched || ovp_is_active() || ovp_is_pending();
  bool ocp = hw->data->pwm.ocp_latched || ocp_is_active(hw) || ocp_is_pending(hw);
  if (ovp) {
    latch_OVP_fault();
  }
  if (ocp) {
    latch_OCP_fault(hw);
  }
  return ovp || ocp;
}

void pwm_init(uint32_t channel) {
  const channel_hw_t *hw = channel_hw(channel);
  channel_t *ch = hw->data;

  pwm_stop(channel);

  ch->pwm.duty_applied = (uint16_t)clamp(ch->pwm.duty_applied, PWM_MIN_DUTY_CYCLE, PWM_MAX_DUTY_CYCLE);
  pwm_set_frequency(channel, ch->pwm.frequency_hz);
  (void)pwm_set_duty_cycle(channel, ch->pwm.duty_applied);
  pwm_set_dead_time(channel, ch->pwm.dead_time_ns);

  HRTIM_OutputCfgTypeDef output1_cfg = {0};
  output1_cfg.SetSource = HRTIM_OUTPUTSET_TIMPER;
  output1_cfg.ResetSource = HRTIM_OUTPUTRESET_TIMCMP1;
  output1_cfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, hw->timer_index, hw->output1, &output1_cfg) != HAL_OK) {
    Error_Handler();
  }

  HRTIM_OutputCfgTypeDef output2_cfg = {0};
  output2_cfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
  if (HAL_HRTIM_WaveformOutputConfig(&hhrtim, hw->timer_index, hw->output2, &output2_cfg) != HAL_OK) {
    Error_Handler();
  }

  HRTIM_TimerCfgTypeDef timer_cfg = {0};
  timer_cfg.PreloadEnable = HRTIM_PRELOAD_ENABLED;
  timer_cfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_ENABLED;
  timer_cfg.FaultEnable = hw->fault_enable;
  timer_cfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_ENABLED;
  if (HAL_HRTIM_WaveformTimerConfig(&hhrtim, hw->timer_index, &timer_cfg) != HAL_OK) {
    Error_Handler();
  }

  uint32_t primask = enter_critical();
  set_op_state(ch, PWM_STATE_STOPPED);
  (void)latch_present_faults(hw);
  __HAL_HRTIM_ENABLE_IT(&hhrtim, hw->fault_irq);
  exit_critical(primask);
}

bool pwm_set_duty_cycle(uint32_t channel, uint16_t duty_cycle) {
  if ((duty_cycle < PWM_MIN_DUTY_CYCLE) || (duty_cycle > PWM_MAX_DUTY_CYCLE)) {
    return false;
  }
  const channel_hw_t *hw = channel_hw(channel);
  channel_t *ch = hw->data;
  uint32_t period_ticks = PWM_KERNEL_CLOCK_HZ / clamp(ch->pwm.frequency_hz, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ);
  uint32_t compare = (period_ticks * clamp(duty_cycle, PWM_MIN_DUTY_CYCLE, PWM_MAX_DUTY_CYCLE)) / PWM_DUTY_SCALE;

  if (compare < PWM_MIN_COMPARE_TICKS) {
    compare = PWM_MIN_COMPARE_TICKS;
  }

  __HAL_HRTIM_SETCOMPARE(&hhrtim, hw->timer_index, HRTIM_COMPAREUNIT_1, compare);
  ch->pwm.duty_applied = duty_cycle;

  if (ch->pwm.op_state != PWM_STATE_RUNNING) {
    if (HAL_HRTIM_SoftwareUpdate(&hhrtim, hw->timer_update) != HAL_OK) {
      Error_Handler();
    }
  }
  return true;
}

void pwm_set_dead_time(uint32_t channel, uint16_t dead_time) {
  const channel_hw_t *hw = channel_hw(channel);
  channel_t *ch = hw->data;
  bool restart = ch->pwm.op_state == PWM_STATE_RUNNING;

  if (restart) {
    pwm_stop(channel);
  }

  dead_time = (uint16_t)clamp(dead_time, PWM_MIN_DEAD_TIME_NS, PWM_MAX_DEAD_TIME_NS);

  /* Rounds up, so the actual dead time is never shorter than requested, and
     never rounds down to zero. */
  uint64_t ns_ticks = (((uint64_t)dead_time * PWM_KERNEL_CLOCK_HZ) + 999999999ULL) / 1000000000ULL;
  uint32_t dead_time_ticks = (ns_ticks == 0U) ? 1U : (uint32_t)ns_ticks;

  HRTIM_DeadTimeCfgTypeDef dead_time_cfg = {0};
  dead_time_cfg.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
  dead_time_cfg.RisingValue = dead_time_ticks;
  dead_time_cfg.FallingValue = dead_time_ticks;
  if (HAL_HRTIM_DeadTimeConfig(&hhrtim, hw->timer_index, &dead_time_cfg) != HAL_OK) {
    Error_Handler();
  }
  ch->pwm.dead_time_ns = dead_time;

  if (restart) {
    (void)pwm_start(channel);
  }
}

void pwm_set_frequency(uint32_t channel, uint32_t frequency) {
  const channel_hw_t *hw = channel_hw(channel);
  channel_t *ch = hw->data;
  bool restart = ch->pwm.op_state == PWM_STATE_RUNNING;

  if (restart) {
    pwm_stop(channel);
  }

  frequency = clamp(frequency, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ);
  /* Integer division truncates, so the actual frequency lands at or just above
     what was asked for. */
  uint32_t period_ticks = PWM_KERNEL_CLOCK_HZ / frequency;

  HRTIM_TimeBaseCfgTypeDef time_base_cfg = {0};
  time_base_cfg.Period = period_ticks;
  time_base_cfg.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  time_base_cfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, hw->timer_index, &time_base_cfg) != HAL_OK) {
    Error_Handler();
  }

  /* Reapply duty against the new period so frequency changes preserve duty. */
  uint32_t compare = (period_ticks * clamp(ch->pwm.duty_applied, PWM_MIN_DUTY_CYCLE, PWM_MAX_DUTY_CYCLE)) / PWM_DUTY_SCALE;

  /* Never below the floor - see PWM_MIN_COMPARE_TICKS. */
  if (compare < PWM_MIN_COMPARE_TICKS) {
    compare = PWM_MIN_COMPARE_TICKS;
  }

  __HAL_HRTIM_SETCOMPARE(&hhrtim, hw->timer_index, HRTIM_COMPAREUNIT_1, compare);
  if (HAL_HRTIM_SoftwareUpdate(&hhrtim, hw->timer_update) != HAL_OK) {
    Error_Handler();
  }
  ch->pwm.frequency_hz = frequency;

  if (restart) {
    (void)pwm_start(channel);
  }
}

bool pwm_start(uint32_t channel) {
  const channel_hw_t *hw = channel_hw(channel);
  uint32_t primask = enter_critical();

  if (hw->data->pwm.op_state != PWM_STATE_STOPPED) {
    exit_critical(primask);
    return false;
  }
  if (latch_present_faults(hw)) {
    exit_critical(primask);
    return false;
  }

  /* Transfer preloaded values, start the counter, enable the outputs. */
  SET_BIT(hhrtim.Instance->sCommonRegs.CR2, hw->timer_update);
  SET_BIT(hhrtim.Instance->sMasterRegs.MCR, hw->timer_id);
  SET_BIT(hhrtim.Instance->sCommonRegs.OENR, hw->output1 | hw->output2);

  /* Re-check: a fault edge could have arrived during the enable sequence. */
  if (latch_present_faults(hw)) {
    exit_critical(primask);
    return false;
  }

  set_op_state(hw->data, PWM_STATE_RUNNING);
  exit_critical(primask);
  return true;
}

void pwm_stop(uint32_t channel) {
  const channel_hw_t *hw = channel_hw(channel);
  channel_t *ch = hw->data;
  uint32_t primask = enter_critical();

  channel_stop_hw(hw);
  if (ch->pwm.op_state == PWM_STATE_RUNNING) {
    set_op_state(ch, PWM_STATE_STOPPED);
  }
  exit_critical(primask);
}

bool pwm_clear_OCP_fault(uint32_t channel) {
  const channel_hw_t *hw = channel_hw(channel);
  channel_t *ch = hw->data;
  uint32_t primask = enter_critical();

  if (ch->pwm.op_state == PWM_STATE_UNINITIALIZED) {
    exit_critical(primask);
    return false;
  }
  /* A live/pending OVP takes over; a latched OVP blocks per-channel clears. */
  if (ovp_is_active() || ovp_is_pending()) {
    latch_OVP_fault();
    exit_critical(primask);
    return false;
  }
  if (sys.ovp_latched) {
    exit_critical(primask);
    return false;
  }
  /* Nothing latched or pending here - already clear (idempotent success). */
  if (!ch->pwm.ocp_latched && !ocp_is_active(hw) && !ocp_is_pending(hw)) {
    exit_critical(primask);
    return true;
  }

  /* Refuse while the OCP condition is still physically present. */
  channel_stop_hw(hw);
  if (ocp_is_active(hw)) {
    latch_OCP_fault(hw);
    exit_critical(primask);
    return false;
  }
  __HAL_HRTIM_CLEAR_FLAG(&hhrtim, hw->fault_flag);
  __DMB();
  if (ocp_is_active(hw) || ocp_is_pending(hw)) {
    latch_OCP_fault(hw);
    exit_critical(primask);
    return false;
  }

  ch->pwm.ocp_latched = false;
  set_op_state(ch, PWM_STATE_STOPPED);
  exit_critical(primask);
  return true;
}

bool pwm_clear_OVP_fault(void) {
  uint32_t primask = enter_critical();
  bool had_OVP_fault = sys.ovp_latched || ovp_is_pending();

  if (ovp_is_active()) {
    latch_OVP_fault();
    exit_critical(primask);
    return false;
  }
  if (!had_OVP_fault) {
    exit_critical(primask);
    return true;
  }

  /* A pending edge is a fault even if its pulse ended before this call. */
  latch_OVP_fault();
  __HAL_GPIO_EXTI_CLEAR_IT(OVP_Pin);
  __DMB();
  if (ovp_is_active() || ovp_is_pending()) {
    exit_critical(primask);
    return false;
  }

  sys.ovp_latched = false;
  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++) {
    const channel_hw_t *hw = channel_hw(i);
    channel_t *ch = hw->data;

    if (ch->pwm.op_state == PWM_STATE_UNINITIALIZED) {
      continue;
    }
    /* Re-latch any channel still holding its own OCP fault; the rest fall
       back to STOPPED now the OVP flag is clear. That recovery is explicit
       now that FAULTED is stored rather than derived - nothing else would
       take these channels back out of it. */
    if (ch->pwm.ocp_latched || ocp_is_active(hw) || ocp_is_pending(hw)) {
      latch_OCP_fault(hw);
    } else if (ch->pwm.op_state == PWM_STATE_FAULTED) {
      set_op_state(ch, PWM_STATE_STOPPED);
    }
  }

  exit_critical(primask);
  return true;
}

/* ISR context: hardware has already forced the outputs off; keep them
   disabled after the physical fault signal clears. */
void pwm_OCP_fault(uint32_t channel) {
  latch_OCP_fault(channel_hw(channel));
}

void pwm_OVP_fault(void) {
  latch_OVP_fault();
}
