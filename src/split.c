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
#include "matrix.h"
#include "pointing_device.h"
#include "split_protocol.h"

//--------------------------------------------------------------------+
// Configuration Defaults
//--------------------------------------------------------------------+

#include "tusb.h"

#if !defined(SPLIT_CONNECTION_TIMEOUT_MS)
// Timeout for receiving a frame from the other half
#define SPLIT_CONNECTION_TIMEOUT_MS 8
#endif

#if !defined(SPLIT_POLL_INTERVAL_MS)
// Minimum interval between master polls. The AT32 USART has only a 1-byte
// receive FIFO, so polling every USB/matrix cycle overruns the slave while it
// is busy scanning and stalls the link. 2 ms gives a 30-key half enough time
// to finish scanning and a 39-key half enough margin before the next poll.
#define SPLIT_POLL_INTERVAL_MS 2
#endif

#if !defined(SPLIT_RECONNECT_POLL_INTERVAL_MS)
// Poll interval while the link is down. Much longer than the connected poll
// interval so a missing slave does not make the master block on a response
// timeout every matrix cycle. The slave loops on a short receive window, so
// it still catches these slower polls quickly.
#define SPLIT_RECONNECT_POLL_INTERVAL_MS 50
#endif

#if !defined(SPLIT_SLAVE_DISCONNECTED_TIMEOUT_MS)
// Poll wait timeout for a disconnected slave. Kept short so an unpaired half
// does not stall its main loop waiting for a master that is not polling; the
// fast loop still catches the master's reconnect polls.
#define SPLIT_SLAVE_DISCONNECTED_TIMEOUT_MS 2
#endif

#if !defined(SPLIT_MAX_CONNECTION_ERRORS)
// Number of consecutive communication errors before considering disconnected
#define SPLIT_MAX_CONNECTION_ERRORS 8
#endif

#if !defined(SPLIT_ANALOG_SYNC_INTERVAL_MS)
// Interval between full analog state synchronizations
#define SPLIT_ANALOG_SYNC_INTERVAL_MS 500
#endif

//--------------------------------------------------------------------+
// Local Types
//--------------------------------------------------------------------+

// Max keys per ANALOG_STATE chunk: offset(1) + 4B/key <= 128
#define SPLIT_ANALOG_STATE_KEYS_PER_FRAME 28
#define SPLIT_KEY_STATE_KEYS_PER_FRAME \
  ((SPLIT_MAX_PAYLOAD_SIZE - 1) / (uint8_t)sizeof(uint16_t))

typedef struct __attribute__((packed)) {
  uint8_t offset;
  uint16_t distance[SPLIT_KEY_STATE_KEYS_PER_FRAME];
} split_key_state_payload_t;

typedef struct __attribute__((packed)) {
  uint16_t adc_filtered;
  uint16_t distance;
} split_analog_key_t;

