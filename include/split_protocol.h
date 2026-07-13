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
// Split Protocol Constants
//--------------------------------------------------------------------+

// Frame sync byte
#define SPLIT_SYNC_BYTE 0xAB

// Maximum payload size for a single frame
#define SPLIT_MAX_PAYLOAD_SIZE 128

//--------------------------------------------------------------------+
// Split Frame Types
//--------------------------------------------------------------------+

typedef enum {
  // Master -> Slave: poll request (half-duplex)
  SPLIT_FRAME_POLL = 0x00,
  // Slave -> Master: key press bitmap and distance array
  SPLIT_FRAME_KEY_STATE = 0x01,
  // Slave -> Master: full analog state for hmkconf compatibility
  SPLIT_FRAME_ANALOG_STATE = 0x02,
  // Master -> Slave: layer state
  SPLIT_FRAME_LAYER_STATE = 0x03,
  // Master -> Slave: control command (e.g., recalibrate)
  SPLIT_FRAME_CONTROL = 0x04,
  // Slave -> Master: pointing device motion deltas
  SPLIT_FRAME_POINTING = 0x05,
} split_frame_type_t;

//--------------------------------------------------------------------+
// Split Poll Flags
//--------------------------------------------------------------------+

typedef enum {
  // Request the slave to send a full analog state frame after key state
  SPLIT_POLL_FLAG_REQUEST_ANALOG = 0x01,
  // Master will send a layer-state frame after the slave response
  SPLIT_POLL_FLAG_FOLLOWUP_LAYER = 0x02,
  // Master will send a control frame after the slave response
  SPLIT_POLL_FLAG_FOLLOWUP_CONTROL = 0x04,
} split_poll_flags_t;

//--------------------------------------------------------------------+
// Split Control Commands
//--------------------------------------------------------------------+

typedef enum {
  SPLIT_CONTROL_RECALIBRATE = 0x01,
} split_control_command_t;

//--------------------------------------------------------------------+
// Split Frame Header
//--------------------------------------------------------------------+

typedef struct __attribute__((packed)) {
  // Sync byte
  uint8_t sync;
  // Frame type
  uint8_t type;
  // Payload length
  uint8_t len;
} split_frame_header_t;

//--------------------------------------------------------------------+
// Split Payload Structures
//--------------------------------------------------------------------+

// Key/analog payloads are defined in src/split.c and are always sized to
// SPLIT_NUM_KEYS_LOCAL_MAX so asymmetric halves (e.g. 30 vs 39) keep a stable
// on-wire layout. The pressed bitmap uses 32-bit bitmap_t words:
//   bitmap words = ceil(SPLIT_NUM_KEYS_LOCAL_MAX / 32)
//   distance[SPLIT_NUM_KEYS_LOCAL_MAX]

typedef struct __attribute__((packed)) {
  uint8_t flags;
} split_poll_payload_t;

typedef struct __attribute__((packed)) {
  uint16_t layer_mask;
  uint8_t default_layer;
} split_layer_state_payload_t;

typedef struct __attribute__((packed)) {
  uint8_t command;
} split_control_payload_t;

typedef struct __attribute__((packed)) {
  int16_t dx;
  int16_t dy;
} split_pointing_payload_t;

//--------------------------------------------------------------------+
// Split Protocol API
//--------------------------------------------------------------------+

/**
 * @brief Calculate CRC8 checksum for a buffer
 *
 * @param data Data buffer
 * @param len Data length
 *
 * @return CRC8 checksum
 */
uint8_t split_protocol_crc8(const uint8_t *data, uint8_t len);

/**
 * @brief Encode a frame into a buffer
 *
 * @param type Frame type
 * @param payload Payload data
 * @param payload_len Payload length
 * @param out_buf Output buffer
 * @param out_len Output buffer size
 *
 * @return Encoded frame length, or 0 on error
 */
uint8_t split_protocol_encode_frame(split_frame_type_t type,
                                    const uint8_t *payload, uint8_t payload_len,
                                    uint8_t *out_buf, uint8_t out_len);
