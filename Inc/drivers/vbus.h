/**
  ******************************************************************************
  * @file    vbus.h
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
#ifndef VBUS_H
#define VBUS_H

#include <stdint.h>

#include "config.h"

/* Prepares ADC1 for V_BUS_DIV and starts the cadence. Call once from
 * app_setup(), after MX_ADC1_Init(). */
void vbus_init(void);

/* Non-blocking: takes one conversion per VBUS_PERIOD_MS and returns. A 16-bit
 * conversion at the configured 76 MHz ADC clock is 73 cycles - under a
 * microsecond - so this polls rather than earning an interrupt. */
void vbus_service(void);

/* Latest bus voltage in millivolts. Zero until the first conversion. */
uint32_t vbus_millivolts(void);

#endif /* VBUS_H */
