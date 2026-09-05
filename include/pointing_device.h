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
#include "eeconfig.h"
#include "split.h"

//--------------------------------------------------------------------+
// Pointing Device Configuration
//--------------------------------------------------------------------+

#if defined(POINTING_DEVICE_SIDE_LEFT) && defined(POINTING_DEVICE_SIDE_RIGHT)
// Dual-sensor build (keyboard.json pointing_device.side "both"): both halves
// carry a sensor on the same wiring, so every half senses and relays motion.
#define POINTING_DEVICE_DUAL_SENSOR 1
#endif

#if defined(SPLIT_KEYBOARD)
#if defined(POINTING_DEVICE_DUAL_SENSOR)
#define POINTING_DEVICE_ON_THIS_HALF (true)
#define POINTING_DEVICE_ON_REMOTE_HALF (true)
#elif defined(SPLIT_HANDEDNESS_USB)
// When handedness is determined by the USB connection, the master half is
// treated as the left half. Pointing device side then refers to the logical
// half: "left" means the USB-connected (master) half, "right" means the other
// half.
#if defined(POINTING_DEVICE_SIDE_LEFT)
#define POINTING_DEVICE_ON_THIS_HALF (split_is_master())
#else
#define POINTING_DEVICE_ON_THIS_HALF (!split_is_master())
#endif
#define POINTING_DEVICE_ON_REMOTE_HALF (!POINTING_DEVICE_ON_THIS_HALF)
#else
#if defined(POINTING_DEVICE_SIDE_LEFT)
#define POINTING_DEVICE_ON_THIS_HALF (split_is_left())
#else
#define POINTING_DEVICE_ON_THIS_HALF (!split_is_left())
#endif
#define POINTING_DEVICE_ON_REMOTE_HALF (!POINTING_DEVICE_ON_THIS_HALF)
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

#if defined(POINTING_DEVICE_ENABLED)
/**
 * @brief Apply pointing configuration locally (sensor + AML)
 *
 * Validates the configuration and falls back to the build-time defaults when
 * invalid. Never relays to the other half.
 *
 * @param cfg Configuration to apply
 *
 * @return None
 */
void pointing_device_apply_local(const pointing_config_t *cfg);

/**
 * @brief Apply pointing configuration and relay to the sensor half if needed
 *
 * Persists happen in the command handler. Relays runtime fields to the slave
 * when the sensor lives on the opposite half.
 *
 * @param cfg Configuration to apply
 *
 * @return None
 */
void pointing_device_set_config(const pointing_config_t *cfg);

/**
 * @brief Get the active runtime pointing configuration
 *
 * @return Pointer to the runtime configuration
 */
const pointing_config_t *pointing_device_get_config(void);
/**
 * @brief Reload the pointing configuration from EEPROM and reapply it
 *
 * Used when this half is promoted to master: pointing_device_init() may have
 * run before USB enumeration settled the role and left the build-time
 * defaults in place.
 *
 * @return None
 */
void pointing_device_reload_config(void);

/**
 * @brief Check whether a side slot has a sensor on this build
 *
 * Dual-sensor builds report both sides; single-sensor builds report only the
 * side the sensor is wired to, so GET_SIDE_CONFIG can return supported=0 for
 * the sensor-less side. On non-split builds this follows the
 * POINTING_DEVICE_SIDE_* build flag.
 *
 * @param side POINTING_SIDE_LEFT or POINTING_SIDE_RIGHT
 *
 * @return true when the side carries a sensor
 */
bool pointing_device_side_supported(uint8_t side);

/**
 * @brief Get this half's side identifier (POINTING_SIDE_LEFT/RIGHT)
 *
 * On split keyboards this is the physical side from split_is_left(). On
 * non-split builds it follows the POINTING_DEVICE_SIDE_* build flag.
 *
 * @return 1 for left, 2 for right
 */
uint8_t pointing_device_my_side(void);

/**
 * @brief Get the runtime side orientation for this half's sensor
 *
 * @return Pointer to the runtime side configuration
 */
const pointing_side_config_t *pointing_device_get_side_runtime(void);

/**
 * @brief Apply a side orientation locally without persisting or relaying
 *
 * @param cfg Side configuration to apply (must target this half's side)
 *
 * @return None
 */
void pointing_device_apply_side_local(const pointing_side_config_t *cfg);

/**
 * @brief Apply a side orientation and relay to the slave half
 *
 * Persistence happens in the caller (command handler / split RX). The master
 * relays every side update (own side included) so both halves' EEPROM side
 * tables converge; the pending slot for that side clears when the slave ACKs.
 *
 * @param side POINTING_SIDE_LEFT or POINTING_SIDE_RIGHT
 * @param cfg Side configuration to apply
 *
 * @return None
 */
void pointing_device_set_side_config(uint8_t side,
                                     const pointing_side_config_t *cfg);
#endif

