/**
  ******************************************************************************
  * @file    serial.c
  * @author  Angus Macdonald
  * @brief   Telemetry streaming over UART5 (onboard UART-to-USB).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include "serial.h"

#include <stdio.h>

#include "adc.h"
#include "main.h"
#include "usart.h"

/* ADC1 runs 16-bit single-ended off a 3V3 reference. */
#define SERIAL_ADC_FULL_SCALE 65535U
#define SERIAL_ADC_VREF_MV    3300U

/* Bounded so a stalled peripheral costs one late line, never a hung loop. */
#define SERIAL_ADC_TIMEOUT_MS 2U
#define SERIAL_TX_TIMEOUT_MS  10U

static uint32_t serial_last_tx_ms;

/* MX_ADC1_Init() leaves rank 1 on ADC_CHANNEL_6 (NTC_CH1). Nothing else drives
   ADC1 yet, so claim rank 1 for V_BUS_DIV rather than touching generated code.
   Revisit when the NTC channels need sampling too. */
static void serial_adc_select_vbus(void)
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

static uint16_t serial_read_vbus_raw(void)
{
  uint16_t raw = 0U;

  if (HAL_ADC_Start(&hadc1) == HAL_OK)
  {
    if (HAL_ADC_PollForConversion(&hadc1, SERIAL_ADC_TIMEOUT_MS) == HAL_OK)
    {
      raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
  }
  (void)HAL_ADC_Stop(&hadc1);

  return raw;
}

/* Bus millivolts, undoing the divider. Done in one 64-bit expression so the
   intermediate pin voltage is not truncated to whole millivolts first. */
static uint32_t serial_vbus_mv(uint16_t raw)
{
  uint64_t numerator = (uint64_t)raw * SERIAL_ADC_VREF_MV *
                       (SERIAL_VBUS_DIV_TOP_OHMS + SERIAL_VBUS_DIV_BOTTOM_OHMS);
  uint64_t denominator = (uint64_t)SERIAL_ADC_FULL_SCALE * SERIAL_VBUS_DIV_BOTTOM_OHMS;

  return (uint32_t)(numerator / denominator);
}

void serial_init(void)
{
  /* Offset calibration needs the ADC disabled, so it must precede any start. */
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }

  serial_adc_select_vbus();

  serial_last_tx_ms = HAL_GetTick();
}

void serial_service(void)
{
  uint32_t now = HAL_GetTick();
  char line[32];
  int len;

  /* Unsigned subtraction, so this stays correct across the 32-bit tick
     wrap at ~49.7 days. */
  if ((now - serial_last_tx_ms) < SERIAL_TELEM_PERIOD_MS)
  {
    return;
  }
  serial_last_tx_ms = now;

  uint16_t raw = serial_read_vbus_raw();

  /* "<tick_ms>,<raw>,<vbus_mv>" - integer only, so no float formatting is
     linked in. The Python side scales to volts. */
  len = snprintf(line, sizeof(line), "%lu,%u,%lu\r\n",
                 (unsigned long)now,
                 (unsigned)raw,
                 (unsigned long)serial_vbus_mv(raw));

  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart5, (const uint8_t *)line, (uint16_t)len,
                            SERIAL_TX_TIMEOUT_MS);
  }
}
