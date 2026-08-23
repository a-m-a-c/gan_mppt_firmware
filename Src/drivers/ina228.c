#include "ina228.h"

/* Register map (datasheet section 7.6). */
#define INA228_REG_CONFIG     0x00U
#define INA228_REG_ADC_CONFIG 0x01U
#define INA228_REG_SHUNT_CAL  0x02U
#define INA228_REG_VBUS       0x05U
#define INA228_REG_CURRENT    0x07U
#define INA228_REG_MFG_ID     0x3EU
#define INA228_REG_DEVICE_ID  0x3FU

#define INA228_MFG_ID    0x5449U /* "TI" */
#define INA228_DEVICE_ID 0x228U  /* DEVICE_ID bits [15:4]; [3:0] is the die revision */

#define INA228_CONFIG_RST 0x8000U

/* MODE=0xB (continuous shunt+bus, temperature disabled), 1052 us conversions,
 * 16-sample averaging -> a full averaged set every ~34 ms. Dropping the
 * temperature conversion shortens the cycle from ~50 ms, and is why the
 * DIETEMP register is not read anywhere. */
#define INA228_ADC_CONFIG_VALUE 0xBB6AU

#define INA228_I2C_TIMEOUT_MS 10U

/* Scale factor (datasheet section 7.5): the fixed LSB weight of VBUS.
 * CURRENT scales with current_lsb instead, derived per device. */
#define INA228_VBUS_LSB_V    195.3125e-6f

/* CURRENT_LSB = Imax / 2^19 and SHUNT_CAL = 13107.2e6 * CURRENT_LSB * Rshunt
   (datasheet section 8.1.2, ADCRANGE = 0). Named so ina228_init() reads like
   the datasheet rather than like two unexplained floats. */
#define INA228_CURRENT_LSB_FULL_SCALE 524288.0f /* 2^19 */
#define INA228_SHUNT_CAL_CONSTANT     13107.2e6f

/* A 20-bit result register is left-justified in 24 bits, so the payload is the
   top 20 bits and the low nibble is padding. */
#define INA228_REG20_SHIFT    4U
#define INA228_REG20_SIGN_BIT 0x00080000U
#define INA228_REG20_SIGN_EXT 0xFFF00000U

/* --------------------------------------------------------------------------
   Transfers and register decoding. Everything below the public surface.
   -------------------------------------------------------------------------- */

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
  uint32_t raw = (((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2]);

  raw >>= INA228_REG20_SHIFT;

  if ((raw & INA228_REG20_SIGN_BIT) != 0U)
  {
    raw |= INA228_REG20_SIGN_EXT; /* sign-extend 20 -> 32 bits */
  }

  return (int32_t)raw;
}

/* --------------------------------------------------------------------------
   Public surface.
   -------------------------------------------------------------------------- */

uint8_t ina228_register(ina228_quantity_t quantity)
{
  if (quantity == INA228_QTY_CURRENT)
  {
    return INA228_REG_CURRENT;
  }

  return INA228_REG_VBUS;
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

  if (quantity == INA228_QTY_CURRENT)
  {
    return (float)value * dev->current_lsb;
  }

  return (float)value * INA228_VBUS_LSB_V;
}

bool ina228_init(ina228_t *dev)
{
  uint16_t mfg_id;
  uint16_t device_id;
  uint16_t shunt_cal;

  if ((dev == NULL) || (dev->i2c == NULL) || (dev->shunt_ohms <= 0.0f) ||
      (dev->max_current_a <= 0.0f))
  {
    return false;
  }

  /* Prove the right chip answers at this address before configuring it. */
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

  dev->current_lsb = dev->max_current_a / INA228_CURRENT_LSB_FULL_SCALE;
  shunt_cal = (uint16_t)(INA228_SHUNT_CAL_CONSTANT * dev->current_lsb * dev->shunt_ohms);

  if (!write_reg16(dev, INA228_REG_SHUNT_CAL, shunt_cal))
  {
    return false;
  }

  return write_reg16(dev, INA228_REG_ADC_CONFIG, INA228_ADC_CONFIG_VALUE);
}

