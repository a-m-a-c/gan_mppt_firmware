#include "channel_telem.h"
#include "i2c.h"

/* Board wiring, common to all 10 sensors. TELEM_MAX_CURRENT_A sits just
 * under the +/-54.6 A that ADCRANGE=0 can represent across 3 mOhm. */
#define TELEM_SHUNT_OHMS    0.003f
#define TELEM_MAX_CURRENT_A 50.0f

/* 7-bit I2C addresses - must match each sensor's A0/A1 strapping. */
#define TELEM_ADDR_A_IN  0x40U
#define TELEM_ADDR_A_OUT 0x41U
#define TELEM_ADDR_B_IN  0x42U
#define TELEM_ADDR_B_OUT 0x43U
#define TELEM_ADDR_C_IN  0x44U
#define TELEM_ADDR_C_OUT 0x45U
#define TELEM_ADDR_D_IN  0x46U
#define TELEM_ADDR_D_OUT 0x47U
#define TELEM_ADDR_E_IN  0x48U
#define TELEM_ADDR_E_OUT 0x49U

telem_channel_t telem_a = {
    .number = PWM_CHANNEL_A,
    .input = {.i2c = &hi2c1, .address = TELEM_ADDR_A_IN,
              .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
    .output = {.i2c = &hi2c1, .address = TELEM_ADDR_A_OUT,
               .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
};

telem_channel_t telem_b = {
    .number = PWM_CHANNEL_B,
    .input = {.i2c = &hi2c1, .address = TELEM_ADDR_B_IN,
              .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
    .output = {.i2c = &hi2c1, .address = TELEM_ADDR_B_OUT,
               .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
};

telem_channel_t telem_c = {
    .number = PWM_CHANNEL_C,
    .input = {.i2c = &hi2c1, .address = TELEM_ADDR_C_IN,
              .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
    .output = {.i2c = &hi2c1, .address = TELEM_ADDR_C_OUT,
               .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
};

telem_channel_t telem_d = {
    .number = PWM_CHANNEL_D,
    .input = {.i2c = &hi2c1, .address = TELEM_ADDR_D_IN,
              .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
    .output = {.i2c = &hi2c1, .address = TELEM_ADDR_D_OUT,
               .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
};

telem_channel_t telem_e = {
    .number = PWM_CHANNEL_E,
    .input = {.i2c = &hi2c1, .address = TELEM_ADDR_E_IN,
              .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
    .output = {.i2c = &hi2c1, .address = TELEM_ADDR_E_OUT,
               .shunt_ohms = TELEM_SHUNT_OHMS, .max_current_a = TELEM_MAX_CURRENT_A},
};

static bool channel_is_valid(const telem_channel_t *t)
{
  return (t != NULL) && (t->number < PWM_CHANNEL_COUNT);
}

bool telem_init(telem_channel_t *t)
{
  if (!channel_is_valid(t))
  {
    return false;
  }

  bool ok = ina228_init(&t->input);
  ok = ina228_init(&t->output) && ok;

  t->history_head = 0U;
  t->history_count = 0U;
  t->valid = ok;
  if (!ok)
  {
    t->error_count++;
  }
  return ok;
}

bool telem_update(telem_channel_t *t)
{
  if (!channel_is_valid(t))
  {
    return false;
  }

  telem_sample_t sample;
  bool ok = ina228_read_bus_voltage(&t->input, &sample.vin_v);
  ok = ina228_read_current(&t->input, &sample.iin_a) && ok;
  ok = ina228_read_bus_voltage(&t->output, &sample.vout_v) && ok;
  ok = ina228_read_current(&t->output, &sample.iout_a) && ok;
  ok = ina228_read_die_temp(&t->input, &t->temp_in_c) && ok;
  ok = ina228_read_die_temp(&t->output, &t->temp_out_c) && ok;

  if (!ok)
  {
    /* Discard the partial sample; `latest`/`history` keep the last good one. */
    t->valid = false;
    t->error_count++;
    return false;
  }

  sample.tick_ms = HAL_GetTick();
  t->latest = sample;

  t->history_head = (t->history_head + 1U) % TELEM_HISTORY_DEPTH;
  t->history[t->history_head] = sample;
  if (t->history_count < TELEM_HISTORY_DEPTH)
  {
    t->history_count++;
  }

  t->valid = true;
  return true;
}

bool telem_history_get(const telem_channel_t *t, uint32_t age, telem_sample_t *out)
{
  if (!channel_is_valid(t) || (out == NULL) || (age >= t->history_count))
  {
    return false;
  }

  uint32_t index = (t->history_head + TELEM_HISTORY_DEPTH - age) % TELEM_HISTORY_DEPTH;
  *out = t->history[index];
  return true;
}
