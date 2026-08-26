# MDBT50Q-1MV2 / MDBT50Q-P1MV2 (nRF52840) Pinout

Source: Raytac MDBT50Q-1MV2 & MDBT50Q-P1MV2 datasheet, Ver. L (2023/05/24), section 2.5 Pin Assignment.

61-pad module. "Usable as GPIO" = pin can be driven/read as a digital I/O (`nrf_gpio` / Zephyr GPIO), even if it also has an alternate function.

| Pin No. | Name | Usable as GPIO | Notes |
|---|---|---|---|
| 1 | GND | No | Ground |
| 2 | GND | No | Ground |
| 3 | P1.10 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 4 | P1.11 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 5 | P1.12 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 6 | P1.13 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 7 | P1.14 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 8 | P1.15 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 9 | P0.03 / AIN1 | Yes | Standard drive, low-frequency I/O only; also analog input AIN1 |
| 10 | P0.29 / AIN5 | Yes | Standard drive, low-frequency I/O only; also analog input AIN5 |
| 11 | P0.02 / AIN0 | Yes | Standard drive, low-frequency I/O only; also analog input AIN0 |
| 12 | P0.31 / AIN7 | Yes | Standard drive, low-frequency I/O only; also analog input AIN7 |
| 13 | P0.28 / AIN4 | Yes | Standard drive, low-frequency I/O only; also analog input AIN4 |
| 14 | P0.30 / AIN6 | Yes | Standard drive, low-frequency I/O only; also analog input AIN6 |
| 15 | GND | No | Ground |
| 16 | P0.27 | Yes | General-purpose I/O |
| 17 | P0.00 / XL1 | Yes | General-purpose I/O; alt function is 32.768kHz crystal connection (unused unless you add an external 32kHz crystal) |
| 18 | P0.01 / XL2 | Yes | General-purpose I/O; alt function is 32.768kHz crystal connection (unused unless you add an external 32kHz crystal) |
| 19 | P0.26 | Yes | General-purpose I/O |
| 20 | P0.04 / AIN2 | Yes | Also analog input AIN2 |
| 21 | P0.05 / AIN3 | Yes | Also analog input AIN3 |
| 22 | P0.06 | Yes | General-purpose I/O |
| 23 | P0.07 / TRACECLK | Yes | Also trace buffer clock (only relevant if using debug trace) |
| 24 | P0.08 | Yes | General-purpose I/O |
| 25 | P1.08 | Yes | General-purpose I/O |
| 26 | P1.09 / TRACEDATA3 | Yes | Also trace buffer TRACEDATA[3] |
| 27 | P0.11 / TRACEDATA2 | Yes | Also trace buffer TRACEDATA[2] |
| 28 | VDD | No | Power supply |
| 29 | P0.12 / TRACEDATA1 | Yes | Also trace buffer TRACEDATA[1] |
| 30 | VDDH | No | High voltage power supply |
| 31 | DCCH | No | DC/DC converter output |
| 32 | VBUS | No | 5V input for USB 3.3V regulator |
| 33 | GND | No | Ground |
| 34 | D− | No | Dedicated USB D- |
| 35 | D+ | No | Dedicated USB D+ |
| 36 | P0.14 | Yes | General-purpose I/O |
| 37 | P0.13 | Yes | General-purpose I/O |
| 38 | P0.16 | Yes | General-purpose I/O |
| 39 | P0.15 | Yes | General-purpose I/O |
| 40 | P0.18 / nRESET | Yes | Defaults to system RESET; must be reconfigured (UICR) to use as GPIO |
| 41 | P0.17 | Yes | General-purpose I/O |
| 42 | P0.19 | Yes | Recommended usage QSPI/SCK, but usable as plain GPIO if QSPI unused |
| 43 | P0.21 | Yes | Recommended usage QSPI |
| 44 | P0.20 | Yes | General-purpose I/O |
| 45 | P0.23 | Yes | Recommended usage QSPI |
| 46 | P0.22 | Yes | Recommended usage QSPI |
| 47 | P1.00 / TRACEDATA0 | Yes | Recommended usage QSPI; also trace buffer TRACEDATA[0] |
| 48 | P0.24 | Yes | General-purpose I/O |
| 49 | P0.25 | Yes | General-purpose I/O |
| 50 | P1.02 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 51 | SWDIO | No | Dedicated serial wire debug I/O |
| 52 | P0.09 / NFC1 | Yes | Standard drive, low-frequency I/O only; alt function NFC antenna pin (unavailable as GPIO if NFC is enabled) |
| 53 | SWDCLK | No | Dedicated serial wire debug clock |
| 54 | P0.10 / NFC2 | Yes | Standard drive, low-frequency I/O only; alt function NFC antenna pin (unavailable as GPIO if NFC is enabled) |
| 55 | GND | No | Ground |
| 56 | P1.04 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 57 | P1.06 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 58 | P1.07 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 59 | P1.05 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 60 | P1.03 | Yes | Standard drive, low-frequency I/O only (near radio) |
| 61 | P1.01 | Yes | Standard drive, low-frequency I/O only (near radio) |

## Notes

- **Low-frequency I/O only** pins (all P1.01–P1.15 except P1.00, plus P0.02, P0.03, P0.09, P0.10, P0.28–P0.31) are located near the radio. They support digital toggle/read but should be limited to signals up to ~10 kHz for best RF performance — SPI, I2C, UART, and PWM are explicitly called out as NOT low-frequency and should be avoided on these pins.
- **P0.09/P0.10 (NFC1/NFC2)** default to NFC antenna functionality on power-up; to use them as plain GPIO you must disable NFC via the `UICR.NFCPINS` register (or the equivalent Zephyr devicetree/Kconfig setting).
- **P0.18/nRESET** defaults to hardware reset; to reuse as GPIO, reconfigure `UICR.PSELRESET` (removes reset functionality).
- **P0.00/XL1, P0.01/XL2** are free general-purpose I/O on this module since no external 32.768 kHz crystal is fitted — only the 32 MHz crystal is built in.
- **SWDIO/SWDCLK** are dedicated debug pins on this module (not listed with an alternate P0.xx/P1.xx designation), so they're excluded here as GPIO-usable; reserve them for programming/debug unless you specifically intend to give up SWD access.
- **D+/D−, VBUS** are dedicated USB pins, not general GPIO.
