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

#include "commands.h"

#include "advanced_keys.h"
#include "hardware/hardware.h"
#include "layout.h"
#include "matrix.h"
#include "metadata.h"
#include "pointing_device.h"
#include "sensors/pmw3610.h"
#include "split.h"
#include "split_protocol.h"
#include "tusb.h"

// Helper macro to verify command parameters
#define COMMAND_VERIFY(cond)                                                   \
  if (!(cond)) {                                                               \
    success = false;                                                           \
    break;                                                                     \
  }

static const uint8_t keyboard_metadata[] = {KEYBOARD_METADATA};

// `volatile` to prevent compiler optimizations
static volatile bool command_request_pending;
static volatile bool command_response_pending;
static uint8_t in_buf[RAW_HID_EP_SIZE];
static uint8_t out_buf[RAW_HID_EP_SIZE];

command_staged_buffer_t staged_buffer;

static void command_reset_staged_buffer(void) {
  staged_buffer.staged_id = COMMAND_STAGED_NONE;
  staged_buffer.profile = 0;
  staged_buffer.offset = 0;
}


static void command_actuation_to_u8(command_actuation_u8_t *dst,
                                    const actuation_t *src) {
  dst->actuation_point = distance_to_u8(src->actuation_point);
  dst->rt_down = distance_to_u8(src->rt_down);
  dst->rt_up = distance_to_u8(src->rt_up);
  dst->continuous = src->continuous;
}

static void command_actuation_from_u8(actuation_t *dst,
                                      const command_actuation_u8_t *src) {
  dst->actuation_point = distance_from_u8(src->actuation_point);
  dst->rt_down = distance_from_u8(src->rt_down);
  dst->rt_up = distance_from_u8(src->rt_up);
  dst->continuous = src->continuous;
}

static void command_actuation_to_u16(command_actuation_u16_t *dst,
                                     const actuation_t *src) {
  dst->actuation_point = src->actuation_point;
  dst->rt_down = src->rt_down;
  dst->rt_up = src->rt_up;
  dst->continuous = src->continuous;
}

static void command_actuation_from_u16(actuation_t *dst,
                                       const command_actuation_u16_t *src) {
  dst->actuation_point = src->actuation_point;
  dst->rt_down = src->rt_down;
  dst->rt_up = src->rt_up;
  dst->continuous = src->continuous;
}

static void command_advanced_key_to_u8(command_advanced_key_u8_t *dst,
                                       const advanced_key_t *src) {
  memset(dst, 0, sizeof(*dst));
  dst->layer = src->layer;
  dst->key = src->key;
  dst->type = src->type;
  switch (src->type) {
  case AK_TYPE_NULL_BIND:
    dst->null_bind.secondary_key = src->null_bind.secondary_key;
    dst->null_bind.behavior = src->null_bind.behavior;
    dst->null_bind.bottom_out_point =
        distance_to_u8(src->null_bind.bottom_out_point);
    break;
  case AK_TYPE_DYNAMIC_KEYSTROKE:
    memcpy(dst->dynamic_keystroke.keycodes, src->dynamic_keystroke.keycodes,
           sizeof(dst->dynamic_keystroke.keycodes));
    memcpy(dst->dynamic_keystroke.bitmap, src->dynamic_keystroke.bitmap,
           sizeof(dst->dynamic_keystroke.bitmap));
    dst->dynamic_keystroke.bottom_out_point =
        distance_to_u8(src->dynamic_keystroke.bottom_out_point);
    break;
  case AK_TYPE_TAP_HOLD:
    dst->tap_hold = src->tap_hold;
    break;
  case AK_TYPE_TOGGLE:
    dst->toggle = src->toggle;
    break;
  case AK_TYPE_MACRO:
    dst->macro = src->macro;
    break;
  default:
    break;
  }
}

