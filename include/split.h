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
// Split Keyboard Configuration
//--------------------------------------------------------------------+

#if defined(SPLIT_KEYBOARD)

#if !defined(SPLIT_KEY_OFFSET_LEFT)
#error "SPLIT_KEY_OFFSET_LEFT is not defined"
#endif

#if !defined(SPLIT_KEY_OFFSET_RIGHT)
#error "SPLIT_KEY_OFFSET_RIGHT is not defined"
#endif

#if !defined(SPLIT_NUM_KEYS_LEFT)
#error "SPLIT_NUM_KEYS_LEFT is not defined"
#endif

#if !defined(SPLIT_NUM_KEYS_RIGHT)
#error "SPLIT_NUM_KEYS_RIGHT is not defined"
#endif

#if !defined(SPLIT_NUM_KEYS_LOCAL_MAX)
#error "SPLIT_NUM_KEYS_LOCAL_MAX is not defined"
#endif

#if !defined(SPLIT_UART_BAUD_RATE)
#error "SPLIT_UART_BAUD_RATE is not defined"
#endif

#if !defined(SPLIT_UART_INSTANCE)
#error "SPLIT_UART_INSTANCE is not defined"
#endif

#if (SPLIT_KEY_OFFSET_LEFT + SPLIT_NUM_KEYS_LEFT) > NUM_KEYS
#error "Left half local key range exceeds total number of keys"
#endif

#if (SPLIT_KEY_OFFSET_RIGHT + SPLIT_NUM_KEYS_RIGHT) > NUM_KEYS
#error "Right half local key range exceeds total number of keys"
#endif

//--------------------------------------------------------------------+
// Split Keyboard State
//--------------------------------------------------------------------+

/**
 * @brief Pre-initialize the split keyboard module
 *
 * This must be called after board_init() and timer_init() but before
 * analog_init(). It performs handedness detection and other setup that does not
 * require USB state.
 *
 * @return None
 */
void split_pre_init(void);

/**
 * @brief Post-initialize the split keyboard module
 *
 * This must be called after tud_init(). It initializes the transport layer as
 * a slave so the link is ready immediately, and promotes to master once USB
 * connection is observed.
 *
 * @return None
 */
void split_post_init(void);

/**
 * @brief Split keyboard task
 *
 * This should be called in the main loop. It promotes a slave to master when
 * USB is detected. On the master side, it receives key states from the slave.
 * On the slave side, it sends key states to the master.
 *
 * @return None
 */
void split_task(void);

/**
 * @brief Check if this half is the master (USB connected)
 *
 * @return true if this half is the master, false otherwise
 */
bool split_is_master(void);

/**
 * @brief Check if this half is the left half
 *
 * @return true if this half is the left half, false otherwise
 */
bool split_is_left(void);

/**
 * @brief Get the global key offset for this half
 *
 * @return Global key index offset for local key 0
 */
uint8_t split_get_key_offset(void);

/**
 * @brief Get the global key offset for the opposite half
 *
 * @return Global key index offset for the remote half's local key 0
 */
uint8_t split_get_remote_key_offset(void);

/**
 * @brief Get the number of keys on this half
 *
 * @return Number of keys on this half
 */
uint8_t split_get_num_local_keys(void);

/**
 * @brief Get the number of keys on the opposite half
 *
 * @return Number of keys on the remote half
 */
uint8_t split_get_num_remote_keys(void);

/**
 * @brief Notify the split module of the current layer state
 *
 * @param layer_mask Current layer mask
 * @param default_layer Default layer
 *
 * @return None
 */
void split_notify_layer_state(uint16_t layer_mask, uint8_t default_layer);

/**
 * @brief Check if the transport to the other half is connected
 *
 * @return true if connected, false otherwise
 */
bool split_is_connected(void);

/**
 * @brief Trigger a control command on the other half
 *
 * This is used by the master to request actions on the slave, such as
 * recalibration.
 *
 * @param command Control command
 *
 * @return true if successful, false otherwise
 */
bool split_send_control_command(uint8_t command);

/**
 * @brief Force one master split transaction
 *
 * Used to deliver a pending control/layer frame before the caller blocks.
 *
 * @return None
 */
void split_flush(void);

/**
 * @brief Idle hook for blocking calibration windows
 *
 * Drains split UART RX once the transport is initialized so post-init
 * recalibration cannot leave sticky overrun errors.
 */
void split_calibration_idle(void);

#else // defined(SPLIT_KEYBOARD)

static inline void split_calibration_idle(void) {}

static inline uint8_t split_get_key_offset(void) { return 0; }

static inline bool split_is_left(void) { return true; }

#endif // defined(SPLIT_KEYBOARD)
