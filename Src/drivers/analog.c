/**
  ******************************************************************************
  * @file    analog.c
  * @author  Angus Macdonald
  * @brief   Slow analog inputs on ADC1: bus voltage and the five NTCs.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "analog.h"

#include "adc.h"
#include "main.h"
#include "ntc_table.h"

/* ADC1 runs 16-bit single-ended off a 3V3 reference. */
#define ANALOG_FULL_SCALE 65535U
#define ANALOG_VREF_MV    3300U

/* The NTC table is in tenths of a millivolt (33000 = 3.3 V), so raw counts are
   converted to the same units rather than to whole millivolts - at the hot end
   the curve flattens to ~2 mV per degC, where whole millivolts would quantise
   the result to about half a degree. */
#define ANALOG_VREF_DMV 33000U

/* Bounded so a stalled peripheral costs one missed sample, never a hung loop.
   The conversion itself is microseconds; this is pure insurance. */
#define ANALOG_TIMEOUT_MS 2U

/* Sampling time has to cover the source impedance charging the ADC's sample
   capacitor to 16-bit accuracy - roughly twelve RC time constants.

   V_BUS_DIV is 100k || 5.23k = 4.97 kOhm, and 64.5 cycles at the 76 MHz ADC
   clock is 850 ns against the ~650 ns that needs.

   An NTC divider is 10k || R_ntc, which is 5 kOhm at 25 degC but rises toward
   9.5 kOhm at -40 degC as the thermistor goes high - the worst case, and the
   one worth sizing for. 387.5 cycles is 5.1 us, far more than the ~800 ns it
   needs, and at this cadence the margin is free. */
#define ANALOG_SAMPLE_VBUS ADC_SAMPLETIME_64CYCLES_5
#define ANALOG_SAMPLE_NTC  ADC_SAMPLETIME_387CYCLES_5

/* Sweep order: bus voltage first, then NTC 1..5. Fixed wiring, so the ADC
   channel for each input is a constant - see .agents/hardware.md for the pin
   map and why these five NTCs have to live on ADC1. */
typedef struct
{
  uint32_t channel;
  uint32_t sampling_time;
} analog_input_t;

#define ANALOG_INPUT_VBUS 0U
#define ANALOG_INPUT_NTC1 1U
#define ANALOG_INPUT_COUNT (ANALOG_INPUT_NTC1 + CHANNEL_COUNT)

static const analog_input_t analog_inputs[ANALOG_INPUT_COUNT] = {
    [ANALOG_INPUT_VBUS]      = {ADC_CHANNEL_3, ANALOG_SAMPLE_VBUS}, /* PA6  V_BUS_DIV */
    [ANALOG_INPUT_NTC1 + 0U] = {ADC_CHANNEL_6, ANALOG_SAMPLE_NTC},  /* PF12 NTC_CH1   */
    [ANALOG_INPUT_NTC1 + 1U] = {ADC_CHANNEL_2, ANALOG_SAMPLE_NTC},  /* PF11 NTC_CH2   */
    [ANALOG_INPUT_NTC1 + 2U] = {ADC_CHANNEL_5, ANALOG_SAMPLE_NTC},  /* PB1  NTC_CH3   */
    [ANALOG_INPUT_NTC1 + 3U] = {ADC_CHANNEL_4, ANALOG_SAMPLE_NTC},  /* PC4  NTC_CH4   */
    [ANALOG_INPUT_NTC1 + 4U] = {ADC_CHANNEL_7, ANALOG_SAMPLE_NTC},  /* PA7  NTC_CH5   */
};

static uint32_t analog_vbus_latest_mv;
static int16_t analog_ntc_latest[CHANNEL_COUNT];
/* Pin voltages kept alongside the converted values purely for diagnosis - see
   analog_ntc_pin_mv(). Cheap: six halfwords, no extra conversions. */
