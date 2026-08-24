#ifndef CONFIG_H
#define CONFIG_H

/* Channel definitions */
#define CHANNEL_A     0U
#define CHANNEL_B     1U
#define CHANNEL_C     2U
#define CHANNEL_D     3U
#define CHANNEL_E     4U
#define CHANNEL_COUNT 5U

/* CHECK State timeout before failure */
#define CHECK_TIMEOUT_MS 5000U // 5 seconds.

/* Default converter settings */
#define PWM_DEFAULT_FREQUENCY_HZ 500000U // 500 kHz
#define PWM_DEFAULT_DUTY_CYCLE   0U      // 0.0%
#define PWM_DEFAULT_DEAD_TIME_NS 20U     // 20ns

/* Converter input ranges */
#define PWM_MIN_FREQUENCY_HZ 100000U // 100 kHz
#define PWM_MAX_FREQUENCY_HZ 800000U // 800 kHz
#define PWM_MIN_DUTY_CYCLE   0U      // 0.0%
#define PWM_MAX_DUTY_CYCLE   850U    // 85.0%
#define PWM_MIN_DEAD_TIME_NS 5U      // 5 ns
#define PWM_MAX_DEAD_TIME_NS 300U    // 300 ns

/* VBUS LED indicator thresholds */
#define LED_BUS_ON_MV  35000U /* 35.0 V */
#define LED_BUS_OFF_MV 34000U /* 34.0 V */

/* INA228 Telemetry Parameters */
#define TELEM_SWEEP_PERIOD_MS 40U
#define TELEM_STEP_TIMEOUT_MS 5U

/* Analog Telemetry Parameters */
#define ANALOG_PERIOD_MS 1U

#endif /* CONFIG_H */
