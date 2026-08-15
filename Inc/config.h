/**
  ******************************************************************************
  * @file    config.h
  * @author  Angus Macdonald
  * @brief   System parameters - one place for the values worth changing.
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef CONFIG_H
#define CONFIG_H

/* Everything in here is a knob: a limit, a cadence, a threshold or a board
 * value. Facts that follow from the hardware - register layouts, ADC
 * resolution, unit scales - stay with the code that depends on them.
 *
 * No includes, no types, no logic. Just numbers, so this header can be pulled
 * into anything without dragging dependencies along with it. */

/* =========================== PWM =========================== */

/* Where a channel starts before anything reconfigures it. */
#define PWM_DEFAULT_FREQUENCY_HZ 500000U /* 500 kHz */
#define PWM_DEFAULT_DUTY_CYCLE   500U    /* 50.0% - tenths of a percent */
#define PWM_DEFAULT_DEAD_TIME_NS 20U     /* 20 ns */

/* Accepted range for each parameter. The drivers clamp to these, and the host
 * command parser rejects anything outside them - so widening a limit here is
 * all it takes to allow a value on the bench. */
#define PWM_MIN_FREQUENCY_HZ 100000U /* 100 kHz */
#define PWM_MAX_FREQUENCY_HZ 800000U /* 800 kHz */
#define PWM_MIN_DUTY_CYCLE   100U    /* 10.0% */
#define PWM_MAX_DUTY_CYCLE   900U    /* 90.0% */
#define PWM_MIN_DEAD_TIME_NS 5U      /* 5 ns */
#define PWM_MAX_DEAD_TIME_NS 300U    /* 300 ns */

/* fHRTIM with prescaler DIV1 (RCC.HRTIMFreq_Value): c_ck 480 MHz, giving
 * 2.083 ns per tick. Must match what SystemClock_Config() actually sets up -
 * every period and dead-time conversion is derived from it. */
#define PWM_KERNEL_CLOCK_HZ 480000000U

/* =========================== LEDs ========================== */

/* Drive polarity: 1 = a high pin lights the LED, 0 = a low pin does. The two
 * groups sit on different circuits, hence the two knobs. This is the only
 * place either is decided - flipping one inverts every line in its group. */
#define LED_TOG_ACTIVE_HIGH    0 /* LED_TOG_1..5, the per-channel lines */
#define LED_STATUS_ACTIVE_HIGH 1 /* LED_ACTIVE, LED_ERR, LED_OUT_CONN */

/* LED_OUT_CONN lights when the bus is up, i.e. a battery is connected. Two
 * thresholds rather than one so the LED does not chatter when the bus sits
 * right at the edge: it lights above the first and goes out below the second.
 * Keep OFF below ON. */
#define LED_BUS_ON_MV  35000U /* 35.0 V */
#define LED_BUS_OFF_MV 34000U /* 34.0 V */

/* ====================== Host serial link ==================== */

/* UART5 ring buffers, both powers of two so the ISR masks instead of comparing.
 *
 * TX has to absorb the worst burst that can be queued between drains: a
 * telemetry line (181 B worst case - 20 fields that can reach "-102400"), the
 * five #cfg lines (130 B) and one reply (96 B) = 418 B. 1024 leaves ~600 B of
 * headroom so the back-pressure rule in command.c rarely bites, for 1 KB of a
 * 128 KB DTCM. Steady state is 3.8 B/ms against 11.52 B/ms of wire at 115200,
 * so the link runs at a third of capacity.
 *
 * RX only has to hold commands arriving faster than one loop pass drains them,
 * which is a much smaller problem. */
#define SERIAL_TX_RING_LEN 1024U
#define SERIAL_RX_RING_LEN 256U

/* Telemetry line cadence. Now a throughput budget rather than a loop cost -
 * nothing here blocks - and 50 ms is still where three independent limits
 * land: the INA228s average a set every ~34 ms so polling faster returns the
 * same numbers twice; a worst-case line is 181 bytes, which is ~16 ms of wire
 * time at 115200 baud. Raising UART5's baud rate in CubeMX buys the headroom
 * to go faster. */
#define REPORT_TELEM_PERIOD_MS 50U

/* How often the five "#cfg" lines are repeated when nothing has changed. Slow
 * enough to stay out of the telemetry's way (~130 bytes), fast enough that a
 * host which connects mid-run, or a channel that faults without being asked,
 * shows up promptly. Any command also triggers a set. */
#define REPORT_CFG_PERIOD_MS 1000U

/* Commands parsed per loop pass. Bounds how long one pass can spend on a
 * backlog, so loop period stays predictable once a control loop shares it. */
#define COMMAND_MAX_LINES_PER_PASS 4U

/* ========================= Telemetry ======================== */

/* How often a full five-channel sweep starts. The INA228s average 16 samples
 * of a 2 x 1052 us conversion, producing a new averaged set every ~34 ms, so
 * sweeping faster just re-reads the same numbers. 40 ms also guarantees the
 * 50 ms report always has a sweep less than one report period old. */
#define TELEM_SWEEP_PERIOD_MS 40U

/* How long one transfer may take before the sequencer gives up on it. A 3-byte
 * read at 100 kHz is ~540 us, so this is generous - it exists to catch a bus
 * that has stopped answering entirely, which produces no error callback at
 * all, not to second-guess a slow sensor. */
#define TELEM_STEP_TIMEOUT_MS 5U

/* ========================= Board wiring ===================== */

/* V_BUS_DIV resistor divider (PA6 / ADC1_INP3): 100k top, 5.23k bottom. */
#define VBUS_DIV_TOP_OHMS    100000U
#define VBUS_DIV_BOTTOM_OHMS 5230U

/* How often the bus voltage is sampled. A conversion costs under a
 * microsecond, so this is set by how quickly the bus LED should react rather
 * than by any cost - and sampling well inside the telemetry period means the
 * reported value is never stale. */
#define VBUS_PERIOD_MS 10U

#endif /* CONFIG_H */
