/**
  ******************************************************************************
  * @file    ina228.h
  * @author  Angus Macdonald
  * @brief   TI INA228 I2C power/current monitor driver.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef INA228_H
#define INA228_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx_hal.h"

/* One physical INA228. Set i2c/address/shunt_ohms/max_current_a before
 * calling ina228_init(); current_lsb is derived there and used by the
 * current/power reads. */
typedef struct
{
  I2C_HandleTypeDef *i2c;
  uint8_t address;     /* 7-bit address, 0x40-0x4F via the A0/A1 straps */
  float shunt_ohms;
  float max_current_a; /* full-scale current mapped onto the 20-bit CURRENT register */
  float current_lsb;   /* amps per CURRENT bit - derived by ina228_init() */
} ina228_t;

/* Every function returns false on I2C failure (init also on bad device ID) -
 * a missing or failed sensor must never halt the converter. */
bool ina228_init(ina228_t *dev);
bool ina228_read_bus_voltage(ina228_t *dev, float *volts);
bool ina228_read_shunt_voltage(ina228_t *dev, float *volts);
bool ina228_read_current(ina228_t *dev, float *amps);
bool ina228_read_power(ina228_t *dev, float *watts);
bool ina228_read_die_temp(ina228_t *dev, float *celsius);

#endif /* INA228_H */
