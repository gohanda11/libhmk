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

#include "hid.h"
#include "layout.h"
#include "sensors/pmw3610.h"

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static int16_t local_dx;
static int16_t local_dy;
static int16_t remote_dx;
static int16_t remote_dy;
static bool pmw3610_initialized;
static uint8_t init_attempts;

//--------------------------------------------------------------------+
// Helpers
//--------------------------------------------------------------------+

static bool pointing_device_init_sensor(void) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (pmw3610_init())
      return true;
  }
  return false;
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
  local_dx = 0;
  local_dy = 0;
  remote_dx = 0;
  remote_dy = 0;
  pmw3610_initialized = false;
  init_attempts = 0;
}

void pointing_device_task(void) {
  if (POINTING_DEVICE_ON_THIS_HALF) {
    if (!pmw3610_initialized && init_attempts < 3) {
      init_attempts++;
      pmw3610_initialized = pointing_device_init_sensor();
    }

    if (!pmw3610_initialized)
      return;

    int16_t dx = 0;
    int16_t dy = 0;

    if (pmw3610_read_motion(&dx, &dy)) {
      local_dx += dx;
      local_dy += dy;

#if defined(POINTING_DEVICE_AUTO_MOUSE_LAYER)
      layout_set_auto_mouse_layer(POINTING_DEVICE_AUTO_MOUSE_LAYER);
#endif
    }
  }

#if defined(SPLIT_KEYBOARD)
  if (split_is_master()) {
    const int16_t total_dx = local_dx + remote_dx;
    const int16_t total_dy = local_dy + remote_dy;
    local_dx = 0;
    local_dy = 0;
    remote_dx = 0;
    remote_dy = 0;
    pointing_device_send_hid(total_dx, total_dy);
  }
#else
  const int16_t total_dx = local_dx;
  const int16_t total_dy = local_dy;
  local_dx = 0;
  local_dy = 0;
  pointing_device_send_hid(total_dx, total_dy);
#endif
}

void pointing_device_get_local_delta(int16_t *dx, int16_t *dy) {
  *dx = local_dx;
  *dy = local_dy;
  local_dx = 0;
  local_dy = 0;
}

void pointing_device_restore_local_delta(int16_t dx, int16_t dy) {
  local_dx += dx;
  local_dy += dy;
}

void pointing_device_add_remote_delta(int16_t dx, int16_t dy) {
  remote_dx += dx;
  remote_dy += dy;
}

#endif // defined(POINTING_DEVICE_ENABLED)
