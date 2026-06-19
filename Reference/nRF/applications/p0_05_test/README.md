# P0.05 GPIO Toggle Test

**Minimal hardware connectivity test for nRF52840 P0.05 pin**

## Purpose

This application provides a **bare-bones test** to verify P0.05 hardware connectivity by eliminating all potential software conflicts. It uses only basic GPIO functionality with no SPI, I2C, or other complex drivers.

## What It Does

- ✅ **Pure GPIO control**: Only uses basic GPIO driver
- ✅ **Continuous toggle**: P0.05 toggles every 500ms (2Hz)
- ✅ **Clear logging**: Shows toggle count and state
- ✅ **No conflicts**: Disables all other drivers

## Expected Behavior

### If Hardware is Good:
```
🔧 P0.05 GPIO Toggle Test Starting
✅ GPIO device ready: GPIO_0
✅ P0.05 configured as GPIO output
🔄 Toggle #10: P0.05 = HIGH
🔄 Toggle #20: P0.05 = LOW
🔄 Toggle #30: P0.05 = HIGH
...
```

**Oscilloscope/Logic Analyzer**: Should show 2Hz square wave on P0.05

### If Hardware Issue:
```
🔧 P0.05 GPIO Toggle Test Starting
❌ GPIO device not ready
```
OR
```
✅ GPIO device ready: GPIO_0
❌ Failed to configure P0.05 as output: -22
```

**Oscilloscope/Logic Analyzer**: No activity on P0.05

## Building and Testing

### Build
```bash
# From nRF root directory
west build -b nrf52840dk_nrf52840 applications/p0_05_test
```

### Flash
```bash
west flash
```

### Monitor
```bash
west monitor
```

## Hardware Setup

1. **Connect oscilloscope/logic analyzer** to P0.05
2. **Power the board**
3. **Flash this application**
4. **Monitor P0.05 for 2Hz square wave**

## Troubleshooting

### No Toggle Activity
- ❌ **Hardware issue confirmed**
- Check solder joints on P0.05
- Verify P0.05 is not shorted to ground/VCC
- Check if P0.05 is connected to LIS2DH12 CS

### Toggle Activity Present
- ✅ **Hardware is good**
- The issue is in SPI/LIS2DH12 configuration
- Return to main application debugging

## Next Steps

Based on results:

1. **If P0.05 toggles**: Hardware is good, focus on SPI/LIS2DH12 configuration
2. **If P0.05 doesn't toggle**: Hardware issue, check physical connections

This test provides **definitive hardware verification** before continuing with complex SPI debugging. 