/**
  ******************************************************************************
  * @file    ntc_table.h
  * @author  Angus Macdonald
  * @brief   NCU18XH103F6SRB divider voltage vs temperature (GENERATED).
  ******************************************************************************
  * @attention
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#ifndef NTC_TABLE_H
#define NTC_TABLE_H

#include <stdint.h>

/* DO NOT EDIT BY HAND. Regenerate with:
 *
 *     uv run tools/gen_ntc_table.py
 *
 * Source: .agents/NCU18XH103F6SRB.csv (Murata's typical curve), with the
 * thermistor curve backed out of Murata's reference divider and recomputed
 * for THIS BOARD's divider:
 *
 *     10000 ohm NTC to ground, 100000 ohm pull-up to 3.3 V
 *
 * If that is not the circuit on the board, every temperature this table
 * produces is wrong - and wrong smoothly, so it looks like a reading rather
 * than a fault. Regenerate with --pullup / --rail.
 *
 * Values are the voltage at the ADC pin in tenths of a millivolt, strictly
 * decreasing, one entry per 1 degC from NTC_TABLE_MIN_C upward. */

#define NTC_TABLE_MIN_C   -40
#define NTC_TABLE_STEP_C  1
#define NTC_TABLE_LEN     191U

/* Tenths of a millivolt, so 33000 is 3.3 V. */
static const uint16_t ntc_table_dmv[NTC_TABLE_LEN] = {
    21838, 21418, 20993, 20566, 20135, 19703, 19269, 18834,  /*  -40 degC */
    18400, 17965, 17532, 17101, 16673, 16247, 15824, 15406,  /*  -32 degC */
    14991, 14581, 14177, 13778, 13385, 12999, 12619, 12247,  /*  -24 degC */
    11881, 11523, 11171, 10828, 10492, 10164, 9843, 9529,  /*  -16 degC */
    9223, 8925, 8635, 8353, 8079, 7813, 7555, 7304,  /*   -8 degC */
    7060, 6825, 6597, 6377, 6163, 5955, 5755, 5561,  /*   +0 degC */
    5373, 5192, 5016, 4846, 4682, 4524, 4371, 4223,  /*   +8 degC */
    4080, 3942, 3809, 3681, 3557, 3438, 3322, 3211,  /*  +16 degC */
    3104, 3000, 2900, 2803, 2710, 2620, 2533, 2450,  /*  +24 degC */
    2369, 2291, 2216, 2144, 2074, 2007, 1942, 1879,  /*  +32 degC */
    1819, 1761, 1704, 1650, 1597, 1547, 1498, 1450,  /*  +40 degC */
    1405, 1361, 1318, 1277, 1238, 1199, 1162, 1127,  /*  +48 degC */
    1092, 1059, 1027, 996, 966, 937, 909, 883,  /*  +56 degC */
    857, 832, 808, 784, 762, 740, 719, 699,  /*  +64 degC */
    679, 660, 641, 623, 606, 589, 573, 557,  /*  +72 degC */
    542, 527, 513, 499, 485, 472, 460, 448,  /*  +80 degC */
    436, 424, 413, 402, 392, 382, 372, 362,  /*  +88 degC */
    353, 344, 335, 327, 318, 310, 303, 295,  /*  +96 degC */
    288, 281, 274, 267, 261, 254, 248, 242,  /* +104 degC */
    237, 231, 225, 220, 215, 210, 205, 200,  /* +112 degC */
    196, 191, 187, 183, 178, 174, 170, 167,  /* +120 degC */
    163, 159, 156, 152, 149, 146, 143, 139,  /* +128 degC */
    136, 134, 131, 128, 125, 123, 120, 117,  /* +136 degC */
    115, 113, 110, 108, 106, 104, 101,  /* +144 degC */
};

#endif /* NTC_TABLE_H */
