# As-built rework log

Deviations between the design files (schematic / BOM / PCB) and the boards
as actually assembled or reworked. The design files are NOT authoritative
where an entry below says otherwise.

| Date | Ref | Design files say | As built | Why / notes |
|------|-----|------------------|----------|-------------|
| 2025 (assembly) | Y1 | 25 MHz crystal | **24 MHz crystal** | Substituted at assembly. `HSE_VALUE = 24000000` in `stm32g4xx_hal_conf.h` matches the real part; the PLL runs 144 MHz. Schematic/BOM not yet updated. |
