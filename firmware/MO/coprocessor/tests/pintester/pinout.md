# MDBT50Q-1MV2 / MDBT50Q-P1MV2 (nRF52840) Pinout

Source: Raytac MDBT50Q-1MV2 & MDBT50Q-P1MV2 datasheet, Ver. L (2023/05/24), section 2.5 Pin Assignment.

61-pad module. "Usable as GPIO" = pin can be driven/read as a digital I/O (`nrf_gpio` / Zephyr GPIO), even if it also has an alternate function. "TH-1/MO usage" = net this pin is wired to on the TH-1/MO carrier board (per `connections.md` and `mo.dts`); these are marked do not test since something on the board already drives/reads them.

| Pin No. | Name | Usable as GPIO | TH-1/MO usage | Notes |
|---|---|---|---|---|
| 1 | GND | No | — | Ground |
| 2 | GND | No | — | Ground |
| 3 | P1.10 | Yes | Do not test — MOD3 (Pi GPIO24) | Standard drive, low-frequency I/O only (near radio) |
| 4 | P1.11 | Yes | Do not test — MOD1 (Pi GPIO8) | Standard drive, low-frequency I/O only (near radio) |
| 5 | P1.12 | Yes | Do not test — MOD2 (Pi GPIO25) | Standard drive, low-frequency I/O only (near radio) |
| 6 | P1.13 | Yes | Do not test — MOD0 (Pi GPIO7) | Standard drive, low-frequency I/O only (near radio) |
| 7 | P1.14 | Yes | Do not test — MOD4, Pi power sense (+3V3_PI via R2, 1k) | Standard drive, low-frequency I/O only (near radio) |
| 8 | P1.15 | Yes | — | Standard drive, low-frequency I/O only (near radio) |
| 9 | P0.03 / AIN1 | Yes | — | Standard drive, low-frequency I/O only; also analog input AIN1 |
| 10 | P0.29 / AIN5 | Yes | — | Standard drive, low-frequency I/O only; also analog input AIN5 |
| 11 | P0.02 / AIN0 | Yes | — | Standard drive, low-frequency I/O only; also analog input AIN0 |
| 12 | P0.31 / AIN7 | Yes | — | Standard drive, low-frequency I/O only; also analog input AIN7 |
| 13 | P0.28 / AIN4 | Yes | — | Standard drive, low-frequency I/O only; also analog input AIN4 |
| 14 | P0.30 / AIN6 | Yes | — | Standard drive, low-frequency I/O only; also analog input AIN6 |
| 15 | GND | No | — | Ground |
| 16 | P0.27 | Yes | — | General-purpose I/O |
| 17 | P0.00 / XL1 | Yes | Do not test — 32.768kHz crystal | General-purpose I/O; alt function is 32.768kHz crystal connection |
| 18 | P0.01 / XL2 | Yes | Do not test — 32.768kHz crystal | General-purpose I/O; alt function is 32.768kHz crystal connection |
| 19 | P0.26 | Yes | — | General-purpose I/O |
| 20 | P0.04 / AIN2 | Yes | — | Also analog input AIN2 |
| 21 | P0.05 / AIN3 | Yes | — | Also analog input AIN3 |
| 22 | P0.06 | Yes | — | General-purpose I/O |
| 23 | P0.07 / TRACECLK | Yes | — | Also trace buffer clock (only relevant if using debug trace) |
| 24 | P0.08 | Yes | — | General-purpose I/O |
| 25 | P1.08 | Yes | — | General-purpose I/O |
| 26 | P1.09 / TRACEDATA3 | Yes | — | Also trace buffer TRACEDATA[3] |
| 27 | P0.11 / TRACEDATA2 | Yes | — | Also trace buffer TRACEDATA[2] |
| 28 | VDD | No | — | Power supply |
| 29 | P0.12 / TRACEDATA1 | Yes | — | Also trace buffer TRACEDATA[1] |
| 30 | VDDH | No | — | High voltage power supply |
| 31 | DCCH | No | — | DC/DC converter output |
| 32 | VBUS | No | — | 5V input for USB 3.3V regulator |
| 33 | GND | No | — | Ground |
| 34 | D− | No | — | Dedicated USB D- |
| 35 | D+ | No | — | Dedicated USB D+ |
| 36 | P0.14 | Yes | — | General-purpose I/O |
| 37 | P0.13 | Yes | — | General-purpose I/O |
| 38 | P0.16 | Yes | — | General-purpose I/O |
| 39 | P0.15 | Yes | — | General-purpose I/O |
| 40 | P0.18 / nRESET | Yes | Do not test — nRESET (Pi GPIO16) | Defaults to system RESET; must be reconfigured (UICR) to use as GPIO |
| 41 | P0.17 | Yes | — | General-purpose I/O |
| 42 | P0.19 | Yes | — | Recommended usage QSPI/SCK, but usable as plain GPIO if QSPI unused |
| 43 | P0.21 | Yes | — | Recommended usage QSPI |
| 44 | P0.20 | Yes | — | General-purpose I/O |
| 45 | P0.23 | Yes | Do not test — IR_TX | Recommended usage QSPI |
| 46 | P0.22 | Yes | Do not test — IR_RX | Recommended usage QSPI |
| 47 | P1.00 / TRACEDATA0 | Yes | Do not test — UART0 RX (nRF), U0_TX net | Recommended usage QSPI; also trace buffer TRACEDATA[0] |
| 48 | P0.24 | Yes | Do not test — UART0 TX (nRF), U0_RX net | General-purpose I/O |
| 49 | P0.25 | Yes | Do not test — UART1 TX (nRF), U1_RX net | General-purpose I/O |
| 50 | P1.02 | Yes | Do not test — UART1 RX (nRF), U1_TX net | Standard drive, low-frequency I/O only (near radio) |
| 51 | SWDIO | No | — | Dedicated serial wire debug I/O |
| 52 | P0.09 / NFC1 | Yes | Do not test — unconnected, NFC-owned, low priority | Standard drive, low-frequency I/O only; alt function NFC antenna pin (unavailable as GPIO if NFC is enabled) |
| 53 | SWDCLK | No | — | Dedicated serial wire debug clock |
| 54 | P0.10 / NFC2 | Yes | Do not test — unconnected, NFC-owned, low priority | Standard drive, low-frequency I/O only; alt function NFC antenna pin (unavailable as GPIO if NFC is enabled) |
| 55 | GND | No | — | Ground |
| 56 | P1.04 | Yes | — | Standard drive, low-frequency I/O only (near radio) |
| 57 | P1.06 | Yes | — | Standard drive, low-frequency I/O only (near radio) |
| 58 | P1.07 | Yes | — | Standard drive, low-frequency I/O only (near radio) |
| 59 | P1.05 | Yes | — | Standard drive, low-frequency I/O only (near radio) |
| 60 | P1.03 | Yes | — | Standard drive, low-frequency I/O only (near radio) |
| 61 | P1.01 | Yes | — | Standard drive, low-frequency I/O only (near radio) |

