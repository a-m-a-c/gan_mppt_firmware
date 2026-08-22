#ifndef ANALOG_H
#define ANALOG_H

#include <stdint.h>

/* Battery bus voltage, polled off ADC1. Sole owner of ADC1. */

/* Calibrates ADC1. Call once from app_setup(), after MX_ADC1_Init(). */
void analog_init(void);

/* Converts the bus input every ANALOG_PERIOD_MS and returns; between cadences
   it returns immediately. Non-blocking. */
void analog_service(void);

/* Latest bus voltage in millivolts, divider undone. Zero until the first
   conversion, and zero if the ADC fails. */
uint32_t analog_vbus_mv(void);

#endif /* ANALOG_H */
