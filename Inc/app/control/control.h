#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stdint.h>
/* Provides an interface layer for modes to the converter 
- Stuff like gating the duty setting to apply ramp limits, soft starting, etc.
- It is not a requirement to use this layer, but it is recommended to use it.
- Hardstop protections are implemented in the driver layer, but this layer can provide additional limiters and protections.

Usage:
- in a mode_begin function, call control_init()?
- decorate control functionality with desired functionality with additional inits?
- control_set_duty() to set the duty cycle, which will apply limiters and protections
- control_service() to update background
- control_stop() to teardown, called on mode exit.

Duties:
- Limiters
  - Thermal limiters
  - ramp limiters
- soft start
- dynamic frequency control (background)
- dynamic deadtime control (background)
- dynamic protection ceilings (background)
- duty setting with limiters

Possible future features:
- input voltage setting (abstract pid loops out of modes)
- input current setting (abstract pid loops out of modes)
- inductor current regulation

*/
typedef struct {
  bool ramp_limit_enabled;
  bool thermal_limit_enabled;
  bool dynamic_frequency_enabled;
  bool dynamic_deadtime_enabled;
  bool dynamic_protection_ceilings_enabled;
  bool channel_a_enabled;
  bool channel_b_enabled;
  bool channel_c_enabled;
  bool channel_d_enabled;
  bool channel_e_enabled;

  uint32_t ramp_rate_per_ms; // in duty cycle units per ms
} control_config_t;

// Call in mode_x_begin to init control interface with desired config.
void control_init(control_config_t *config);

// Call in mode_x_service to update background control functionality.
void control_service(void);

// Call to apply a duty cycle to a channel, will apply limiters and protections.
void control_set_duty(uint32_t channel, uint16_t duty_cycle);

// Call to stop control interface, called on mode exit.
void control_stop(void);

#endif /* CONTROL_H */