static void command_advanced_key_from_u8(advanced_key_t *dst,
                                         const command_advanced_key_u8_t *src) {
  memset(dst, 0, sizeof(*dst));
  dst->layer = src->layer;
  dst->key = src->key;
  dst->type = src->type;
  switch (src->type) {
  case AK_TYPE_NULL_BIND:
    dst->null_bind.secondary_key = src->null_bind.secondary_key;
    dst->null_bind.behavior = src->null_bind.behavior;
    dst->null_bind.bottom_out_point =
        distance_from_u8(src->null_bind.bottom_out_point);
    break;
  case AK_TYPE_DYNAMIC_KEYSTROKE:
    memcpy(dst->dynamic_keystroke.keycodes, src->dynamic_keystroke.keycodes,
           sizeof(dst->dynamic_keystroke.keycodes));
    memcpy(dst->dynamic_keystroke.bitmap, src->dynamic_keystroke.bitmap,
           sizeof(dst->dynamic_keystroke.bitmap));
    dst->dynamic_keystroke.bottom_out_point =
        distance_from_u8(src->dynamic_keystroke.bottom_out_point);
    break;
  case AK_TYPE_TAP_HOLD:
    dst->tap_hold = src->tap_hold;
    break;
  case AK_TYPE_TOGGLE:
    dst->toggle = src->toggle;
    break;
  case AK_TYPE_MACRO:
    dst->macro = src->macro;
    break;
  default:
    break;
  }
}


/**
 * @brief Write the advanced key staged in `staged_buffer`
 *
 * Caller must make sure that the staged advanced key is in a valid state.
 *
 * @return `true` if the write was successful
 */
static bool command_write_staged_advanced_key(void) {
  if (staged_buffer.staged_id != COMMAND_STAGED_ADVANCED_KEYS)
    return false;

  const uint8_t profile = staged_buffer.profile;
  const uint8_t key_index = staged_buffer.offset / sizeof(advanced_key_t);

  if (profile >= NUM_PROFILES || key_index >= NUM_ADVANCED_KEYS)
    return false;

  if (profile == eeconfig->current_profile)
    advanced_key_clear();

  const bool success = EECONFIG_WRITE_N(
      profiles[profile].advanced_keys[key_index],
      &staged_buffer.data.advanced_key, sizeof(advanced_key_t));

  if (profile == eeconfig->current_profile)
    layout_load_advanced_keys();

  return success;
}

static bool command_write_staged_advanced_key_u8(void) {
  if (staged_buffer.staged_id != COMMAND_STAGED_ADVANCED_KEYS_U8)
    return false;

  const uint8_t profile = staged_buffer.profile;
  const uint8_t key_index =
      staged_buffer.offset / sizeof(command_advanced_key_u8_t);

  if (profile >= NUM_PROFILES || key_index >= NUM_ADVANCED_KEYS)
    return false;

  advanced_key_t ak;
  command_advanced_key_from_u8(&ak, &staged_buffer.data.advanced_key_u8);

  if (profile == eeconfig->current_profile)
    advanced_key_clear();

  const bool success = EECONFIG_WRITE_N(
      profiles[profile].advanced_keys[key_index], &ak, sizeof(advanced_key_t));

  if (profile == eeconfig->current_profile)
    layout_load_advanced_keys();

  return success;
}

/**
 * @brief Write the macro node staged in `staged_buffer`
 *
 * Caller must make sure that the staged macro node is in a valid state.
 *
 * @return `true` if the write was successful
 */
static bool command_write_staged_macro(void) {
  if (staged_buffer.staged_id != COMMAND_STAGED_MACROS)
    return false;

  const uint8_t profile = staged_buffer.profile;
  const uint8_t node_id = staged_buffer.offset / sizeof(macro_node_t);

  if (profile >= NUM_PROFILES || node_id >= NUM_MACRO_NODES)
    return false;

  if (profile == eeconfig->current_profile)
    advanced_key_clear();

  const bool success =
      EECONFIG_WRITE_N(profiles[profile].macros[node_id],
                       &staged_buffer.data.macro_node, sizeof(macro_node_t));

  if (profile == eeconfig->current_profile)
    layout_load_advanced_keys();

  return success;
}

/**
 * @brief Stage the staged protocol payload to be written at a later time to
 * prevent partial writes to the persistent configuration
 *
 * @return `true` if the stage was successful
 */