static uint16_t analog_vbus_pin_dmv;
static uint16_t analog_ntc_pin_dmv[CHANNEL_COUNT];
static uint32_t analog_last_sweep_ms;
static uint16_t analog_vrefint_raw;
static uint16_t analog_ntc5_adc2_dmv;

/* Points rank 1 at one input. Cheap - it writes the sequence, sampling time
   and channel-preselect registers - so doing it per conversion costs less than
   a scan sequence would cost in generated-code changes. */
static void analog_select(const analog_input_t *input)
{
  ADC_ChannelConfTypeDef config = {0};

  config.Channel = input->channel;
  config.Rank = ADC_REGULAR_RANK_1;
  config.SamplingTime = input->sampling_time;
  config.SingleDiff = ADC_SINGLE_ENDED;
  config.OffsetNumber = ADC_OFFSET_NONE;
  config.Offset = 0;
  config.OffsetSignedSaturation = DISABLE;

  if (HAL_ADC_ConfigChannel(&hadc1, &config) != HAL_OK)
  {
    Error_Handler();
  }
}

/* Zero on any failure, which reads as 0 V. That is out of range for every
   consumer here - the bus reports 0 mV and an NTC falls off the bottom of the
   table into ANALOG_TEMP_INVALID - so a dead ADC degrades to "no reading"
   rather than to a plausible wrong one. */
