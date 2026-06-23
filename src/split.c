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

#include "split.h"

#if defined(SPLIT_KEYBOARD)

#include "eeconfig.h"
#include "hardware/hardware.h"
#include "hardware/split_transport_api.h"
#include "lib/bitmap.h"
#include "matrix.h"
#include "split_protocol.h"

//--------------------------------------------------------------------+
// Configuration Defaults
//--------------------------------------------------------------------+

#include "tusb.h"

#if !defined(SPLIT_CONNECTION_TIMEOUT_MS)
// Timeout for receiving a frame from the other half
#define SPLIT_CONNECTION_TIMEOUT_MS 5
#endif

#if !defined(SPLIT_MAX_CONNECTION_ERRORS)
// Number of consecutive communication errors before considering disconnected
#define SPLIT_MAX_CONNECTION_ERRORS 10
#endif

#if !defined(SPLIT_ANALOG_SYNC_INTERVAL_MS)
// Interval between full analog state synchronizations
#define SPLIT_ANALOG_SYNC_INTERVAL_MS 500
#endif

#if !defined(SPLIT_LAYER_SYNC_INTERVAL_MS)
// Interval between layer state synchronizations
#define SPLIT_LAYER_SYNC_INTERVAL_MS 50
#endif

#if !defined(SPLIT_USB_DETECT_TIMEOUT_MS)
// Timeout for detecting USB connection during master/slave determination
#define SPLIT_USB_DETECT_TIMEOUT_MS 2000
#endif

#if !defined(SPLIT_USB_DETECT_POLL_MS)
// Poll interval for USB connection detection
#define SPLIT_USB_DETECT_POLL_MS 10
#endif

//--------------------------------------------------------------------+
// Local Types
//--------------------------------------------------------------------+

typedef struct __attribute__((packed)) {
  bitmap_t pressed_bitmap[M_DIV_CEIL(SPLIT_NUM_KEYS_LOCAL_MAX, 32)];
  uint8_t distance[SPLIT_NUM_KEYS_LOCAL_MAX];
} split_key_state_payload_t;

typedef struct __attribute__((packed)) {
  uint16_t adc_filtered[SPLIT_NUM_KEYS_LOCAL_MAX];
  uint16_t adc_rest_value[SPLIT_NUM_KEYS_LOCAL_MAX];
  uint16_t adc_bottom_out_value[SPLIT_NUM_KEYS_LOCAL_MAX];
  uint8_t distance[SPLIT_NUM_KEYS_LOCAL_MAX];
} split_analog_state_payload_t;

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static bool is_master;
static bool is_left;
static uint8_t key_offset;
static uint8_t remote_key_offset;
static uint8_t num_local_keys;
static uint8_t num_remote_keys;
static bool connected;
static uint8_t connection_errors;

static uint16_t local_layer_mask;
static uint8_t local_default_layer;
static bool layer_state_changed;

static uint32_t last_analog_sync;
static uint32_t last_layer_sync;

static split_analog_state_payload_t cached_analog_state;
static bool analog_state_valid;

static uint8_t pending_control_command;

//--------------------------------------------------------------------+
// Master/Slave Detection
//--------------------------------------------------------------------+

static bool split_detect_master(void) {
  // Wait for USB connection to be established. The half that enumerates is the
  // master. We must run tud_task() so that TinyUSB can process bus events.
  const uint32_t start = timer_read();
  while (timer_elapsed(start) < SPLIT_USB_DETECT_TIMEOUT_MS) {
    tud_task();
    if (tud_mounted() || tud_connected())
      return true;
    timer_delay(SPLIT_USB_DETECT_POLL_MS);
  }
  return false;
}

//--------------------------------------------------------------------+
// Handedness Detection
//--------------------------------------------------------------------+

#if defined(SPLIT_HANDEDNESS_PIN)

static bool split_detect_handedness_pin(void) {
  gpio_set_input_pullup(SPLIT_HANDEDNESS_PIN_PORT, SPLIT_HANDEDNESS_PIN_PIN);
  // Allow the pull-up to settle before reading
  timer_delay(1);
#if defined(SPLIT_HANDEDNESS_PIN_LOW_IS_LEFT)
  return !gpio_read(SPLIT_HANDEDNESS_PIN_PORT, SPLIT_HANDEDNESS_PIN_PIN);
#else
  return gpio_read(SPLIT_HANDEDNESS_PIN_PORT, SPLIT_HANDEDNESS_PIN_PIN);
#endif
}

#endif // defined(SPLIT_HANDEDNESS_PIN)

