// TH-A1OC: Basic keyboard module
include <parts.scad>

translate([CASE_THICKNESS+MODULE_MARGIN, FPT_D, CASE_THICKNESS+MODULE_MARGIN])
    Keyboard();
