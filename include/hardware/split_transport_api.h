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
// Split Transport API
//--------------------------------------------------------------------+

#if defined(SPLIT_KEYBOARD)

/**
 * @brief Initialize the split transport as master
 *
 * @return None
 */
void split_transport_master_init(void);

/**
 * @brief Initialize the split transport as slave
 *
 * @return None
 */
void split_transport_slave_init(void);

/**
 * @brief Send data to the other half
 *
 * @param data Data buffer
 * @param len Data length
 *
 * @return true if successful, false otherwise
 */
bool split_transport_send(const uint8_t *data, uint8_t len);

/**
 * @brief Receive data from the other half
 *
 * @param data Buffer to store received data
 * @param len Expected data length
 * @param timeout_ms Timeout in milliseconds
 *
 * @return true if successful, false otherwise
 */
bool split_transport_receive(uint8_t *data, uint8_t len, uint32_t timeout_ms);

/**
 * @brief Clear any pending receive data
 *
 * @return None
 */
void split_transport_clear(void);

/**
 * @brief Check if the transport has data available to read
 *
 * @return true if data is available, false otherwise
 */
bool split_transport_available(void);

#endif // defined(SPLIT_KEYBOARD)
