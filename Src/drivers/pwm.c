#include "pwm.h"
#include "hrtim.h"
#include "main.h"

#define ALL_TIMERS                                                         \
  (HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B | HRTIM_TIMERID_TIMER_C | \
   HRTIM_TIMERID_TIMER_D | HRTIM_TIMERID_TIMER_E)
#define ALL_OUTPUTS                                                            \
  (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2 | \
   HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2 | HRTIM_OUTPUT_TD1 | HRTIM_OUTPUT_TD2 | \
   HRTIM_OUTPUT_TE1 | HRTIM_OUTPUT_TE2)

#define CHANNEL_DEFAULTS(id)                                                \
  {.number = (id), .frequency = PWM_DEFAULT_FREQUENCY_HZ,                   \
   .duty_cycle = PWM_DEFAULT_DUTY_CYCLE, .dead_time = PWM_DEFAULT_DEAD_TIME_NS}

pwm_channel_t channel_a = CHANNEL_DEFAULTS(PWM_CHANNEL_A);
pwm_channel_t channel_b = CHANNEL_DEFAULTS(PWM_CHANNEL_B);
pwm_channel_t channel_c = CHANNEL_DEFAULTS(PWM_CHANNEL_C);
pwm_channel_t channel_d = CHANNEL_DEFAULTS(PWM_CHANNEL_D);
pwm_channel_t channel_e = CHANNEL_DEFAULTS(PWM_CHANNEL_E);

volatile bool pwm_OVP_fault_latched = false;

/* Fixed per-channel wiring: HRTIM timer, output pair, and the OCP fault line
   (FLT1->A ... FLT5->E) with its GPIO for reading the live pin level. */
