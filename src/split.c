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
#if !defined(SPLIT_POINTING_CONFIG_MAX_RETRIES)
// Max polls an unacked pointing-config follow-up is retransmitted before the
// master gives up. Delivery stays pending-until-ACK while the slave reports,
// but an ACK-less peer can never cause a permanent retransmit storm.
#define SPLIT_POINTING_CONFIG_MAX_RETRIES 25
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
// Global sensor config relay slot. Stays set until the slave ACKs the apply
// (SPLIT_FRAME_POINTING_CONFIG_ACK), so a lost follow-up is retransmitted
// instead of silently dropped. Retransmits are bounded by
// SPLIT_POINTING_CONFIG_MAX_RETRIES so an ACK-less peer can never cause a
// permanent storm; the slot then simply clears (best-effort).
static bool pending_pointing_config;
static split_pointing_config_payload_t pending_pointing_config_payload;
static uint8_t pending_pointing_config_retries;
// Per-side orientation relay slots (index 0 = left, 1 = right). Separate slots
// so near-simultaneous SETs for both sides never overwrite each other. A slot
// stays set until the slave ACKs that side (SPLIT_FRAME_POINTING_SIDE_ACK),
// with the same bounded retransmit as the global slot.
static bool pending_pointing_side_config[POINTING_NUM_SIDES];
static split_pointing_side_config_payload_t
    pending_pointing_side_payload[POINTING_NUM_SIDES];
static uint8_t pending_pointing_side_retries[POINTING_NUM_SIDES];
static bool was_connected;
// Slave-side apply report for a newly applied global config. Sent on the next
// slave response, motion first.
static bool pending_config_ack;
// Slave-side apply report for a newly persisted side slot (0 = none, else the
// side id). Sent on the next slave response, motion first.
static uint8_t pending_side_ack;
// Last relay payload the slave applied. A retransmit carrying the identical
// value is a duplicate (the earlier ACK was lost) and must not re-queue an
// ACK, otherwise the two halves ping-pong forever.
static bool slave_applied_config_valid;
static split_pointing_config_payload_t slave_applied_config;
static bool slave_applied_side_valid[POINTING_NUM_SIDES];
static split_pointing_side_config_payload_t slave_applied_side[POINTING_NUM_SIDES];
// Deferred slave flash writes. wear_leveling_write stalls long enough to blow
// the in-transaction timing, so the RX path only stashes the value here and
// split_slave_flush_side_writes() persists it between transactions.
static bool slave_side_write_pending[POINTING_NUM_SIDES];
static pointing_side_config_t slave_side_write_cfg[POINTING_NUM_SIDES];
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
  pending_pointing_config_retries = 0;
  pending_pointing_side_config[0] = false;
  pending_pointing_side_config[1] = false;
  pending_pointing_side_retries[0] = 0;
  pending_pointing_side_retries[1] = 0;
  pending_config_ack = false;
  pending_side_ack = 0;
  slave_applied_config_valid = false;
  slave_applied_side_valid[0] = false;
  slave_applied_side_valid[1] = false;
  slave_side_write_pending[0] = false;
  slave_side_write_pending[1] = false;
  was_connected = false;
  // pointing_device_init() may have run before USB enumeration settled the
  // role, leaving the build-time defaults in place. The promoted half now
  // owns the persisted configuration, so reload it from EEPROM; the first
  // successful poll re-queues it to the slave half through the existing
  // split_queue_pointing_config_from_eeconfig() relay.
  pointing_device_reload_config();
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
// Record a slave side-config apply report. Clears the matching master pending
// slot; optionally flags that an ACK arrived during this poll.
static void split_handle_side_ack(const uint8_t *payload, uint8_t payload_len,
                                  bool *ack_received) {
  if (payload_len == sizeof(split_side_ack_payload_t)) {
    const uint8_t side = ((const split_side_ack_payload_t *)payload)->side;
    if (side == POINTING_SIDE_LEFT || side == POINTING_SIDE_RIGHT) {
      pending_pointing_side_config[side - 1u] = false;
      pending_pointing_side_retries[side - 1u] = 0;
      if (ack_received != NULL)
        *ack_received = true;
    }
  }
}

