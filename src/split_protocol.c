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

#include "split_protocol.h"

// CRC8 polynomial: x^8 + x^2 + x + 1 (CRC-8-CCITT)
#define CRC8_POLYNOMIAL 0x07

uint8_t split_protocol_crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0xFF;

  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80)
        crc = (crc << 1) ^ CRC8_POLYNOMIAL;
      else
        crc <<= 1;
    }
  }

  return crc;
}

uint8_t split_protocol_encode_frame(split_frame_type_t type,
                                    const uint8_t *payload, uint8_t payload_len,
                                    uint8_t *out_buf, uint8_t out_len) {
  if (out_len < payload_len + 4)
    return 0;

  out_buf[0] = SPLIT_SYNC_BYTE;
  out_buf[1] = (uint8_t)type;
  out_buf[2] = payload_len;

  if (payload_len > 0)
    memcpy(&out_buf[3], payload, payload_len);

  out_buf[3 + payload_len] =
      split_protocol_crc8(out_buf, 3 + payload_len);

  return 4 + payload_len;
}
