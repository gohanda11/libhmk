# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.

import json
import os
import utils

Import("env")

keyboard = env["PIOENV"]

# Load driver (only for validation)
utils.get_driver(keyboard)

# Load JSON files and validate
kb_json = utils.get_kb_json(keyboard)

# Validate default keymaps
default_keymaps = utils.resolve_default_keymaps(kb_json)
if len(default_keymaps) != kb_json.keyboard.num_profiles:
    raise ValueError(
        f"Expected default keymaps to have {kb_json.keyboard.num_profiles} profiles"
    )
for keymap in default_keymaps:
    if len(keymap) != kb_json.keyboard.num_layers:
        raise ValueError(
            f"Expected default keymaps to have {kb_json.keyboard.num_layers} layers"
        )
    for layer in keymap:
        if len(layer) != kb_json.keyboard.num_keys:
            raise ValueError(
                f"Expected default keymaps to have {kb_json.keyboard.num_keys} keys"
            )


# Validate analog multiplexer matrix key indices. Each entry must be 0
# (unconnected) or a unique 1-based local key index no larger than num_keys,
# and the mapped keys must match required_keys (the keys referenced by the
# layout) exactly. required_keys may be None to skip the coverage check, e.g.
# when raw ADC inputs may map some of the keys instead.
def validate_mux_matrix(matrix, num_keys, required_keys, context):
    seen = set()
    for row in matrix:
        for value in row:
            if value == 0:
                continue
            if value > num_keys:
                raise ValueError(
                    f"Mux matrix entry {value} exceeds the key count of {num_keys} in {context}"
                )
            if value in seen:
                raise ValueError(f"Duplicate mux matrix entry {value} in {context}")
            seen.add(value)
    if required_keys is not None:
        missing = sorted(required_keys - seen)
        if missing:
            raise ValueError(
                f"Mux matrix in {context} does not map layout keys: {missing}"
            )
        extra = sorted(seen - required_keys)
        if extra:
            raise ValueError(
                f"Mux matrix in {context} maps keys not present in the layout: {extra}"
            )


# Collect the GPIO pins used by an analog configuration. ADC input entries
# given as integers are raw ADC channels, not GPIO pins, so they are skipped.
def collect_analog_pins(analog):
    pins = []
    if analog.mux is not None:
        pins += [(pin, "mux select pin") for pin in analog.mux.select]
        pins += [
            (pin, "mux ADC input pin")
            for pin in analog.mux.input
            if isinstance(pin, str)
        ]
    if analog.raw is not None:
        pins += [
            (pin, "raw ADC input pin")
            for pin in analog.raw.input
            if isinstance(pin, str)
        ]
    return pins


# Collect the GPIO pins used by the pointing device. The PMW3610 supports a
# 3-wire SPI mode where MOSI and MISO share a single pin, so that pair is
# allowed to overlap.
def collect_pointing_device_pins(pd):
    pins = [
        (pd.pins.cs, "pointing device CS pin"),
        (pd.pins.sck, "pointing device SCK pin"),
        (pd.pins.mosi, "pointing device MOSI pin"),
    ]
    if utils.pin_name_to_port_pin(pd.pins.miso) != utils.pin_name_to_port_pin(
        pd.pins.mosi
    ):
        pins.append((pd.pins.miso, "pointing device MISO pin"))
    if pd.pins.irq is not None:
        pins.append((pd.pins.irq, "pointing device IRQ pin"))
    return pins


# Ensure that no GPIO pin is assigned to two different functions.
def validate_unique_pins(pins, context):
    seen = {}
    for pin_name, description in pins:
        if pin_name is None:
            continue
        port_pin = utils.pin_name_to_port_pin(pin_name)
        if port_pin in seen:
            raise ValueError(
                f"Pin {pin_name} is used both as {description} and as {seen[port_pin]} in {context}"
            )
        seen[port_pin] = description


kb = kb_json.keyboard
split = kb_json.split
split_enabled = split is not None and split.enabled
pd = kb_json.pointing_device
pd_enabled = pd is not None and pd.enabled