// Record a slave global-config apply report. Clears the master pending global
// slot; optionally flags that an ACK arrived during this poll.
static void split_handle_config_ack(const uint8_t *payload, uint8_t payload_len,
                                    bool *ack_received) {
  (void)payload;
  if (payload_len == 0) {
    pending_pointing_config = false;
    pending_pointing_config_retries = 0;
    if (ack_received != NULL)
      *ack_received = true;
  }
}

static void split_queue_pointing_config_from_eeconfig(void) {
  // Always push the global sensor fields on reconnect. A slave-side sensor
  // needs them; on a sensor-less slave the apply is a harmless runtime-only
  // update. Dual-sensor builds need them on the slave regardless.
  const pointing_config_t *cfg = pointing_device_get_config();
  pending_pointing_config_payload.enabled = cfg->enabled ? 1 : 0;
  pending_pointing_config_payload.auto_mouse_layer_enabled =
      cfg->auto_mouse_layer_enabled ? 1 : 0;
  pending_pointing_config_payload.cpi = cfg->cpi;
  pending_pointing_config_payload.auto_mouse_layer = cfg->auto_mouse_layer;
  pending_pointing_config = true;
  pending_pointing_config_retries = 0;
}

static void split_queue_pointing_side_config_from_eeconfig(void) {
  // Push both orientation slots so a late or reconnected slave converges to
  // the master's EEPROM table even if it missed runtime SETs while offline.
  for (uint8_t s = 0; s < POINTING_NUM_SIDES; s++) {
    if (!pointing_side_config_is_valid(&eeconfig->pointing_side[s]))
      continue;
    pending_pointing_side_payload[s].side = (uint8_t)(s + 1u);
    pending_pointing_side_payload[s].rotation_deg =
        eeconfig->pointing_side[s].rotation_deg;
    pending_pointing_side_payload[s].invert_x =
        eeconfig->pointing_side[s].invert_x ? 1 : 0;
    pending_pointing_side_payload[s].invert_y =
        eeconfig->pointing_side[s].invert_y ? 1 : 0;
    pending_pointing_side_payload[s].swap_axes =
        eeconfig->pointing_side[s].swap_axes ? 1 : 0;
    pending_pointing_side_config[s] = true;
    pending_pointing_side_retries[s] = 0;
  }
}
#endif

#if defined(POINTING_DEVICE_ENABLED)
// True when a relayed global payload carries nothing new. The slave records
// every applied payload, so a retransmit of the same value is recognized as
// a duplicate of an already-reported apply.
static bool split_slave_config_is_duplicate(
    const split_pointing_config_payload_t *incoming) {
  if (!slave_applied_config_valid)
    return false;
  return slave_applied_config.enabled == incoming->enabled &&
         slave_applied_config.auto_mouse_layer_enabled ==
             incoming->auto_mouse_layer_enabled &&
         slave_applied_config.cpi == incoming->cpi &&
         slave_applied_config.auto_mouse_layer == incoming->auto_mouse_layer;
}

// True when a relayed side payload carries nothing new for its slot.
static bool split_slave_side_is_duplicate(
    const split_pointing_side_config_payload_t *incoming) {
  const uint8_t idx = (uint8_t)(incoming->side - 1u);
  if (idx >= POINTING_NUM_SIDES || !slave_applied_side_valid[idx])
    return false;
  const split_pointing_side_config_payload_t *prev = &slave_applied_side[idx];
  return prev->side == incoming->side &&
         prev->rotation_deg == incoming->rotation_deg &&
         prev->invert_x == incoming->invert_x &&
         prev->invert_y == incoming->invert_y &&
         prev->swap_axes == incoming->swap_axes;
}

