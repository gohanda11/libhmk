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

#include "advanced_keys.h"
#include "commands.h"
#include "crc32.h"
#include "deferred_actions.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "hid.h"
#include "layout.h"
#include "matrix.h"
#include "pointing_device.h"
#include "split.h"
#include "tusb.h"
#include "wear_leveling.h"
#include "xinput.h"

int main(void) {
  // Initialize the hardware
  board_init();
  timer_init();
  crc32_init();
  flash_init();

  // Initialize the persistent configuration
  wear_leveling_init();
  eeconfig_init();

#if defined(SPLIT_KEYBOARD)
  // Detect handedness before analog_init() so the ADC mapping can use the
  // correct global key offset for this half.
  split_pre_init();
#endif

  // Initialize the core modules
  analog_init();
  matrix_init();
  hid_init();
  deferred_action_init();
  advanced_key_init();
  xinput_init();
  layout_init();
  command_init();

  tud_init(BOARD_TUD_RHPORT);

#if defined(SPLIT_KEYBOARD)
  split_post_init();
#endif

#if defined(POINTING_DEVICE_ENABLED)
  pointing_device_init();
#endif

  while (1) {
    tud_task();

    command_task();
    analog_task();
    matrix_scan();
#if defined(SPLIT_KEYBOARD)
    split_task();
#endif
#if defined(POINTING_DEVICE_ENABLED)
    pointing_device_task();
#endif
#if defined(SPLIT_KEYBOARD)
    // The slave half does not resolve keycodes or send HID reports; all of
    // that is handled on the master. Skipping layout_task() also prevents
    // slave-side effects such as SP_BOOT bootloader resets and PROFILE_*
    // EEPROM writes.
    if (split_is_master())
      layout_task();
#else
    layout_task();
#endif
    xinput_task();
  }

  return 0;
}
