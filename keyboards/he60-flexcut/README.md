# HE60 FlexCut

HE60 FlexCut is a modified 60% Hall-effect keyboard based on HE60, with support for ISO Enter and additional flex-cut layout options.

- **Keyboard Maintainer**: [gohanda](https://github.com/gohanda11)
- **Hardware Support**: [gohanda11/HE60-FlexCut](https://github.com/gohanda11/HE60-FlexCut)
- **Bootloader**: Factory AT32 DFU bootloader. Enter the bootloader through the following methods:
  - Hold **BOOT** button while plugging in the keyboard.
  - Press the `SP_BOOT` binding.
  - Use the web configurator.

## Additional Keys

Compared to HE60, this variant adds 2 extra Hall-effect sensors:

| Key Index | Label       | Description                              |
|-----------|-------------|------------------------------------------|
| 77        | ISO Enter   | ISO Enter (L-shape, replaces key 30)     |
| 78        | 1u          | 1u key next to ISO Enter (replaces key 43) |

The ISO Enter option can be toggled in the web configurator under the **ISO Enter** layout label.