// Persist side configs stashed by the RX path. Runs between transactions so a
// slow flash write never eats into the follow-up timing. A failed write keeps
// its stash and is retried on a later call; each reception is written exactly
// once, so there is no double write against the command-layer SET path (which
// persists the master's own copy on the other half).
static void split_slave_flush_side_writes(void) {
  for (uint8_t s = 0; s < POINTING_NUM_SIDES; s++) {
    if (!slave_side_write_pending[s])
      continue;
    if (wear_leveling_write(offsetof(eeconfig_t, pointing_side) +
                                s * sizeof(pointing_side_config_t),
                            &slave_side_write_cfg[s],
                            sizeof(slave_side_write_cfg[s])))
      slave_side_write_pending[s] = false;
  }
}
#endif

static void split_update_connection(bool success) {
  if (success) {
    connection_errors = 0;
#if defined(POINTING_DEVICE_ENABLED)
    // Re-push pointing configs whenever the slave link comes back so a late
    // slave boot or reconnect picks up the master's EEPROM settings.
    if (is_master && !was_connected) {
      split_queue_pointing_config_from_eeconfig();
      split_queue_pointing_side_config_from_eeconfig();
    }
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
  // One pointing follow-up per poll (see the send section): the global frame
  // wins while pending so the two slots never double the follow-up traffic.
  const bool send_pointing_config = pending_pointing_config;
  const bool send_pointing_side_config =
      !send_pointing_config && (pending_pointing_side_config[0] ||
                                pending_pointing_side_config[1]);
#else
  const bool send_pointing_config = false;
  const bool send_pointing_side_config = false;
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
  if (send_pointing_side_config)
    poll_payload.flags |= SPLIT_POLL_FLAG_FOLLOWUP_POINTING_SIDE_CONFIG;

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
  bool ack_received_this_poll = false;
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
        if (POINTING_DEVICE_ON_REMOTE_HALF) {
          memcpy(&pending_pointing, payload, sizeof(pending_pointing));
          have_pointing = true;
        }
#endif
        goto apply_frames;
      }
#if defined(POINTING_DEVICE_ENABLED)
      if (type == SPLIT_FRAME_POINTING_SIDE_ACK) {
        // Slave apply report for a side slot: clears that side's pending so
        // the follow-up below stops retransmitting it. Keep waiting for the
        // remaining analog chunks.
        split_handle_side_ack(payload, payload_len, &ack_received_this_poll);
        continue;
      }
      if (type == SPLIT_FRAME_POINTING_CONFIG_ACK) {
        // Slave apply report for the global slot: clears the pending global
        // so the follow-up below stops retransmitting it. Keep waiting for
        // the remaining analog chunks.
        split_handle_config_ack(payload, payload_len, &ack_received_this_poll);
        continue;
      }
#endif
      break;
    }
  }

  // Pointing is best-effort. A miss after a good KEY_STATE must not count as a
  // link failure — that regression made the remote half look completely dead.
#if defined(POINTING_DEVICE_ENABLED)
  // The slave reports applied global/side slots right after KEY_STATE, so the
  // slave may send POINTING plus CONFIG_ACK plus SIDE_ACK in one response;
  // collect up to one of each.
  if (POINTING_DEVICE_ON_REMOTE_HALF && !have_pointing) {
    for (uint8_t i = 0; i < 3; i++) {
      if (!split_receive_frame(&type, payload, &payload_len,
                               SPLIT_CONNECTION_TIMEOUT_MS))
        break;
      if (type == SPLIT_FRAME_POINTING &&
          payload_len == sizeof(split_pointing_payload_t)) {
        memcpy(&pending_pointing, payload, sizeof(pending_pointing));
        have_pointing = true;
        break;
      }
      if (type == SPLIT_FRAME_POINTING_SIDE_ACK) {
        split_handle_side_ack(payload, payload_len, &ack_received_this_poll);
        continue;
      }
      if (type == SPLIT_FRAME_POINTING_CONFIG_ACK) {
        split_handle_config_ack(payload, payload_len, &ack_received_this_poll);
        continue;
      }
      break;
    }
  }
