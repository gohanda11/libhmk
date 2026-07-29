/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "matrix.h"

#include "distance.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "lib/bitmap.h"
#include "split.h"
#include "tusb.h"

// Exponential moving average (EMA) filter
#define EMA(x, y)                                                              \
  (((uint32_t)(x) +                                                            \
    ((uint32_t)(y) * ((1 << MATRIX_EMA_ALPHA_EXPONENT) - 1))) >>               \
   MATRIX_EMA_ALPHA_EXPONENT)

__attribute__((always_inline)) static inline uint16_t
matrix_analog_read(uint8_t key) {
#if defined(MATRIX_INVERT_ADC_VALUES)
  return ADC_MAX_VALUE - analog_read(key);
#else
  return analog_read(key);
#endif
}

__attribute__((always_inline)) static inline uint16_t
matrix_bottom_out_value(uint8_t key, uint16_t rest_value) {
  return M_MIN(rest_value +
                   M_MAX(eeconfig->calibration.initial_bottom_out_threshold,
                         eeconfig->bottom_out_threshold[key]),
               ADC_MAX_VALUE);
}

key_state_t key_matrix[NUM_KEYS];

// Bitmap for tracking which keys have Rapid Trigger disabled
static bitmap_t rapid_trigger_disabled[] = MAKE_BITMAP(NUM_KEYS);

//--------------------------------------------------------------------+
// Travel Distance AGC
//--------------------------------------------------------------------+
// Compensates for reduced ADC swing on a voltage-sagged split half (e.g. when
// the slave is powered only through a 3.3V/GND/DATA tether). Peak travel is
// tracked per key and used to stretch distance toward the full 0-255 range.
// On a healthy half the peak quickly reaches 255 and scaling becomes a no-op.
// Split-only: on non-split keyboards the ADC range is always healthy and the
// saturation behavior while the peak is still learning would distort the
// distance curve.

#if defined(SPLIT_KEYBOARD)

#if !defined(MATRIX_DISTANCE_AGC_MIN_PEAK)
// Do not engage scaling until a key has traveled at least this far. Prevents
// soft taps from permanently over-amplifying subsequent presses.
#define MATRIX_DISTANCE_AGC_MIN_PEAK ((uint16_t)(96u * (uint32_t)TRAVEL_UNITS / 255u))
#endif

#if !defined(MATRIX_DISTANCE_AGC_DECAY_MS)
// Slowly forget peaks so a stale low peak (or recovering supply) can relearn.
#define MATRIX_DISTANCE_AGC_DECAY_MS 2000
#endif

static uint16_t distance_peak[NUM_KEYS];
static uint32_t distance_agc_decay_timer;

static uint16_t matrix_apply_distance_agc(uint8_t key, uint16_t distance) {
  if (distance > distance_peak[key])
    distance_peak[key] = distance;

  const uint16_t peak = distance_peak[key];
  if (peak < MATRIX_DISTANCE_AGC_MIN_PEAK || peak >= (uint16_t)TRAVEL_UNITS)
    return distance;

  // distance <= peak, so the scaled value fits in uint16_t for TRAVEL_UNITS.
  return (uint16_t)((uint32_t)distance * (uint32_t)TRAVEL_UNITS / peak);
}

static void matrix_distance_agc_decay(void) {
  if (timer_elapsed(distance_agc_decay_timer) < MATRIX_DISTANCE_AGC_DECAY_MS)
    return;
  distance_agc_decay_timer = timer_read();

  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    if (key_matrix[i].is_pressed)
      continue;
    if (distance_peak[i] > 0)
      distance_peak[i]--;
  }
}

#endif // defined(SPLIT_KEYBOARD)

void matrix_init(void) { matrix_recalibrate(false); }

void matrix_recalibrate(bool reset_bottom_out_threshold) {
  if (reset_bottom_out_threshold) {
    uint16_t bottom_out_threshold[NUM_KEYS] = {0};
    EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
  }

  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    key_matrix[i].adc_filtered = eeconfig->calibration.initial_rest_value;
    key_matrix[i].adc_rest_value = eeconfig->calibration.initial_rest_value;
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value(i, eeconfig->calibration.initial_rest_value);
    key_matrix[i].distance = 0;
    key_matrix[i].extremum = 0;
    key_matrix[i].key_dir = KEY_DIR_INACTIVE;
    key_matrix[i].is_pressed = false;
#if defined(SPLIT_KEYBOARD)
    distance_peak[i] = 0;
#endif
  }
#if defined(SPLIT_KEYBOARD)
  distance_agc_decay_timer = timer_read();
#endif

  // We only calibrate the rest value. The bottom-out value will be updated
  // during the scan process.
  const uint32_t calibration_start = timer_read();
  while (timer_elapsed(calibration_start) < MATRIX_CALIBRATION_DURATION) {
#if defined(SPLIT_KEYBOARD)
    // Keep USB alive during the blocking calibration window so post-init
    // recalibration cannot stall enumeration on the master half. Only split
    // keyboards do this: COMMAND_RECALIBRATE may invoke matrix_recalibrate()
    // from within a tud_task() callback, so non-split builds keep the
    // original behavior and avoid re-entering tud_task() recursively.
    tud_task();
    // Drain split RX only after the UART is initialized (see split_post_init).
    split_calibration_idle();
#endif
    // Run the analog task to possibly update the ADC values
    analog_task();

#if defined(SPLIT_KEYBOARD)
    // On split keyboards, each half only has ADC inputs for its local keys.
    // Remote keys will receive their analog state through the split transport.
    for (uint32_t i = 0; i < split_get_num_local_keys(); i++) {
      const uint8_t key = split_get_key_offset() + i;
#else
    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      const uint8_t key = i;
#endif
      const uint16_t new_adc_filtered =
          EMA(matrix_analog_read(key), key_matrix[key].adc_filtered);

      key_matrix[key].adc_filtered = new_adc_filtered;

      if (new_adc_filtered + MATRIX_CALIBRATION_EPSILON <=
          key_matrix[key].adc_rest_value)
        // Only update the rest value if the new value is smaller and the
        // difference is at least the calibration epsilon
        key_matrix[key].adc_rest_value = new_adc_filtered;

      // Update the bottom-out value to be the minimum bottom-out value based on
      // the updated rest value
      key_matrix[key].adc_bottom_out_value =
          matrix_bottom_out_value(key, key_matrix[key].adc_rest_value);
    }
  }
}

