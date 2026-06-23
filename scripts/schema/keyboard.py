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

from enum import Enum
from typing import Literal
from pydantic import BaseModel, Field, NonNegativeInt, PositiveFloat, PositiveInt


class KeyboardUSBPort(str, Enum):
    FULL_SPEED = "fs"
    HIGH_SPEED = "hs"


# USB Configuration
class KeyboardUSB(BaseModel):
    # USB Vendor ID
    vid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    # USB Product ID
    pid: str = Field(pattern=r"^0x[0-9A-Fa-f]{4}$")
    port: KeyboardUSBPort


# Keyboard Configuration
class KeyboardKeyboard(BaseModel):
    num_profiles: int = Field(ge=1, le=8)
    num_layers: int = Field(ge=1, le=8)
    num_keys: int = Field(ge=1, le=256)
    num_advanced_keys: int = Field(ge=1, le=64)


# Hardware Configuration
class KeyboardHardware(BaseModel):
    # High-speed external oscillator value in Hz
    hse_value: PositiveInt
    # Keyboard driver name
    driver: str


# Raw ADC input configuration
class KeyboardAnalogRaw(BaseModel):
    # Array of raw ADC input channels. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Array of key index mappings for the raw ADC inputs
    vector: list[NonNegativeInt]


# Analog Multiplexer ADC input configuration
class KeyboardAnalogMux(BaseModel):
    # Array of GPIO pin names for the multiplexer select lines
    select: list[str]
    # Array of ADC input channels for the multiplexers. If a string is provided, it is used as the GPIO pin name
    input: list[NonNegativeInt | str]
    # Mapping from multiplexers to ADC input channels
    matrix: list[list[NonNegativeInt]]


# Analog Configuration
class KeyboardAnalog(BaseModel):
    # ADC resolution for this keyboard. A higher value means higher accuracy but slower matrix scans. Default to the maximum resolution supported by the MCU if not provided.
    adc_resolution: PositiveInt | None = None
    # Set to true if the ADC value is inversely proportional to the travel distance of the keys
    invert_adc: bool = False
    # Delay in microseconds between ADC scans
    delay: int | None = None
    raw: KeyboardAnalogRaw | None = None
    mux: KeyboardAnalogMux | None = None


# Calibration Configuration
class KeyboardCalibration(BaseModel):
    # See `include/lib/eeconfig.h`
    initial_rest_value: NonNegativeInt
    # See `include/lib/eeconfig.h`
    initial_bottom_out_threshold: NonNegativeInt


# Wear leveling Configuration
class KeyboardWearLeveling(BaseModel):
    # Size of the virtual persistent storage in bytes. There must be enough RAM of this size to hold the entire virtual storage.
    virtual_size: int = Field(ge=1, le=8192)
    # Size of the write log in bytes
    write_log_size: int = Field(ge=1, le=65536)


class KeyboardLayoutKey(BaseModel):
    key: NonNegativeInt
    w: PositiveFloat | None = None
    h: PositiveFloat | None = None
    x: float | None = None
    y: float | None = None
    # Option key and value pairs for the corresponding labels
    option: tuple[int, int] | None = None


# Keyboard Layout
class KeyboardLayout(BaseModel):
    # Labels for each layout option. Use a string for a toggle option, or an array for a select option
    labels: list[str | list[str]] | None = None
    # Metadata for how the keyboard should be rendered in the web configurator
    keymap: list[list[KeyboardLayoutKey]]


# Per-half analog configuration for split keyboards
class KeyboardSplitAnalog(BaseModel):
    raw: KeyboardAnalogRaw | None = None
    mux: KeyboardAnalogMux | None = None


# Split Keyboard Configuration
class KeyboardSplit(BaseModel):
    # Enable split keyboard support
    enabled: bool = False
    # UART peripheral instance (e.g., 1 for USART1, 2 for USART2)
    uart_instance: int = Field(default=1, ge=1, le=8)
    # UART TX pin (half-duplex) or TX pin (full-duplex)
    uart_tx_pin: str | None = None
    # GPIO alternate function mux for TX pin (0-15)
    uart_tx_mux: int = Field(default=7, ge=0, le=15)
    # UART RX pin for full-duplex operation
    uart_rx_pin: str | None = None
    # GPIO alternate function mux for RX pin (0-15)
    uart_rx_mux: int = Field(default=7, ge=0, le=15)
    # UART baud rate
    baud_rate: int = Field(default=1000000, ge=9600, le=10000000)
    # Handedness detection method
    handedness: Literal["pin", "eeprom", "left", "right", "usb"] = "left"
    # GPIO pin for handedness detection (high = left, low = right unless inverted)
    handedness_pin: str | None = None
    # Number of keys connected to the left half
    left_keys: int = Field(default=1, ge=1, le=256)
    # Number of keys connected to the right half
    right_keys: int = Field(default=1, ge=1, le=256)
    # Key index offset of the left half in the global keymap
    left_key_offset: int = Field(default=0, ge=0, le=255)
    # Key index offset of the right half in the global keymap
    right_key_offset: int = Field(default=0, ge=0, le=255)
    # Invert handedness pin logic (low = left, high = right)
    handedness_pin_low_is_left: bool = False
    # Default handedness when using EEPROM detection (left/right)
    eeprom_default_handedness: Literal["left", "right"] = "left"
    # Analog configuration for the left half (falls back to global analog if omitted)
    analog_left: KeyboardSplitAnalog | None = None
    # Analog configuration for the right half (falls back to global analog if omitted)
    analog_right: KeyboardSplitAnalog | None = None


# Actuation Configuration
class KeyboardActuation(BaseModel):
    # Default actuation point
    actuation_point: int = Field(ge=0, le=255)


# keyboard.json Schema
class Keyboard(BaseModel):
    name: str
    manufacturer: str
    maintainer: str
    usb: KeyboardUSB
    keyboard: KeyboardKeyboard
    hardware: KeyboardHardware
    analog: KeyboardAnalog
    calibration: KeyboardCalibration
    wear_leveling: KeyboardWearLeveling | None = None
    split: KeyboardSplit | None = None
    layout: KeyboardLayout
    # Default keymap
    keymap: list[list[str]] | None = None
    # Default keymaps for each profile. If not specified, the default keymap will be used for all profiles.
    keymaps: list[list[list[str]]] | None = None
    actuation: KeyboardActuation | None = None
