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

#pragma once

#include "common.h"
#include "wear_leveling.h"

//--------------------------------------------------------------------+
// Keyboard Persistent Configuration
//--------------------------------------------------------------------+

// Magic number to identify the start of the configuration
#define EECONFIG_MAGIC_START 0x0A42494C
// Magic number to identify the end of the configuration
#define EECONFIG_MAGIC_END 0x0A4B4D48

// Keyboard calibration configuration
typedef struct __attribute__((packed)) {
  // Initial rest value of the key matrix. If the value is smaller than the
  // actual rest value, the key will have a dead zone at the beginning of the
  // keystroke. If the value is larger than the actual rest value, a longer
  // calibration process may be required.
  uint16_t initial_rest_value;
  // Minimum change in ADC values for the key to be considered bottom-out. If
  // the value is larger than the actual bottom-out threshold, the key will have
  // a dead zone at the end of the keystroke. If the value is smaller than the
  // actual bottom-out threshold, the distance calculation may be inaccurate
  // until the first bottom-out event.
  uint16_t initial_bottom_out_threshold;
} eeconfig_calibration_t;

// Keyboard options configuration
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    // Whether the XInput interface is enabled
    bool xinput_enabled : 1;
    bool _unused0 : 1;
    // Whether 8kHz polling rate is enabled. Only applicable if USB HS is
    // enabled. If disabled, the 1kHz polling rate is used instead.
    bool high_polling_rate_enabled : 1;
    // Reserved bits for future use
    uint16_t reserved : 13;
  };
  uint16_t raw;
} eeconfig_options_t;

_Static_assert(sizeof(eeconfig_options_t) == sizeof(uint16_t),
               "Invalid eeconfig_options_t size");

// `scroll_layer` value that disables scroll mode
#define POINTING_SCROLL_LAYER_OFF 0xFF

// `snap_axis` values
#define POINTING_SNAP_AXIS_OFF 0
#define POINTING_SNAP_AXIS_X 1
#define POINTING_SNAP_AXIS_Y 2

// Pointing device runtime configuration v3 (device-common, not per-profile).
// The field order matches the GET/SET_POINTING_CONFIG v3 wire payload exactly
// (10 bytes, little-endian, packed) so the command layer can copy the struct
// verbatim after the `supported`/`side` response header. Orientation
// (rotation/invert/swap) lives in pointing_side_config_t instead.
typedef struct __attribute__((packed)) {
  bool enabled;
  bool auto_mouse_layer_enabled;
  bool invert_scroll;
  // Layer that turns pointer motion into scroll input (0xFF = off)
  uint8_t scroll_layer;
  // Raw sensor counts per wheel tick (1-255, default 32)
  uint8_t scroll_divisor;
  // Axis snapping: POINTING_SNAP_AXIS_*
  uint8_t snap_axis;
  // Snap threshold in percent of the dominant axis (0-100)
  uint8_t snap_threshold;
  uint8_t auto_mouse_layer;
  uint16_t cpi;
} pointing_config_t;

_Static_assert(sizeof(pointing_config_t) == 10,
               "pointing_config_t must match the 10-byte v3 wire format");

// Per-side physical-axis compensation. Each half persists both slots locally;
// index 0 is the left half, index 1 is the right half. The owning half
// applies its slot to its own sensor output before the split link / HID.
typedef struct __attribute__((packed)) {
  // Sensor rotation correction in degrees (0-359)
  uint16_t rotation_deg;
  bool invert_x;
  bool invert_y;
  bool swap_axes;
} pointing_side_config_t;

_Static_assert(sizeof(pointing_side_config_t) == 5,
               "pointing_side_config_t must be 5 bytes");

// Side identifiers used by GET/SET_SIDE_CONFIG and pointing_side[] indexing
// (side value = index + 1, matching GET_POINTING_CONFIG side coding).
#define POINTING_SIDE_LEFT 1
#define POINTING_SIDE_RIGHT 2
#define POINTING_NUM_SIDES 2
// Keyboard profile configuration
typedef struct __attribute__((packed)) {
  uint8_t keymap[NUM_LAYERS][NUM_KEYS];
  actuation_t actuation_map[NUM_KEYS];
  advanced_key_t advanced_keys[NUM_ADVANCED_KEYS];
  macro_node_t macros[NUM_MACRO_NODES];
  uint8_t gamepad_buttons[NUM_KEYS];
  gamepad_options_t gamepad_options;
  uint8_t tick_rate;
} eeconfig_profile_t;

// Persistent configuration version. The size of the configuration must be
// non-decreasing, so that the migration can assume that the new version is at
// least as large as the previous version.
#define EECONFIG_VERSION 0x010b

