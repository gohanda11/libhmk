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
#include "split.h"

//--------------------------------------------------------------------+
// Pointing Device Configuration
//--------------------------------------------------------------------+

#if defined(SPLIT_KEYBOARD)
#if defined(SPLIT_HANDEDNESS_USB)
// When handedness is determined by the USB connection, the master half is
// treated as the left half. Pointing device side then refers to the logical
// half: "left" means the USB-connected (master) half, "right" means the other
// half.
#if defined(POINTING_DEVICE_SIDE_LEFT)
#define POINTING_DEVICE_ON_THIS_HALF (split_is_master())
#else
#define POINTING_DEVICE_ON_THIS_HALF (!split_is_master())
#endif
#else
#if defined(POINTING_DEVICE_SIDE_LEFT)
#define POINTING_DEVICE_ON_THIS_HALF (split_is_left())
#else
#define POINTING_DEVICE_ON_THIS_HALF (!split_is_left())
#endif
#endif
#else
#define POINTING_DEVICE_ON_THIS_HALF (true)
#endif

//--------------------------------------------------------------------+
// Pointing Device API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the pointing device subsystem
 *
 * @return None
 */
void pointing_device_init(void);

/**
 * @brief Run the pointing device task
 *
 * This reads local sensor motion, accumulates remote motion (on the master
 * half), and sends HID mouse reports when appropriate.
 *
 * @return None
 */
void pointing_device_task(void);

/**
 * @brief Get accumulated motion on the local half and reset the accumulator
 *
 * This is used by the split slave half to send local motion to the master.
 *
 * @param dx Pointer to store the X delta
 * @param dy Pointer to store the Y delta
 *
 * @return None
 */
void pointing_device_get_local_delta(int16_t *dx, int16_t *dy);
void pointing_device_restore_local_delta(int16_t dx, int16_t dy);

/**
 * @brief Add motion received from the remote half
 *
 * This is used by the split master half to incorporate slave-side motion.
 *
 * @param dx X delta from the remote half
 * @param dy Y delta from the remote half
 *
 * @return None
 */
void pointing_device_add_remote_delta(int16_t dx, int16_t dy);
