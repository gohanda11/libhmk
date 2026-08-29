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

//--------------------------------------------------------------------+
// HID API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the HID module
 *
 * @return None
 */
void hid_init(void);

/**
 * @brief Add a keycode to the appropriate HID report
 *
 * @param keycode Keycode to add
 *
 * @return None
 */
void hid_keycode_add(uint8_t keycode);

/**
 * @brief Remove a keycode from the appropriate HID report
 *
 * @param keycode Keycode to remove
 *
 * @return None
 */
void hid_keycode_remove(uint8_t keycode);

/**
 * @brief Send all HID reports
 *
 * This function will block until the device is ready to send the reports.
 *
 * @return None
 */
void hid_send_reports(void);

/**
 * @brief Add a mouse movement delta to the mouse HID report
 *
 * @param x X-axis delta (int8_t range)
 * @param y Y-axis delta (int8_t range)
 *
 * @return None
 */
void hid_mouse_move(int8_t x, int8_t y);

/**
 * @brief Add a mouse wheel/pan delta to the mouse HID report
 *
 * @param wheel Vertical wheel delta (int8_t range), positive scrolls up
 * @param pan Horizontal wheel delta (int8_t range), positive scrolls right
 *
 * @return None
 */
void hid_mouse_scroll(int8_t wheel, int8_t pan);

/**
 * @brief Send only the mouse HID report
 *
 * This is used for relative pointing-device motion, which must not wait for a
 * keyboard state change before being emitted.
 *
 * @return None
 */
void hid_send_mouse_report(void);