static bool split_detect_left(void) {
#if defined(SPLIT_HANDEDNESS_LEFT)
  return true;
#elif defined(SPLIT_HANDEDNESS_RIGHT)
  return false;
#elif defined(SPLIT_HANDEDNESS_PIN)
  return split_detect_handedness_pin();
#elif defined(SPLIT_HANDEDNESS_EEPROM)
  return eeconfig->split_handedness != 0;
#elif defined(SPLIT_HANDEDNESS_USB)
  // USB-side handedness is resolved after USB enumeration in split_post_init().
  // split_pre_init() defaults to left in this mode.
  return true;
#else
  // Default to left if no handedness method is configured
  return true;
#endif
}

static void split_set_handedness(bool left) {
  is_left = left;
  key_offset = is_left ? SPLIT_KEY_OFFSET_LEFT : SPLIT_KEY_OFFSET_RIGHT;
  remote_key_offset = is_left ? SPLIT_KEY_OFFSET_RIGHT : SPLIT_KEY_OFFSET_LEFT;
  num_local_keys = is_left ? SPLIT_NUM_KEYS_LEFT : SPLIT_NUM_KEYS_RIGHT;
  num_remote_keys = is_left ? SPLIT_NUM_KEYS_RIGHT : SPLIT_NUM_KEYS_LEFT;
}

//--------------------------------------------------------------------+
// Payload Helpers
//--------------------------------------------------------------------+

static uint8_t split_key_state_payload_size(uint8_t n) {
  return (uint8_t)(M_DIV_CEIL((uint32_t)n, 32) * sizeof(bitmap_t) + (uint32_t)n);
}

static uint8_t split_analog_state_payload_size(uint8_t n) {
  return (uint8_t)((uint32_t)n * (3 * sizeof(uint16_t) + sizeof(uint8_t)));
}

static void split_build_key_state_payload(split_key_state_payload_t *payload) {
  memset(payload, 0, sizeof(*payload));

  for (uint32_t i = 0; i < num_local_keys; i++) {
    const uint8_t key = key_offset + i;
    bitmap_set(payload->pressed_bitmap, i, key_matrix[key].is_pressed);
    payload->distance[i] = key_matrix[key].distance;
  }
}

static void split_apply_key_state_payload(
    const split_key_state_payload_t *payload) {
  for (uint32_t i = 0; i < num_remote_keys; i++) {
    const uint8_t key = remote_key_offset + i;
    key_matrix[key].is_pressed = bitmap_get(payload->pressed_bitmap, i);
    key_matrix[key].distance = payload->distance[i];
  }
}

static void split_build_analog_state_payload(
    split_analog_state_payload_t *payload) {
  for (uint32_t i = 0; i < num_local_keys; i++) {
    const uint8_t key = key_offset + i;
    payload->adc_filtered[i] = key_matrix[key].adc_filtered;
    payload->adc_rest_value[i] = key_matrix[key].adc_rest_value;
    payload->adc_bottom_out_value[i] = key_matrix[key].adc_bottom_out_value;
    payload->distance[i] = key_matrix[key].distance;
  }
}

static void split_apply_analog_state_payload(
    const split_analog_state_payload_t *payload) {
  for (uint32_t i = 0; i < num_remote_keys; i++) {
    const uint8_t key = remote_key_offset + i;
    key_matrix[key].adc_filtered = payload->adc_filtered[i];
    key_matrix[key].adc_rest_value = payload->adc_rest_value[i];
    key_matrix[key].adc_bottom_out_value = payload->adc_bottom_out_value[i];
    key_matrix[key].distance = payload->distance[i];
  }
  memcpy(&cached_analog_state, payload, sizeof(cached_analog_state));
  analog_state_valid = true;
}

//--------------------------------------------------------------------+
// Frame Reception
//--------------------------------------------------------------------+

static uint32_t split_remaining_timeout(uint32_t start, uint32_t timeout_ms) {
  const uint32_t elapsed = timer_elapsed(start);
  return elapsed >= timeout_ms ? 0 : timeout_ms - elapsed;
}

