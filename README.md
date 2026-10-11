# TH-1/MO

Base computing platform for the TH-1.

> Teahaz 1: Modular Operator

---

The inagural hardware project for the Teaház Computer Company. The TH-1 is a modular computer made with custom components. It runs its own distribution of Linux, and uses a terminal-based interface system named CEL for its UI. All of its apps are built on CEL, including everything from configuration to the built in software.

The primary physical inspirations are pre-smartphone electronics and the unrealized future predictions made by the likes of Star Wars, Evangelion and Marathon (2026). The UI takes more inspiration from these properties than it does existing software, drawing from their high-contrast, high-saturation vector graphics and turns them from movie props to usable UI.

The core of the TH-1's design are three modular interfaces: one keyboard slot, 2 Framework Expansion Cards and a set of GPIO pins with associated mounting hardware. All of these are hot-swappable open standards, and together with our software suite allow for bespoke configurations for any usecase. For example, the default keyboard module uses a standard 50% layout, but it could be replaced by one tailor made for music creation in a specialized CEL application, or even game-specific input hardware.

![th_core](https://github.com/user-attachments/assets/d86a45d6-040c-4b14-a7cd-5f6938ff506a)



## Components

TH-1-MO is based of the raspberry pi CM5. Additionally it includes an [NRF based co-processor](https://github.com/teahaz/TH-1-PDB), [a keyboard](https://github.com/teahaz/TH-1-KB50), and 2 Lithium cells with a custom [battery management system](https://github.com/teahaz/TH-1-BMSS).

<img width="585" height="978" alt="image" src="https://github.com/user-attachments/assets/dba8decb-a698-440c-957e-cb0578c5f4a0" />


Simplified schematic-like diagram of the core TH-1 components (not a real schematic)