typedef struct {
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

/* Fields in struct order: index, id, out1, out2, update, fault_en, fault_irq,
   fault_flag, port, pin. */
static const channel_hw_t channel_hardware[PWM_CHANNEL_COUNT] = {
    [PWM_CHANNEL_A] = {HRTIM_TIMERINDEX_TIMER_A, HRTIM_TIMERID_TIMER_A, HRTIM_OUTPUT_TA1, HRTIM_OUTPUT_TA2, HRTIM_TIMERUPDATE_A,
                       HRTIM_TIMFAULTENABLE_FAULT1, HRTIM_IT_FLT1, HRTIM_FLAG_FLT1, GPIOA, GPIO_PIN_15},
    [PWM_CHANNEL_B] = {HRTIM_TIMERINDEX_TIMER_B, HRTIM_TIMERID_TIMER_B, HRTIM_OUTPUT_TB1, HRTIM_OUTPUT_TB2, HRTIM_TIMERUPDATE_B,
                       HRTIM_TIMFAULTENABLE_FAULT2, HRTIM_IT_FLT2, HRTIM_FLAG_FLT2, GPIOC, GPIO_PIN_11},
    [PWM_CHANNEL_C] = {HRTIM_TIMERINDEX_TIMER_C, HRTIM_TIMERID_TIMER_C, HRTIM_OUTPUT_TC1, HRTIM_OUTPUT_TC2, HRTIM_TIMERUPDATE_C,
                       HRTIM_TIMFAULTENABLE_FAULT3, HRTIM_IT_FLT3, HRTIM_FLAG_FLT3, GPIOD, GPIO_PIN_4},
    [PWM_CHANNEL_D] = {HRTIM_TIMERINDEX_TIMER_D, HRTIM_TIMERID_TIMER_D, HRTIM_OUTPUT_TD1, HRTIM_OUTPUT_TD2, HRTIM_TIMERUPDATE_D,
                       HRTIM_TIMFAULTENABLE_FAULT4, HRTIM_IT_FLT4, HRTIM_FLAG_FLT4, GPIOB, GPIO_PIN_3},
    [PWM_CHANNEL_E] = {HRTIM_TIMERINDEX_TIMER_E, HRTIM_TIMERID_TIMER_E, HRTIM_OUTPUT_TE1, HRTIM_OUTPUT_TE2, HRTIM_TIMERUPDATE_E,
                       HRTIM_TIMFAULTENABLE_FAULT5, HRTIM_IT_FLT5, HRTIM_FLAG_FLT5, GPIOG, GPIO_PIN_10}};

static pwm_channel_t *const channels[PWM_CHANNEL_COUNT] = {
    &channel_a, &channel_b, &channel_c, &channel_d, &channel_e};

pwm_channel_t *pwm_channel(pwm_channel_id_t id) {
  if ((uint32_t)id >= (uint32_t)PWM_CHANNEL_COUNT) {
    return NULL;
  }
  return channels[(uint32_t)id];
}

static const channel_hw_t *channel_hw(pwm_channel_id_t channel) {
  return &channel_hardware[(uint32_t)channel];
}

static bool channel_is_valid(const pwm_channel_t *channel) {
  if ((channel == NULL) || ((uint32_t)channel->number >= (uint32_t)PWM_CHANNEL_COUNT)) {
    return false;
  }
  return channel == channels[(uint32_t)channel->number];
}

/* Sole writer of op_state - only ever UNINITIALIZED/STOPPED/RUNNING. Faults
   live in the boolean flags and are surfaced by pwm_get_state(). */
static void set_op_state(pwm_channel_t *channel, pwm_state_t op_state) {
  channel->op_state = op_state;
}

pwm_state_t pwm_get_state(const pwm_channel_t *channel) {
  if (!channel_is_valid(channel) || (channel->op_state == PWM_STATE_UNINITIALIZED)) {
    return PWM_STATE_UNINITIALIZED;
  }
  if (channel->ocp_fault || pwm_OVP_fault_latched) {
    return PWM_STATE_FAULTED;
  }
  return channel->op_state;
}

static uint32_t clamp(uint32_t value, uint32_t min, uint32_t max) {
  return (value < min) ? min : ((value > max) ? max : value);
}

/* Truncates: actual frequency lands at or just above requested. */
static uint32_t hz_to_period_ticks(uint32_t frequency_hz) {
  return PWM_KERNEL_CLOCK_HZ / frequency_hz;
}

/* Rounds up: actual dead time is never shorter than requested. */
static uint32_t ns_to_ticks(uint16_t nanoseconds) {
  uint64_t ticks = (((uint64_t)nanoseconds * PWM_KERNEL_CLOCK_HZ) + 999999999ULL) / 1000000000ULL;
  return (ticks == 0U) ? 1U : (uint32_t)ticks;
}

/* Smallest legal HRTIM compare value at prescaler DIV1: 3 periods of f_HRTIM,
   i.e. 6.25 ns at 480 MHz. Zero is not legal and its behaviour is undefined -
   plausibly leaving output1 permanently set, which with output1 driving the
   control FET is a continuous inductor short. Never program below this. */
#define PWM_MIN_COMPARE_TICKS 3U

/* A 0% request lands on the floor above rather than on zero. The resulting
   pulse is 3 ticks wide, and dead-time insertion delays output1's turn-on by
   the rising dead time - itself never fewer than 3 ticks, since
   PWM_MIN_DEAD_TIME_NS (5 ns) rounds up to 3 ticks at 480 MHz. So the pulse is
   fully consumed and the control FET does not conduct at all, which is what
   makes 0% the safe pass-through state a channel starts from.

   Output1 being the control FET was confirmed on the bench 2026-08-15; the
   safety of a 0% default rests on it. See .agents/hardware.md. */
static uint32_t duty_to_compare_ticks(uint32_t period_ticks, uint16_t duty_tenths_pct) {
  uint32_t duty = clamp(duty_tenths_pct, PWM_MIN_DUTY_CYCLE, PWM_MAX_DUTY_CYCLE);
  uint32_t compare = (period_ticks * duty) / PWM_DUTY_SCALE;
  return (compare < PWM_MIN_COMPARE_TICKS) ? PWM_MIN_COMPARE_TICKS : compare;
}

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

static void latch_OCP_fault(pwm_channel_t *channel, const channel_hw_t *hw) {
  channel_stop_hw(hw);
  channel->ocp_fault = true;
  if (channel->op_state == PWM_STATE_RUNNING) {
    set_op_state(channel, PWM_STATE_STOPPED);
  }
}

static void latch_OVP_fault(void) {
  all_channels_stop_hw();
  pwm_OVP_fault_latched = true;
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++) {
    if (channels[i]->op_state == PWM_STATE_RUNNING) {
      set_op_state(channels[i], PWM_STATE_STOPPED);
    }
  }
}

/* Latches whatever fault is present now (latched flag, live pin, or pending
   flag - for either OVP or this channel's OCP). Returns true if any latched.
   Call inside a critical section. */
static bool latch_present_faults(pwm_channel_t *channel, const channel_hw_t *hw) {
  bool ovp = pwm_OVP_fault_latched || ovp_is_active() || ovp_is_pending();
  bool ocp = channel->ocp_fault || ocp_is_active(hw) || ocp_is_pending(hw);
  if (ovp) {
    latch_OVP_fault();
  }
  if (ocp) {
    latch_OCP_fault(channel, hw);
  }
  return ovp || ocp;
}

