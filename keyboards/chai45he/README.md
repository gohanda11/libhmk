# Chai45HE

A 45-key Hall-effect keyboard.

## Hardware

- **MCU**: AT32F405RCT7
- **Hall Sensors**: 45x MT9102ET
- **Analog Multiplexers**: 6x SN74LV4051A (8:1)
- **External Crystal**: 16MHz

## Building

```bash
python setup.py -k chai45he
pio run
```

## Notes

- This configuration is based on the schematic analysis.
- The matrix configuration may need adjustment based on actual hardware testing.
- If keys are not registering correctly, verify the multiplexer connections and update the `matrix` array in `keyboard.json`.

## Pin Configuration

- **MUX Select**: PC13, PC14, PC15
- **MUX Outputs**: PA0, PA1, PA2, PA3, PA4, PA5 (6 channels for 6 multiplexers)

## Layout

The layout uses a compact 45-key configuration:
- Row 1: 12 keys
- Row 2: 12 keys (with 1.25U Tab and 1.75U Enter)
- Row 3: 11 keys (with 1.75U Caps and 2.25U Enter)
- Row 4: 10 keys (bottom row with various modifiers)

Adjust the `layout` section in `keyboard.json` to match your physical keyboard layout.