typedef struct __attribute__((packed)) {
  uint8_t offset;
  split_analog_key_t keys[SPLIT_ANALOG_STATE_KEYS_PER_FRAME];
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
static uint32_t last_poll_time;

static bool analog_state_valid;

static uint8_t pending_control_command;
#if defined(POINTING_DEVICE_ENABLED)
static bool pending_pointing_config;
static split_pointing_config_payload_t pending_pointing_config_payload;
static bool was_connected;
#endif
static bool slave_recalibrate_pending;
// After sending RECALIBRATE to the slave, pause polling while it blocks in
// matrix_recalibrate() so connection_errors do not accumulate and clear keys.
static uint32_t poll_pause_start;
static uint32_t poll_pause_ms;
static bool transport_initialized;

//--------------------------------------------------------------------+
// Master/Slave Promotion
//--------------------------------------------------------------------+

static void split_clear_remote_keys(void);

static bool split_usb_active(void) {
  return tud_mounted() || tud_connected();
}

static void split_promote_to_master(void) {
  is_master = true;

#if defined(SPLIT_HANDEDNESS_USB)
  // The half that enumerates over USB is always treated as the left half.
  split_set_handedness(true);
  analog_reconfigure_handedness(true);
  // The local key slots changed with the handedness switch (right -> left),
  // so the boot-time calibration no longer applies. Recalibrate the new
  // local keys; tud_task() inside keeps USB enumeration alive.
  matrix_recalibrate(false);
#endif

  split_transport_master_init();
  split_transport_clear();

  connected = false;
  connection_errors = 0;
  last_poll_time = 0;
  poll_pause_start = 0;
  poll_pause_ms = 0;
  analog_state_valid = false;
  pending_control_command = 0;
#if defined(POINTING_DEVICE_ENABLED)
  pending_pointing_config = false;
  was_connected = false;
#endif
  slave_recalibrate_pending = false;
  local_layer_mask = 0;
  local_default_layer = 0;
  layer_state_changed = false;
  last_analog_sync = timer_read();
  split_clear_remote_keys();
}

static void split_demote_to_slave(void) {
  is_master = false;

#if defined(SPLIT_HANDEDNESS_USB)
  // Without USB this half reverts to the right half.
  split_set_handedness(false);
  analog_reconfigure_handedness(false);
  matrix_recalibrate(false);
#endif

  split_transport_slave_init();
  split_transport_clear();

  connected = false;
  connection_errors = 0;
  slave_recalibrate_pending = false;
  // This half may still hold pressed state for the other half's keys from
  // when it was the master.
  split_clear_remote_keys();
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
  // Stored handedness convention: 0 = left, 1 = right (see eeconfig.h)
  return eeconfig->split_handedness == 0;
#elif defined(SPLIT_HANDEDNESS_USB)
  // In USB handedness mode the USB-connected half promotes to master and is
  // treated as the left half. Until USB is detected this half runs as a
  // slave, which is always the right half in this mode.
  return false;
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

// Protocol v2: KEY/ANALOG payloads are offset-chunked so uint16 distances fit
// in SPLIT_MAX_PAYLOAD_SIZE. KEY_STATE still fits a full asymmetric half in one
// frame (39 keys); ANALOG_STATE may require multiple frames (28 keys max).

_Static_assert(1 + SPLIT_KEY_STATE_KEYS_PER_FRAME * sizeof(uint16_t) <=
                   SPLIT_MAX_PAYLOAD_SIZE,
               "Key state chunk exceeds SPLIT_MAX_PAYLOAD_SIZE");
_Static_assert(1 + SPLIT_ANALOG_STATE_KEYS_PER_FRAME * sizeof(split_analog_key_t) <=
                   SPLIT_MAX_PAYLOAD_SIZE,
               "Analog state chunk exceeds SPLIT_MAX_PAYLOAD_SIZE");

static uint8_t split_key_state_payload_size(uint8_t key_count) {
  return (uint8_t)(1u + (uint32_t)key_count * sizeof(uint16_t));
}

static uint8_t split_analog_state_payload_size(uint8_t key_count) {
  return (uint8_t)(1u + (uint32_t)key_count * sizeof(split_analog_key_t));
}

static uint8_t
split_build_key_state_payload(split_key_state_payload_t *payload,
                              uint8_t offset) {
  memset(payload, 0, sizeof(*payload));
  if (offset >= num_local_keys)
    return 0;

  const uint8_t count = (uint8_t)M_MIN(
      (uint32_t)(num_local_keys - offset), (uint32_t)SPLIT_KEY_STATE_KEYS_PER_FRAME);
  payload->offset = offset;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t key =
        (uint8_t)((uint32_t)key_offset + (uint32_t)offset + i);
    payload->distance[i] = key_matrix[key].distance;
  }
  return count;
}

static void
split_apply_key_state_payload(const split_key_state_payload_t *payload,
                              uint8_t key_count) {
  if (payload->offset >= num_remote_keys)
    return;
  const uint8_t count = (uint8_t)M_MIN(
      (uint32_t)key_count, (uint32_t)(num_remote_keys - payload->offset));
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t key = (uint8_t)((uint32_t)remote_key_offset +
                                  (uint32_t)payload->offset + i);
    // Configuration writes only reach the USB-connected half, so the slave's
    // local actuation profile may be stale. Evaluate the received distance
    // against the master's authoritative profile.
    matrix_update_press_state(key, payload->distance[i]);
  }
}

