#include "channel_telem.h"

#include "i2c.h"
#include "main.h"

/* Board wiring, common to all 10 sensors. TELEM_MAX_CURRENT_A sits just
 * under the +/-54.6 A that ADCRANGE=0 can represent across 3 mOhm. */
#define TELEM_SHUNT_OHMS    0.003f
#define TELEM_MAX_CURRENT_A 50.0f

/* Alongside UART5, below the priority-0 OVP/OCP fault vectors. */
#define TELEM_IRQ_PRIORITY 5U

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

  /* ina228_init() is blocking, and the sequencer's interrupt-driven transfers
     would fight it for the handle. The vectors are enabled once, after the
     last channel has been initialised - see telem_start_sweeps(). */
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

/* Commits a completed set of four readings, or throws away a partial one.
   Shared by telem_update() and the sequencer so the two cannot drift apart.

   `valid` and `error_count` move here and nowhere else. Updating them as
   individual reads fail would make the CSV's valid_mask flicker according to
   where the sweep cursor happened to be when the reporter ran. */
static bool telem_commit(telem_channel_t *t, telem_sample_t *sample, bool ok)
{
  if (!ok)
  {
    /* Discard the partial sample; `latest`/`history` keep the last good one. */
    t->valid = false;
    t->error_count++;
    return false;
  }

  sample->tick_ms = HAL_GetTick();
  t->latest = *sample;

  t->history_head = (t->history_head + 1U) % TELEM_HISTORY_DEPTH;
  t->history[t->history_head] = *sample;
  if (t->history_count < TELEM_HISTORY_DEPTH)
  {
    t->history_count++;
  }

  t->valid = true;
  return true;
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

  return telem_commit(t, &sample, ok);
}

/* ---------------------------------------------------------------------------
   Non-blocking sweep.

   Twenty transfers - five channels, an input and an output sensor each, a bus
   voltage and a current from each sensor - walked one at a time across main
   loop passes. The cursor is the only thing that has to survive between calls,
   which is what makes this a real state machine rather than a dirty flag.

   The transfer itself is HAL_I2C_Mem_Read_IT: unlike the UART, I2C has a
   single user issuing strictly sequential transfers, so there is no lock to
   fight, and an I2C memory read is a multi-phase protocol the HAL already
   implements correctly.
   --------------------------------------------------------------------------- */

/* Four steps per channel, in the order the CSV reports them. */
#define TELEM_STEPS_PER_CHANNEL 4U

typedef enum
{
  TELEM_XFER_IDLE = 0,
  TELEM_XFER_RUNNING,
  TELEM_XFER_DONE,
  TELEM_XFER_FAILED
} telem_xfer_t;

typedef enum
{
  TELEM_SEQ_IDLE = 0,
  TELEM_SEQ_BUSY,
  TELEM_SEQ_RECOVER
} telem_seq_state_t;

static telem_channel_t *const telem_channels[PWM_CHANNEL_COUNT] = {
    &telem_a, &telem_b, &telem_c, &telem_d, &telem_e};

static struct
{
  telem_seq_state_t state;
  uint32_t channel;
  uint32_t step;
  uint8_t raw[3];
  telem_sample_t working;
  bool working_ok;
  uint32_t step_start_ms;
  uint32_t sweep_start_ms;
  uint32_t sweep_done_ms;
  uint32_t sweep_id;
  /* The only variable shared with the I2C ISR. Written RUNNING/IDLE here and
     DONE/FAILED there, never both at once, and always read once into a local. */
  volatile telem_xfer_t xfer;
} telem_seq;

/* Which sensor and quantity step 0..3 refers to. */
static ina228_t *step_device(telem_channel_t *t, uint32_t step)
{
  return (step < 2U) ? &t->input : &t->output;
}

static ina228_quantity_t step_quantity(uint32_t step)
{
  return ((step % 2U) == 0U) ? INA228_QTY_BUS_VOLTAGE : INA228_QTY_CURRENT;
}

/* Stores a decoded value into the field step 0..3 owns. */
static void step_store(telem_sample_t *sample, uint32_t step, float value)
{
  switch (step)
  {
    case 0U:  sample->vin_v = value;  break;
    case 1U:  sample->iin_a = value;  break;
    case 2U:  sample->vout_v = value; break;
    default:  sample->iout_a = value; break;
  }
}

/* Kicks the transfer for the current cursor position. Sets xfer to RUNNING on
   success; a HAL refusal is treated as a failed step rather than waiting for a
   callback that will never come. */
static void telem_start_step(void)
{
  telem_channel_t *t = telem_channels[telem_seq.channel];
  ina228_t *dev = step_device(t, telem_seq.step);
  ina228_quantity_t quantity = step_quantity(telem_seq.step);

  telem_seq.step_start_ms = HAL_GetTick();
  telem_seq.xfer = TELEM_XFER_RUNNING;

  if (HAL_I2C_Mem_Read_IT(dev->i2c, (uint16_t)(dev->address << 1),
                          ina228_register(quantity), I2C_MEMADD_SIZE_8BIT,
                          telem_seq.raw, ina228_register_size(quantity)) != HAL_OK)
  {
    telem_seq.xfer = TELEM_XFER_FAILED;
  }
}

/* Leaves the current channel behind - already committed by the caller - and
   starts the next, or closes the sweep if that was the last. */
static void telem_next_channel(void)
{
  telem_seq.step = 0U;
  telem_seq.working_ok = true;
  telem_seq.channel++;

  if (telem_seq.channel < (uint32_t)PWM_CHANNEL_COUNT)
  {
    telem_seq.state = TELEM_SEQ_BUSY;
    telem_start_step();
    return;
  }

  telem_seq.sweep_id++;
  telem_seq.sweep_done_ms = HAL_GetTick();
  telem_seq.state = TELEM_SEQ_IDLE;
  telem_seq.xfer = TELEM_XFER_IDLE;
}

