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
#include "hardware/timer_api.h"
#include "hid.h"
#include "layout.h"
#include "sensors/pmw3610.h"
#include "split.h"
// Scroll mode: while the layer selected by POINTING_DEVICE_SCROLL_LAYER is
// active, pointer motion is sent as wheel/pan ticks instead of cursor
// movement. -1 disables the feature (the default for keyboards that do not
// opt in via keyboard.json "scroll_layer").
#ifndef POINTING_DEVICE_SCROLL_LAYER
#define POINTING_DEVICE_SCROLL_LAYER -1
#endif

// Accumulated raw sensor counts per wheel tick. The PMW3610 at 800 CPI
// reports roughly 31.5 counts/mm, so the default divisor of 32 yields about
// one tick per millimetre of ball travel, close to a mouse wheel notch.
#ifndef POINTING_DEVICE_SCROLL_DIVISOR
#define POINTING_DEVICE_SCROLL_DIVISOR 32
#endif

// Minimum interval between PMW3610 init attempts. A failed attempt blocks for
// roughly 260 ms, so retrying on every scan loop would stall key scanning and
// the split link; the cooldown bounds the retry rate to about once per second
// until the sensor comes up (e.g. after the power rail settles on a
// tether-powered slave half).
#ifndef POINTING_DEVICE_INIT_RETRY_MS
#define POINTING_DEVICE_INIT_RETRY_MS 1000
#endif

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static int16_t local_dx;
static int16_t local_dy;
static int16_t remote_dx;
static int16_t remote_dy;
static bool pmw3610_initialized;
static uint32_t last_init_attempt;
static pointing_config_t runtime_config;
static bool runtime_config_valid;

//--------------------------------------------------------------------+
// Helpers
//--------------------------------------------------------------------+

#if POINTING_DEVICE_SCROLL_LAYER >= 0
// Keep the auto mouse layer off the scroll layer: if AML equals the scroll
// layer, ball motion pops the scroll layer and every cursor move is then
// swallowed as wheel/pan ticks, so the cursor can never move. The build-time
// default never collides (scripts/validate.py rejects the combination), so it
// is a safe fallback for values persisted or relayed by an older build.
// Returns true when the value was corrected.
static bool pointing_device_sanitize_auto_mouse_layer(pointing_config_t *cfg) {
  if (cfg->auto_mouse_layer != POINTING_DEVICE_SCROLL_LAYER)
    return false;
  cfg->auto_mouse_layer = DEFAULT_POINTING_AUTO_MOUSE_LAYER;
  return true;
}
#endif

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
#if POINTING_DEVICE_SCROLL_LAYER >= 0
  // A colliding value can also arrive at the current version (e.g. persisted
  // through the set-config command handler after the v1.9 migration ran), so
  // repair the EEPROM copy the same way the invalid branch above does.
  if (pointing_device_sanitize_auto_mouse_layer(&runtime_config))
    EECONFIG_WRITE(pointing_config, &runtime_config);
#endif
  runtime_config.cpi = pointing_device_effective_cpi(runtime_config.cpi);
  runtime_config_valid = true;
}

