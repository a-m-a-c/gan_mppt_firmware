#ifndef ANALOG_H
#define ANALOG_H

#include <stdint.h>

/* Battery bus voltage, polled off ADC1. Sole owner of ADC1.
   The reading is published into sys.vbus_mv; there is no getter here. */

/* Calibrates ADC1. Call once from app_setup(), after MX_ADC1_Init(). */
void analog_init(void);

/* Converts the bus input every ANALOG_PERIOD_MS and stores the result, divider
   undone, in sys.vbus_mv. Between cadences it returns immediately. Zero on any
   ADC failure - out of range for every consumer, so a dead ADC degrades to
   "no reading" rather than to a plausible wrong one. Non-blocking. */
void analog_service(void);

#endif /* ANALOG_H */
