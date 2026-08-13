/**
  ******************************************************************************
  * @file    serial.h
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
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* Line cadence. */
#define SERIAL_TELEM_PERIOD_MS 10U

/* V_BUS_DIV resistor divider (PA6 / ADC1_INP3): 100k top, 5.23k bottom. */
#define SERIAL_VBUS_DIV_TOP_OHMS    100000U
#define SERIAL_VBUS_DIV_BOTTOM_OHMS 5230U

/* Prepares ADC1 for V_BUS_DIV and starts the cadence. Call once from main(),
 * after MX_ADC1_Init() and MX_UART5_Init(). */
void serial_init(void);

/* Non-blocking: samples V_BUS_DIV and emits one line when the period has
 * elapsed, otherwise returns immediately. Must be called often enough that
 * the caller does not starve it - see the delay helper in main.c. */
void serial_service(void);

#endif /* SERIAL_H */
