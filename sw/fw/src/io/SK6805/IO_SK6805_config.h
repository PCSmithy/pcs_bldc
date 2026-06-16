#ifndef IO_SK6805_CONFIG_H
#define IO_SK6805_CONFIG_H

/* Includes */

/* Defines */

// Status-LED string on the board: 36 SK6805 in a daisy chain (D8..D43),
// driven from SPI3 MOSI (PB5). See hw/rgb_LEDs.kicad_sch.
#define IO_SK6805_PIXEL_COUNT  36U

#endif // IO_SK6805_CONFIG_H