__attribute__((always_inline)) static inline bool
command_stage_write(const command_staged_write_t args) {
  const uint8_t staged_id = args.staged_id;
  const command_in_staged_profile_t *p = args.p;
  const uint32_t field_size = args.field_size;
  const uint32_t item_size = args.item_size;
  bool (*write_func)(void) = args.write_func;

  if (p->offset + p->len > field_size || p->len > M_ARRAY_SIZE(p->data) ||
      p->len == 0)
    goto fail;

  if (p->offset % item_size == 0) {
    // It is always safe to start staging at the beginning of an item.
    staged_buffer.staged_id = staged_id;
    staged_buffer.profile = p->profile;
    staged_buffer.offset = p->offset;
  }

  if (staged_id != staged_buffer.staged_id ||
      p->offset != staged_buffer.offset || p->profile != staged_buffer.profile)
    // Unexpected staged id, write offset, or profile mismatch.
    goto fail;

  for (uint32_t i = 0; i < p->len;) {
    const uint32_t current_item_offset = staged_buffer.offset % item_size;
    const uint32_t write_len =
        M_MIN(p->len - i, item_size - current_item_offset);

    memcpy(staged_buffer.raw_data + current_item_offset, p->data + i,
           write_len);

    if (p->len - i >= item_size - current_item_offset) {
      const bool success = write_func();
      if (!success)
        goto fail;
    }

    staged_buffer.offset += write_len;
    i += write_len;
  }

  return true;

fail:
  command_reset_staged_buffer();
  return false;
}

void command_init(void) {
  command_request_pending = false;
  command_response_pending = false;
  command_reset_staged_buffer();
}

bool command_enqueue(const uint8_t *buf, uint16_t len) {
  if (len != RAW_HID_EP_SIZE || command_request_pending ||
      command_response_pending)
    // Either `command_request_pending` or `command_response_pending` is set
    // means that there is already a command queued.
    return false;

  memcpy(in_buf, buf, RAW_HID_EP_SIZE);
  command_request_pending = true;

  return true;
}

/**
 * @brief Process the queued command and write the response
 *
 * @return None
 */
