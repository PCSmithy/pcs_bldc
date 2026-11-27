# As-built rework log

Deviations between the design files (schematic / BOM / PCB) and the boards
as actually assembled or reworked. The design files are NOT authoritative
where an entry below says otherwise.

| Date | Ref | Design files say | As built | Why / notes |
|------|-----|------------------|----------|-------------|
| 2025 (assembly) | Y1 | 25 MHz crystal | **24 MHz crystal** | Substituted at assembly. `HSE_VALUE = 24000000` in `stm32g4xx_hal_conf.h` matches the real part; the PLL runs 170 MHz. Schematic/BOM not yet updated. |
| 2026 (rework) | C28 | 0.1 µF from `MCU_BOOT0/I2C_SCL` to GND | **Removed** | With the 5.1 k pull-up R46 it gave SCL a ~510 µs rise, so the CYPD3177 could not be clocked. BOOT0 is taken from the option bytes (`nSWBOOT0 = 0`), so the pin needs no debounce. Schematic/BOM not yet updated. |