/* Moves the cursor on after a step, committing the channel once its fourth
   reading has landed. */
static void telem_advance(void)
{
  telem_seq.step++;
  if (telem_seq.step < TELEM_STEPS_PER_CHANNEL)
  {
    telem_start_step();
    return;
  }

  (void)telem_commit(telem_channels[telem_seq.channel], &telem_seq.working,
                     telem_seq.working_ok);
  telem_next_channel();
}

/* A transfer stopped answering: no completion, no error callback. Abort it,
   and if the peripheral will not come back READY, re-initialise it.

   This recovers the controller, not the bus. A slave that is physically
   holding SDA low needs SCL pulsed until it lets go, which would mean
   bit-banging the pins - worth adding if it ever happens in anger. What this
   does guarantee is that the sequencer keeps running and every channel reports
   invalid, rather than telemetry stopping dead with no diagnostic. */
static void telem_recover(void)
{
  I2C_HandleTypeDef *i2c = telem_channels[telem_seq.channel]->input.i2c;

  (void)HAL_I2C_Master_Abort_IT(i2c, 0U);

  if (HAL_I2C_GetState(i2c) != HAL_I2C_STATE_READY)
  {
    (void)HAL_I2C_DeInit(i2c);
    if (HAL_I2C_Init(i2c) != HAL_OK)
    {
      /* The bus is gone. Leave telemetry stale rather than trapping the loop. */
      telem_seq.state = TELEM_SEQ_IDLE;
      telem_seq.xfer = TELEM_XFER_IDLE;
      return;
    }
  }

  /* Drop anything the aborted transfer left pending in the NVIC. Without this
     a stale interrupt can land just after the next step starts and report it
     complete before the bytes arrive. */
  telem_seq.xfer = TELEM_XFER_IDLE;
  HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
  HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);

  /* Give up on this channel - marked invalid, not left half-updated - and
     pick up at the next one. */
  (void)telem_commit(telem_channels[telem_seq.channel], &telem_seq.working, false);
  telem_next_channel();
}

void telem_start_sweeps(void)
{
  /* Enabled only now that every telem_init() has finished its blocking reads,
     so HAL's interrupt state machine never contends with the polled path for
     hi2c1. Priority 5 sits alongside UART5 and below the priority-0 OVP/OCP
     fault vectors, which must not be delayed. */
  HAL_NVIC_SetPriority(I2C1_EV_IRQn, TELEM_IRQ_PRIORITY, 0U);
  HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  HAL_NVIC_SetPriority(I2C1_ER_IRQn, TELEM_IRQ_PRIORITY, 0U);
  HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);

  telem_seq.state = TELEM_SEQ_IDLE;
  telem_seq.xfer = TELEM_XFER_IDLE;
  telem_seq.sweep_start_ms = HAL_GetTick();
  telem_seq.sweep_done_ms = telem_seq.sweep_start_ms;
}

void telem_service(void)
{
  uint32_t now = HAL_GetTick();

  switch (telem_seq.state)
  {
    case TELEM_SEQ_IDLE:
      /* Unsigned subtraction, so this survives the 32-bit tick wrap. */
      if ((now - telem_seq.sweep_start_ms) < TELEM_SWEEP_PERIOD_MS)
      {
        return;
      }
      telem_seq.sweep_start_ms = now;
      telem_seq.channel = 0U;
      telem_seq.step = 0U;
      telem_seq.working_ok = true;
      telem_seq.state = TELEM_SEQ_BUSY;
      telem_start_step();
      return;

    case TELEM_SEQ_BUSY:
      switch (telem_seq.xfer)
      {
        case TELEM_XFER_RUNNING:
          /* The common path: nothing to do but check we are not wedged. A
             NACKing sensor raises the error callback, but a slave holding the
             bus raises nothing at all - without this the sweep would park
             here forever and telemetry would stop with no diagnostic. */
          if ((now - telem_seq.step_start_ms) > TELEM_STEP_TIMEOUT_MS)
          {
            telem_seq.state = TELEM_SEQ_RECOVER;
          }
          return;

        case TELEM_XFER_DONE:
        {
          telem_channel_t *t = telem_channels[telem_seq.channel];
          step_store(&telem_seq.working, telem_seq.step,
                     ina228_decode(step_device(t, telem_seq.step),
                                   step_quantity(telem_seq.step), telem_seq.raw));
          telem_advance();
          return;
        }

        case TELEM_XFER_FAILED:
          telem_seq.working_ok = false;
          telem_advance();
          return;

        case TELEM_XFER_IDLE:
        default:
          /* Should not happen; restart the step rather than stall. */
          telem_start_step();
          return;
      }

    case TELEM_SEQ_RECOVER:
    default:
      telem_recover();
      return;
  }
}

/* ISR context, both of these. They only move the shared flag - all decoding
   and cursor work happens back in telem_service(). */
void telem_i2c_complete(void)
{
  if (telem_seq.xfer == TELEM_XFER_RUNNING)
  {
    telem_seq.xfer = TELEM_XFER_DONE;
  }
}

void telem_i2c_error(void)
{
  if (telem_seq.xfer == TELEM_XFER_RUNNING)
  {
    telem_seq.xfer = TELEM_XFER_FAILED;
  }
}

uint32_t telem_sweep_id(void)
{
  return telem_seq.sweep_id;
}

uint32_t telem_sweep_age_ms(void)
{
  return HAL_GetTick() - telem_seq.sweep_done_ms;
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