// Keyboard configuration
// Whenever there is a change in the configuration, `EECONFIG_VERSION` must be
// bumped. Make sure to update `eeconfig_reset()`, and add a migration function
// in `migration.c`.
typedef struct __attribute__((packed)) {
  // Global configurations
  // Magic number to identify the start of the configuration
  uint32_t magic_start;
  // Version of the configuration
  uint16_t version;

  // Calibration configuration
  eeconfig_calibration_t calibration;
  // Saved bottom-out threshold
  uint16_t bottom_out_threshold[NUM_KEYS];
  // Options configuration
  eeconfig_options_t options;

  // Current profile index
  uint8_t current_profile;
  // Last non-default profile index, used for profile swapping
  uint8_t last_non_default_profile;
  // Split keyboard handedness: 0 = left, 1 = right
  uint8_t split_handedness;
  // Pointing device global configuration (master EEPROM is authoritative)
  pointing_config_t pointing_config;
  // Per-side orientation, index 0 = left, 1 = right. Each half persists both
  // slots locally; the owning half applies its slot to its sensor output.
  pointing_side_config_t pointing_side[POINTING_NUM_SIDES];
  // End of global configurations

  // Profiles
  eeconfig_profile_t profiles[NUM_PROFILES];

  // Magic number to identify the end of the configuration
  uint32_t magic_end;
} eeconfig_t;

_Static_assert(
    sizeof(eeconfig_t) <= WL_VIRTUAL_SIZE,
    "Keyboard configuration size must be at most the virtual storage size.");

extern const eeconfig_t *eeconfig;

#define CURRENT_PROFILE (eeconfig->profiles[eeconfig->current_profile])

//--------------------------------------------------------------------+
// Default Keyboard Configuration
//--------------------------------------------------------------------+

#if !defined(DEFAULT_CALIBRATION)
#error "DEFAULT_CALIBRATION is not defined"
#endif

#if !defined(DEFAULT_OPTIONS)
// Default global options
#define DEFAULT_OPTIONS                                                        \
  {                                                                            \
      .xinput_enabled = false,                                                 \
      .high_polling_rate_enabled = true,                                       \
  }
#endif

#if !defined(DEFAULT_KEYMAPS)
#error "DEFAULT_KEYMAPS is not defined"
#endif

#if !defined(DEFAULT_ACTUATION_POINT)
// Default actuation point (2.00mm when TRAVEL_UNITS=400)
#define DEFAULT_ACTUATION_POINT 200
#endif

#if !defined(DEFAULT_GAMEPAD_OPTIONS)
// Default gamepad options
#define DEFAULT_GAMEPAD_OPTIONS                                                \
  {                                                                            \
      .analog_curve = {{4, 20}, {85, 95}, {165, 170}, {255, 255}},             \
      .keyboard_enabled = true,                                                \
      .snappy_joystick = true,                                                 \
  }
#endif

#if !defined(DEFAULT_TICK_RATE)
// Default tick rate
#define DEFAULT_TICK_RATE 30
#endif

#if !defined(DEFAULT_SPLIT_HANDEDNESS)
// Default split keyboard handedness (0 = left, 1 = right)
#define DEFAULT_SPLIT_HANDEDNESS 0
#endif

#if !defined(DEFAULT_POINTING_CPI)
#if defined(PMW3610_CPI)
#define DEFAULT_POINTING_CPI PMW3610_CPI
#else
#define DEFAULT_POINTING_CPI 800
#endif
#endif

#if !defined(DEFAULT_POINTING_AUTO_MOUSE_LAYER_ENABLED)
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
#define DEFAULT_POINTING_AUTO_MOUSE_LAYER_ENABLED true
#else
#define DEFAULT_POINTING_AUTO_MOUSE_LAYER_ENABLED false
#endif
#endif

#if !defined(DEFAULT_POINTING_AUTO_MOUSE_LAYER)
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
#define DEFAULT_POINTING_AUTO_MOUSE_LAYER POINTING_DEVICE_AUTO_MOUSE_LAYER
#else
#define DEFAULT_POINTING_AUTO_MOUSE_LAYER 0
#endif
#endif

#if !defined(DEFAULT_POINTING_INVERT_X)
// Per-side sensor orientation correction. make.py seeds these from the
// keyboard.json pointing_device angle/swap_xy/invert_x/invert_y composition;
// they are runtime-adjustable through SET_SIDE_CONFIG.
#define DEFAULT_POINTING_INVERT_X false
#endif

#if !defined(DEFAULT_POINTING_INVERT_Y)
#define DEFAULT_POINTING_INVERT_Y false
#endif

#if !defined(DEFAULT_POINTING_SWAP_AXES)
#define DEFAULT_POINTING_SWAP_AXES false
#endif

#if !defined(DEFAULT_POINTING_SCROLL_LAYER)
#if defined(POINTING_DEVICE_SCROLL_LAYER)
#define DEFAULT_POINTING_SCROLL_LAYER POINTING_DEVICE_SCROLL_LAYER
#else
#define DEFAULT_POINTING_SCROLL_LAYER POINTING_SCROLL_LAYER_OFF
#endif
#endif

