- [x] SD card reader
- [x] Power button for the raspberry
- [x] i2s audio
- [x] 3v3 converter
- [x] Wire up DAC to raspberry pi.
- [x] Clean up and unify power lines
- [x] Add additional power pins so PD can be bypassed
- [x] Add test pad for nRESET
- [x] High side switch so DAC is not wasting power when pi is off.
- [ ] Double check NRF setup
    - Verify power wiring
    - does nRESET need to be pulled high?
    - should we add a way to detect if the pi is on?
- [x] Ferrite bead for the NRF
- [ ] Ferrite bead for 3v3 out (or make it pi rail)
- [ ] Add NRF test points where bridging is not testable
- [ ] Wire up USB connectors
- [ ] Signal analyser pins
- [ ] find part for display connector
- [ ] find part for HDMI
- [ ] Deal with the current limit ICs
- [ ] ESD protection
- [ ] Remove unused pins


# DAC notes

1. Digital and analog power
There are two separate power rails in the DAC chip. One for powering the digital side and one for powering the analog side. These are fed from the same source. While they could be connected together, I should get better output by having something filtering in between the two power sources. My LLM suggests either a small resistor or some sort of inductor? Need to look into it a bit mroel.

2. Mute
Power loss can create a big pop sound. To mitigate this there is a mute pin on the chip. This needs about 0.2ms of "warning" before the power goes out.
The way I've seen it done on other designs is by tying it to the source pre regulator (5v) with some voltage dividers. This along with the capacitors already there should cut the power to the mute pin before it does to the rest of the system.

Could potentially test this out?

It seems like we will def need a preamp and many of them already have anti pop filters. This system might not be needed, but not sure yet.

a: we are not using a preamp
b: yes it should still be implemented

Additionally since its sitting on the 5v rail, which is noisy from the CM5, it should also have a capacitor.
Additionally, if not already, then the regulator also needs a capacitor

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

2. SWD

SWD will be nominally wired to the raspberry pi. I think it should also have a dedicated pin (separate from pinout) for swd. Both of these inputs should be gated by a solder jumper so you can choose which to enable.

# 3v3 regulator notes

1. DAC mute handling

I've not gotten to the regulator yet so not sure if its already included, but if not, it should have a larger capacitor on it to give the DAC enough time to mute when the power goes out. Should be at least 10uf I think, but at any rate should be orders of magnitude larger than the capacitor on XSMT.






# potential downfalls

In this section I'm documenting deviations from the datasheets or potential issues to look at first if the PCB comes out broken.


## 1. Pop on power down DAC

Its possible that the XSMT capacitor is too large and does not power down in enough time to avoid a pop. This will happen if the capacitor on the voltage regulator drains faster than the one on XSMT.