static void pointing_device_clear_deltas(void) {
  local_dx = 0;
  local_dy = 0;
  remote_dx = 0;
  remote_dy = 0;
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
#if POINTING_DEVICE_SCROLL_LAYER >= 0
// True while the scroll layer is active.
static bool pointing_device_in_scroll_mode(void) {
  return layout_get_current_layer() == POINTING_DEVICE_SCROLL_LAYER;
}

// Sub-tick motion carried over between task calls.
static int16_t scroll_acc_x;
static int16_t scroll_acc_y;

// Convert accumulated motion into wheel/pan ticks. The vertical axis is
// inverted so rolling the ball up (cursor up, dy < 0) scrolls up (positive
// wheel); the horizontal axis maps directly to pan (positive = right).
// Remainders below the divisor carry over so slow motion is not lost.
static void pointing_device_send_scroll(int16_t dx, int16_t dy) {
  scroll_acc_x += dx;
  scroll_acc_y += dy;

  const int16_t pan_ticks = scroll_acc_x / POINTING_DEVICE_SCROLL_DIVISOR;
  const int16_t wheel_ticks = -scroll_acc_y / POINTING_DEVICE_SCROLL_DIVISOR;
  scroll_acc_x %= POINTING_DEVICE_SCROLL_DIVISOR;
  scroll_acc_y %= POINTING_DEVICE_SCROLL_DIVISOR;

  int16_t pan = pan_ticks;
  int16_t wheel = wheel_ticks;
  while (pan != 0 || wheel != 0) {
    const int16_t p16 = pan > 127 ? 127 : (pan < -128 ? -128 : pan);
    const int16_t w16 = wheel > 127 ? 127 : (wheel < -128 ? -128 : wheel);

    hid_mouse_scroll((int8_t)w16, (int8_t)p16);
    hid_send_mouse_report();

    pan -= p16;
    wheel -= w16;
  }
}
#endif // POINTING_DEVICE_SCROLL_LAYER >= 0

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void pointing_device_init(void) {
  pointing_device_clear_deltas();
  pmw3610_initialized = false;
  // The subtraction wraps on purpose: the first task call may retry right
  // away instead of waiting out the full cooldown.
  last_init_attempt = timer_read() - POINTING_DEVICE_INIT_RETRY_MS;
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
#if POINTING_DEVICE_SCROLL_LAYER >= 0
  // Same defense for relayed / set-config values: never leave AML on the
  // scroll layer regardless of where the configuration came from.
  pointing_device_sanitize_auto_mouse_layer(&runtime_config);
#endif
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

// Re-read the persisted configuration and reapply it. Used when this half is
// promoted to master: pointing_device_init() may have run before USB
// enumeration settled the role and left the build-time defaults in place.
void pointing_device_reload_config(void) {
  pointing_device_load_config();
  pointing_device_apply_layout_config();
  pointing_device_apply_sensor_config();

  if (!runtime_config.enabled)
    pointing_device_clear_deltas();
}

void pointing_device_task(void) {
  if (POINTING_DEVICE_ON_THIS_HALF) {
    if (!pmw3610_initialized &&
        timer_elapsed(last_init_attempt) >= POINTING_DEVICE_INIT_RETRY_MS) {
      last_init_attempt = timer_read();
      pmw3610_initialized = pmw3610_init();
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
    // Scroll-mode decision and conversion run on the HID-owning half only.
    // The master holds the authoritative layer state (the slave's copy is not
    // synced into layout.c), so this is evaluated right where AML fires.
#if POINTING_DEVICE_SCROLL_LAYER >= 0
    const bool scroll_mode = pointing_device_in_scroll_mode();
#elif defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
    const bool scroll_mode = false;
#endif
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
    // Enable AML from the USB master's combined motion so slave-side sensor
    // movement reaches the half that owns layer state / HID. Skip it while
    // scrolling so scroll motion does not pop the auto mouse layer.
    if (!scroll_mode && (total_dx != 0 || total_dy != 0))
      layout_set_auto_mouse_layer(runtime_config.auto_mouse_layer);
#endif
#if POINTING_DEVICE_SCROLL_LAYER >= 0
    if (scroll_mode)
      pointing_device_send_scroll(total_dx, total_dy);
    else
#endif
      pointing_device_send_hid(total_dx, total_dy);
  }
#else
  const int16_t total_dx = local_dx;
  const int16_t total_dy = local_dy;
  local_dx = 0;
  local_dy = 0;
  // Non-split builds always own layer state locally.
#if POINTING_DEVICE_SCROLL_LAYER >= 0
  const bool scroll_mode = pointing_device_in_scroll_mode();
#elif defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
  const bool scroll_mode = false;
#endif
#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
  if (!scroll_mode && (total_dx != 0 || total_dy != 0))
    layout_set_auto_mouse_layer(runtime_config.auto_mouse_layer);
#endif
#if POINTING_DEVICE_SCROLL_LAYER >= 0
  if (scroll_mode)
    pointing_device_send_scroll(total_dx, total_dy);
  else
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