static uint8_t
split_build_analog_state_payload(split_analog_state_payload_t *payload,
                                 uint8_t offset) {
  memset(payload, 0, sizeof(*payload));
  if (offset >= num_local_keys)
    return 0;

  const uint8_t count = (uint8_t)M_MIN(
      (uint32_t)(num_local_keys - offset),
      (uint32_t)SPLIT_ANALOG_STATE_KEYS_PER_FRAME);
  payload->offset = offset;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t key =
        (uint8_t)((uint32_t)key_offset + (uint32_t)offset + i);
    payload->keys[i].adc_filtered = key_matrix[key].adc_filtered;
    payload->keys[i].distance = key_matrix[key].distance;
  }
  return count;
}

static void
split_apply_analog_state_payload(const split_analog_state_payload_t *payload,
                                 uint8_t key_count) {
  if (payload->offset >= num_remote_keys)
    return;
  const uint8_t count = (uint8_t)M_MIN(
      (uint32_t)key_count, (uint32_t)(num_remote_keys - payload->offset));
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t key = (uint8_t)((uint32_t)remote_key_offset +
                                  (uint32_t)payload->offset + i);
    key_matrix[key].adc_filtered = payload->keys[i].adc_filtered;
    key_matrix[key].distance = payload->keys[i].distance;
  }
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
                                uint8_t *payload_len, uint32_t timeout_ms) {
  const uint32_t start = timer_read();
  uint8_t header[4];

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

  // Read the rest of the header (version, type, length)
  {
    uint32_t remaining = split_remaining_timeout(start, timeout_ms);
    if (remaining == 0)
      return false;
    if (!split_transport_receive(&header[1], 3, remaining))
      return false;
  }

  // Reject frames from incompatible firmware versions
  if (header[1] != SPLIT_PROTOCOL_VERSION)
    return false;

  *type = header[2];
  *payload_len = header[3];

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

  uint8_t frame_buf[SPLIT_MAX_PAYLOAD_SIZE + 4];
  frame_buf[0] = header[0];
  frame_buf[1] = header[1];
  frame_buf[2] = header[2];
  frame_buf[3] = header[3];
  if (*payload_len > 0)
    memcpy(&frame_buf[4], payload, *payload_len);

  if (split_protocol_crc8(frame_buf, 4 + *payload_len) != rx_crc)
    return false;

  return true;
}

static bool split_send_frame(split_frame_type_t type, const uint8_t *payload,
                             uint8_t payload_len) {
  uint8_t frame[SPLIT_MAX_PAYLOAD_SIZE + 5];
  uint8_t frame_len = split_protocol_encode_frame(type, payload, payload_len,
                                                  frame, sizeof(frame));
  if (frame_len == 0)
    return false;

  return split_transport_send(frame, frame_len);
}

//--------------------------------------------------------------------+
// Connection Management
//--------------------------------------------------------------------+

static void split_clear_remote_keys(void) {
  for (uint32_t i = 0; i < num_remote_keys; i++) {
    const uint8_t key = remote_key_offset + i;
    key_matrix[key].distance = 0;
    key_matrix[key].extremum = 0;
    key_matrix[key].key_dir = KEY_DIR_INACTIVE;
    key_matrix[key].is_pressed = false;
  }
}

#if defined(POINTING_DEVICE_ENABLED)
static void split_queue_pointing_config_from_eeconfig(void) {
  if (POINTING_DEVICE_ON_THIS_HALF)
    return;

  const pointing_config_t *cfg = pointing_device_get_config();
  pending_pointing_config_payload.enabled = cfg->enabled ? 1 : 0;
  pending_pointing_config_payload.auto_mouse_layer_enabled =
      cfg->auto_mouse_layer_enabled ? 1 : 0;
  pending_pointing_config_payload.cpi = cfg->cpi;
  pending_pointing_config_payload.auto_mouse_layer = cfg->auto_mouse_layer;
  pending_pointing_config = true;
}
#endif

