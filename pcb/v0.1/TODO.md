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


# DAC notes

1. Digital and analog power
There are two separate power rails in the DAC chip. One for powering the digital side and one for powering the analog side. These are fed from the same source. While they could be connected together, I should get better output by having something filtering in between the two power sources. My LLM suggests either a small resistor or some sort of inductor? Need to look into it a bit mroel.

2. Mute
Power loss can create a big pop sound. To mitigate this there is a mute pin on the chip. This needs about 0.2ms of "warning" before the power goes out.
The way I've seen it done on other designs is by tying it to the source pre regulator (5v) with some voltage dividers. This along with the capacitors already there should cut the power to the mute pin before it does to the rest of the system.

Could potentially test this out?


3. Clean power

For clean power, the DAC system needs its own island power plane so it does not get mixed with the main one. In fact it needs two of them, one for the analog side and one for the digital side. Additionally, its worth beading every other noisy device on the circuit like the nrf chip. It should ideally sit further away from the RF/noisy components as well.


The ground plane should still be unified with the rest. as long as there is no return paths going right under this

4. output power
I am not sure if this will provide a good enough signal to run my high impedance headphones cleanly, but thats okay for now.


5. Series terminating resistors
Its unclear if its absolutely needed, but might be a good just in case on the signal lines, specifically BCK

6. AVDD capacitor

This absolutely must be as close to the pin as possible.


7. Double capacitors

The datasheet reference implementation has a 0.1uF ceramic and 10uf electrolytic capacitor in parallel at multiple different points. Apparently this is in large part because large ceramic capacitors were expensive at the time this datasheet was written. Now they are not, and should be replaced by a single 10uf

# nRF notes

1. Clean power
This is just a reminder taken over from the DAC notes. The nRF needs a ferrite bead on its power lines so it doesn't dirty up the power plane for the DAC when its doing RF.