static bool split_receive_frame(uint8_t *type, uint8_t *payload,
                                uint8_t *payload_len,
                                uint32_t timeout_ms) {
  const uint32_t start = timer_read();
  uint8_t header[3];

  // Resync: discard bytes until the sync byte is found or we time out
  while (true) {
    uint32_t remaining = split_remaining_timeout(start, timeout_ms);
    if (remaining == 0)
      return false;
    if (!split_transport_receive(&header[0], 1, remaining))
      return false;
    if (header[0] == SPLIT_SYNC_BYTE)
      break;
  }

  // Read the rest of the header
  {
    uint32_t remaining = split_remaining_timeout(start, timeout_ms);
    if (remaining == 0)
      return false;
    if (!split_transport_receive(&header[1], 2, remaining))
      return false;
  }

  *type = header[1];
  *payload_len = header[2];

  if (*payload_len > SPLIT_MAX_PAYLOAD_SIZE)
    return false;

  if (*payload_len > 0) {
    uint32_t remaining = split_remaining_timeout(start, timeout_ms);
    if (remaining == 0)
      return false;
    if (!split_transport_receive(payload, *payload_len, remaining))
      return false;
  }

  uint8_t rx_crc;
  {
    uint32_t remaining = split_remaining_timeout(start, timeout_ms);
    if (remaining == 0)
      return false;
    if (!split_transport_receive(&rx_crc, 1, remaining))
      return false;
  }

  uint8_t frame_buf[SPLIT_MAX_PAYLOAD_SIZE + 3];
  frame_buf[0] = header[0];
  frame_buf[1] = header[1];
  frame_buf[2] = header[2];
  if (*payload_len > 0)
    memcpy(&frame_buf[3], payload, *payload_len);

  if (split_protocol_crc8(frame_buf, 3 + *payload_len) != rx_crc)
    return false;

  return true;
}

static bool split_send_frame(split_frame_type_t type, const uint8_t *payload,
                             uint8_t payload_len) {
  uint8_t frame[SPLIT_MAX_PAYLOAD_SIZE + 4];
  uint8_t frame_len =
      split_protocol_encode_frame(type, payload, payload_len, frame,
                                  sizeof(frame));
  if (frame_len == 0)
    return false;

  return split_transport_send(frame, frame_len);
}

//--------------------------------------------------------------------+
// Connection Management
//--------------------------------------------------------------------+

static void split_update_connection(bool success) {
  if (success) {
    connection_errors = 0;
    connected = true;
  } else {
    if (connection_errors < UINT8_MAX)
      connection_errors++;
    if (connection_errors >= SPLIT_MAX_CONNECTION_ERRORS)
      connected = false;
  }
}

//--------------------------------------------------------------------+
// Master Task
//--------------------------------------------------------------------+

static void split_master_task(void) {
  uint8_t type;
  uint8_t payload[SPLIT_MAX_PAYLOAD_SIZE];
  uint8_t payload_len;

  // Clear any stale receive data, then poll the slave for its key state.
  split_transport_clear();

  // Request analog state periodically from the slave
  const bool request_analog =
      timer_elapsed(last_analog_sync) >= SPLIT_ANALOG_SYNC_INTERVAL_MS;

  split_poll_payload_t poll_payload = {
      .flags = request_analog ? SPLIT_POLL_FLAG_REQUEST_ANALOG : 0,
  };
  if (!split_send_frame(SPLIT_FRAME_POLL, (uint8_t *)&poll_payload,
                        sizeof(poll_payload))) {
    split_update_connection(false);
    return;
  }

  bool got_key_state =
      split_receive_frame(&type, payload, &payload_len,
                          SPLIT_CONNECTION_TIMEOUT_MS);

  if (got_key_state && type == SPLIT_FRAME_KEY_STATE &&
      payload_len == split_key_state_payload_size(num_remote_keys)) {
    split_apply_key_state_payload((const split_key_state_payload_t *)payload);
    split_update_connection(true);
  } else {
    split_update_connection(false);
    return;
  }

  // Receive full analog state when requested
  if (request_analog) {
    bool got_analog =
        split_receive_frame(&type, payload, &payload_len,
                            SPLIT_CONNECTION_TIMEOUT_MS);
    if (got_analog && type == SPLIT_FRAME_ANALOG_STATE &&
        payload_len == split_analog_state_payload_size(num_remote_keys)) {
      split_apply_analog_state_payload(
          (const split_analog_state_payload_t *)payload);
      last_analog_sync = timer_read();
    }
  }

  // Send layer state / control commands to slave after receiving its response
  if (layer_state_changed ||
      timer_elapsed(last_layer_sync) >= SPLIT_LAYER_SYNC_INTERVAL_MS) {
    split_layer_state_payload_t layer_payload = {
        .layer_mask = local_layer_mask,
        .default_layer = local_default_layer,
    };
    split_send_frame(SPLIT_FRAME_LAYER_STATE, (uint8_t *)&layer_payload,
                     sizeof(layer_payload));
    layer_state_changed = false;
    last_layer_sync = timer_read();
  }

  if (pending_control_command != 0) {
    split_control_payload_t control_payload = {.command =
                                                   pending_control_command};
    split_send_frame(SPLIT_FRAME_CONTROL, (uint8_t *)&control_payload,
                     sizeof(control_payload));
    pending_control_command = 0;
  }
}