static void split_update_connection(bool success) {
  if (success) {
    connection_errors = 0;
#if defined(POINTING_DEVICE_ENABLED)
    // Re-push pointing config whenever the slave link comes back so a late
    // slave boot or reconnect picks up the master's EEPROM settings.
    if (is_master && !was_connected)
      split_queue_pointing_config_from_eeconfig();
    was_connected = true;
#endif
    connected = true;
  } else {
    if (connection_errors < UINT8_MAX)
      connection_errors++;
    if (connection_errors >= SPLIT_MAX_CONNECTION_ERRORS) {
      if (connected && is_master)
        split_clear_remote_keys();
      connected = false;
#if defined(POINTING_DEVICE_ENABLED)
      was_connected = false;
#endif
    }
  }
}

//--------------------------------------------------------------------+
// Master Task
//--------------------------------------------------------------------+

static void split_master_task(void) {
  uint8_t type;
  uint8_t payload[SPLIT_MAX_PAYLOAD_SIZE];
  uint8_t payload_len;

  // Pause polling while the slave performs a blocking recalibration.
  if (poll_pause_ms != 0) {
    if (timer_elapsed(poll_pause_start) < poll_pause_ms)
      return;
    poll_pause_ms = 0;
  }

  // Rate-limit polls so the slave is not overrun while scanning. Measure the
  // interval from the end of the previous exchange so a slow round-trip cannot
  // collapse into back-to-back polls. While disconnected, back off to the
  // slower reconnect interval so the response wait does not stall every cycle.
  const uint32_t poll_interval =
      connected ? SPLIT_POLL_INTERVAL_MS : SPLIT_RECONNECT_POLL_INTERVAL_MS;
  if (timer_elapsed(last_poll_time) < poll_interval)
    return;

  // Clear any stale receive data, then poll the slave for its key state.
  split_transport_clear();

  const bool request_analog =
      timer_elapsed(last_analog_sync) >= SPLIT_ANALOG_SYNC_INTERVAL_MS;
  // Layer state is only needed on change. Periodic resync previously forced a
  // FOLLOWUP every 50ms and, combined with a mismatched wait count on the
  // slave, ate the next POLL under continuous typing.
  const bool send_layer = layer_state_changed;
  const bool send_control = pending_control_command != 0;
#if defined(POINTING_DEVICE_ENABLED)
  const bool send_pointing_config = pending_pointing_config;
#else
  const bool send_pointing_config = false;
#endif

  split_poll_payload_t poll_payload = {.flags = 0};
  if (request_analog)
    poll_payload.flags |= SPLIT_POLL_FLAG_REQUEST_ANALOG;
  if (send_layer)
    poll_payload.flags |= SPLIT_POLL_FLAG_FOLLOWUP_LAYER;
  if (send_control)
    poll_payload.flags |= SPLIT_POLL_FLAG_FOLLOWUP_CONTROL;
  if (send_pointing_config)
    poll_payload.flags |= SPLIT_POLL_FLAG_FOLLOWUP_POINTING_CONFIG;

  if (!split_send_frame(SPLIT_FRAME_POLL, (uint8_t *)&poll_payload,
                        sizeof(poll_payload))) {
    split_update_connection(false);
    // Still stamp the poll timer so a dead/unpowered slave cannot collapse
    // into back-to-back retries that starve matrix_scan().
    last_poll_time = timer_read();
    return;
  }

  const uint32_t response_timeout =
      request_analog ? (SPLIT_CONNECTION_TIMEOUT_MS + 5)
                     : SPLIT_CONNECTION_TIMEOUT_MS;

  // Buffer slave payloads and apply them only after the full response has been
  // received. Applying KEY_STATE for up to 39 keys is slow enough that the
  // AT32 USART's 1-byte RX FIFO overflows and drops a following POINTING frame.
  split_key_state_payload_t pending_key_state;
  uint8_t pending_key_count = 0;
  bool have_key_state = false;
#define SPLIT_ANALOG_MAX_CHUNKS                                                \
  ((SPLIT_NUM_KEYS_LOCAL_MAX + SPLIT_ANALOG_STATE_KEYS_PER_FRAME - 1) /        \
   SPLIT_ANALOG_STATE_KEYS_PER_FRAME)
  split_analog_state_payload_t pending_analog_chunks[SPLIT_ANALOG_MAX_CHUNKS];
  uint8_t pending_analog_counts[SPLIT_ANALOG_MAX_CHUNKS];
  uint8_t pending_analog_n = 0;
#if defined(POINTING_DEVICE_ENABLED)
  split_pointing_payload_t pending_pointing;
  bool have_pointing = false;
#endif

  bool got_key_state = split_receive_frame(&type, payload, &payload_len,
                                           response_timeout);

  if (got_key_state && type == SPLIT_FRAME_KEY_STATE && payload_len >= 1 &&
      (((uint32_t)payload_len - 1u) % sizeof(uint16_t)) == 0u) {
    pending_key_count =
        (uint8_t)(((uint32_t)payload_len - 1u) / sizeof(uint16_t));
    if (pending_key_count > 0 &&
        pending_key_count <= SPLIT_KEY_STATE_KEYS_PER_FRAME) {
      memcpy(&pending_key_state, payload, payload_len);
      have_key_state = true;
      split_update_connection(true);
    }
  }
  if (!have_key_state) {
    split_transport_clear();
    split_update_connection(false);
    // The poll already promised a follow-up; still send it so the slave does
    // not block in its receive window for the full timeout.
    goto followup;
  }

  // Receive chunked analog state when requested. Analog is best-effort and must
  // never discard bytes that may belong to a following POINTING frame.
  if (request_analog) {
    uint8_t analog_covered = 0;
    last_analog_sync = timer_read();
    while (analog_covered < num_remote_keys &&
           pending_analog_n < SPLIT_ANALOG_MAX_CHUNKS) {
      bool got_analog = split_receive_frame(&type, payload, &payload_len,
                                            response_timeout);
      if (!got_analog)
        break;
      if (type == SPLIT_FRAME_ANALOG_STATE && payload_len >= 1 &&
          (((uint32_t)payload_len - 1u) % sizeof(split_analog_key_t)) == 0u) {
        const uint8_t count = (uint8_t)(((uint32_t)payload_len - 1u) /
                                        sizeof(split_analog_key_t));
        if (count == 0 || count > SPLIT_ANALOG_STATE_KEYS_PER_FRAME)
          break;
        memcpy(&pending_analog_chunks[pending_analog_n], payload, payload_len);
        pending_analog_counts[pending_analog_n] = count;
        const uint8_t chunk_offset = pending_analog_chunks[pending_analog_n].offset;
        pending_analog_n++;
        analog_covered = (uint8_t)M_MAX(
            (uint32_t)analog_covered, (uint32_t)chunk_offset + count);
        continue;
      }
      if (type == SPLIT_FRAME_POINTING &&
          payload_len == sizeof(split_pointing_payload_t)) {
        // Analog was lost/skipped; still consume pointing so the exchange stays
        // aligned and we do not falsely mark the link unhealthy.
#if defined(POINTING_DEVICE_ENABLED)
        if (!POINTING_DEVICE_ON_THIS_HALF) {
          memcpy(&pending_pointing, payload, sizeof(pending_pointing));
          have_pointing = true;
        }
#endif
        goto apply_frames;
      }
      break;
    }
  }

  // Pointing is best-effort. A miss after a good KEY_STATE must not count as a
  // link failure — that regression made the remote half look completely dead.
#if defined(POINTING_DEVICE_ENABLED)
  if (!have_pointing && !POINTING_DEVICE_ON_THIS_HALF) {
    if (split_receive_frame(&type, payload, &payload_len,
                            SPLIT_CONNECTION_TIMEOUT_MS) &&
        type == SPLIT_FRAME_POINTING &&
        payload_len == sizeof(split_pointing_payload_t)) {
      memcpy(&pending_pointing, payload, sizeof(pending_pointing));
      have_pointing = true;
    }
  }
#endif

apply_frames:
  if (have_key_state)
    split_apply_key_state_payload(&pending_key_state, pending_key_count);
  for (uint8_t i = 0; i < pending_analog_n; i++)
    split_apply_analog_state_payload(&pending_analog_chunks[i],
                                     pending_analog_counts[i]);
#if defined(POINTING_DEVICE_ENABLED)
  if (have_pointing)
    pointing_device_add_remote_delta(pending_pointing.dx, pending_pointing.dy);
#endif

followup:
  // Send layer state / control commands only when advertised in the poll so
  // the slave does not sit in a receive window that can swallow the next POLL.
  // Only clear pending state when the frame is actually accepted by the UART.
  if (send_layer) {
    split_layer_state_payload_t layer_payload = {
        .layer_mask = local_layer_mask,
        .default_layer = local_default_layer,
    };
    if (split_send_frame(SPLIT_FRAME_LAYER_STATE, (uint8_t *)&layer_payload,
                         sizeof(layer_payload))) {
      layer_state_changed = false;
    }
  }

  if (send_control) {
    const uint8_t command = pending_control_command;
    split_control_payload_t control_payload = {.command = command};
    if (split_send_frame(SPLIT_FRAME_CONTROL, (uint8_t *)&control_payload,
                         sizeof(control_payload))) {
      pending_control_command = 0;
      if (command == SPLIT_CONTROL_RECALIBRATE) {
        // Slave will block in matrix_recalibrate() after this transaction.
        poll_pause_start = timer_read();
        poll_pause_ms = MATRIX_CALIBRATION_DURATION + 100;
      }
    }
  }

#if defined(POINTING_DEVICE_ENABLED)
  if (send_pointing_config) {
    if (split_send_frame(SPLIT_FRAME_POINTING_CONFIG,
                         (uint8_t *)&pending_pointing_config_payload,
                         sizeof(pending_pointing_config_payload))) {
      pending_pointing_config = false;
    }
  }
#endif

  last_poll_time = timer_read();
}

