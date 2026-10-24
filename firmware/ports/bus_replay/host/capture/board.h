/*
 * The Pi side of the device's wiring: the GPIO lines of the SPI control binding and pin
 * reset, and the data UART. The only place the host names Pi pins or devices.
 */

#ifndef CAPTURE_BOARD_H
#define CAPTURE_BOARD_H


#define BOARD_GPIO_CHIP "/dev/gpiochip0"

// nRESET: open drain, driven only for the reset pulse.
#define BOARD_PIN_RESET 16

// SPI control binding: SCLK, MOSI and CS driven only during a transaction; MISO and IRQ
// read only.
#define BOARD_PIN_SCLK 7
#define BOARD_PIN_MOSI 8
#define BOARD_PIN_CS 24
#define BOARD_PIN_MISO 25
#define BOARD_PIN_IRQ 19

// Data channel 0: serial at 1 Mbaud 8N1 (a termios speed).
#define BOARD_DATA_PORT "/dev/ttyAMA4"
#define BOARD_DATA_SPEED B1000000

// For the capture file's header (docs/capture-file.md, Binding).
#define BOARD_BINDING "control=spi data0=serial:/dev/ttyAMA4:1000000"

#endif