//--------------------------------------------------------------------+
// Slave Task
//--------------------------------------------------------------------+

static void split_slave_task(void) {
  uint8_t type;
  uint8_t payload[SPLIT_MAX_PAYLOAD_SIZE];
  uint8_t payload_len;

  // Wait for a poll from the master. This also works for full-duplex, as the
  // master always sends a poll before expecting a response.
  bool got_poll = split_receive_frame(&type, payload, &payload_len,
                                      SPLIT_CONNECTION_TIMEOUT_MS);
  if (!got_poll || type != SPLIT_FRAME_POLL) {
    split_update_connection(false);
    return;
  }

  bool request_analog = false;
  if (payload_len == sizeof(split_poll_payload_t)) {
    const split_poll_payload_t *poll =
        (const split_poll_payload_t *)payload;
    request_analog = (poll->flags & SPLIT_POLL_FLAG_REQUEST_ANALOG) != 0;
  }

  // Send key state to master
  split_key_state_payload_t key_payload;
  split_build_key_state_payload(&key_payload);
  if (!split_send_frame(SPLIT_FRAME_KEY_STATE, (uint8_t *)&key_payload,
                        split_key_state_payload_size(num_local_keys))) {
    split_update_connection(false);
    return;
  }
  split_update_connection(true);

  // Send full analog state when requested by the master
  if (request_analog) {
    split_analog_state_payload_t analog_payload;
    split_build_analog_state_payload(&analog_payload);
    split_send_frame(SPLIT_FRAME_ANALOG_STATE, (uint8_t *)&analog_payload,
                     split_analog_state_payload_size(num_local_keys));
    last_analog_sync = timer_read();
  }

  // Receive any pending master frames (layer state, control commands)
  while (split_transport_available() &&
         split_receive_frame(&type, payload, &payload_len, 0)) {
    switch (type) {
    case SPLIT_FRAME_LAYER_STATE:
      if (payload_len == sizeof(split_layer_state_payload_t)) {
        const split_layer_state_payload_t *layer_payload =
            (const split_layer_state_payload_t *)payload;
        local_layer_mask = layer_payload->layer_mask;
        local_default_layer = layer_payload->default_layer;
      }
      break;

    case SPLIT_FRAME_CONTROL:
      if (payload_len == sizeof(split_control_payload_t)) {
        const split_control_payload_t *control_payload =
            (const split_control_payload_t *)payload;
        if (control_payload->command == SPLIT_CONTROL_RECALIBRATE)
          matrix_recalibrate(true);
      }
      break;

    default:
      break;
    }
  }
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void split_pre_init(void) {
  // Detect handedness before analog initialization so that the ADC mapping can
  // use the correct global key offset for this half.
  split_set_handedness(split_detect_left());

  // Master/slave will be determined in split_post_init() after USB init.
  is_master = false;
  connected = false;
  connection_errors = 0;
  layer_state_changed = false;
  local_layer_mask = 0;
  local_default_layer = 0;
  last_analog_sync = 0;
  last_layer_sync = 0;
  analog_state_valid = false;
  pending_control_command = 0;
}

void split_post_init(void) {
  is_master = split_detect_master();

#if defined(SPLIT_HANDEDNESS_USB)
  // The half that enumerates over USB is always treated as the left half.
  split_set_handedness(is_master);
  analog_reconfigure_handedness(is_master);
#endif

  if (is_master)
    split_transport_master_init();
  else
    split_transport_slave_init();
}

void split_task(void) {
  if (is_master)
    split_master_task();
  else
    split_slave_task();
}

bool split_is_master(void) { return is_master; }

bool split_is_left(void) { return is_left; }

uint8_t split_get_key_offset(void) { return key_offset; }

uint8_t split_get_remote_key_offset(void) { return remote_key_offset; }

uint8_t split_get_num_local_keys(void) { return num_local_keys; }

uint8_t split_get_num_remote_keys(void) { return num_remote_keys; }

void split_notify_layer_state(uint16_t layer_mask, uint8_t default_layer) {
  if (is_master) {
    local_layer_mask = layer_mask;
    local_default_layer = default_layer;
    layer_state_changed = true;
  }
}

bool split_is_connected(void) { return connected; }

bool split_send_control_command(uint8_t command) {
  if (!is_master)
    return false;

  pending_control_command = command;
  return true;
}

#endif // defined(SPLIT_KEYBOARD)
