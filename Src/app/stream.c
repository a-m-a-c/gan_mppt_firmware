#include "stream.h"
#include "main.h"
#include "system.h"
#include "channel.h"
#include "mode_single_ch_mppt.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

#define STREAM_PERIOD_MS 1U

/*
Stream packet structure (outgoing):
[id][size][data]
id: 1 byte, identifies the value
size: 1 byte, size of the data field
data: size bytes, little endian
*/

// Wire values. The host decodes on these, so they are not free to renumber.
#define STREAM_ID_VBUS_MV 0x60U
#define STREAM_ID_DUTY    0x61U
#define STREAM_ID_FLAGS   0x62U
#define STREAM_ID_VIN_MV  0x63U
#define STREAM_ID_IIN_MA  0x64U
#define STREAM_ID_VIN_TARGET_MV 0x65U

#define STREAM_PACKET_COUNT 6U

/* A set is 3*(2+4) + 2*(2+2) + (2+1) = 29 bytes. At 921600 baud, 8N1, that is
   29 * 10 / 921600 = 315 us of line time per 1 ms period - 32% loaded. */

static uint32_t last_send_ms;
static uint8_t next_packet;

/* Frozen when a set starts. vin and iin leave in different loop passes, and a
   telem commit landing between them would put a voltage and a current from
   different sweeps on the same curve point. */
static chan_telem_t telem_a;

static void put_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

// Clamped, not signed: a negative input voltage is a broken sensor, not a
// reading, and 0 is already the out-of-range value analog.c publishes.
static uint32_t volts_to_mv(float v) {
  if (v <= 0.0f) return 0U;
  return (uint32_t)(v * 1000.0f);
}

/* Signed - reverse current through the channel is real and the host has to be
   able to see it. Two's complement on the wire; the host decodes it back. */
static int32_t amps_to_ma(float a) {
  return (int32_t)(a * 1000.0f);
}

// False if the UART was busy, so the caller retries on the next pass.
static bool send_packet(uint8_t index) {
  uint8_t payload[4];

  switch (index) {
    case 0:
      put_u32(payload, sys.vbus_mv);
      return serial_send(STREAM_ID_VBUS_MV, payload, 4U);
    case 1:
      put_u32(payload, volts_to_mv(telem_a.vin_v));
      return serial_send(STREAM_ID_VIN_MV, payload, 4U);
    case 2:
      put_u32(payload, (uint32_t)amps_to_ma(telem_a.iin_a));
      return serial_send(STREAM_ID_IIN_MA, payload, 4U);
    case 3:
      put_u16(payload, channel_a.pwm.duty_applied);
      return serial_send(STREAM_ID_DUTY, payload, 2U);
    case 4:
      // The running mode's setpoint, 0 when nothing is regulating vin.
      put_u16(payload, mode_single_ch_mppt_target_mv());
      return serial_send(STREAM_ID_VIN_TARGET_MV, payload, 2U);
    case 5: {
      /* bit 0: channel A telemetry valid.
         bit 1: channel A switching. pwm_stop() zeroes the duty, so a stopped
         channel and one running in pass-through both report 0 - this bit is
         the only thing separating them. */
      uint8_t flags = telem_a.valid ? 0x01U : 0x00U;
      if (channel_a.pwm.op_state == PWM_STATE_RUNNING) flags |= 0x02U;
      payload[0] = flags;
      return serial_send(STREAM_ID_FLAGS, payload, 1U);
    }
  }
  return false;
}

void stream_init(void) {
  last_send_ms = HAL_GetTick();
  next_packet = STREAM_PACKET_COUNT; // Idle until the first period elapses.
}

void stream_service(void) {
  const uint32_t now = HAL_GetTick();

  if ((now - last_send_ms) >= STREAM_PERIOD_MS) {
    last_send_ms = now;
    next_packet = 0; // Restart the set. Anything still unsent is abandoned -
                     // a fresh value is worth more than a stale one.
    telem_a = channel_a.telem;
  }

  if (next_packet >= STREAM_PACKET_COUNT) {
    return;
  }

  /* One packet leaves per pass at most, and only when the UART is free. The
     loop runs far faster than the line, so the whole set is out well inside
     the period. */
  if (send_packet(next_packet)) {
    next_packet++;
  }
}
