# libhmk

This repository is a fork of [peppapighs/libhmk](https://github.com/peppapighs/libhmk/tree/main).

## Changes from upstream

- **Chai45HE keyboard**: Added a custom 45-key Hall-effect keyboard (`keyboards/chai45he`).
- **Tap-Hold layer switch fix**: Restructured `layout_task` to use a two-pass key processing approach. When a Tap-Hold key (e.g., LT with MO layer) and another key are pressed nearly simultaneously, the Tap-Hold is now promoted to hold before the other key's keycode is resolved. This ensures the correct layer is active for keycode lookup, matching ZMK's default behavior.
- **Split keyboard support**: Added split keyboard support with runtime handedness detection, master/slave detection over UART, and per-half analog configurations. The same firmware binary runs on both halves. See the Split Keyboards section below for details.

## Split Keyboards

libhmk supports split keyboards with two halves connected over UART. The same firmware binary runs on both halves; the side is determined at runtime, and each half can have its own analog pin assignments and matrix.

### How It Works

- **Handedness (left/right)**: Detected at boot before analog initialization. The result selects the key offset, the number of local keys, and the analog matrix for that half.
- **Master/slave**: Determined after USB initialization. The half that successfully enumerates USB becomes the master; the other half becomes the slave and sends its key states over UART.
- **Key offsets**: Each half reports its keys using local indices. The firmware adds the half's global key offset so the full keymap is addressed consistently.

### `keyboard.json` Configuration

Split behavior is configured under the top-level `split` object:

```json
"split": {
  "enabled": true,
  "uart_instance": 1,
  "uart_tx_pin": "B6",
  "uart_tx_mux": 7,
  "baud_rate": 1000000,
  "handedness": "pin",
  "handedness_pin": "B7",
  "left_keys": 30,
  "right_keys": 33,
  "left_key_offset": 0,
  "right_key_offset": 30,
  "analog_left": {
    "mux": {
      "select": ["C1", "C2", "C3"],
      "input": ["A3", "A4", "A5", "A6", "A7", "C4", "C5", "B0", "B1"],
      "matrix": [
        [4, 3, 2, 0, 5, 0, 6, 7],
        [0, 0, 0, 8, 0, 0, 0, 0],
        [10, 9, 1, 11, 12, 0, 0, 13],
        [0, 0, 0, 14, 0, 0, 0, 0],
        [0, 0, 0, 0, 16, 19, 17, 18],
        [0, 26, 20, 0, 0, 0, 0, 0],
        [29, 28, 27, 30, 15, 22, 0, 21],
        [0, 0, 0, 0, 23, 0, 24, 25],
        [0, 0, 0, 0, 0, 0, 0, 0]
      ]
    }
  },
  "analog_right": {
    "mux": {
      "select": ["C1", "C2", "C3"],
      "input": ["A3", "A4", "A5", "A6", "A7", "C4", "C5", "B0", "B1"],
      "matrix": [
        [0, 0, 0, 0, 0, 1, 0, 0],
        [7, 0, 6, 0, 2, 5, 3, 4],
        [0, 0, 0, 0, 0, 8, 0, 0],
        [15, 14, 13, 0, 9, 12, 10, 11],
        [17, 16, 0, 18, 0, 0, 0, 0],
        [0, 0, 0, 27, 19, 22, 20, 21],
        [0, 0, 0, 0, 0, 0, 0, 0],
        [0, 29, 30, 0, 0, 23, 0, 0],
        [32, 33, 0, 31, 24, 28, 25, 26]
      ]
    }
  }
}
```

| Field | Description |
|---|---|
| `enabled` | Enable split keyboard support. |
| `uart_instance` | UART peripheral instance (e.g., `1` for USART1). |
| `uart_tx_pin` | UART TX pin. Use half-duplex (`uart_rx_pin` omitted) or full-duplex. |
| `uart_tx_mux` | GPIO alternate function mux for the TX pin. |
| `uart_rx_pin` | UART RX pin for full-duplex operation (optional). |
| `uart_rx_mux` | GPIO alternate function mux for the RX pin. |
| `baud_rate` | UART baud rate (default: `1000000`). |
| `handedness` | How the half decides if it is left or right: `"pin"`, `"eeprom"`, `"left"`, or `"right"`. |
| `handedness_pin` | GPIO pin used when `handedness` is `"pin"`. |
| `handedness_pin_low_is_left` | Invert the handedness pin logic so LOW means left (default: `false`). |
| `eeprom_default_handedness` | Default side when `handedness` is `"eeprom"`. |
| `left_keys` / `right_keys` | Number of keys on each half. The two values must add up to `keyboard.num_keys`. |
| `left_key_offset` / `right_key_offset` | Global keymap index where each half's local key 0 starts. |
| `analog_left` / `analog_right` | Per-half analog configuration. If omitted, the global `analog` section is used. |

### Handedness Detection

Handedness determines whether a half acts as the **left** or **right** side of the keyboard. It is independent of master/slave selection: the half that enumerates over USB becomes the master, while the other half becomes the slave.

| Value | Behavior | Use case |
|---|---|---|
| `"usb"` | The USB-connected half is always treated as the **left** half. This is the simplest method when the same firmware binary is flashed to both halves and the left half is normally the one plugged into USB. | No extra GPIO wiring required. |
| `"pin"` | The configured `handedness_pin` is read with an internal pull-up. By default, **HIGH = left**, **LOW = right**. Set `handedness_pin_low_is_left: true` to swap this. | A GPIO is wired to `3.3V` on the left half and `GND` on the right half, so each half knows its physical position regardless of which side is USB-connected. |
| `"eeprom"` | The side is read from EEPROM. It can be changed at runtime with the `COMMAND_SET_SPLIT_HANDEDNESS` command (e.g., from hmkconf). The default is controlled by `eeprom_default_handedness`. | The side is configured in software after flashing, useful when no dedicated handedness pin is available. |
| `"left"` / `"right"` | The side is fixed at compile time. | Useful for testing or for builds that are never swapped between halves. |

#### Pin Wiring Example

For `"handedness": "pin"`, choose any unused GPIO and wire it as follows:

```text
Left half:   handedness_pin ── 3.3V
Right half:  handedness_pin ── GND
```

The pin is configured with an internal pull-up, so it reads HIGH when left floating. On the right half, tying it to GND overrides the pull-up and gives a LOW reading. No external pull-up resistor is required, but a series resistor (e.g., 1 kΩ) between the pin and 3.3V/GND can be added for extra protection.

```json
"split": {
  "enabled": true,
  "handedness": "pin",
  "handedness_pin": "B12",
  "left_keys": 30,
  "right_keys": 39,
  "left_key_offset": 0,
  "right_key_offset": 30
}
```

### Key Numbering and Layout

The `layout.keymap` entries use **global 0-based key indices**. The global keymap is built by placing the left half's keys starting at `left_key_offset` and the right half's keys starting at `right_key_offset`.

It is recommended to keep each half's keys contiguous in the global keymap:

- Left half: global indices `left_key_offset` to `left_key_offset + left_keys - 1`
- Right half: global indices `right_key_offset` to `right_key_offset + right_keys - 1`

For example, with `left_key_offset: 0`, `left_keys: 30`, `right_key_offset: 30`, `right_keys: 33`:

- Left half uses global keys `0–29`
- Right half uses global keys `30–62`

The `keymap` array is indexed by these global numbers, so the first `left_keys` entries belong to the left half and the remaining `right_keys` entries belong to the right half.

### Analog Matrix Local Indices

The `matrix` arrays inside `analog_left` and `analog_right` use **1-based local indices** for that half. A value of `0` means the matrix position is unused. At runtime, the firmware converts a local index to a global index with:

```
global_key = split_key_offset + local_key - 1
```

For the example above:

- Left half: local `1` → global `0`, local `30` → global `29`
- Right half: local `1` → global `30`, local `33` → global `62`

This means the wiring matrices can be edited independently per half without changing the global keymap.

### Asymmetric Halves

Left and right halves do not need to have the same number of keys. Set `left_keys` and `right_keys` to the actual number of keys on each side. The only requirement is:

```
left_keys + right_keys == keyboard.num_keys
```

and that each half's key range fits within the global keymap:

```
left_key_offset + left_keys <= num_keys
right_key_offset + right_keys <= num_keys
```

### Example Reference

See [`keyboards/split60he/keyboard.json`](keyboards/split60he/keyboard.json) for a complete split keyboard configuration with different analog matrices for each half.

## Original description

This repository contains libraries for building a Hall-effect keyboard firmware.

## Table of Contents

- [Split Keyboards](#split-keyboards)
- [Features](#features)
- [Limitations](#limitations)
- [Getting Started](#getting-started)
- [Development](#development)
- [Porting](#porting)
- [Acknowledgements](#acknowledgements)

## Features

- [x] **Analog Input**: Customizable actuation point for each key and many other features.
- [x] **Rapid Trigger**: Register a key press or release based on the change in key position and the direction of that change
- [x] **Continuous Rapid Trigger**: Deactivate Rapid Trigger only when the key is fully released.
- [x] **Null Bind (SOCD + Rappy Snappy)**: Monitor 2 keys and select which one is active based on the chosen behavior.
- [x] **Dynamic Keystroke**: Assign up to 4 keycodes to a single key. Each keycode can be assigned up to 4 actions for 4 different parts of the keystroke.
- [x] **Tap-Hold**: Send a different keycode depending on whether the key is tapped or held.
- [x] **Toggle**: Toggle between key press and key release. Hold the key for normal behavior.
- [x] **N-Key Rollover**: Support for N-Key Rollover and automatically fall back to 6-Key Rollover in BIOS.
- [x] **Automatic Calibration**: Automatically calibrate the analog input without requiring user intervention.
- [x] **EEPROM Emulation**: No external EEPROM required. Emulate EEPROM using the internal flash memory.
- [x] **Web Configurator**: Configure the firmware using [hmkconf](https://github.com/peppapighs/hmkconf) without needing to recompile the firmware.
- [x] **Tick Rate**: Customizable tick rate for Tap-Hold and Dynamic Keystroke.
- [x] **8kHz Polling Rate**: Support for 8kHz polling rate on some microcontrollers (e.g., AT32F405xx).
- [x] **Gamepad**: Support for XInput gamepad mode, allowing the keyboard to be used as a game controller.
- [x] **Split Keyboard Support**: Build split keyboards with separate left/right halves. Each half can use different analog pin assignments and matrices, and the same firmware runs on both sides.

## Limitations

- **RGB Lighting**: The firmware does not support RGB lighting.

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/)
- [Python 3](https://www.python.org/)

### Building the Firmware

1. Clone the repository:

   ```bash
   git clone https://github.com/peppapighs/libhmk.git
   ```

2. Open the project in PlatformIO, such as through Visual Studio Code.

3. Run `python setup.py -k <YOUR_KEYBOARD>` to generate the `platformio.ini` file.

4. Wait for PlatformIO to finish initializing the environment.

5. Build the firmware using either `pio run` in the PlatformIO Core CLI or through the PlatformIO IDE's "Build" option. The firmware binaries will be generated in the `.pio/build/<YOUR_KEYBOARD>/` directory with the following files:
   - `firmware.bin`: The binary firmware file
   - `firmware.elf`: The ELF firmware file

6. Flash the firmware to your keyboard using your preferred method (e.g., DFU, ISP). If your keyboard has a DFU bootloader, you can set `upload_protocol = dfu` in `platformio.ini` and use the command `pio run --target upload` or the PlatformIO IDE's "Upload" option while the keyboard is in DFU mode. If your browser supports WebUSB, you can also use [WebUSB DFU](https://devanlai.github.io/webdfu/dfu-util/) (Recommended method).

## Development

The development branch is `dev`, which contains the latest features and bug fixes. The corresponding `dev` branch of [hmkconf](https://github.com/peppapighs/hmkconf/tree/dev) deployed at [https://dev.hmkconf.com](https://dev.hmkconf.com) is required to configure the `dev` branch of the firmware. To contribute, please create a pull request against the `dev` branch.

### Developing a New Keyboard

To develop a new keyboard, create a new directory under `keyboards/` with your keyboard's name. This directory should include the following files:

- `keyboard.json`: A JSON file containing metadata about your keyboard, used for both firmware compilation and the web configurator. Refer to [`scripts/schema/keyboard.py`](scripts/schema/keyboard.py) for the schema.
- `config.h` (Optional): Additional configuration header for your keyboard to define custom configurations beyond what's specified in `keyboard.json`.

You can use an existing keyboard implementation as a reference. If your keyboard hardware isn't currently supported by the firmware, you'll need to implement the necessary drivers and features. See the [Porting](#porting) section for more details.

## Porting

### Hardware Driver Structure

Hardware drivers follow this directory structure:

- [`hardware/`](hardware/): Contains hardware-specific header files. Each subdirectory may contain `config.h` and `board_def.h` for additional configuration, and board-specific definitions, respectively.
- [`include/hardware/`](include/hardware/): Contains hardware driver interface headers that declare functions to be implemented
- [`src/hardware/`](src/hardware/): Contains hardware driver implementations of the functions declared in the header files
- [`linker/`](linker/): Contains linker scripts for supported microcontrollers
- [`scripts/drivers.py`](scripts/drivers.py): Contains the driver configuration for each supported microcontroller. Each driver must implement the `Driver` class.

You can refer to existing hardware drivers as examples when implementing support for new hardware.

## Acknowledgements

- [hathach/tinyusb](https://github.com/hathach/tinyusb) for the USB stack.
- [qmk/qmk_firmware](https://github.com/qmk/qmk_firmware) for inspiration, including EEPROM emulation and matrix scanning.
- [@riskable](https://github.com/riskable) for pioneering custom Hall-effect keyboard firmware development.
- [@heiso](https://github.com/heiso/) for his [macrolev](https://github.com/heiso/macrolev) and his helpfulness throughout the development process.
- [Wooting](https://wooting.io/) for pioneering Hall-effect gaming keyboards and introducing many advanced features based on analog input.
- [GEONWORKS](https://geon.works/) for the Venom 60HE PCB and inspiring the web configurator.
- [@devanlai](https://github.com/devanlai) for [WebUSB DFU](https://devanlai.github.io/webdfu/dfu-util/).
