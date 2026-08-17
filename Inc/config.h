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

/* ========================== Channels ======================== */

/* Five converter channels, A-E, fixed by the board. Plain defines rather than
 * an enum: an enum bought nothing worth a cast on every loop bound and every
 * index, because C converts int and enum freely and the count can never
 * change. A channel id is a uint32_t. */
#define CHANNEL_A     0U
#define CHANNEL_B     1U
#define CHANNEL_C     2U
#define CHANNEL_D     3U
#define CHANNEL_E     4U
#define CHANNEL_COUNT 5U

/* =========================== PWM =========================== */

/* Where a channel starts before anything reconfigures it.
 *
 * Duty starts at zero, which for a boost is pass-through: the control FET
 * never conducts and the output sits at Vin. That is the only safe starting
 * point. The ideal diode blocks the battery, so a channel's output is
 * *unloaded* until it charges above the bus - and an unloaded boost at fixed
 * duty runs toward Vin/(1-D). At 50% on the large array that is 2 x 30.5 =
 * 61 V, past both the 54.6 V bus maximum and the 55 V OVP trip. A channel is
 * brought up by ramping from here, not by starting at a working duty. */
#define PWM_DEFAULT_FREQUENCY_HZ 500000U /* 500 kHz */
#define PWM_DEFAULT_DUTY_CYCLE   0U      /* 0.0% - tenths of a percent */
#define PWM_DEFAULT_DEAD_TIME_NS 20U     /* 20 ns */

/* Accepted range for each parameter. The drivers clamp to these, and the host
 * command parser rejects anything outside them - so widening a limit here is
 * all it takes to allow a value on the bench.
 *
 * The duty ceiling is set by the worst real operating point across both PV
 * array variants. D = 1 - Vin/Vbus, so the highest duty is demanded by the
 * lowest input into the highest bus: a hot small array (Vmpp 12.11 V, -14% at
 * a 65 C cell = 10.41 V) into a full 54.6 V battery needs 80.9%. 85% leaves
 * ~4 points of margin, and nothing above it corresponds to an operating point
 * that exists. The large array never needs more than 56.5%, so on that array
 * this ceiling is a long way from binding - it is a backstop, and the real
 * protection is the dynamic limit D_max = 1 - Vin/Vbus + margin computed from
 * live sensor values. See .agents/hardware.md. */
#define PWM_MIN_FREQUENCY_HZ 100000U /* 100 kHz */
#define PWM_MAX_FREQUENCY_HZ 800000U /* 800 kHz */
#define PWM_MIN_DUTY_CYCLE   0U      /* 0.0% - pass-through */
#define PWM_MAX_DUTY_CYCLE   850U    /* 85.0% */
#define PWM_MIN_DEAD_TIME_NS 5U      /* 5 ns */
#define PWM_MAX_DEAD_TIME_NS 300U    /* 300 ns */

/* fHRTIM with prescaler DIV1 (RCC.HRTIMFreq_Value): c_ck 480 MHz, giving
 * 2.083 ns per tick. Must match what SystemClock_Config() actually sets up -
 * every period and dead-time conversion is derived from it. */
#define PWM_KERNEL_CLOCK_HZ 480000000U

/* =========================== LEDs ========================== */

/* Drive polarity is not here. It follows from the board and can never change
 * without a respin, so it is written into each setter in led.c - per-channel
 * lines light LOW, status lines light HIGH. See .agents/hardware.md. */

/* Start-up lamp test: walks one lit LED along all eight lines so every one is
 * seen to work, and gives the board a visible "I am alive and not yet
 * switching" state. Each step turns the next line on and the previous one off,
 * so 100 ms a step over 8 lines is an 800 ms sweep and 5 s is a little over
 * six passes - long enough to catch a dead line by eye, short enough that
 * nobody waits on it. */
#define LED_SEQUENCE_MS      5000U
#define LED_SEQUENCE_STEP_MS 100U

/* Then all eight flash together this many times, at the same 100 ms step, to
 * mark the end of the sequence. One on plus one off is two steps, so 5 flashes
 * is 1 s. LED_BLINK_MS is derived rather than written out, so changing the
 * count cannot leave a stale duration behind. */
#define LED_BLINK_COUNT 5U
#define LED_BLINK_MS    (2U * LED_BLINK_COUNT * LED_SEQUENCE_STEP_MS)

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

/* ================= Inductor current sensing ================= */

/* INA310A2 across a 3 mOhm shunt. The "A2" suffix is the 50 V/V gain option,
 * so the amplifier presents gain x shunt = 150 mV per amp, and the ADC's 3.3 V
 * span covers +/-22 A about the calibrated zero - comfortably past the 12 A
 * hardware trip. Board values, hence here; the scale factors they imply are
 * derived in iind.c. */
#define IIND_AMP_GAIN_V_PER_V 50U
#define IIND_SHUNT_MICRO_OHMS 3000U

/* Samples per switching period, as a divider. The HRTIM master timer runs at
 * the switching frequency divided by this, so the sample instant stays locked
 * to the same point in the waveform instead of drifting through it.
 *
 * 5 gives 100 kHz at the default 500 kHz switching, which is far more than any
 * control loop here needs, and leaves ADC3 10 us to convert its four channels
 * rather than the 2 us one switching period would allow. */
#define IIND_SAMPLE_DIVIDER 5U

/* Where in the period to sample, in tenths of a percent. 500 is halfway, which
 * is the middle of the on-time at 50% duty - where the current triangle
 * crosses its own average.
 *
 * The limits keep the sample off the switching edges, where the amplifier is
 * still slewing and the reading is a transient rather than a current. */
#define IIND_DEFAULT_SAMPLE_POINT 500U
#define IIND_MIN_SAMPLE_POINT     50U  /* 5.0% */
#define IIND_MAX_SAMPLE_POINT     950U /* 95.0% */

/* How long without a completed sample set before iind_state() stops claiming
 * RUNNING. Generous against a 100 kHz sample rate - it is there to catch a
 * trigger or DMA that has stopped entirely, not to police jitter. */
#define IIND_STALE_TIMEOUT_MS 5U

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

/* How often ADC1's six slow inputs - the bus divider and the five NTCs - are
 * swept. Set by how quickly the bus LED should react; the temperatures come
 * along for free and would be happy at a tenth of this.
 *
 * One sweep is 27 us: the bus conversion is 73 ADC cycles at 76 MHz (~1 us)
 * and each NTC is 396 (~5.2 us), the difference being sampling time for the
 * thermistor's higher source impedance. Against a 10 ms cadence that is 0.3%
 * of the loop, which is cheaper than running two cadences to save it. */
#define ANALOG_PERIOD_MS 10U

#endif /* CONFIG_H */