static uint16_t analog_read_raw(const analog_input_t *input)
{
  uint16_t raw = 0U;

  analog_select(input);

  if (HAL_ADC_Start(&hadc1) == HAL_OK)
  {
    if (HAL_ADC_PollForConversion(&hadc1, ANALOG_TIMEOUT_MS) == HAL_OK)
    {
      raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
  }
  (void)HAL_ADC_Stop(&hadc1);

  return raw;
}

/* Bus millivolts, undoing the divider. Done in one 64-bit expression so the
   intermediate pin voltage is not truncated to whole millivolts first. */
static uint32_t raw_to_vbus_mv(uint16_t raw)
{
  uint64_t numerator = (uint64_t)raw * ANALOG_VREF_MV *
                       (VBUS_DIV_TOP_OHMS + VBUS_DIV_BOTTOM_OHMS);
  uint64_t denominator = (uint64_t)ANALOG_FULL_SCALE * VBUS_DIV_BOTTOM_OHMS;

  return (uint32_t)(numerator / denominator);
}

/* Rounds rather than truncates. Truncation is a systematic half-count bias
   downward, and at the hot end of the table that is enough to push a reading
   that lands exactly on the last entry just below it - off the end of the
   table and into ANALOG_TEMP_INVALID, reporting a fault for a working sensor
   at 150 degC. */
static uint16_t raw_to_dmv(uint16_t raw)
{
  return (uint16_t)((((uint32_t)raw * ANALOG_VREF_DMV) + (ANALOG_FULL_SCALE / 2U)) /
                    ANALOG_FULL_SCALE);
}

/* How far outside the table a reading may sit and still be treated as sitting
   on the endpoint - one tenth of a millivolt, the table's own storage step. */
#define NTC_ENDPOINT_SLACK_DMV 1U

/* Table lookup with linear interpolation between the 1 degC entries.
   ntc_table_dmv is strictly decreasing - the generator asserts it - so the
   search is for the last entry still at or above the measured voltage, and
   the answer lies between that entry and the next. */
static int16_t dmv_to_decicelsius(uint16_t dmv)
{
  uint32_t low = 0U;
  uint32_t high = NTC_TABLE_LEN - 1U;
  uint32_t span;
  uint32_t into;
  int32_t deci;

  /* Outside the table is not a temperature: above the coldest entry means an
     open sensor pulled up toward 3V3, below the hottest means a short to
     ground. (It also covers a real sensor beyond -40..150 degC, which is far
     outside anything this converter can survive.)

     The one count of slack matters. Table entries and converted readings are
     the same curve rounded independently to tenths of a millivolt, so at the
     extremes they can land a count apart - the 150 degC entry stores 988 while
     the ADC path reconstructs 987. Without the slack a sensor sitting exactly
     at the top of its range reports as a short. */
  if ((dmv > (ntc_table_dmv[0] + NTC_ENDPOINT_SLACK_DMV)) ||
      ((dmv + NTC_ENDPOINT_SLACK_DMV) < ntc_table_dmv[NTC_TABLE_LEN - 1U]))
  {
    return ANALOG_TEMP_INVALID;
  }

  /* Within the slack band, pin to the endpoint so the search has a bracket. */
  if (dmv > ntc_table_dmv[0])
  {
    dmv = ntc_table_dmv[0];
  }
  if (dmv < ntc_table_dmv[NTC_TABLE_LEN - 1U])
  {
    dmv = ntc_table_dmv[NTC_TABLE_LEN - 1U];
  }

  while ((high - low) > 1U)
  {
    uint32_t mid = (low + high) / 2U;

    if (ntc_table_dmv[mid] >= dmv)
    {
      low = mid;
    }
    else
    {
      high = mid;
    }
  }

  /* ntc_table_dmv[low] >= dmv > ntc_table_dmv[high], and high is low + 1. */
  span = (uint32_t)ntc_table_dmv[low] - (uint32_t)ntc_table_dmv[high];
  into = (uint32_t)ntc_table_dmv[low] - (uint32_t)dmv;

  deci = ((int32_t)NTC_TABLE_MIN_C * 10) +
         ((int32_t)low * (int32_t)NTC_TABLE_STEP_C * 10);
  deci += (int32_t)((into * (uint32_t)NTC_TABLE_STEP_C * 10U) / span);

  return (int16_t)deci;
}

/* One conversion of the internal reference on ADC3, at startup. See
   analog_vrefint_measured() for why this exists: it is the only reading on the
   board with no external circuit behind it, so it says whether the ADC itself
   can be believed before any argument about dividers begins.

   Soft-fails to zero rather than calling Error_Handler() - a diagnostic must
   never be the thing that stops the firmware booting. */
static void analog_measure_vrefint(void)
{
  ADC_ChannelConfTypeDef config = {0};

  if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    return;
  }

  /* VREFINT is a high-impedance internal source and needs a long sample; the
     NTC time is already generous and serves here too. */
  config.Channel = ADC_CHANNEL_VREFINT;
  config.Rank = ADC_REGULAR_RANK_1;
  config.SamplingTime = ANALOG_SAMPLE_NTC;
  config.SingleDiff = ADC_SINGLE_ENDED;
  config.OffsetNumber = ADC_OFFSET_NONE;
  config.Offset = 0;
  config.OffsetSignedSaturation = DISABLE;

  if (HAL_ADC_ConfigChannel(&hadc3, &config) != HAL_OK)
  {
    return;
  }

  if (HAL_ADC_Start(&hadc3) == HAL_OK)
  {
    if (HAL_ADC_PollForConversion(&hadc3, ANALOG_TIMEOUT_MS) == HAL_OK)
    {
      analog_vrefint_raw = (uint16_t)HAL_ADC_GetValue(&hadc3);
    }
  }
  (void)HAL_ADC_Stop(&hadc3);
}

/* One conversion of NTC_CH5 (PA7) through ADC2, for comparison against ADC1's
   reading of the same pad. See analog_ntc5_via_adc2_mv(). Soft-fails to zero -
   a cross-check must not be able to stop a sweep.

   Taken ONCE, at init, and never again: iind.c owns ADC2 at runtime and keeps
   it in circular DMA off an HRTIM trigger. A polled conversion here would be
   fighting that for the same converter, which is the two-owners collision this
   project avoids everywhere else. The pin voltage is static anyway, so one
   reading at startup carries the same information the sweep did. */