#endif

#if defined(POINTING_DEVICE_ENABLED)
  // Collect config ACKs that neither phase above consumed (e.g. no analog was
  // requested and the remote half carries no sensor). Strictly non-blocking:
  // only consume frames already sitting in the RX FIFO and never wait, so the
  // poll period stays flat. A missing ACK keeps the slot pending and the
  // follow-up below retransmits it (bounded by the retry cap) on the next
  // poll. Drain up to two frames: the pointing loop above may already have
  // consumed one ACK, leaving the other still in flight, and the slave may
  // send both ACKs in one response.
  if (pending_pointing_side_config[0] || pending_pointing_side_config[1] ||
      pending_pointing_config) {
    for (uint8_t i = 0; i < 2; i++) {
      if (!pending_pointing_side_config[0] &&
          !pending_pointing_side_config[1] && !pending_pointing_config)
        break;
      // Peek only: nothing waiting means the ACK simply has not arrived yet.
      if (!split_transport_available())
        break;
      if (!split_receive_frame(&type, payload, &payload_len, 1))
        break;
      if (type == SPLIT_FRAME_POINTING_SIDE_ACK) {
        split_handle_side_ack(payload, payload_len, &ack_received_this_poll);
        continue;
      }
      if (type == SPLIT_FRAME_POINTING_CONFIG_ACK) {
        split_handle_config_ack(payload, payload_len, &ack_received_this_poll);
        continue;
      }
      if (type == SPLIT_FRAME_POINTING &&
          payload_len == sizeof(split_pointing_payload_t) &&
          POINTING_DEVICE_ON_REMOTE_HALF && !have_pointing) {
        memcpy(&pending_pointing, payload, sizeof(pending_pointing));
        have_pointing = true;
        continue;
      }
      // Any other late frame is best-effort and ignored here.
      break;
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
    // A newer SET simply overwrites the payload first. The frame is always
    // sent when advertised so the slave's follow-up wait stays aligned with
    // the poll flags; the slot clears on ACK, and without one the retry cap
    // gives up (best-effort for ACK-less firmware).
    split_send_frame(SPLIT_FRAME_POINTING_CONFIG,
                     (uint8_t *)&pending_pointing_config_payload,
                     sizeof(pending_pointing_config_payload));
    if (pending_pointing_config) {
      if (pending_pointing_config_retries >=
          SPLIT_POINTING_CONFIG_MAX_RETRIES) {
        pending_pointing_config = false;
        pending_pointing_config_retries = 0;
      } else {
        pending_pointing_config_retries++;
      }
    }
  } else if (send_pointing_side_config) {
    // At most one pointing follow-up per poll; the global frame above wins
    // while pending. A newer SET for the same side simply overwrites the
    // payload first.
    const uint8_t side_idx = pending_pointing_side_config[0] ? 0 : 1;
    split_send_frame(SPLIT_FRAME_POINTING_SIDE_CONFIG,
                     (uint8_t *)&pending_pointing_side_payload[side_idx],
                     sizeof(pending_pointing_side_payload[side_idx]));
    if (pending_pointing_side_config[side_idx]) {
      if (pending_pointing_side_retries[side_idx] >=
          SPLIT_POINTING_CONFIG_MAX_RETRIES) {
        pending_pointing_side_config[side_idx] = false;
        pending_pointing_side_retries[side_idx] = 0;
      } else {
        pending_pointing_side_retries[side_idx]++;
      }
    }
  }
#endif

  last_poll_time = timer_read();
}

