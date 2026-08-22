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

#include "pointing_device.h"

#if defined(POINTING_DEVICE_ENABLED)

#include "eeconfig.h"
#include "hid.h"
#include "layout.h"
#include "sensors/pmw3610.h"
#include "split.h"

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static int16_t local_dx;
static int16_t local_dy;
static int16_t remote_dx;
static int16_t remote_dy;
static bool pmw3610_initialized;
static uint8_t init_attempts;
static pointing_config_t runtime_config;
static bool runtime_config_valid;

//--------------------------------------------------------------------+
// Helpers
//--------------------------------------------------------------------+

static uint16_t pointing_device_effective_cpi(uint16_t cpi) {
  if (cpi == 0)
    return DEFAULT_POINTING_CPI;
  return cpi;
}

// Load the persisted pointing configuration, falling back to the build-time
// defaults and repairing the EEPROM copy when the persisted value fails
// validation. Pointing configuration is persisted on the master half only.
static void pointing_device_load_config(void) {
  if (pointing_config_is_valid(&eeconfig->pointing_config)) {
    runtime_config = eeconfig->pointing_config;
  } else {
    runtime_config = (pointing_config_t)DEFAULT_POINTING_CONFIG;
    EECONFIG_WRITE(pointing_config, &runtime_config);
  }
  runtime_config.cpi = pointing_device_effective_cpi(runtime_config.cpi);
  runtime_config_valid = true;
}

static void pointing_device_clear_deltas(void) {
  local_dx = 0;
  local_dy = 0;
  remote_dx = 0;
  remote_dy = 0;
}

static bool pointing_device_init_sensor(void) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (pmw3610_init())
      return true;
  }
  return false;
}

static void pointing_device_apply_sensor_config(void) {
  if (!POINTING_DEVICE_ON_THIS_HALF || !pmw3610_initialized)
    return;

  pmw3610_set_enabled(runtime_config.enabled);
  if (runtime_config.enabled)
    pmw3610_set_cpi(pointing_device_effective_cpi(runtime_config.cpi));
}

static void pointing_device_apply_layout_config(void) {
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
  layout_set_auto_mouse_enabled(runtime_config.auto_mouse_layer_enabled);
#endif
}