void pwm_init(pwm_channel_t *channel) {
  if (!channel_is_valid(channel)) {
    return;
  }
  const channel_hw_t *hw = channel_hw(channel->number);

  /* Never leave live outputs enabled across reconfiguration. A latched fault
     is left untouched, so pwm_get_state() keeps reporting FAULTED. */
  pwm_stop(channel);

  /* Duty is clamped here because pwm_set_duty_cycle() rejects (not clamps)
     out-of-range values; the other setters clamp and write back themselves. */
  channel->duty_cycle = (uint16_t)clamp(channel->duty_cycle, PWM_MIN_DUTY_CYCLE, PWM_MAX_DUTY_CYCLE);
  pwm_set_frequency(channel, channel->frequency);
  (void)pwm_set_duty_cycle(channel, channel->duty_cycle);
  pwm_set_dead_time(channel, channel->dead_time);

  /* Output 1 set at period, reset at CMP1; output 2 is its dead-time
     complement. FaultLevel INACTIVE forces both outputs off on a fault. */
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
  (void)latch_present_faults(channel, hw);
  set_op_state(channel, PWM_STATE_STOPPED);
  /* Valid state is in place before an already-pending fault IRQ can run. */
  __HAL_HRTIM_ENABLE_IT(&hhrtim, hw->fault_irq);
  exit_critical(primask);
}

bool pwm_set_duty_cycle(pwm_channel_t *channel, uint16_t duty_cycle) {
  if (!channel_is_valid(channel) || (duty_cycle < PWM_MIN_DUTY_CYCLE) || (duty_cycle > PWM_MAX_DUTY_CYCLE)) {
    return false;
  }
  const channel_hw_t *hw = channel_hw(channel->number);
  uint32_t period_ticks = hz_to_period_ticks(clamp(channel->frequency, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ));
  __HAL_HRTIM_SETCOMPARE(&hhrtim, hw->timer_index, HRTIM_COMPAREUNIT_1, duty_to_compare_ticks(period_ticks, duty_cycle));
  channel->duty_cycle = duty_cycle;

  /* A running timer transfers the preloaded compare at the next period; a
     stopped one needs an explicit transfer before it is started. */
  if (channel->op_state != PWM_STATE_RUNNING) {
    if (HAL_HRTIM_SoftwareUpdate(&hhrtim, hw->timer_update) != HAL_OK) {
      Error_Handler();
    }
  }
  return true;
}

void pwm_set_dead_time(pwm_channel_t *channel, uint16_t dead_time) {
  if (!channel_is_valid(channel)) {
    return;
  }
  bool restart = channel->op_state == PWM_STATE_RUNNING;
  if (restart) {
    pwm_stop(channel);
  }

  dead_time = (uint16_t)clamp(dead_time, PWM_MIN_DEAD_TIME_NS, PWM_MAX_DEAD_TIME_NS);
  const channel_hw_t *hw = channel_hw(channel->number);
  HRTIM_DeadTimeCfgTypeDef dead_time_cfg = {0};
  dead_time_cfg.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV1;
  dead_time_cfg.RisingValue = ns_to_ticks(dead_time);
  dead_time_cfg.FallingValue = ns_to_ticks(dead_time);
  if (HAL_HRTIM_DeadTimeConfig(&hhrtim, hw->timer_index, &dead_time_cfg) != HAL_OK) {
    Error_Handler();
  }
  channel->dead_time = dead_time;

  if (restart) {
    (void)pwm_start(channel);
  }
}

void pwm_set_frequency(pwm_channel_t *channel, uint32_t frequency) {
  if (!channel_is_valid(channel)) {
    return;
  }
  bool restart = channel->op_state == PWM_STATE_RUNNING;
  if (restart) {
    pwm_stop(channel);
  }

  frequency = clamp(frequency, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ);
  const channel_hw_t *hw = channel_hw(channel->number);
  uint32_t period_ticks = hz_to_period_ticks(frequency);

  HRTIM_TimeBaseCfgTypeDef time_base_cfg = {0};
  time_base_cfg.Period = period_ticks;
  time_base_cfg.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  time_base_cfg.Mode = HRTIM_MODE_CONTINUOUS;
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, hw->timer_index, &time_base_cfg) != HAL_OK) {
    Error_Handler();
  }

  /* Reapply duty against the new period so frequency changes preserve duty. */
  __HAL_HRTIM_SETCOMPARE(&hhrtim, hw->timer_index, HRTIM_COMPAREUNIT_1,
                         duty_to_compare_ticks(period_ticks, channel->duty_cycle));
  if (HAL_HRTIM_SoftwareUpdate(&hhrtim, hw->timer_update) != HAL_OK) {
    Error_Handler();
  }
  channel->frequency = frequency;

  if (restart) {
    (void)pwm_start(channel);
  }
}