static uint16_t analog_read_ntc5_via_adc2(void)
{
  ADC_ChannelConfTypeDef config = {0};
  uint16_t raw = 0U;

  config.Channel = ADC_CHANNEL_7; /* PA7, reachable from ADC1 and ADC2 alike */
  config.Rank = ADC_REGULAR_RANK_1;
  config.SamplingTime = ANALOG_SAMPLE_NTC;
  config.SingleDiff = ADC_SINGLE_ENDED;
  config.OffsetNumber = ADC_OFFSET_NONE;
  config.Offset = 0;
  config.OffsetSignedSaturation = DISABLE;

  if (HAL_ADC_ConfigChannel(&hadc2, &config) != HAL_OK)
  {
    return 0U;
  }

  if (HAL_ADC_Start(&hadc2) == HAL_OK)
  {
    if (HAL_ADC_PollForConversion(&hadc2, ANALOG_TIMEOUT_MS) == HAL_OK)
    {
      raw = (uint16_t)HAL_ADC_GetValue(&hadc2);
    }
  }
  (void)HAL_ADC_Stop(&hadc2);

  return raw_to_dmv(raw);
}

void analog_init(void)
{
  /* Offset calibration needs the ADC disabled, so it must precede any start. */
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  /* ADC2 is only used for the cross-check, but it needs calibrating too or the
     comparison measures the calibration difference rather than the pad. */
  if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }

  analog_measure_vrefint();
  analog_ntc5_adc2_dmv = analog_read_ntc5_via_adc2();

  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++)
  {
    analog_ntc_latest[i] = ANALOG_TEMP_INVALID;
  }

  analog_last_sweep_ms = HAL_GetTick();
}

void analog_service(void)
{
  uint32_t now = HAL_GetTick();

  /* Unsigned subtraction, so this stays correct across the 32-bit tick
     wrap at ~49.7 days. */
  if ((now - analog_last_sweep_ms) < ANALOG_PERIOD_MS)
  {
    return;
  }
  analog_last_sweep_ms = now;

  uint16_t raw = analog_read_raw(&analog_inputs[ANALOG_INPUT_VBUS]);

  analog_vbus_pin_dmv = raw_to_dmv(raw);
  analog_vbus_latest_mv = raw_to_vbus_mv(raw);

  for (uint32_t i = 0U; i < CHANNEL_COUNT; i++)
  {
    uint16_t dmv = raw_to_dmv(analog_read_raw(&analog_inputs[ANALOG_INPUT_NTC1 + i]));

    analog_ntc_pin_dmv[i] = dmv;
    analog_ntc_latest[i] = dmv_to_decicelsius(dmv);
  }
}

uint32_t analog_vbus_mv(void)
{
  return analog_vbus_latest_mv;
}

int16_t analog_ntc_decicelsius(uint32_t channel)
{
  if ((uint32_t)channel >= CHANNEL_COUNT)
  {
    return ANALOG_TEMP_INVALID;
  }
  return analog_ntc_latest[(uint32_t)channel];
}

/* Tenths of a millivolt are the working unit; callers want millivolts, so
   round on the way out rather than truncating a diagnostic. */
static uint32_t dmv_to_mv(uint16_t dmv)
{
  return ((uint32_t)dmv + 5U) / 10U;
}

uint32_t analog_ntc_pin_mv(uint32_t channel)
{
  if ((uint32_t)channel >= CHANNEL_COUNT)
  {
    return 0U;
  }
  return dmv_to_mv(analog_ntc_pin_dmv[(uint32_t)channel]);
}

uint32_t analog_vbus_pin_mv(void)
{
  return dmv_to_mv(analog_vbus_pin_dmv);
}

uint16_t analog_vrefint_measured(void)
{
  return analog_vrefint_raw;
}

uint16_t analog_vrefint_factory(void)
{
  return *VREFINT_CAL_ADDR;
}

uint32_t analog_ntc5_via_adc2_mv(void)
{
  return dmv_to_mv(analog_ntc5_adc2_dmv);
}
