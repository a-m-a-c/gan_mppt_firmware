/**
  ******************************************************************************
  * @file    iind.c
  * @author  Angus Macdonald
  * @brief   Inductor current sensing (INA310 amplifiers, HRTIM-triggered ADC).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "iind.h"

#include "adc.h"
#include "hrtim.h"
#include "main.h"

/* ADCs run 16-bit single-ended off a 3V3 reference. */
#define IIND_FULL_SCALE 65535U
#define IIND_VREF_MV    3300U

/* The amplifier presents gain x shunt volts per amp: 50 V/V across 3 mOhm is
   150 mV/A. The ADC's 3.3 V span therefore covers 22 A about the zero, and one
   count is 0.34 mA - finer than the amplifier's own offset and noise, so the
   part is the limit here, not the converter. */
#define IIND_MV_PER_AMP (((IIND_AMP_GAIN_V_PER_V) * (IIND_SHUNT_MICRO_OHMS)) / 1000U)
#define IIND_FULL_SCALE_MA ((IIND_VREF_MV * 1000U) / IIND_MV_PER_AMP)

/* Samples averaged when capturing the zero-current offset. 64 at 100 kHz is
   under a millisecond and takes the amplifier's noise down by 8x, which is
   worth having in a constant that every later reading is measured against. */
#define IIND_ZERO_SAMPLES 64U

/* ---------------------------------------------------------------------------
   DMA buffer placement - not a style choice, a hardware constraint.

   This project links .data and .bss into DTCMRAM, and on the STM32H7 the DMA
   controllers cannot reach DTCM at all. A buffer declared the ordinary way
   would sit at an address DMA silently cannot write, and the symptom is a
   buffer that never updates rather than any kind of error.

   Worse, the two ADCs have different reach. ADC2 is in domain D2 and is served
   by DMA1/DMA2, which can address D1 and D2 memory. ADC3 is in domain D3 and
   is served by BDMA, which can address *only* D3 SRAM. So the two buffers have
   to live in different places.

   Fixed addresses rather than linker sections: this project's .ld is CubeMX
   generated and gets overwritten on regeneration, and a silently dropped
   section would reintroduce exactly the failure above. RAM_D2 (0x30000000,
   288K) and RAM_D3 (0x38000000, 64K) are both entirely unused - the map file
   reports 0 bytes in each - so the first few words of each are claimed here.

   D-cache is not enabled in this project (main.c never calls
   SCB_EnableDCache), so no cache maintenance is needed around these. If it is
   ever enabled, these regions must be made non-cacheable in MPU_Config() or
   every read will return stale data. */
#define IIND_ADC2_BUFFER ((volatile uint16_t *)0x30000000UL) /* RAM_D2, DMA1/2 */
#define IIND_ADC3_BUFFER ((volatile uint16_t *)0x38000000UL) /* RAM_D3, BDMA   */

#define IIND_ADC3_CHANNELS 4U /* I_IND_2..5; I_IND_1 is alone on ADC2 */

/* Fixed per-channel wiring. Channel A is the odd one out - its amplifier is
   the only one that reaches ADC2 - so it is read from a different buffer than
   the rest. See .agents/hardware.md for why the split is not negotiable. */
typedef struct
{
  volatile uint16_t *buffer;
  uint32_t index;
} channel_source_t;

static const channel_source_t channel_source[PWM_CHANNEL_COUNT] = {
    [PWM_CHANNEL_A] = {IIND_ADC2_BUFFER, 0U}, /* PF13 ADC2_INP2 */
    [PWM_CHANNEL_B] = {IIND_ADC3_BUFFER, 0U}, /* PF3  ADC3_INP5 */
    [PWM_CHANNEL_C] = {IIND_ADC3_BUFFER, 1U}, /* PF5  ADC3_INP4 */
    [PWM_CHANNEL_D] = {IIND_ADC3_BUFFER, 2U}, /* PF7  ADC3_INP3 */
    [PWM_CHANNEL_E] = {IIND_ADC3_BUFFER, 3U}, /* PF9  ADC3_INP2 */
};

static uint16_t iind_zero_counts[PWM_CHANNEL_COUNT];
static iind_state_t iind_op_state = IIND_STATE_UNINITIALIZED;
static uint16_t iind_point = IIND_DEFAULT_SAMPLE_POINT;
static uint32_t iind_samples;
static uint32_t iind_last_sample_ms;

static uint32_t clamp(uint32_t value, uint32_t min, uint32_t max)
{
  return (value < min) ? min : ((value > max) ? max : value);
}

static uint16_t read_raw(pwm_channel_id_t channel)
{
  const channel_source_t *source = &channel_source[(uint32_t)channel];

  return source->buffer[source->index];
}