void matrix_update_press_state(uint8_t key, uint16_t distance) {
  const actuation_t *actuation = &CURRENT_PROFILE.actuation_map[key];

  key_matrix[key].distance = distance;

  if (bitmap_get(rapid_trigger_disabled, key) | (actuation->rt_down == 0)) {
    key_matrix[key].key_dir = KEY_DIR_INACTIVE;
    key_matrix[key].is_pressed =
        (key_matrix[key].distance >= actuation->actuation_point);
  } else {
    const uint16_t reset_point =
        actuation->continuous ? 0 : actuation->actuation_point;
    const uint16_t rt_up =
        actuation->rt_up == 0 ? actuation->rt_down : actuation->rt_up;

    switch (key_matrix[key].key_dir) {
    case KEY_DIR_INACTIVE:
      if (key_matrix[key].distance > actuation->actuation_point) {
        // Pressed down past actuation point
        key_matrix[key].extremum = key_matrix[key].distance;
        key_matrix[key].key_dir = KEY_DIR_DOWN;
        key_matrix[key].is_pressed = true;
      }
      break;

    case KEY_DIR_DOWN:
      if (key_matrix[key].distance <= reset_point) {
        // Released past reset point
        key_matrix[key].extremum = key_matrix[key].distance;
        key_matrix[key].key_dir = KEY_DIR_INACTIVE;
        key_matrix[key].is_pressed = false;
      } else if (key_matrix[key].distance + rt_up < key_matrix[key].extremum) {
        // Released by Rapid Trigger
        key_matrix[key].extremum = key_matrix[key].distance;
        key_matrix[key].key_dir = KEY_DIR_UP;
        key_matrix[key].is_pressed = false;
      } else if (key_matrix[key].distance > key_matrix[key].extremum)
        // Pressed down further
        key_matrix[key].extremum = key_matrix[key].distance;
      break;

    case KEY_DIR_UP:
      if (key_matrix[key].distance <= reset_point) {
        // Released past reset point
        key_matrix[key].extremum = key_matrix[key].distance;
        key_matrix[key].key_dir = KEY_DIR_INACTIVE;
        key_matrix[key].is_pressed = false;
      } else if (key_matrix[key].extremum + actuation->rt_down <
                 key_matrix[key].distance) {
        // Pressed by Rapid Trigger
        key_matrix[key].extremum = key_matrix[key].distance;
        key_matrix[key].key_dir = KEY_DIR_DOWN;
        key_matrix[key].is_pressed = true;
      } else if (key_matrix[key].distance < key_matrix[key].extremum)
        // Released further
        key_matrix[key].extremum = key_matrix[key].distance;
      break;

    default:
      break;
    }
  }
}

static void matrix_scan_key(uint32_t i) {
  const uint16_t new_adc_filtered =
      EMA(matrix_analog_read(i), key_matrix[i].adc_filtered);

  key_matrix[i].adc_filtered = new_adc_filtered;

#if defined(SPLIT_KEYBOARD)
  // Soft rest adaptation: if the key is released and the filtered ADC is
  // clearly below the calibrated rest, drift rest downward. This removes the
  // dead zone that remains on some slave keys after boot-time calibration.
  if (!key_matrix[i].is_pressed &&
      new_adc_filtered + MATRIX_CALIBRATION_EPSILON <=
          key_matrix[i].adc_rest_value) {
    key_matrix[i].adc_rest_value = new_adc_filtered;
    key_matrix[i].adc_bottom_out_value =
        matrix_bottom_out_value((uint8_t)i, key_matrix[i].adc_rest_value);
  }
#endif

  if (new_adc_filtered >=
      key_matrix[i].adc_bottom_out_value + MATRIX_CALIBRATION_EPSILON)
    // Only update the bottom-out value if the new value is larger and the
    // difference is at least the calibration epsilon.
    key_matrix[i].adc_bottom_out_value = new_adc_filtered;

  const uint16_t raw_distance = adc_to_distance(
      new_adc_filtered, key_matrix[i].adc_rest_value,
      key_matrix[i].adc_bottom_out_value);
#if defined(SPLIT_KEYBOARD)
  matrix_update_press_state((uint8_t)i,
                            matrix_apply_distance_agc((uint8_t)i, raw_distance));
#else
  matrix_update_press_state((uint8_t)i, raw_distance);
#endif
}

void matrix_scan(void) {
#if defined(SPLIT_KEYBOARD)
  // On split keyboards, each half only scans its own local keys. The global key
  // offset is determined at runtime from the handedness detection.
  for (uint32_t i = 0; i < split_get_num_local_keys(); i++)
    matrix_scan_key(split_get_key_offset() + i);
#else
  for (uint32_t i = 0; i < NUM_KEYS; i++)
    matrix_scan_key(i);
#endif
#if defined(SPLIT_KEYBOARD)
  matrix_distance_agc_decay();
#endif
}

void matrix_disable_rapid_trigger(uint8_t key, bool disable) {
  bitmap_set(rapid_trigger_disabled, key, disable);
}
