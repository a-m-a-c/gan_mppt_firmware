#ifndef INA228_H
#define INA228_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx_hal.h"

// An ina228 device.
typedef struct
{
  I2C_HandleTypeDef *i2c;
  uint8_t address;     /* 7-bit address, 0x40-0x4F via the A0/A1 straps */
  float shunt_ohms;
  float max_current_a; /* full-scale current mapped onto the 20-bit CURRENT register */
  float current_lsb;   /* amps per CURRENT bit - derived by ina228_init() */
} ina228_t;


typedef enum
{
  INA228_QTY_BUS_VOLTAGE = 0,
  INA228_QTY_CURRENT
} ina228_quantity_t;

uint8_t ina228_register(ina228_quantity_t quantity);
uint16_t ina228_register_size(ina228_quantity_t quantity);
float ina228_decode(const ina228_t *dev, ina228_quantity_t quantity, const uint8_t *raw);

// Blocking
bool ina228_init(ina228_t *dev);
bool ina228_read_bus_voltage(ina228_t *dev, float *volts);
bool ina228_read_shunt_voltage(ina228_t *dev, float *volts);
bool ina228_read_current(ina228_t *dev, float *amps);
bool ina228_read_power(ina228_t *dev, float *watts);
#endif /* INA228_H */