bool pwm_start(pwm_channel_t *channel) {
  if (!channel_is_valid(channel)) {
    return false;
  }
  const channel_hw_t *hw = channel_hw(channel->number);
  uint32_t primask = enter_critical();

  /* Rejects a faulted channel too: pwm_get_state() reports FAULTED, not
     STOPPED, while any fault is latched. */
  if (pwm_get_state(channel) != PWM_STATE_STOPPED) {
    exit_critical(primask);
    return false;
  }
  if (latch_present_faults(channel, hw)) {
    exit_critical(primask);
    return false;
  }

  /* Transfer preloaded values, start the counter, enable the outputs. */
  SET_BIT(hhrtim.Instance->sCommonRegs.CR2, hw->timer_update);
  SET_BIT(hhrtim.Instance->sMasterRegs.MCR, hw->timer_id);
  SET_BIT(hhrtim.Instance->sCommonRegs.OENR, hw->output1 | hw->output2);

  /* Re-check: a fault edge could have arrived during the enable sequence. */
  if (latch_present_faults(channel, hw)) {
    exit_critical(primask);
    return false;
  }

  set_op_state(channel, PWM_STATE_RUNNING);
  exit_critical(primask);
  return true;
}

void pwm_stop(pwm_channel_t *channel) {
  if (!channel_is_valid(channel)) {
    return;
  }
  const channel_hw_t *hw = channel_hw(channel->number);
  uint32_t primask = enter_critical();
  channel_stop_hw(hw);
  if (channel->op_state == PWM_STATE_RUNNING) {
    set_op_state(channel, PWM_STATE_STOPPED);
  }
  exit_critical(primask);
}

bool pwm_clear_OCP_fault(pwm_channel_t *channel) {
  if (!channel_is_valid(channel)) {
    return false;
  }
  const channel_hw_t *hw = channel_hw(channel->number);
  uint32_t primask = enter_critical();

  if (channel->op_state == PWM_STATE_UNINITIALIZED) {
    exit_critical(primask);
    return false;
  }
  /* A live/pending OVP takes over; a latched OVP blocks per-channel clears. */
  if (ovp_is_active() || ovp_is_pending()) {
    latch_OVP_fault();
    exit_critical(primask);
    return false;
  }
  if (pwm_OVP_fault_latched) {
    exit_critical(primask);
    return false;
  }
  /* Nothing latched or pending here - already clear (idempotent success). */
  if (!channel->ocp_fault && !ocp_is_active(hw) && !ocp_is_pending(hw)) {
    exit_critical(primask);
    return true;
  }

  /* Refuse while the OCP condition is still physically present. */
  channel_stop_hw(hw);
  if (ocp_is_active(hw)) {
    latch_OCP_fault(channel, hw);
    exit_critical(primask);
    return false;
  }
  __HAL_HRTIM_CLEAR_FLAG(&hhrtim, hw->fault_flag);
  __DMB();
  if (ocp_is_active(hw) || ocp_is_pending(hw)) {
    latch_OCP_fault(channel, hw);
    exit_critical(primask);
    return false;
  }

  channel->ocp_fault = false;
  set_op_state(channel, PWM_STATE_STOPPED);
  exit_critical(primask);
  return true;
}

bool pwm_clear_OVP_fault(void) {
  uint32_t primask = enter_critical();
  bool had_OVP_fault = pwm_OVP_fault_latched || ovp_is_pending();

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

  pwm_OVP_fault_latched = false;
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++) {
    pwm_channel_t *channel = channels[i];
    if (channel->op_state == PWM_STATE_UNINITIALIZED) {
      continue;
    }
    /* Re-latch any channel still holding its own OCP fault; the rest fall
       back to STOPPED now the OVP flag is clear. */
    const channel_hw_t *hw = channel_hw(channel->number);
    if (channel->ocp_fault || ocp_is_active(hw) || ocp_is_pending(hw)) {
      latch_OCP_fault(channel, hw);
    }
  }

  exit_critical(primask);
  return true;
}

/* ISR context: hardware has already forced the outputs off; keep them
   disabled after the physical fault signal clears. */
void pwm_OCP_fault(pwm_channel_id_t channel) {
  if ((uint32_t)channel >= (uint32_t)PWM_CHANNEL_COUNT) {
    return;
  }
  latch_OCP_fault(channels[(uint32_t)channel], channel_hw(channel));
}

void pwm_OVP_fault(void) {
  latch_OVP_fault();
}