#if defined(POINTING_DEVICE_ENABLED)
// Send this half's accumulated motion when it carries a sensor. Best-effort
// like on the master side: a failed send restores the accumulators for a
// later transfer and never counts as a link error.
static void split_slave_send_pointing(void) {
  if (!POINTING_DEVICE_ON_THIS_HALF)
    return;
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
    // back so a later successful transfer can deliver it.
    pointing_device_restore_local_delta(dx, dy);
  }
}

// Report newly applied configs so the master can clear its pending slots.
// Best-effort: a failed send keeps the report queued, and the master's
// still-pending retransmit covers the loss either way.
static void split_slave_send_acks(void) {
  if (pending_config_ack) {
    if (split_send_frame(SPLIT_FRAME_POINTING_CONFIG_ACK, NULL, 0))
      pending_config_ack = false;
  }
  if (pending_side_ack != 0) {
    const split_side_ack_payload_t ack_payload = {.side = pending_side_ack};
    if (split_send_frame(SPLIT_FRAME_POINTING_SIDE_ACK,
                         (uint8_t *)&ack_payload, sizeof(ack_payload)))
      pending_side_ack = 0;
  }
}
#endif

static void split_slave_task(void) {
  uint8_t type;
  uint8_t payload[SPLIT_MAX_PAYLOAD_SIZE];
  uint8_t payload_len;
#if defined(POINTING_DEVICE_ENABLED)
  // Drain stashed flash writes between transactions, never inside one, so a
  // slow write cannot blow the follow-up timing below.
  split_slave_flush_side_writes();
#endif

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
    if (poll->flags & SPLIT_POLL_FLAG_FOLLOWUP_POINTING_SIDE_CONFIG)
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
#if defined(POINTING_DEVICE_ENABLED)
  // Motion normally goes first so cursor deltas never queue behind config
  // reports; the master accepts POINTING and ACKs in any order. While an
  // analog sync is in flight the master ends collection at the first POINTING
  // frame, so on those polls the ACKs (and POINTING itself) wait until after
  // the analog chunks below.
  const bool pointing_first = !request_analog;
  if (pointing_first)
    split_slave_send_pointing();
  else
    split_slave_send_acks();
#endif

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

#if defined(POINTING_DEVICE_ENABLED)
  // Finish the response in the matching order: motion after the chunks on
  // analog-sync polls, ACKs after motion otherwise. Each frame is sent at
  // most once per response.
  if (!pointing_first)
    split_slave_send_pointing();
  else
    split_slave_send_acks();
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
          // Duplicate retransmits (the earlier ACK was lost) carry the
          // identical value: skip them without re-queueing an ACK so the two
          // halves cannot ping-pong forever. The master gives up on its own
          // retry cap; the value is already applied here.
          if (!split_slave_config_is_duplicate(cfg_payload)) {
            // The relay carries only sensor-relevant global fields;
            // scroll/snap stay at the slave's local EEPROM values (unused for
            // HID there).
            pointing_config_t cfg = DEFAULT_POINTING_CONFIG;
            if (pointing_config_is_valid(&eeconfig->pointing_config))
              cfg = eeconfig->pointing_config;
            cfg.enabled = cfg_payload->enabled != 0;
            cfg.auto_mouse_layer_enabled =
                cfg_payload->auto_mouse_layer_enabled != 0;
            cfg.cpi = cfg_payload->cpi;
            cfg.auto_mouse_layer = cfg_payload->auto_mouse_layer;
            // Slave applies sensor settings only; global EEPROM lives on the
            // master.
            pointing_device_apply_local(&cfg);
            // Queue the apply report: the master clears its pending global
            // slot (and stops retransmitting) only on this ACK, so report
            // only genuinely new values.
            slave_applied_config = *cfg_payload;
            slave_applied_config_valid = true;
            pending_config_ack = true;
          }
        }
        remaining--;
        break;
      case SPLIT_FRAME_POINTING_SIDE_CONFIG:
        if (payload_len == sizeof(split_pointing_side_config_payload_t)) {
          const split_pointing_side_config_payload_t *side_payload =
              (const split_pointing_side_config_payload_t *)payload;
          const uint8_t side = side_payload->side;
          if (side == POINTING_SIDE_LEFT || side == POINTING_SIDE_RIGHT) {
            pointing_side_config_t side_cfg = {
                .rotation_deg = side_payload->rotation_deg,
                .invert_x = side_payload->invert_x != 0,
                .invert_y = side_payload->invert_y != 0,
                .swap_axes = side_payload->swap_axes != 0,
            };
            if (pointing_side_config_is_valid(&side_cfg) &&
                !split_slave_side_is_duplicate(side_payload)) {
              const uint8_t idx = (uint8_t)(side - 1);
              // Record first so duplicate retransmits are recognized even
              // before the deferred flash write below lands.
              slave_applied_side[idx] = *side_payload;
              slave_applied_side_valid[idx] = true;
              // Stash the EEPROM write for between transactions: flash stalls
              // long enough to blow the follow-up timing, so the RX path must
              // not write here. The single write for this reception happens in
              // split_slave_flush_side_writes(), never twice.
              slave_side_write_cfg[idx] = side_cfg;
              slave_side_write_pending[idx] = true;
              // Apply when this half owns the side (normally always true for
              // the relayed remote side).
              if (side == pointing_device_my_side())
                pointing_device_apply_side_local(&side_cfg);
              // Queue the apply report even when this half does not own the
              // side: the master clears its per-side pending (and stops
              // retransmitting) only on this ACK, so report only genuinely
              // new values.
              pending_side_ack = side;
            }
          }
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
  pending_pointing_config_retries = 0;
  pending_pointing_side_config[0] = false;
  pending_pointing_side_config[1] = false;
  pending_pointing_side_retries[0] = 0;
  pending_pointing_side_retries[1] = 0;
  pending_config_ack = false;
  pending_side_ack = 0;
  slave_applied_config_valid = false;
  slave_applied_side_valid[0] = false;
  slave_applied_side_valid[1] = false;
  slave_side_write_pending[0] = false;
  slave_side_write_pending[1] = false;
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
  pending_pointing_config_retries = 0;
  return true;
#else
  (void)enabled;
  (void)auto_mouse_layer_enabled;
  (void)cpi;
  (void)auto_mouse_layer;
  return false;
#endif
}

