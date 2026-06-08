# schematic
- [x] SD card reader
- [x] Power button for the raspberry
- [ ] 3v3 converter
- [ ] PD sync
- [ ] Alternate power pins (2-3 each for higher current)
- [ ] i2s audio
- [ ] Wire up USB connectors
- [ ] Signal analyser pins
    - can these be combined with other gpio when not in signal analyser mode?
    - how will it work with the resistors?
    - I anyway need to figure out the correct resistors here.
- [ ] find part for display connector
- [ ] find part for HDMI

# layout
- [ ] Dac chip
    - Use layout guidelines for the DAC chip
    - Island with single contact for power plane
    - keep main ground plane


# DAC notes

1. Digital and analog power
There are two separate power rails in the DAC chip. One for powering the digital side and one for powering the analog side. These are fed from the same source. While they could be connected together, I should get better output by having something filtering in between the two power sources. My LLM suggests either a small resistor or some sort of inductor? Need to look into it a bit mroel.

2. Mute
Power loss can create a big pop sound. To mitigate this there is a mute pin on the chip. This needs about 0.2ms of "warning" before the power goes out.
The way I've seen it done on other designs is by tying it to the source pre regulator (5v) with some voltage dividers. This along with the capacitors already there should cut the power to the mute pin before it does to the rest of the system.

Could potentially test this out?


3. Clean routing
For clean power the whole DAC system should be on a sort of island power plane. It should get 3v3 from the same LDO source, but with only one connection point to its own power plane rather than sitting on the main power plane.

The ground plane should still be unified with the rest

4. output power
I am not sure if this will provide a good enough signal to run my high impedance headphones cleanly, but thats okay for now.