static void split_slave_task(void) {
  uint8_t type;
  uint8_t payload[SPLIT_MAX_PAYLOAD_SIZE];
  uint8_t payload_len;

  // Wait for a poll from the master. This also works for full-duplex, as the
  // master always sends a poll before expecting a response. While disconnected
  // use a short window so the slave does not stall its loop; the master polls
  // slowly in this state, but the fast slave loop still catches each poll.
  bool got_poll = split_receive_frame(
      &type, payload, &payload_len,
      connected ? SPLIT_CONNECTION_TIMEOUT_MS
                : SPLIT_SLAVE_DISCONNECTED_TIMEOUT_MS);
  if (!got_poll || type != SPLIT_FRAME_POLL) {
    // Recover from framing/overrun garbage so the next poll can be recognized.
    split_transport_clear();
    split_update_connection(false);
    return;
  }

  bool request_analog = false;
  uint8_t expected_followups = 0;
  if (payload_len == sizeof(split_poll_payload_t)) {
    const split_poll_payload_t *poll = (const split_poll_payload_t *)payload;
    request_analog = (poll->flags & SPLIT_POLL_FLAG_REQUEST_ANALOG) != 0;
    if (poll->flags & SPLIT_POLL_FLAG_FOLLOWUP_LAYER)
      expected_followups++;
    if (poll->flags & SPLIT_POLL_FLAG_FOLLOWUP_CONTROL)
      expected_followups++;
    if (poll->flags & SPLIT_POLL_FLAG_FOLLOWUP_POINTING_CONFIG)
      expected_followups++;
  }

  // Send key state to master (single chunk covers current halves)
  split_key_state_payload_t key_payload;
  const uint8_t key_count = split_build_key_state_payload(&key_payload, 0);
  if (key_count == 0 ||
      !split_send_frame(SPLIT_FRAME_KEY_STATE, (uint8_t *)&key_payload,
                        split_key_state_payload_size(key_count))) {
    split_update_connection(false);
    return;
  }
  split_update_connection(true);

  // Send chunked analog state when requested by the master
  if (request_analog) {
    // Analog is best-effort like on the master side: a failed send must not
    // count as a link error. Keep going so pointing/follow-ups stay aligned
    // with the master's plan.
    bool any_analog_sent = false;
    for (uint8_t offset = 0; offset < num_local_keys;) {
      split_analog_state_payload_t analog_payload;
      const uint8_t count =
          split_build_analog_state_payload(&analog_payload, offset);
      if (count == 0)
        break;
      if (split_send_frame(SPLIT_FRAME_ANALOG_STATE, (uint8_t *)&analog_payload,
                           split_analog_state_payload_size(count))) {
        any_analog_sent = true;
      }
      offset = (uint8_t)(offset + count);
    }
    if (any_analog_sent)
      last_analog_sync = timer_read();
  }

  // Send pointing device motion to the master half
#if defined(POINTING_DEVICE_ENABLED)
  if (POINTING_DEVICE_ON_THIS_HALF) {
    int16_t dx = 0;
    int16_t dy = 0;
    pointing_device_get_local_delta(&dx, &dy);
    split_pointing_payload_t pointing_payload = {
        .dx = dx,
        .dy = dy,
    };
    if (!split_send_frame(SPLIT_FRAME_POINTING, (uint8_t *)&pointing_payload,
                          sizeof(pointing_payload))) {
      // get_local_delta() already cleared the accumulators; put the motion
      // back so a later successful transfer can deliver it. Pointing is
      // best-effort like on the master side, so a failed send does not count
      // as a link error. Still fall through to the follow-up wait when the
      // poll promised layer/control frames.
      pointing_device_restore_local_delta(dx, dy);
    }
  }
#endif

  // Wait for exactly the number of follow-up frames advertised in the poll.
  // Waiting for more frames than the master sends lets the next POLL land in
  // this window, which desynchronizes the link under continuous typing.
  if (expected_followups > 0) {
    uint8_t remaining = expected_followups;
    const uint32_t receive_start = timer_read();
    while (remaining > 0 && timer_elapsed(receive_start) < 8) {
      if (!split_receive_frame(&type, payload, &payload_len, 3))
        break;

      switch (type) {
      case SPLIT_FRAME_LAYER_STATE:
        if (payload_len == sizeof(split_layer_state_payload_t)) {
          const split_layer_state_payload_t *layer_payload =
              (const split_layer_state_payload_t *)payload;
          local_layer_mask = layer_payload->layer_mask;
          local_default_layer = layer_payload->default_layer;
        }
        remaining--;
        break;

      case SPLIT_FRAME_CONTROL:
        if (payload_len == sizeof(split_control_payload_t)) {
          const split_control_payload_t *control_payload =
              (const split_control_payload_t *)payload;
          // Defer blocking recalibration until after this transaction so the
          // master does not see a 500ms silence and drop remote keys.
          if (control_payload->command == SPLIT_CONTROL_RECALIBRATE)
            slave_recalibrate_pending = true;
        }
        remaining--;
        break;

#if defined(POINTING_DEVICE_ENABLED)
      case SPLIT_FRAME_POINTING_CONFIG:
        if (payload_len == sizeof(split_pointing_config_payload_t)) {
          const split_pointing_config_payload_t *cfg_payload =
              (const split_pointing_config_payload_t *)payload;
          pointing_config_t cfg = {
              .enabled = cfg_payload->enabled != 0,
              .auto_mouse_layer_enabled =
                  cfg_payload->auto_mouse_layer_enabled != 0,
              .cpi = cfg_payload->cpi,
              .auto_mouse_layer = cfg_payload->auto_mouse_layer,
          };
          // Slave applies sensor settings only; EEPROM lives on the master.
          pointing_device_apply_local(&cfg);
        }
        remaining--;
        break;
#endif

      default:
        // Unexpected frame (e.g. next POLL). Stop waiting so we do not dig a
        // deeper desync hole; the next slave task can resynchronize.
        remaining = 0;
        break;
      }
    }
  }

  if (slave_recalibrate_pending) {
    slave_recalibrate_pending = false;
    // Preserve learned bottom-out thresholds; only retake rest values.
    matrix_recalibrate(false);
  }
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void split_pre_init(void) {
  // Detect handedness before analog initialization so that the ADC mapping can
  // use the correct global key offset for this half.
  split_set_handedness(split_detect_left());

  // Start as slave; promotion to master happens in split_task() once USB is
  // detected.
  is_master = false;
  connected = false;
  connection_errors = 0;
  layer_state_changed = false;
  local_layer_mask = 0;
  local_default_layer = 0;
  // Defer the first analog sync until after the link is up. Starting with
  // last_analog_sync=0 forces a large analog frame on the very first polls and
  // readily desynchronizes half-duplex before KEY_STATE traffic stabilizes.
  last_analog_sync = timer_read();
  last_poll_time = 0;
  analog_state_valid = false;
  pending_control_command = 0;
#if defined(POINTING_DEVICE_ENABLED)
  pending_pointing_config = false;
  was_connected = false;
#endif
  slave_recalibrate_pending = false;
  poll_pause_start = 0;
  poll_pause_ms = 0;
  transport_initialized = false;
}

void split_post_init(void) {
  // Start as slave so the UART is immediately ready to receive polls even
  // when USB has not enumerated yet. Promote to master as soon as USB activity
  // is observed in split_task().
  split_transport_slave_init();
  transport_initialized = true;

  // matrix_init() calibrates before the role is finalized. Recalibrate both
  // halves now that power has settled.
  matrix_recalibrate(false);

  // Start the link with compact KEY_STATE polls only. Analog can wait until
  // the half-duplex exchange has proven stable.
  last_analog_sync = timer_read();
  split_transport_clear();

  // If USB is already enumerated at startup, promote immediately.
  if (split_usb_active())
    split_promote_to_master();
}

void split_task(void) {
  if (is_master) {
    if (!split_usb_active()) {
      // USB was unplugged (or moved to the other half). Demote so both halves
      // cannot remain masters and collide on the shared UART line.
      split_demote_to_slave();
      return;
    }
    split_master_task();
    return;
  }

  if (split_usb_active()) {
    split_promote_to_master();
    return;
  }

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
    // Only mark dirty on an actual change. layout_task() calls this every scan;
    // treating every call as dirty forced a FOLLOWUP on every poll and made the
    // half-duplex exchange fragile.
    if (layer_mask != local_layer_mask || default_layer != local_default_layer) {
      local_layer_mask = layer_mask;
      local_default_layer = default_layer;
      layer_state_changed = true;
    }
  }
}