bool split_send_pointing_side_config(uint8_t side, uint16_t rotation_deg,
                                     uint8_t invert_x, uint8_t invert_y,
                                     uint8_t swap_axes) {
#if defined(POINTING_DEVICE_ENABLED)
  if (!is_master)
    return false;
  if (side != POINTING_SIDE_LEFT && side != POINTING_SIDE_RIGHT)
    return false;
  if (rotation_deg >= 360 || invert_x > 1 || invert_y > 1 || swap_axes > 1)
    return false;
  // Queue into the per-side slot: a newer SET for the same side overwrites
  // the payload, while the other side's slot is untouched. The master task
  // sends one side frame per poll and the slot clears when the slave ACKs
  // that side (or when the retry cap gives up); a fresh queue restarts the
  // retry budget.
  const uint8_t idx = (uint8_t)(side - 1u);
  pending_pointing_side_payload[idx].side = side;
  pending_pointing_side_payload[idx].rotation_deg = rotation_deg;
  pending_pointing_side_payload[idx].invert_x = invert_x ? 1 : 0;
  pending_pointing_side_payload[idx].invert_y = invert_y ? 1 : 0;
  pending_pointing_side_payload[idx].swap_axes = swap_axes ? 1 : 0;
  pending_pointing_side_config[idx] = true;
  pending_pointing_side_retries[idx] = 0;
  return true;
#else
  (void)side;
  (void)rotation_deg;
  (void)invert_x;
  (void)invert_y;
  (void)swap_axes;
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