## Notes

- **Low-frequency I/O only** pins (all P1.01–P1.15 except P1.00, plus P0.02, P0.03, P0.09, P0.10, P0.28–P0.31) are located near the radio. They support digital toggle/read but should be limited to signals up to ~10 kHz for best RF performance — SPI, I2C, UART, and PWM are explicitly called out as NOT low-frequency and should be avoided on these pins.
- **P0.09/P0.10 (NFC1/NFC2)** default to NFC antenna functionality on power-up; to use them as plain GPIO you must disable NFC via the `UICR.NFCPINS` register (or the equivalent Zephyr devicetree/Kconfig setting). Confirmed via netlist (`~/TH-1-MO/pcb/v0.1/mainboard.kicad_sch`, KiCad `unconnected-(...)` nets) that neither pin is wired to anything on TH-1/MO. `pintester` doesn't test them: getting the UICR write to actually apply (the devicetree `&uicr { nfct-pins-as-gpios; }` property has no effect on nRF52840 in this Zephyr version — see project notes) isn't worth it for two unpopulated pads, and a solder bridge on either one, to each other or to an inactive neighbor, wasn't judged worth catching.
- **P0.18/nRESET** defaults to hardware reset; to reuse as GPIO, reconfigure `UICR.PSELRESET` (removes reset functionality). `mo.dts`'s `&uicr { gpio-as-nreset; }` is meant to set this, but like `nfct-pins-as-gpios` above, this property has no effect on nRF52840 in this Zephyr version (same dead `nordic,nrf-uicr` binding — the UICR generator that would consume it is gated to nRF54H/nRF92 only). P0.18 is not actually configured as a hardware reset pin right now. Not currently a problem: OpenOCD's nRF52 reset flow goes through CTRL-AP/SYSRESETREQ over SWD, not the physical SRST line, so nothing depends on this working.
- **P0.00/XL1, P0.01/XL2** are wired to a populated external 32.768 kHz crystal on TH-1/MO — confirmed present on this board, not general-purpose I/O here.
- **SWDIO/SWDCLK** are dedicated debug pins on this module (not listed with an alternate P0.xx/P1.xx designation), so they're excluded here as GPIO-usable; reserve them for programming/debug unless you specifically intend to give up SWD access.
- **D+/D−, VBUS** are dedicated USB pins, not general GPIO.
- **"Do not test" pins above** are ones the TH-1/MO board already wires to something (Pi UART, mode-select lines, reset, or the onboard IR transceiver) — the pin bridge tester (`pintester`) skips these. `pintester`'s own excluded list (in `src/pinout.c`) still has its original UART0/crystal exclusions carried over unreviewed; those get reconciled against this table one at a time, not all at once.