bool split_is_connected(void) { return connected; }

bool split_send_control_command(uint8_t command) {
  if (!is_master || !connected)
    return false;

  pending_control_command = command;
  return true;
}

bool split_send_pointing_config(uint8_t enabled,
                                uint8_t auto_mouse_layer_enabled,
                                uint16_t cpi,
                                uint8_t auto_mouse_layer) {
#if defined(POINTING_DEVICE_ENABLED)
  if (!is_master)
    return false;

  pending_pointing_config_payload.enabled = enabled ? 1 : 0;
  pending_pointing_config_payload.auto_mouse_layer_enabled =
      auto_mouse_layer_enabled ? 1 : 0;
  pending_pointing_config_payload.cpi = cpi;
  pending_pointing_config_payload.auto_mouse_layer = auto_mouse_layer;
  pending_pointing_config = true;
  return true;
#else
  (void)enabled;
  (void)auto_mouse_layer_enabled;
  (void)cpi;
  (void)auto_mouse_layer;
  return false;
#endif
}

void split_calibration_idle(void) {
  if (transport_initialized)
    split_transport_clear();
}

void split_flush(void) {
  // Run one master transaction immediately so a queued control/layer frame is
  // delivered before the caller blocks (e.g. COMMAND_RECALIBRATE).
  if (!is_master)
    return;
  last_poll_time = 0;
  poll_pause_ms = 0;
  split_master_task();
}

#endif // defined(SPLIT_KEYBOARD)
