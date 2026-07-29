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

// Protocol version. Frames with a mismatched version are rejected so halves
// running incompatible firmware fail fast instead of misreading payloads.
#define SPLIT_PROTOCOL_VERSION 2

// Maximum payload size for a single frame
#define SPLIT_MAX_PAYLOAD_SIZE 128

//--------------------------------------------------------------------+
// Split Frame Types
//--------------------------------------------------------------------+

typedef enum {
  // Master -> Slave: poll request (half-duplex)
  SPLIT_FRAME_POLL = 0x00,
  // Slave -> Master: key distance array (uint16, chunked with offset)
  SPLIT_FRAME_KEY_STATE = 0x01,
  // Slave -> Master: full analog state for hmkconf compatibility
  // (adc u16 + distance u16 per key, chunked with offset; max 28 keys/frame)
  SPLIT_FRAME_ANALOG_STATE = 0x02,
  // Master -> Slave: layer state
  SPLIT_FRAME_LAYER_STATE = 0x03,
  // Master -> Slave: control command (e.g., recalibrate)
  SPLIT_FRAME_CONTROL = 0x04,
  // Slave -> Master: pointing device motion deltas
  SPLIT_FRAME_POINTING = 0x05,
  // Master -> Slave: pointing device runtime config relay
  SPLIT_FRAME_POINTING_CONFIG = 0x06,
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
  // Master will send a pointing-config frame after the slave response
  SPLIT_POLL_FLAG_FOLLOWUP_POINTING_CONFIG = 0x08,
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
  // Protocol version (SPLIT_PROTOCOL_VERSION)
  uint8_t version;
  // Frame type
  uint8_t type;
  // Payload length
  uint8_t len;
} split_frame_header_t;

//--------------------------------------------------------------------+
// Split Payload Structures
//--------------------------------------------------------------------+

// Key/analog payloads are defined in src/split.c.
// Protocol v2 uses offset-chunked frames:
//   KEY_STATE:   offset u8 + distance u16[]
//   ANALOG_STATE: offset u8 + {adc u16, distance u16}[]  (max 28 keys/frame)

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

typedef struct __attribute__((packed)) {
  uint8_t enabled;
  uint8_t auto_mouse_layer_enabled;
  uint16_t cpi;
} split_pointing_config_payload_t;

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