# Keys referenced by the layout (zero-based global key indices). The mux
# matrix of each half must map exactly these keys, converted to 1-based local
# key indices. Note that num_keys may be larger than the number of layout keys
# when layout options leave gaps in the key numbering (e.g. he60-flexcut).
layout_keys = {key.key for row in kb_json.layout.keymap for key in row}

if split_enabled:
    # The key ranges of both halves must cover 0..num_keys without gaps or
    # overlaps, so the per-half key counts must add up to the total.
    if split.left_keys + split.right_keys != kb.num_keys:
        raise ValueError(
            f"Split key counts ({split.left_keys} left + {split.right_keys} right) do not add up to keyboard.num_keys ({kb.num_keys})"
        )
    covered = [False] * kb.num_keys
    for offset, count, side in (
        (split.left_key_offset, split.left_keys, "left"),
        (split.right_key_offset, split.right_keys, "right"),
    ):
        if offset + count > kb.num_keys:
            raise ValueError(f"Split {side} half key range exceeds keyboard.num_keys")
        for key in range(offset, offset + count):
            if covered[key]:
                raise ValueError("Split left and right key ranges overlap")
            covered[key] = True
    if not all(covered):
        raise ValueError("Split left and right key ranges do not cover all keys")

    # Validate the mux matrix and pin assignments of each half separately,
    # since each half has its own MCU and its own local key indices. A half
    # without its own analog configuration falls back to the global one.
    for side, analog, offset, num_half_keys in (
        (
            "left",
            split.analog_left if split.analog_left is not None else kb_json.analog,
            split.left_key_offset,
            split.left_keys,
        ),
        (
            "right",
            split.analog_right if split.analog_right is not None else kb_json.analog,
            split.right_key_offset,
            split.right_keys,
        ),
    ):
        if analog.mux is not None:
            required = {
                key - offset + 1
                for key in layout_keys
                if offset <= key < offset + num_half_keys
            }
            validate_mux_matrix(
                analog.mux.matrix,
                num_half_keys,
                required if analog.raw is None else None,
                f"split {side} half",
            )
        pins = collect_analog_pins(analog)
        pins.append((split.uart_tx_pin, "split UART TX pin"))
        pins.append((split.uart_rx_pin, "split UART RX pin"))
        if split.handedness == "pin":
            pins.append((split.handedness_pin, "handedness pin"))
        if pd_enabled and (pd.side == side or pd.side == "both"):
            pins += collect_pointing_device_pins(pd)
        validate_unique_pins(pins, f"split {side} half")
else:
    if kb_json.analog.mux is not None:
        validate_mux_matrix(
            kb_json.analog.mux.matrix,
            kb.num_keys,
            {key + 1 for key in layout_keys} if kb_json.analog.raw is None else None,
            "analog.mux",
        )
    pins = collect_analog_pins(kb_json.analog)
    if pd_enabled:
        pins += collect_pointing_device_pins(pd)
    validate_unique_pins(pins, "keyboard")

# The auto mouse layer must reference an existing layer.
if pd_enabled and pd.auto_mouse_layer >= kb.num_layers:
    raise ValueError(
        f"pointing_device.auto_mouse_layer ({pd.auto_mouse_layer}) must be less than keyboard.num_layers ({kb.num_layers})"
    )

# The scroll layer must reference an existing layer.
if pd_enabled and pd.scroll_layer >= kb.num_layers:
    raise ValueError(
        f"pointing_device.scroll_layer ({pd.scroll_layer}) must be less than keyboard.num_layers ({kb.num_layers})"
    )

# The auto mouse layer must not reference the scroll layer: while the scroll
# layer is active every cursor move is sent as scroll input, so an auto mouse
# layer on the same layer would never move the cursor.
if (
    pd_enabled
    and pd.auto_mouse_layer >= 0
    and pd.scroll_layer >= 0
    and pd.scroll_layer == pd.auto_mouse_layer
):
    raise ValueError(
        f"pointing_device.auto_mouse_layer ({pd.auto_mouse_layer}) must not equal pointing_device.scroll_layer ({pd.scroll_layer})"
    )
