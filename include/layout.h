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
// Layout API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the layout module
 *
 * @return None
 */
void layout_init(void);

/**
 * @brief Load advanced keys
 *
 * This function loads the advanced keys from the current profile. It should be
 * called whenever the profile changes or the advanced keys are updated.
 *
 * @return None
 */
void layout_load_advanced_keys(void);

/**
 * @brief Layout task
 *
 * @return None
 */
void layout_task(void);

/**
 * @brief Manually register a key press
 *
 * @param key Key code
 * @param keycode Key code
 *
 * @return None
 */
void layout_register(uint8_t key, uint8_t keycode);

/**
 * @brief Manually register a key release
 *
 * @param key Key code
 * @param keycode Key code
 *
 * @return None
 */
void layout_unregister(uint8_t key, uint8_t keycode);

/**
 * @brief Get the current layer
 *
 * The current layer is the highest layer that is currently active. If no
 * layers are active, the default layer is returned.
 *
 * @return Current layer
 */
uint8_t layout_get_current_layer(void);

/**
 * @brief Temporarily activate a layer while the pointing device is in use
 *
 * The layer will remain active until AUTO_MOUSE_TIMEOUT_MS has elapsed without
 * a subsequent call to this function.
 *
 * @param layer Layer to activate
 *
 * @return None
 */
void layout_set_auto_mouse_layer(uint8_t layer);

/**
 * @brief Enable or disable the auto mouse layer feature
 *
 * When disabled while the auto mouse layer is active, the layer is deactivated
 * immediately. The persistent source of truth is eeconfig.pointing_config;
 * this updates the runtime gate used by layout_set_auto_mouse_layer().
 *
 * @param enabled true to allow automatic layer activation
 *
 * @return None
 */
void layout_set_auto_mouse_enabled(bool enabled);

/**
 * @brief Toggle the auto mouse layer feature on/off
 *
 * When toggled off while the auto mouse layer is active, the layer is
 * deactivated immediately. Persistence / split relay are handled by the
 * pointing-device configuration path when available.
 *
 * @return None
 */
void layout_toggle_auto_mouse(void);