/* Master timer period and compare, recomputed whenever the switching frequency
   or the sample point changes.

   The master period is a whole multiple of the switching period, which is what
   keeps the sample instant locked to the same point on the current waveform
   instead of walking through it. The compare lands inside the *first* of those
   switching periods - the sample point is a position within one switching
   cycle, not within the master's longer cycle, or "halfway" would mean halfway
   through five periods and land nowhere useful. */
static bool configure_master(void)
{
  uint32_t frequency = clamp(channel_a.frequency, PWM_MIN_FREQUENCY_HZ, PWM_MAX_FREQUENCY_HZ);
  uint32_t period_ticks = PWM_KERNEL_CLOCK_HZ / frequency;
  uint32_t master_ticks = period_ticks * IIND_SAMPLE_DIVIDER;
  uint32_t compare_ticks = (period_ticks * iind_point) / PWM_DUTY_SCALE;

  HRTIM_TimeBaseCfgTypeDef time_base = {0};
  HRTIM_CompareCfgTypeDef compare = {0};

  time_base.Period = master_ticks;
  time_base.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV1;
  time_base.Mode = HRTIM_MODE_CONTINUOUS;
  /* Soft-fails rather than calling Error_Handler(). A current sensor that
     cannot configure itself must not take the board down with it - the
     converter runs perfectly well without this module, and pwm.c owns the
     paths where halting is the right answer. */
  if (HAL_HRTIM_TimeBaseConfig(&hhrtim, HRTIM_TIMERINDEX_MASTER, &time_base) != HAL_OK)
  {
    return false;
  }

  /* Never zero: a compare of 0 is not a legal HRTIM value, and a sample point
     at the very start of the period would sit on the switching edge anyway. */
  compare.CompareValue = (compare_ticks < 3U) ? 3U : compare_ticks;
  if (HAL_HRTIM_WaveformCompareConfig(&hhrtim, HRTIM_TIMERINDEX_MASTER,
                                      HRTIM_COMPAREUNIT_1, &compare) != HAL_OK)
  {
    return false;
  }
  return true;
}

/* Master compare 1 raises HRTIM ADC trigger 1. Both ADCs select that same
   trigger, so one event starts both sequences - channel A converts at the
   trigger instant and B..E follow behind it in ADC3's sequence. Only triggers
   1 and 3 can reach an ADC on this part, so this is not an arbitrary choice. */
static bool configure_adc_trigger(void)
{
  HRTIM_ADCTriggerCfgTypeDef trigger = {0};

  trigger.UpdateSource = HRTIM_ADCTRIGGERUPDATE_MASTER;
  trigger.Trigger = HRTIM_ADCTRIGGEREVENT13_MASTER_CMP1;

  return HAL_HRTIM_ADCTriggerConfig(&hhrtim, HRTIM_ADCTRIGGER_1, &trigger) == HAL_OK;
}

bool iind_init(void)
{
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    iind_zero_counts[i] = 0U;
  }

  /* Offset calibration needs the ADC disabled, so it precedes any start. */
  if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    return false;
  }

  if (!configure_master() || !configure_adc_trigger())
  {
    return false;
  }

  iind_op_state = IIND_STATE_STOPPED;
  return true;
}

bool iind_start(void)
{
  if (iind_op_state == IIND_STATE_UNINITIALIZED)
  {
    return false;
  }

  /* Claim circular DMA here rather than relying on the CubeMX setting.
     HAL_ADC_Start_DMA() writes DMNGT from Init.ConversionDataManagement every
     time it is called, so this is the value that takes effect - the .ioc's
     copy is never read. Setting it in the module that depends on it means the
     mode cannot drift out from under the driver, and it removes a CubeMX
     dropdown from the bring-up instructions that is not always offered. */
  hadc2.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
  hadc3.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;

  /* Both ADCs are armed before the trigger source runs, so the first event
     finds them waiting rather than arriving mid-sequence.

     These return HAL_ERROR if CubeMX has not been told to give ADC2 and ADC3 a
     DMA request and an HRTIM trigger - see .agents/hardware.md. That is the
     expected failure until those changes are made, and it is reported rather
     than ignored so it cannot look like a wiring fault later. */
  if (HAL_ADC_Start_DMA(&hadc2, (uint32_t *)IIND_ADC2_BUFFER, 1U) != HAL_OK)
  {
    return false;
  }
  if (HAL_ADC_Start_DMA(&hadc3, (uint32_t *)IIND_ADC3_BUFFER, IIND_ADC3_CHANNELS) != HAL_OK)
  {
    (void)HAL_ADC_Stop_DMA(&hadc2);
    return false;
  }

  if (HAL_HRTIM_WaveformCountStart(&hhrtim, HRTIM_TIMERID_MASTER) != HAL_OK)
  {
    (void)HAL_ADC_Stop_DMA(&hadc2);
    (void)HAL_ADC_Stop_DMA(&hadc3);
    return false;
  }

  iind_samples = 0U;
  iind_last_sample_ms = HAL_GetTick();
  iind_op_state = IIND_STATE_RUNNING;
  return true;
}