#if !defined(DEFAULT_POINTING_SCROLL_DIVISOR)
#if defined(POINTING_DEVICE_SCROLL_DIVISOR)
#define DEFAULT_POINTING_SCROLL_DIVISOR POINTING_DEVICE_SCROLL_DIVISOR
#else
// Roughly one wheel tick per millimetre of ball travel at 800 CPI
#define DEFAULT_POINTING_SCROLL_DIVISOR 32
#endif
#endif

#if !defined(DEFAULT_POINTING_SNAP_THRESHOLD)
// Snap threshold in percent of the dominant axis
#define DEFAULT_POINTING_SNAP_THRESHOLD 50
#endif

#if !defined(DEFAULT_POINTING_ROTATION_DEG)
#define DEFAULT_POINTING_ROTATION_DEG 0
#endif

#if !defined(DEFAULT_ADVANCED_KEYS)
// Default advanced keys applied to every profile on reset. make.py expands
// the keyboard.json `advanced_keys` entries; remaining slots default to
// AK_TYPE_NONE.
#define DEFAULT_ADVANCED_KEYS                                                \
  {                                                                          \
      {0},                                                                   \
  }
#endif

#if !defined(DEFAULT_POINTING_CONFIG)
// Default pointing global configuration follows keyboard.json build-time
// values (v3, 10 bytes, no orientation fields).
#define DEFAULT_POINTING_CONFIG                                                \
  {                                                                            \
      .enabled = true,                                                         \
      .auto_mouse_layer_enabled = DEFAULT_POINTING_AUTO_MOUSE_LAYER_ENABLED,   \
      .invert_scroll = false,                                                  \
      .scroll_layer = DEFAULT_POINTING_SCROLL_LAYER,                           \
      .scroll_divisor = DEFAULT_POINTING_SCROLL_DIVISOR,                       \
      .snap_axis = POINTING_SNAP_AXIS_OFF,                                     \
      .snap_threshold = DEFAULT_POINTING_SNAP_THRESHOLD,                       \
      .auto_mouse_layer = DEFAULT_POINTING_AUTO_MOUSE_LAYER,                   \
      .cpi = DEFAULT_POINTING_CPI,                                             \
  }
#endif

#if !defined(DEFAULT_POINTING_SIDE_CONFIG)
// Default per-side orientation follows the keyboard.json angle composition.
#define DEFAULT_POINTING_SIDE_CONFIG                                           \
  {                                                                            \
      .rotation_deg = DEFAULT_POINTING_ROTATION_DEG,                           \
      .invert_x = DEFAULT_POINTING_INVERT_X,                                   \
      .invert_y = DEFAULT_POINTING_INVERT_Y,                                   \
      .swap_axes = DEFAULT_POINTING_SWAP_AXES,                                 \
  }
#endif

//--------------------------------------------------------------------+
// Persistent Configuration API
//--------------------------------------------------------------------+

/**
 * @brief Check whether a pointing global configuration holds supported values
 *
 * Validates every field of the v3 pointing global configuration: the boolean
 * flags must be 0 or 1, the CPI must be in the PMW3610-supported range
 * (200-3200) in 200 CPI steps, the scroll layer must be
 * POINTING_SCROLL_LAYER_OFF or an existing layer, the scroll divisor must be
 * non-zero, the snap axis must be a POINTING_SNAP_AXIS_* value with a
 * threshold of at most 100 percent, and the auto-mouse layer must exist.
 *
 * @param cfg Configuration to validate
 *
 * @return true if the configuration is valid, false otherwise
 */
bool pointing_config_is_valid(const pointing_config_t *cfg);

/**
 * @brief Check whether a per-side orientation configuration is valid
 *
 * The boolean flags must be 0 or 1 and the rotation must be below 360
 * degrees.
 *
 * @param cfg Configuration to validate
 *
 * @return true if the configuration is valid, false otherwise
 */
bool pointing_side_config_is_valid(const pointing_side_config_t *cfg);

/**
 * @brief Initialize the persistent configuration module
 *
 * @return None
 */
void eeconfig_init(void);

/**
 * @brief Reset the persistent configuration to default values
 *
 * @return true if successful, false otherwise
 */
bool eeconfig_reset(void);

/**
 * @brief Reset a specific profile to default values
 *
 * @param profile Profile index
 *
 * @return true if successful, false otherwise
 */
bool eeconfig_reset_profile(uint8_t profile);

/**
 * @brief Write a value to a field in the persistent configuration
 *
 * @param field Field to write to
 * @param value Value to write
 *
 * @return true if successful, false otherwise
 */
#define EECONFIG_WRITE(field, value)                                           \
  wear_leveling_write(offsetof(eeconfig_t, field), value,                      \
                      sizeof(((eeconfig_t *)0)->field))

/**
 * @brief Write a value to a field in the persistent configuration
 *
 * @param field Field to write to
 * @param value Value to write
 * @param len Length of the value in bytes
 *
 * @return true if successful, false otherwise
 */
#define EECONFIG_WRITE_N(field, value, len)                                    \
  wear_leveling_write(offsetof(eeconfig_t, field), value, len)