static void command_process(void) {
  const command_in_buffer_t *in = (const command_in_buffer_t *)in_buf;
  command_out_buffer_t *out = (command_out_buffer_t *)out_buf;

  bool success = true;
  switch (in->command_id) {
  case COMMAND_FIRMWARE_VERSION: {
    out->firmware_version = FIRMWARE_VERSION;
    break;
  }
  case COMMAND_REBOOT: {
    board_reset();
    break;
  }
  case COMMAND_BOOTLOADER: {
    board_enter_bootloader();
    break;
  }
  case COMMAND_FACTORY_RESET: {
    advanced_key_clear();
    success = eeconfig_reset();
    layout_load_advanced_keys();
#if defined(POINTING_DEVICE_ENABLED)
    pointing_device_reload_config();
#if defined(SPLIT_KEYBOARD)
    // The reset only rewrote this half's flash; push the default runtime to
    // the slave so its EEPROM table converges to the master's. The global
    // fields are always pushed, and both side slots are pushed so a
    // sensor-less slave still repairs its copies.
    if (split_is_master()) {
      const pointing_config_t *rcfg = pointing_device_get_config();
      split_send_pointing_config(rcfg->enabled,
                                 rcfg->auto_mouse_layer_enabled, rcfg->cpi,
                                 rcfg->auto_mouse_layer);
      for (uint8_t s = 0; s < POINTING_NUM_SIDES; s++) {
        const pointing_side_config_t *slot = &eeconfig->pointing_side[s];
        if (pointing_side_config_is_valid(slot)) {
          split_send_pointing_side_config(
              (uint8_t)(s + 1u), slot->rotation_deg, slot->invert_x ? 1 : 0,
              slot->invert_y ? 1 : 0, slot->swap_axes ? 1 : 0);
        }
      }
    }
#endif
#endif
    break;
  }
  case COMMAND_RECALIBRATE: {
#if defined(SPLIT_KEYBOARD)
    // Queue + flush one transaction before the local 500ms block, otherwise the
    // control frame sits pending until after master recalibration.
    split_send_control_command(SPLIT_CONTROL_RECALIBRATE);
    split_flush();
#endif
    // Preserve learned bottom-out thresholds; only retake rest values.
    matrix_recalibrate(false);
    break;
  }
  case COMMAND_ANALOG_INFO: {
    const command_in_analog_info_t *p = &in->analog_info;
    command_out_analog_info_t *o = out->analog_info;

    COMMAND_VERIFY(p->offset < NUM_KEYS);

    for (uint32_t i = 0;
         i < M_ARRAY_SIZE(out->analog_info) && i + p->offset < NUM_KEYS; i++) {
      o[i].adc_value = key_matrix[i + p->offset].adc_filtered;
      o[i].distance = distance_to_u8(key_matrix[i + p->offset].distance);
    }
    break;
  }
  case COMMAND_ANALOG_INFO_U16: {
    const command_in_analog_info_t *p = &in->analog_info;
    command_out_analog_info_u16_t *o = out->analog_info_u16;

    COMMAND_VERIFY(p->offset < NUM_KEYS);

    for (uint32_t i = 0;
         i < M_ARRAY_SIZE(out->analog_info_u16) && i + p->offset < NUM_KEYS;
         i++) {
      o[i].adc_value = key_matrix[i + p->offset].adc_filtered;
      o[i].distance = key_matrix[i + p->offset].distance;
    }
    break;
  }
  case COMMAND_GET_CALIBRATION: {
    out->calibration = eeconfig->calibration;
    break;
  }
  case COMMAND_SET_CALIBRATION: {
    success = EECONFIG_WRITE(calibration, &in->calibration);
    break;
  }
  case COMMAND_GET_PROFILE: {
    out->current_profile = eeconfig->current_profile;
    break;
  }
  case COMMAND_GET_OPTIONS: {
    out->options = eeconfig->options;
    break;
  }
  case COMMAND_SET_OPTIONS: {
    success = EECONFIG_WRITE(options, &in->options);
    break;
  }
  case COMMAND_RESET_PROFILE: {
    const command_in_reset_profile_t *p = &in->reset_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    if (p->profile == eeconfig->current_profile)
      advanced_key_clear();
    success = eeconfig_reset_profile(p->profile);
    if (p->profile == eeconfig->current_profile)
      layout_load_advanced_keys();
    break;
  }
  case COMMAND_DUPLICATE_PROFILE: {
    const command_in_duplicate_profile_t *p = &in->duplicate_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->src_profile < NUM_PROFILES);

    if (p->profile == eeconfig->current_profile)
      advanced_key_clear();
    success = EECONFIG_WRITE(profiles[p->profile],
                             &eeconfig->profiles[p->src_profile]);
    if (p->profile == eeconfig->current_profile)
      layout_load_advanced_keys();
    break;
  }
  case COMMAND_GET_KEYMAP: {
    const command_in_keymap_t *p = &in->keymap;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->layer < NUM_LAYERS);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    memcpy(out->keymap,
           eeconfig->profiles[p->profile].keymap[p->layer] + p->offset,
           M_MIN(M_ARRAY_SIZE(out->keymap), (uint32_t)(NUM_KEYS - p->offset)) *
               sizeof(uint8_t));
    break;
  }
  case COMMAND_GET_METADATA: {
    const command_in_metadata_t *p = &in->metadata;

    COMMAND_VERIFY(p->offset < sizeof(keyboard_metadata));

    out->metadata.len = sizeof(keyboard_metadata) - p->offset;
    memcpy(out->metadata.metadata, &keyboard_metadata[p->offset],
           M_MIN(sizeof(out->metadata.metadata), out->metadata.len));
    break;
  }
  case COMMAND_GET_SERIAL: {
    memset(out->serial, 0, sizeof(out->serial));
    board_serial(out->serial);
    break;
  }
  case COMMAND_SAVE_CALIBRATION_THRESHOLD: {
    uint16_t bottom_out_threshold[NUM_KEYS];

    // Preserve any previously stored remote-half values on split keyboards.
    // The master never observes true rest/bottom-out for remote keys, so
    // overwriting them from local matrix state would corrupt calibration.
#if defined(SPLIT_KEYBOARD)
    for (uint32_t i = 0; i < NUM_KEYS; i++)
      bottom_out_threshold[i] = eeconfig->bottom_out_threshold[i];

    for (uint32_t i = 0; i < split_get_num_local_keys(); i++) {
      const uint32_t key = (uint32_t)split_get_key_offset() + i;
#else
    for (uint32_t i = 0; i < NUM_KEYS; i++) {
      const uint32_t key = i;
#endif
      if (key_matrix[key].adc_bottom_out_value < key_matrix[key].adc_rest_value)
        bottom_out_threshold[key] = 0;
      else
        bottom_out_threshold[key] = key_matrix[key].adc_bottom_out_value -
                                    key_matrix[key].adc_rest_value;
    }
    success = EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
    break;
  }
  case COMMAND_SET_SPLIT_HANDEDNESS: {
#if defined(SPLIT_KEYBOARD)
    const command_in_split_handedness_t *p = &in->split_handedness;
    COMMAND_VERIFY(p->handedness <= 1);
    success = EECONFIG_WRITE(split_handedness, &p->handedness);
#else
    success = false;
#endif
    break;
  }
  case COMMAND_POINTING_DEVICE_INFO: {
#if defined(POINTING_DEVICE_ENABLED)
    pmw3610_info_t info;
    pmw3610_get_info(&info);
    out->pointing_device_info.product_id = info.product_id;
    out->pointing_device_info.observation = info.observation;
    out->pointing_device_info.motion = info.motion;
    out->pointing_device_info.irq_low = info.irq_low ? 1 : 0;
    out->pointing_device_info.init_ok = info.init_ok ? 1 : 0;
#else
    success = false;
#endif
    break;
  }
  case COMMAND_GET_POINTING_CONFIG: {
#if defined(POINTING_DEVICE_ENABLED)
    const pointing_config_t *cfg = pointing_device_get_config();
    out->pointing_config.supported = 1;
#if defined(POINTING_DEVICE_DUAL_SENSOR)
    // Both halves carry a sensor; no single sensor side to report.
    out->pointing_config.side = 0;
#elif defined(POINTING_DEVICE_SIDE_LEFT)
    out->pointing_config.side = 1;
#elif defined(POINTING_DEVICE_SIDE_RIGHT)
    out->pointing_config.side = 2;
#else
    out->pointing_config.side = 0;
#endif
#else
    const pointing_config_t *cfg = &eeconfig->pointing_config;
    out->pointing_config.supported = 0;
    out->pointing_config.side = 0;
#endif
    // The v3 payload is byte-identical to pointing_config_t (10B).
    out->pointing_config.enabled = cfg->enabled ? 1 : 0;
    out->pointing_config.auto_mouse_layer_enabled =
        cfg->auto_mouse_layer_enabled ? 1 : 0;
    out->pointing_config.invert_scroll = cfg->invert_scroll ? 1 : 0;
    out->pointing_config.scroll_layer = cfg->scroll_layer;
    out->pointing_config.scroll_divisor = cfg->scroll_divisor;
    out->pointing_config.snap_axis = cfg->snap_axis;
    out->pointing_config.snap_threshold = cfg->snap_threshold;
    out->pointing_config.auto_mouse_layer = cfg->auto_mouse_layer;
    out->pointing_config.cpi = cfg->cpi;
    break;
  }
  case COMMAND_SET_POINTING_CONFIG: {
    const command_in_pointing_config_t *p = &in->pointing_config;
#if defined(POINTING_DEVICE_ENABLED)
    COMMAND_VERIFY(p->enabled <= 1);
    COMMAND_VERIFY(p->auto_mouse_layer_enabled <= 1);
    COMMAND_VERIFY(p->invert_scroll <= 1);
    COMMAND_VERIFY(p->cpi >= PMW3610_MIN_CPI && p->cpi <= PMW3610_MAX_CPI);
    // PMW3610 hardware / make.py both require 200 CPI steps.
    COMMAND_VERIFY((p->cpi % 200) == 0);
    COMMAND_VERIFY(p->auto_mouse_layer < NUM_LAYERS);
    COMMAND_VERIFY(p->scroll_layer == POINTING_SCROLL_LAYER_OFF ||
                   p->scroll_layer < NUM_LAYERS);
    COMMAND_VERIFY(p->scroll_divisor != 0);
    COMMAND_VERIFY(p->snap_axis <= POINTING_SNAP_AXIS_Y);
    COMMAND_VERIFY(p->snap_threshold <= 100);
    // AML on the scroll layer would swallow every cursor move as wheel ticks.
    COMMAND_VERIFY(p->scroll_layer == POINTING_SCROLL_LAYER_OFF ||
                   p->scroll_layer != p->auto_mouse_layer);

    pointing_config_t cfg = {
        .enabled = p->enabled != 0,
        .auto_mouse_layer_enabled = p->auto_mouse_layer_enabled != 0,
        .invert_scroll = p->invert_scroll != 0,
        .scroll_layer = p->scroll_layer,
        .scroll_divisor = p->scroll_divisor,
        .snap_axis = p->snap_axis,
        .snap_threshold = p->snap_threshold,
        .auto_mouse_layer = p->auto_mouse_layer,
        .cpi = p->cpi,
    };
    success = EECONFIG_WRITE(pointing_config, &cfg);
    if (success)
      pointing_device_set_config(&cfg);
#else
    // Unsupported keyboards accept SET as a no-op success.
    (void)p;
    success = true;
#endif
    break;
  }
  case COMMAND_GET_SIDE_CONFIG: {
#if defined(POINTING_DEVICE_ENABLED)
    const uint8_t side = in->get_side_config.side;
    COMMAND_VERIFY(side == POINTING_SIDE_LEFT ||
                   side == POINTING_SIDE_RIGHT);
    const uint8_t idx = (uint8_t)(side - 1);
    const pointing_side_config_t *scfg = &eeconfig->pointing_side[idx];
    // Repair-on-read so a corrupted slot never reaches the host.
    pointing_side_config_t def =
        (pointing_side_config_t)DEFAULT_POINTING_SIDE_CONFIG;
    if (!pointing_side_config_is_valid(scfg))
      scfg = &def;
    // Only the side that actually carries a sensor is reported as supported;
    // a single-sensor build answers 0 for the sensor-less side so the host
    // hides that panel instead of editing a dead slot.
    out->side_config.supported = pointing_device_side_supported(side) ? 1 : 0;
    out->side_config.rotation_deg = scfg->rotation_deg;
    out->side_config.invert_x = scfg->invert_x ? 1 : 0;
    out->side_config.invert_y = scfg->invert_y ? 1 : 0;
    out->side_config.swap_axes = scfg->swap_axes ? 1 : 0;
#else
    const uint8_t side = in->get_side_config.side;
    COMMAND_VERIFY(side == POINTING_SIDE_LEFT ||
                   side == POINTING_SIDE_RIGHT);
    out->side_config.supported = 0;
    out->side_config.rotation_deg = 0;
    out->side_config.invert_x = 0;
    out->side_config.invert_y = 0;
    out->side_config.swap_axes = 0;
#endif
    break;
  }
  case COMMAND_SET_SIDE_CONFIG: {
    const command_in_side_config_t *p = &in->side_config;
#if defined(POINTING_DEVICE_ENABLED)
    COMMAND_VERIFY(p->side == POINTING_SIDE_LEFT ||
                   p->side == POINTING_SIDE_RIGHT);
    COMMAND_VERIFY(p->rotation_deg < 360);
    COMMAND_VERIFY(p->invert_x <= 1);
    COMMAND_VERIFY(p->invert_y <= 1);
    COMMAND_VERIFY(p->swap_axes <= 1);
    pointing_side_config_t side_cfg = {
        .rotation_deg = p->rotation_deg,
        .invert_x = p->invert_x != 0,
        .invert_y = p->invert_y != 0,
        .swap_axes = p->swap_axes != 0,
    };
    const uint8_t idx = (uint8_t)(p->side - 1);
    // Persist the targeted slot in this half's local EEPROM, then relay via
    // pointing_device_set_side_config() (own side included) so the slave
    // persists its copy as well; both halves' side tables converge.
    success = wear_leveling_write(offsetof(eeconfig_t, pointing_side) +
                                      idx * sizeof(pointing_side_config_t),
                                  &side_cfg, sizeof(side_cfg));
    if (success)
      pointing_device_set_side_config(p->side, &side_cfg);
#else
    COMMAND_VERIFY(p->side == POINTING_SIDE_LEFT ||
                   p->side == POINTING_SIDE_RIGHT);
    COMMAND_VERIFY(p->rotation_deg < 360);
    COMMAND_VERIFY(p->invert_x <= 1);
    COMMAND_VERIFY(p->invert_y <= 1);
    COMMAND_VERIFY(p->swap_axes <= 1);
    success = true;
#endif
    break;
  }
    //--------------------------------------------------------------------+
    // Per-profile commands
    //--------------------------------------------------------------------+
  case COMMAND_SET_KEYMAP: {
    const command_in_keymap_t *p = &in->keymap;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->layer < NUM_LAYERS);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->keymap) &&
                   p->len <= NUM_KEYS - p->offset);

    success = EECONFIG_WRITE_N(profiles[p->profile].keymap[p->layer][p->offset],
                               p->keymap, sizeof(uint8_t) * p->len);
    break;
  }
  case COMMAND_GET_ACTUATION_MAP: {
    const command_in_actuation_map_t *p = &in->actuation_map;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    const uint32_t count = M_MIN(M_ARRAY_SIZE(out->actuation_map),
                                 (uint32_t)(NUM_KEYS - p->offset));
    for (uint32_t i = 0; i < count; i++)
      command_actuation_to_u8(
          &out->actuation_map[i],
          &eeconfig->profiles[p->profile].actuation_map[p->offset + i]);
    break;
  }
  case COMMAND_SET_ACTUATION_MAP: {
    const command_in_actuation_map_t *p = &in->actuation_map;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->actuation_map) &&
                   p->len <= NUM_KEYS - p->offset);

    actuation_t converted[15];
    for (uint32_t i = 0; i < p->len; i++)
      command_actuation_from_u8(&converted[i], &p->actuation_map[i]);
    success = EECONFIG_WRITE_N(profiles[p->profile].actuation_map[p->offset],
                               converted, sizeof(actuation_t) * p->len);
    break;
  }
  case COMMAND_GET_ACTUATION_MAP_U16: {
    const command_in_actuation_map_u16_t *p = &in->actuation_map_u16;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    const uint32_t count = M_MIN(M_ARRAY_SIZE(out->actuation_map_u16),
                                 (uint32_t)(NUM_KEYS - p->offset));
    for (uint32_t i = 0; i < count; i++)
      command_actuation_to_u16(
          &out->actuation_map_u16[i],
          &eeconfig->profiles[p->profile].actuation_map[p->offset + i]);
    break;
  }
  case COMMAND_SET_ACTUATION_MAP_U16: {
    const command_in_actuation_map_u16_t *p = &in->actuation_map_u16;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->actuation_map) &&
                   p->len <= NUM_KEYS - p->offset);

    actuation_t converted[8];
    for (uint32_t i = 0; i < p->len; i++)
      command_actuation_from_u16(&converted[i], &p->actuation_map[i]);
    success = EECONFIG_WRITE_N(profiles[p->profile].actuation_map[p->offset],
                               converted, sizeof(actuation_t) * p->len);
    break;
  }
  case COMMAND_GET_ADVANCED_KEYS: {
    const command_in_staged_profile_t *p = &in->staged_profile;
    const uint32_t advanced_keys_size =
        NUM_ADVANCED_KEYS * sizeof(command_advanced_key_u8_t);

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < advanced_keys_size);

    out->staged_profile.len = M_MIN(M_ARRAY_SIZE(out->staged_profile.data),
                                    advanced_keys_size - p->offset);
    for (uint32_t i = 0; i < out->staged_profile.len;) {
      const uint32_t abs_off = p->offset + i;
      const uint32_t key_index = abs_off / sizeof(command_advanced_key_u8_t);
      const uint32_t item_off = abs_off % sizeof(command_advanced_key_u8_t);
      command_advanced_key_u8_t wire;
      command_advanced_key_to_u8(
          &wire, &eeconfig->profiles[p->profile].advanced_keys[key_index]);
      const uint32_t chunk = M_MIN(out->staged_profile.len - i,
                                   sizeof(command_advanced_key_u8_t) - item_off);
      memcpy(out->staged_profile.data + i, ((const uint8_t *)&wire) + item_off,
             chunk);
      i += chunk;
    }
    break;
  }
  case COMMAND_SET_ADVANCED_KEYS: {
    const command_in_staged_profile_t *p = &in->staged_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = command_stage_write((command_staged_write_t){
        .staged_id = COMMAND_STAGED_ADVANCED_KEYS_U8,
        .p = (command_in_staged_profile_t *)p,
        .field_size = NUM_ADVANCED_KEYS * sizeof(command_advanced_key_u8_t),
        .item_size = sizeof(command_advanced_key_u8_t),
        .write_func = command_write_staged_advanced_key_u8,
    });
    break;
  }
  case COMMAND_GET_ADVANCED_KEYS_U16: {
    const command_in_staged_profile_t *p = &in->staged_profile;
    const uint32_t advanced_keys_size =
        sizeof(eeconfig->profiles[p->profile].advanced_keys);

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < advanced_keys_size);

    out->staged_profile.len = M_MIN(M_ARRAY_SIZE(out->staged_profile.data),
                                    advanced_keys_size - p->offset);
    memcpy(out->staged_profile.data,
           (const uint8_t *)eeconfig->profiles[p->profile].advanced_keys +
               p->offset,
           out->staged_profile.len);
    break;
  }
  case COMMAND_SET_ADVANCED_KEYS_U16: {
    const command_in_staged_profile_t *p = &in->staged_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = command_stage_write((command_staged_write_t){
        .staged_id = COMMAND_STAGED_ADVANCED_KEYS,
        .p = (command_in_staged_profile_t *)p,
        .field_size = sizeof(eeconfig->profiles[p->profile].advanced_keys),
        .item_size = sizeof(advanced_key_t),
        .write_func = command_write_staged_advanced_key,
    });
    break;
  }
  case COMMAND_GET_MACROS: {
    const command_in_staged_profile_t *p = &in->staged_profile;
    const uint32_t macros_size = sizeof(eeconfig->profiles[p->profile].macros);

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < macros_size);

    out->staged_profile.len =
        M_MIN(M_ARRAY_SIZE(out->staged_profile.data), macros_size - p->offset);
    memcpy(out->staged_profile.data,
           (const uint8_t *)eeconfig->profiles[p->profile].macros + p->offset,
           out->staged_profile.len);
    break;
  }
  case COMMAND_SET_MACROS: {
    const command_in_staged_profile_t *p = &in->staged_profile;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = command_stage_write((command_staged_write_t){
        .staged_id = COMMAND_STAGED_MACROS,
        .p = (command_in_staged_profile_t *)p,
        .field_size = sizeof(eeconfig->profiles[p->profile].macros),
        .item_size = sizeof(macro_node_t),
        .write_func = command_write_staged_macro,
    });
    break;
  }
  case COMMAND_GET_TICK_RATE: {
    const command_in_tick_rate_t *p = &in->tick_rate;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    out->tick_rate = eeconfig->profiles[p->profile].tick_rate;
    break;
  }
  case COMMAND_SET_TICK_RATE: {
    const command_in_tick_rate_t *p = &in->tick_rate;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = EECONFIG_WRITE(profiles[p->profile].tick_rate, &p->tick_rate);
    break;
  }
  case COMMAND_GET_GAMEPAD_BUTTONS: {
    const command_in_gamepad_buttons_t *p = &in->gamepad_buttons;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);

    memcpy(out->gamepad_buttons,
           eeconfig->profiles[p->profile].gamepad_buttons + p->offset,
           M_MIN(M_ARRAY_SIZE(out->gamepad_buttons),
                 (uint32_t)(NUM_KEYS - p->offset)) *
               sizeof(uint8_t));
    break;
  }
  case COMMAND_SET_GAMEPAD_BUTTONS: {
    const command_in_gamepad_buttons_t *p = &in->gamepad_buttons;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);
    COMMAND_VERIFY(p->offset < NUM_KEYS);
    COMMAND_VERIFY(p->len <= M_ARRAY_SIZE(p->gamepad_buttons) &&
                   p->len <= NUM_KEYS - p->offset);

    success = EECONFIG_WRITE_N(profiles[p->profile].gamepad_buttons[p->offset],
                               p->gamepad_buttons, sizeof(uint8_t) * p->len);
    break;
  }
  case COMMAND_GET_GAMEPAD_OPTIONS: {
    const command_in_gamepad_options_t *p = &in->gamepad_options;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    out->gamepad_options = eeconfig->profiles[p->profile].gamepad_options;
    break;
  }
  case COMMAND_SET_GAMEPAD_OPTIONS: {
    const command_in_gamepad_options_t *p = &in->gamepad_options;

    COMMAND_VERIFY(p->profile < NUM_PROFILES);

    success = EECONFIG_WRITE(profiles[p->profile].gamepad_options,
                             &p->gamepad_options);
    break;
  }
  default: {
    // Unknown command
    success = false;
    break;
  }
  }

  // Echo the command ID back to the host if successful
  out->command_id = success ? in->command_id : COMMAND_UNKNOWN;
}

void command_task(void) {
  if (command_request_pending) {
    command_process();
    command_request_pending = false;
    command_response_pending = true;
  }

  if (command_response_pending && tud_hid_n_ready(USB_ITF_RAW_HID) &&
      tud_hid_n_report(USB_ITF_RAW_HID, 0, out_buf, RAW_HID_EP_SIZE))
    // The command response has been sent, so clear the queue.
    command_response_pending = false;
}