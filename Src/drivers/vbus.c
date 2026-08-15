/**
  ******************************************************************************
  * @file    vbus.c
  * @author  Angus Macdonald
  * @brief   Bus voltage sensing (V_BUS_DIV on ADC1).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "vbus.h"

#include "adc.h"
#include "main.h"

/* ADC1 runs 16-bit single-ended off a 3V3 reference. */
#define VBUS_ADC_FULL_SCALE 65535U
#define VBUS_ADC_VREF_MV    3300U

/* Bounded so a stalled peripheral costs one missed sample, never a hung loop.
   The conversion itself takes under a microsecond; this is pure insurance. */
#define VBUS_ADC_TIMEOUT_MS 2U

static uint32_t vbus_latest_mv;
static uint32_t vbus_last_sample_ms;

/* MX_ADC1_Init() leaves rank 1 on ADC_CHANNEL_6 (NTC_CH1). Nothing else drives
   ADC1 yet, so claim rank 1 for V_BUS_DIV rather than touching generated code.
   Revisit when the NTC channels need sampling too. */
static void vbus_adc_select(void)
{
  ADC_ChannelConfTypeDef config = {0};

  config.Channel = ADC_CHANNEL_3;
  config.Rank = ADC_REGULAR_RANK_1;
  config.SamplingTime = ADC_SAMPLETIME_64CYCLES_5;
  config.SingleDiff = ADC_SINGLE_ENDED;
  config.OffsetNumber = ADC_OFFSET_NONE;
  config.Offset = 0;
  config.OffsetSignedSaturation = DISABLE;

  if (HAL_ADC_ConfigChannel(&hadc1, &config) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint16_t vbus_read_raw(void)
{
  uint16_t raw = 0U;

  if (HAL_ADC_Start(&hadc1) == HAL_OK)
  {
    if (HAL_ADC_PollForConversion(&hadc1, VBUS_ADC_TIMEOUT_MS) == HAL_OK)
    {
      raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
  }
  (void)HAL_ADC_Stop(&hadc1);

  return raw;
}

/* Bus millivolts, undoing the divider. Done in one 64-bit expression so the
   intermediate pin voltage is not truncated to whole millivolts first. */
static uint32_t vbus_to_mv(uint16_t raw)
{
  uint64_t numerator = (uint64_t)raw * VBUS_ADC_VREF_MV *
                       (VBUS_DIV_TOP_OHMS + VBUS_DIV_BOTTOM_OHMS);
  uint64_t denominator = (uint64_t)VBUS_ADC_FULL_SCALE * VBUS_DIV_BOTTOM_OHMS;

  return (uint32_t)(numerator / denominator);
}

void vbus_init(void)
{
  /* Offset calibration needs the ADC disabled, so it must precede any start. */
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }

  vbus_adc_select();

  vbus_last_sample_ms = HAL_GetTick();
}

void vbus_service(void)
{
  uint32_t now = HAL_GetTick();

  /* Unsigned subtraction, so this stays correct across the 32-bit tick
     wrap at ~49.7 days. */
  if ((now - vbus_last_sample_ms) < VBUS_PERIOD_MS)
  {
    return;
  }
  vbus_last_sample_ms = now;

  vbus_latest_mv = vbus_to_mv(vbus_read_raw());
}

uint32_t vbus_millivolts(void)
{
  return vbus_latest_mv;
}