void iind_stop(void)
{
  if (iind_op_state == IIND_STATE_UNINITIALIZED)
  {
    return;
  }

  (void)HAL_HRTIM_WaveformCountStop(&hhrtim, HRTIM_TIMERID_MASTER);
  (void)HAL_ADC_Stop_DMA(&hadc2);
  (void)HAL_ADC_Stop_DMA(&hadc3);

  iind_op_state = IIND_STATE_STOPPED;
}

bool iind_calibrate_zero(void)
{
  uint32_t accumulator[PWM_CHANNEL_COUNT] = {0};
  bool was_running = (iind_op_state == IIND_STATE_RUNNING);

  /* INVARIANT: the zero is only the zero if nothing is switching. Calibrating
     against a live converter would fold its operating current into the offset
     and bias every later reading by exactly that amount - a fault that reads
     as a plausible current rather than as an error. */
  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    pwm_state_t state = pwm_get_state(pwm_channel((pwm_channel_id_t)i));

    if ((state != PWM_STATE_STOPPED) && (state != PWM_STATE_UNINITIALIZED))
    {
      return false;
    }
  }

  if (!was_running && !iind_start())
  {
    return false;
  }

  for (uint32_t sample = 0U; sample < IIND_ZERO_SAMPLES; sample++)
  {
    /* One master period at the slowest allowed switching frequency is 240 us;
       a millisecond per sample is generous and bounded either way. */
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < 1U)
    {
      /* wait for the next conversion to land */
    }

    for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
    {
      accumulator[i] += read_raw((pwm_channel_id_t)i);
    }
  }

  for (uint32_t i = 0U; i < (uint32_t)PWM_CHANNEL_COUNT; i++)
  {
    iind_zero_counts[i] = (uint16_t)(accumulator[i] / IIND_ZERO_SAMPLES);
  }

  if (!was_running)
  {
    iind_stop();
  }
  return true;
}

bool iind_set_sample_point(uint16_t tenths)
{
  if ((tenths < IIND_MIN_SAMPLE_POINT) || (tenths > IIND_MAX_SAMPLE_POINT))
  {
    return false;
  }

  iind_point = tenths;
  return configure_master();
}

uint16_t iind_sample_point(void)
{
  return iind_point;
}

int32_t iind_current_ma(pwm_channel_id_t channel)
{
  int32_t delta;

  if ((uint32_t)channel >= (uint32_t)PWM_CHANNEL_COUNT)
  {
    return 0;
  }

  /* Signed on purpose: a synchronous boost can carry inductor current in
     either direction, and clamping it at zero would hide exactly the reverse
     current that trips OCP on a duty step. */
  delta = (int32_t)read_raw(channel) - (int32_t)iind_zero_counts[(uint32_t)channel];

  return (delta * (int32_t)IIND_FULL_SCALE_MA) / (int32_t)IIND_FULL_SCALE;
}

uint16_t iind_raw(pwm_channel_id_t channel)
{
  if ((uint32_t)channel >= (uint32_t)PWM_CHANNEL_COUNT)
  {
    return 0U;
  }
  return read_raw(channel);
}

uint16_t iind_zero(pwm_channel_id_t channel)
{
  if ((uint32_t)channel >= (uint32_t)PWM_CHANNEL_COUNT)
  {
    return 0U;
  }
  return iind_zero_counts[(uint32_t)channel];
}

iind_state_t iind_state(pwm_channel_id_t channel)
{
  if ((uint32_t)channel >= (uint32_t)PWM_CHANNEL_COUNT)
  {
    return IIND_STATE_UNINITIALIZED;
  }
  if (iind_op_state != IIND_STATE_RUNNING)
  {
    return iind_op_state;
  }
  if ((HAL_GetTick() - iind_last_sample_ms) > IIND_STALE_TIMEOUT_MS)
  {
    return IIND_STATE_STALE;
  }
  return IIND_STATE_RUNNING;
}

uint32_t iind_sample_id(void)
{
  return iind_samples;
}

/* Called from the ADC3 conversion-complete callback in interrupts.c - ADC3
   carries four of the five channels, so its sequence completing is what marks
   a full set. Routing only; the counter is what a control loop waits on. */
void iind_conversion_complete(void)
{
  iind_samples++;
  iind_last_sample_ms = HAL_GetTick();
}
