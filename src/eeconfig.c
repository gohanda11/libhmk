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

#include "eeconfig.h"

#include "keycodes.h"
#include "migration.h"
#include "sensors/pmw3610.h"

const eeconfig_t *eeconfig;

// Default configuration values
static eeconfig_options_t default_options = DEFAULT_OPTIONS;
static eeconfig_calibration_t default_calibration = DEFAULT_CALIBRATION;
static pointing_config_t default_pointing_config = DEFAULT_POINTING_CONFIG;
static pointing_side_config_t default_pointing_side_config =
    DEFAULT_POINTING_SIDE_CONFIG;
static const uint8_t default_keymaps[NUM_PROFILES][NUM_LAYERS][NUM_KEYS] =
    DEFAULT_KEYMAPS;
static const macro_node_t default_macro = {
    .keycode = KC_NO,
    .action = MACRO_ACTION_NONE,
    .delay = 0,
    .next = MACRO_NODE_NONE,
};
static eeconfig_profile_t default_profile = {
    .advanced_keys = DEFAULT_ADVANCED_KEYS,
    .gamepad_options = DEFAULT_GAMEPAD_OPTIONS,
    .tick_rate = DEFAULT_TICK_RATE,
};

static bool eeconfig_write_default_profile(uint8_t profile) {
  if (profile >= NUM_PROFILES)
    return false;

  memcpy(default_profile.keymap, default_keymaps[profile],
         sizeof(default_profile.keymap));
  for (uint32_t i = 0; i < NUM_MACRO_NODES; i++)
    default_profile.macros[i] = default_macro;
  return EECONFIG_WRITE(profiles[profile], &default_profile);
}

static bool eeconfig_is_latest_version(void) {
  return eeconfig->magic_start == EECONFIG_MAGIC_START &&
         eeconfig->magic_end == EECONFIG_MAGIC_END &&
         eeconfig->version == EECONFIG_VERSION;
}

bool pointing_config_is_valid(const pointing_config_t *cfg) {
  if (cfg == NULL)
    return false;

  // The boolean flags are persisted as raw bytes, so a corrupted EEPROM value
  // may be neither 0 nor 1. Inspect the storage instead of the _Bool value.
  const uint8_t *raw = (const uint8_t *)cfg;
  if (raw[offsetof(pointing_config_t, enabled)] > 1 ||
      raw[offsetof(pointing_config_t, auto_mouse_layer_enabled)] > 1 ||
      raw[offsetof(pointing_config_t, invert_scroll)] > 1)
    return false;

  // CPI must be supported by the PMW3610 and quantized to 200 CPI steps.
  if (cfg->cpi < PMW3610_MIN_CPI || cfg->cpi > PMW3610_MAX_CPI ||
      (cfg->cpi % 200) != 0)
    return false;

  if (cfg->auto_mouse_layer >= NUM_LAYERS)
    return false;

  if (cfg->scroll_layer != POINTING_SCROLL_LAYER_OFF &&
      cfg->scroll_layer >= NUM_LAYERS)
    return false;

  // A zero scroll divisor would divide by zero in scroll mode.
  if (cfg->scroll_divisor == 0)
    return false;

  if (cfg->snap_axis > POINTING_SNAP_AXIS_Y || cfg->snap_threshold > 100)
    return false;

  return true;
}

bool pointing_side_config_is_valid(const pointing_side_config_t *cfg) {
  if (cfg == NULL)
    return false;

  const uint8_t *raw = (const uint8_t *)cfg;
  if (raw[offsetof(pointing_side_config_t, invert_x)] > 1 ||
      raw[offsetof(pointing_side_config_t, invert_y)] > 1 ||
      raw[offsetof(pointing_side_config_t, swap_axes)] > 1)
    return false;

  if (cfg->rotation_deg >= 360)
    return false;

  return true;
}

void eeconfig_init(void) {
  // Update default profile with its default values
  for (uint32_t i = 0; i < NUM_KEYS; i++)
    default_profile.actuation_map[i].actuation_point = DEFAULT_ACTUATION_POINT;

  eeconfig = (const eeconfig_t *)wl_cache;
  if (!wear_leveling_has_valid_consolidated() ||
      (!eeconfig_is_latest_version() && !migration_try_migrate()))
    eeconfig_reset();

  // Repair a corrupted pointing configuration so that every consumer observes
  // valid values. layout_init() reads it before pointing_device_init() runs,
  // so validation at the point of use alone is not enough. A fresh reset or
  // migration already wrote the defaults, making this a no-op in that case.
  if (!pointing_config_is_valid(&eeconfig->pointing_config))
    EECONFIG_WRITE(pointing_config, &default_pointing_config);
  for (uint8_t s = 0; s < POINTING_NUM_SIDES; s++) {
    if (!pointing_side_config_is_valid(&eeconfig->pointing_side[s]))
      EECONFIG_WRITE_N(pointing_side[s], &default_pointing_side_config,
                       sizeof(pointing_side_config_t));
  }
}

// Helper macro for writing rvalue
#define EECONFIG_WRITE_LOCAL(field, value)                                     \
  do {                                                                         \
    typeof(((eeconfig_t *)0)->field) _value = value;                           \
    status &= EECONFIG_WRITE(field, &_value);                                  \
  } while (0)

bool eeconfig_reset(void) {
  uint16_t bottom_out_threshold[NUM_KEYS] = {0};

  // We must not perform any action here that requires reading from
  // the configuration as it may be in an invalid state.
  bool status = true;
  EECONFIG_WRITE_LOCAL(magic_start, EECONFIG_MAGIC_START);
  EECONFIG_WRITE_LOCAL(version, EECONFIG_VERSION);
  status &= EECONFIG_WRITE(calibration, &default_calibration);
  status &= EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
  status &= EECONFIG_WRITE(options, &default_options);
  EECONFIG_WRITE_LOCAL(current_profile, 0);
  EECONFIG_WRITE_LOCAL(last_non_default_profile, M_MIN(1, NUM_PROFILES - 1));
  EECONFIG_WRITE_LOCAL(split_handedness, DEFAULT_SPLIT_HANDEDNESS);
  status &= EECONFIG_WRITE(pointing_config, &default_pointing_config);
  for (uint8_t s = 0; s < POINTING_NUM_SIDES; s++)
    status &= EECONFIG_WRITE_N(pointing_side[s], &default_pointing_side_config,
                               sizeof(pointing_side_config_t));
  for (uint32_t i = 0; i < NUM_PROFILES; i++)
    status &= eeconfig_write_default_profile(i);
  EECONFIG_WRITE_LOCAL(magic_end, EECONFIG_MAGIC_END);

  return status;
}

#undef EECONFIG_WRITE_LOCAL

bool eeconfig_reset_profile(uint8_t profile) {
  if (profile >= NUM_PROFILES)
    return false;

  return eeconfig_write_default_profile(profile);
}