static void pointing_device_send_hid(int16_t dx, int16_t dy) {
  while (dx != 0 || dy != 0) {
    const int16_t sx16 = dx > 127 ? 127 : (dx < -128 ? -128 : dx);
    const int16_t sy16 = dy > 127 ? 127 : (dy < -128 ? -128 : dy);
    const int8_t sx = (int8_t)sx16;
    const int8_t sy = (int8_t)sy16;

    hid_mouse_move(sx, sy);
    hid_send_mouse_report();

    dx -= sx;
    dy -= sy;
  }
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void pointing_device_init(void) {
  pointing_device_clear_deltas();
  pmw3610_initialized = false;
  init_attempts = 0;
#if defined(SPLIT_KEYBOARD)
  if (split_is_master()) {
    pointing_device_load_config();
  } else {
    runtime_config = (pointing_config_t)DEFAULT_POINTING_CONFIG;
    runtime_config.cpi = pointing_device_effective_cpi(runtime_config.cpi);
    runtime_config_valid = true;
  }
#else
  pointing_device_load_config();
#endif
  pointing_device_apply_layout_config();

#if defined(SPLIT_KEYBOARD)
  // Relay the persisted configuration to the slave half at boot so the slave
  // does not keep the build-time defaults when the sensor lives on the slave.
  if (split_is_master() && !POINTING_DEVICE_ON_THIS_HALF) {
    split_send_pointing_config(runtime_config.enabled,
                               runtime_config.auto_mouse_layer_enabled,
                               runtime_config.cpi,
                               runtime_config.auto_mouse_layer);
  }
#endif
}

void pointing_device_apply_local(const pointing_config_t *cfg) {
  if (cfg == NULL)
    return;

  if (pointing_config_is_valid(cfg)) {
    runtime_config = *cfg;
  } else {
    // Never feed an invalid configuration (e.g. a corrupt split relay payload)
    // to the sensor or layout; fall back to the build-time defaults.
    runtime_config = (pointing_config_t)DEFAULT_POINTING_CONFIG;
  }
  runtime_config.cpi = pointing_device_effective_cpi(runtime_config.cpi);
  runtime_config_valid = true;
  pointing_device_apply_layout_config();
  pointing_device_apply_sensor_config();

  if (!runtime_config.enabled)
    pointing_device_clear_deltas();
}

void pointing_device_set_config(const pointing_config_t *cfg) {
  pointing_device_apply_local(cfg);

#if defined(SPLIT_KEYBOARD)
  // Persist happens in the command handler. Relay runtime fields to the slave
  // when the sensor lives on the opposite half.
  if (!POINTING_DEVICE_ON_THIS_HALF) {
    split_send_pointing_config(runtime_config.enabled,
                               runtime_config.auto_mouse_layer_enabled,
                               runtime_config.cpi,
                               runtime_config.auto_mouse_layer);
  }
#endif
}

const pointing_config_t *pointing_device_get_config(void) {
  if (!runtime_config_valid)
    pointing_device_load_config();
  return &runtime_config;
}

void pointing_device_task(void) {
  if (POINTING_DEVICE_ON_THIS_HALF) {
    if (!pmw3610_initialized && init_attempts < 3) {
      init_attempts++;
      pmw3610_initialized = pointing_device_init_sensor();
      if (pmw3610_initialized)
        pointing_device_apply_sensor_config();
    }

    if (!pmw3610_initialized)
      return;

    if (!runtime_config.enabled) {
      pointing_device_clear_deltas();
      return;
    }

    int16_t dx = 0;
    int16_t dy = 0;

    if (pmw3610_read_motion(&dx, &dy)) {
      local_dx += dx;
      local_dy += dy;
    }
  }

  if (!runtime_config.enabled) {
    pointing_device_clear_deltas();
    return;
  }

#if defined(SPLIT_KEYBOARD)
  if (split_is_master()) {
    const int16_t total_dx = local_dx + remote_dx;
    const int16_t total_dy = local_dy + remote_dy;
    local_dx = 0;
    local_dy = 0;
    remote_dx = 0;
    remote_dy = 0;
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
    // Enable AML from the USB master's combined motion so slave-side sensor
    // movement reaches the half that owns layer state / HID.
    if (total_dx != 0 || total_dy != 0)
      layout_set_auto_mouse_layer(runtime_config.auto_mouse_layer);
#endif
    pointing_device_send_hid(total_dx, total_dy);
  }
#else
  const int16_t total_dx = local_dx;
  const int16_t total_dy = local_dy;
  local_dx = 0;
  local_dy = 0;
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
  if (total_dx != 0 || total_dy != 0)
    layout_set_auto_mouse_layer(runtime_config.auto_mouse_layer);
#endif
  pointing_device_send_hid(total_dx, total_dy);
#endif
}

void pointing_device_get_local_delta(int16_t *dx, int16_t *dy) {
  if (!runtime_config.enabled) {
    *dx = 0;
    *dy = 0;
    local_dx = 0;
    local_dy = 0;
    return;
  }

  *dx = local_dx;
  *dy = local_dy;
  local_dx = 0;
  local_dy = 0;
}

void pointing_device_restore_local_delta(int16_t dx, int16_t dy) {
  if (!runtime_config.enabled)
    return;
  local_dx += dx;
  local_dy += dy;
}

void pointing_device_add_remote_delta(int16_t dx, int16_t dy) {
  if (!runtime_config.enabled)
    return;
  remote_dx += dx;
  remote_dy += dy;
}

#endif // defined(POINTING_DEVICE_ENABLED)
