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

#if !defined(SPLIT_MAX_CONNECTION_ERRORS)
// Number of consecutive communication errors before considering disconnected
#define SPLIT_MAX_CONNECTION_ERRORS 8
#endif

#if !defined(SPLIT_ANALOG_SYNC_INTERVAL_MS)
// Interval between full analog state synchronizations
#define SPLIT_ANALOG_SYNC_INTERVAL_MS 500
#endif

#if !defined(SPLIT_LAYER_SYNC_INTERVAL_MS)
// Interval between layer state synchronizations
#define SPLIT_LAYER_SYNC_INTERVAL_MS 50
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
static uint32_t last_poll_time;

static split_analog_state_payload_t cached_analog_state;
static bool analog_state_valid;

static uint8_t pending_control_command;
static bool slave_recalibrate_pending;
// After sending RECALIBRATE to the slave, pause polling while it blocks in
// matrix_recalibrate() so connection_errors do not accumulate and clear keys.
static uint32_t poll_pause_start;
static uint32_t poll_pause_ms;
static bool transport_initialized;

//--------------------------------------------------------------------+
// Master/Slave Promotion
//--------------------------------------------------------------------+

static bool split_usb_active(void) {
  return tud_mounted() || tud_connected();
}

static void split_promote_to_master(void) {
  is_master = true;

#if defined(SPLIT_HANDEDNESS_USB)
  // The half that enumerates over USB is always treated as the left half.
  split_set_handedness(true);
  analog_reconfigure_handedness(true);
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
  slave_recalibrate_pending = false;
  local_layer_mask = 0;
  local_default_layer = 0;
  layer_state_changed = false;
  last_analog_sync = timer_read();
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

// Payload sizes are always based on SPLIT_NUM_KEYS_LOCAL_MAX so the on-wire
// layout matches the packed structs below. Using the per-half key count here
// breaks distance/ADC alignment when left and right have different key counts
// (different bitmap word counts).
static uint8_t split_key_state_payload_size(void) {
  return (uint8_t)(M_DIV_CEIL((uint32_t)SPLIT_NUM_KEYS_LOCAL_MAX, 32) *
                       sizeof(bitmap_t) +
                   (uint32_t)SPLIT_NUM_KEYS_LOCAL_MAX);
}

static uint8_t split_analog_state_payload_size(void) {
  return (uint8_t)((uint32_t)SPLIT_NUM_KEYS_LOCAL_MAX *
                   (sizeof(uint16_t) + sizeof(uint8_t)));
}

_Static_assert(
    (M_DIV_CEIL(SPLIT_NUM_KEYS_LOCAL_MAX, 32) * sizeof(bitmap_t) +
     SPLIT_NUM_KEYS_LOCAL_MAX) <= SPLIT_MAX_PAYLOAD_SIZE,
    "Key state payload exceeds SPLIT_MAX_PAYLOAD_SIZE");
_Static_assert((SPLIT_NUM_KEYS_LOCAL_MAX * (sizeof(uint16_t) + sizeof(uint8_t))) <=
                   SPLIT_MAX_PAYLOAD_SIZE,
               "Analog state payload exceeds SPLIT_MAX_PAYLOAD_SIZE");

static void split_build_key_state_payload(split_key_state_payload_t *payload) {
  memset(payload, 0, sizeof(*payload));

  for (uint32_t i = 0; i < num_local_keys; i++) {
    const uint8_t key = key_offset + i;
    bitmap_set(payload->pressed_bitmap, i, key_matrix[key].is_pressed);
    payload->distance[i] = key_matrix[key].distance;
  }
}

static void
split_apply_key_state_payload(const split_key_state_payload_t *payload) {
  for (uint32_t i = 0; i < num_remote_keys; i++) {
    const uint8_t key = remote_key_offset + i;
    // Configuration writes only reach the USB-connected half, so the slave's
    // local actuation profile may be stale. Evaluate the received distance
    // against the master's authoritative profile instead of trusting the
    // slave's pressed bitmap.
    matrix_update_press_state(key, payload->distance[i]);
  }
}

static void
split_build_analog_state_payload(split_analog_state_payload_t *payload) {
  // Zero the full MAX-sized struct so unused asymmetric-half slots are not
  // sent as stack garbage (which also makes the CRC nondeterministic).
  memset(payload, 0, sizeof(*payload));

  for (uint32_t i = 0; i < num_local_keys; i++) {
    const uint8_t key = key_offset + i;
    payload->adc_filtered[i] = key_matrix[key].adc_filtered;
    payload->distance[i] = key_matrix[key].distance;
  }
}

static void
split_apply_analog_state_payload(const split_analog_state_payload_t *payload) {
  for (uint32_t i = 0; i < num_remote_keys; i++) {
    const uint8_t key = remote_key_offset + i;
    key_matrix[key].adc_filtered = payload->adc_filtered[i];
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
                                uint8_t *payload_len, uint32_t timeout_ms) {
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

static void split_update_connection(bool success) {
  if (success) {
    connection_errors = 0;
    connected = true;
  } else {
    if (connection_errors < UINT8_MAX)
      connection_errors++;
    if (connection_errors >= SPLIT_MAX_CONNECTION_ERRORS) {
      if (connected && is_master)
        split_clear_remote_keys();
      connected = false;
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
  // collapse into back-to-back polls.
  if (timer_elapsed(last_poll_time) < SPLIT_POLL_INTERVAL_MS)
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

  split_poll_payload_t poll_payload = {.flags = 0};
  if (request_analog)
    poll_payload.flags |= SPLIT_POLL_FLAG_REQUEST_ANALOG;
  if (send_layer)
    poll_payload.flags |= SPLIT_POLL_FLAG_FOLLOWUP_LAYER;
  if (send_control)
    poll_payload.flags |= SPLIT_POLL_FLAG_FOLLOWUP_CONTROL;

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
  bool have_key_state = false;
  split_analog_state_payload_t pending_analog_state;
  bool have_analog_state = false;
#if defined(POINTING_DEVICE_ENABLED)
  split_pointing_payload_t pending_pointing;
  bool have_pointing = false;
#endif

  bool got_key_state = split_receive_frame(&type, payload, &payload_len,
                                           response_timeout);

  if (got_key_state && type == SPLIT_FRAME_KEY_STATE &&
      payload_len == split_key_state_payload_size()) {
    memcpy(&pending_key_state, payload, sizeof(pending_key_state));
    have_key_state = true;
    split_update_connection(true);
  } else {
    split_transport_clear();
    split_update_connection(false);
    // The poll already promised a follow-up; still send it so the slave does
    // not block in its receive window for the full timeout.
    goto followup;
  }

  // Receive full analog state when requested. Analog is best-effort and must
  // never discard bytes that may belong to a following POINTING frame.
  if (request_analog) {
    bool got_analog = split_receive_frame(&type, payload, &payload_len,
                                          response_timeout);
    // Always advance the analog timer so a failed transfer cannot force analog
    // requests on every subsequent poll and flood the half-duplex link.
    last_analog_sync = timer_read();
    if (got_analog && type == SPLIT_FRAME_ANALOG_STATE &&
        payload_len == split_analog_state_payload_size()) {
      memcpy(&pending_analog_state, payload, sizeof(pending_analog_state));
      have_analog_state = true;
    } else if (got_analog && type == SPLIT_FRAME_POINTING &&
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
  }

  // Pointing is best-effort. A miss after a good KEY_STATE must not count as a
  // link failure — that regression made the remote half look completely dead.
#if defined(POINTING_DEVICE_ENABLED)
  if (!POINTING_DEVICE_ON_THIS_HALF) {
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
    split_apply_key_state_payload(&pending_key_state);
  if (have_analog_state)
    split_apply_analog_state_payload(&pending_analog_state);
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
      last_layer_sync = timer_read();
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

  last_poll_time = timer_read();
}

static void split_slave_task(void) {
  uint8_t type;
  uint8_t payload[SPLIT_MAX_PAYLOAD_SIZE];
  uint8_t payload_len;

  // Wait for a poll from the master. This also works for full-duplex, as the
  // master always sends a poll before expecting a response.
  bool got_poll = split_receive_frame(&type, payload, &payload_len,
                                      SPLIT_CONNECTION_TIMEOUT_MS);
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
  }

  // Send key state to master
  split_key_state_payload_t key_payload;
  split_build_key_state_payload(&key_payload);
  if (!split_send_frame(SPLIT_FRAME_KEY_STATE, (uint8_t *)&key_payload,
                        split_key_state_payload_size())) {
    split_update_connection(false);
    return;
  }
  split_update_connection(true);

  // Send full analog state when requested by the master
  if (request_analog) {
    split_analog_state_payload_t analog_payload;
    split_build_analog_state_payload(&analog_payload);
    if (!split_send_frame(SPLIT_FRAME_ANALOG_STATE, (uint8_t *)&analog_payload,
                          split_analog_state_payload_size())) {
      // Keep going so pointing/follow-ups stay aligned with the master's plan.
      split_update_connection(false);
    } else {
      last_analog_sync = timer_read();
    }
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
      // back so a later successful transfer can deliver it. Still fall through
      // to the follow-up wait when the poll promised layer/control frames.
      pointing_device_restore_local_delta(dx, dy);
      split_update_connection(false);
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
  last_layer_sync = 0;
  last_poll_time = 0;
  analog_state_valid = false;
  pending_control_command = 0;
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
  if (!is_master && split_usb_active()) {
    split_promote_to_master();
    return;
  }

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
  if (!is_master)
    return false;

  pending_control_command = command;
  return true;
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
