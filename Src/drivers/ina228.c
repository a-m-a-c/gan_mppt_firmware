#include "ina228.h"

/* Register map (datasheet section 7.6). */
#define INA228_REG_CONFIG     0x00U
#define INA228_REG_ADC_CONFIG 0x01U
#define INA228_REG_SHUNT_CAL  0x02U
#define INA228_REG_VSHUNT     0x04U
#define INA228_REG_VBUS       0x05U
#define INA228_REG_DIETEMP    0x06U
#define INA228_REG_CURRENT    0x07U
#define INA228_REG_POWER      0x08U
#define INA228_REG_MFG_ID     0x3EU
#define INA228_REG_DEVICE_ID  0x3FU

#define INA228_MFG_ID    0x5449U /* "TI" */
#define INA228_DEVICE_ID 0x228U  /* DEVICE_ID bits [15:4]; [3:0] is the die revision */

#define INA228_CONFIG_RST 0x8000U

/* MODE=0xB (continuous shunt+bus, temperature disabled), 1052 us conversions,
 * 16-sample averaging -> a full averaged set every ~34 ms. Dropping the
 * temperature conversion shortens the cycle from ~50 ms and is why
 * ina228_read_die_temp() is not usable with this configuration. */
#define INA228_ADC_CONFIG_VALUE 0xBB6AU

#define INA228_I2C_TIMEOUT_MS 10U

/* Scale factors (datasheet section 7.5): fixed LSB weights of the result
 * registers. Current/power scale with current_lsb instead. */
#define INA228_VBUS_LSB_V    195.3125e-6f
#define INA228_VSHUNT_LSB_V  312.5e-9f /* ADCRANGE = 0 (+/-163.84 mV) */
#define INA228_DIETEMP_LSB_C 7.8125e-3f
#define INA228_POWER_SCALE   3.2f

static bool read_bytes(ina228_t *dev, uint8_t reg, uint8_t *data, uint16_t len)
{
  return HAL_I2C_Mem_Read(dev->i2c, (uint16_t)(dev->address << 1), reg,
                          I2C_MEMADD_SIZE_8BIT, data, len, INA228_I2C_TIMEOUT_MS) == HAL_OK;
}

static bool write_reg16(ina228_t *dev, uint8_t reg, uint16_t value)
{
  uint8_t data[2] = {(uint8_t)(value >> 8), (uint8_t)value};
  return HAL_I2C_Mem_Write(dev->i2c, (uint16_t)(dev->address << 1), reg,
                           I2C_MEMADD_SIZE_8BIT, data, 2U, INA228_I2C_TIMEOUT_MS) == HAL_OK;
}

static bool read_reg16(ina228_t *dev, uint8_t reg, uint16_t *value)
{
  uint8_t data[2];
  if (!read_bytes(dev, reg, data, 2U))
  {
    return false;
  }
  *value = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
  return true;
}

