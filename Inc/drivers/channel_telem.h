#ifndef CHANNEL_TELEM_H
#define CHANNEL_TELEM_H

#include <stdbool.h>
#include <stdint.h>

/* Two INA228s per channel on I2C1, input side and output side.
 *
 * This module owns the sensors, not the data: every reading lands in
 * channel_x.telem and there is no second copy here. The I2C addresses and the
 * device handles are private, so a caller names a channel and nothing else.
 *
 * Everything soft-fails - telemetry loss must never halt the converter. */

/* Blocking: configures both sensors on one channel. Call once per channel from
 * app_setup(), before telem_start_sweeps(). */
bool telem_init(uint32_t channel);

/* Enables the I2C1 interrupt and arms the first sweep. Call once from
 * app_setup(), after every telem_init() - those use blocking reads and must
 * not overlap the interrupt-driven path. */
void telem_start_sweeps(void);

/* Non-blocking sweep of all five channels, one I2C transfer at a time, driven
 * by the I2C1 interrupt. Call every pass of the main loop; it starts a sweep
 * every TELEM_SWEEP_PERIOD_MS and advances the one in flight, returning in a
 * few microseconds either way.
 *
 * A channel's four values are committed together, so channel_x.telem never
 * shows a half-updated channel. Different channels are sampled up to one sweep
 * apart - telem_sweep_age_ms() says how stale the set is. */
void telem_service(void);

/* Milliseconds since the last sweep completed. */
uint32_t telem_sweep_age_ms(void);

/* Total failed inits and updates for one channel, since boot. */
uint32_t telem_error_count(uint32_t channel);

/* I2C1 completion hooks, called from the HAL callbacks in interrupts.c. Not
 * for application use. */
void telem_i2c_complete(void);
void telem_i2c_error(void);

#endif /* CHANNEL_TELEM_H */