/* VSHUNT/VBUS/CURRENT: a signed 20-bit value left-justified in 24 bits. */
static int32_t decode_reg20(const uint8_t *data)
{
  uint32_t raw = (((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2]) >> 4;

  if ((raw & 0x80000U) != 0U)
  {
    raw |= 0xFFF00000U; /* sign-extend 20 -> 32 bits */
  }
  return (int32_t)raw;
}

static bool read_reg20(ina228_t *dev, uint8_t reg, int32_t *value)
{
  uint8_t data[3];
  if (!read_bytes(dev, reg, data, 3U))
  {
    return false;
  }
  *value = decode_reg20(data);
  return true;
}

uint8_t ina228_register(ina228_quantity_t quantity)
{
  return (quantity == INA228_QTY_CURRENT) ? INA228_REG_CURRENT : INA228_REG_VBUS;
}

uint16_t ina228_register_size(ina228_quantity_t quantity)
{
  (void)quantity; /* both are 20-bit values in 3 bytes */
  return 3U;
}

float ina228_decode(const ina228_t *dev, ina228_quantity_t quantity, const uint8_t *raw)
{
  int32_t value;

  if ((dev == NULL) || (raw == NULL))
  {
    return 0.0f;
  }

  value = decode_reg20(raw);
  return (quantity == INA228_QTY_CURRENT) ? ((float)value * dev->current_lsb)
                                          : ((float)value * INA228_VBUS_LSB_V);
}

/* POWER: a full unsigned 24-bit value (no 4-bit pad). */
static bool read_reg24(ina228_t *dev, uint8_t reg, uint32_t *value)
{
  uint8_t data[3];
  if (!read_bytes(dev, reg, data, 3U))
  {
    return false;
  }
  *value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
  return true;
}

bool ina228_init(ina228_t *dev)
{
  if ((dev == NULL) || (dev->i2c == NULL) || (dev->shunt_ohms <= 0.0f) ||
      (dev->max_current_a <= 0.0f))
  {
    return false;
  }

  /* Prove the right chip answers at this address before configuring it. */
  uint16_t mfg_id;
  uint16_t device_id;
  if (!read_reg16(dev, INA228_REG_MFG_ID, &mfg_id) || (mfg_id != INA228_MFG_ID))
  {
    return false;
  }
  if (!read_reg16(dev, INA228_REG_DEVICE_ID, &device_id) ||
      ((device_id >> 4) != INA228_DEVICE_ID))
  {
    return false;
  }

  if (!write_reg16(dev, INA228_REG_CONFIG, INA228_CONFIG_RST))
  {
    return false;
  }
  HAL_Delay(1);

  /* SHUNT_CAL = 13107.2e6 * CURRENT_LSB * Rshunt, CURRENT_LSB = Imax / 2^19
     (datasheet section 8.1.2, ADCRANGE = 0). */
  dev->current_lsb = dev->max_current_a / 524288.0f;
  uint16_t shunt_cal = (uint16_t)(13107.2e6f * dev->current_lsb * dev->shunt_ohms);
  if (!write_reg16(dev, INA228_REG_SHUNT_CAL, shunt_cal))
  {
    return false;
  }

  return write_reg16(dev, INA228_REG_ADC_CONFIG, INA228_ADC_CONFIG_VALUE);
}

bool ina228_read_bus_voltage(ina228_t *dev, float *volts)
{
  int32_t raw;
  if ((dev == NULL) || (volts == NULL) || !read_reg20(dev, INA228_REG_VBUS, &raw))
  {
    return false;
  }
  *volts = (float)raw * INA228_VBUS_LSB_V;
  return true;
}

bool ina228_read_shunt_voltage(ina228_t *dev, float *volts)
{
  int32_t raw;
  if ((dev == NULL) || (volts == NULL) || !read_reg20(dev, INA228_REG_VSHUNT, &raw))
  {
    return false;
  }
  *volts = (float)raw * INA228_VSHUNT_LSB_V;
  return true;
}

bool ina228_read_current(ina228_t *dev, float *amps)
{
  int32_t raw;
  if ((dev == NULL) || (amps == NULL) || !read_reg20(dev, INA228_REG_CURRENT, &raw))
  {
    return false;
  }
  *amps = (float)raw * dev->current_lsb;
  return true;
}

bool ina228_read_power(ina228_t *dev, float *watts)
{
  uint32_t raw;
  if ((dev == NULL) || (watts == NULL) || !read_reg24(dev, INA228_REG_POWER, &raw))
  {
    return false;
  }
  *watts = (float)raw * INA228_POWER_SCALE * dev->current_lsb;
  return true;
}

/* Only meaningful if ADC_CONFIG selects a MODE that converts temperature.
   INA228_ADC_CONFIG_VALUE deliberately does not, so this returns a stale
   register unless that changes. */
bool ina228_read_die_temp(ina228_t *dev, float *celsius)
{
  uint16_t raw;
  if ((dev == NULL) || (celsius == NULL) || !read_reg16(dev, INA228_REG_DIETEMP, &raw))
  {
    return false;
  }
  *celsius = (float)(int16_t)raw * INA228_DIETEMP_LSB_C;
  return true;
}
