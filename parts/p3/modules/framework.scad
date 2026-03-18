// Framework Expansion Card
// Simplified from Framework Computer LLC's Parametric Expansion Card
// Original © 2021 Nirav Patel at Framework Computer LLC
// Licensed under CC BY 4.0 — http://creativecommons.org/licenses/by/4.0/

fw_exp = [30.0, 32.0, 6.8];

// NopSCADlib usb_C connector height (h inside usb_C module)
_usb_c_h = 3.26;

module Framework(cutout=false) {
    translate([0, -fw_exp[2], -e-1])
    mirror([0, 1, 0])
    rotate([90, 0, 0]) {
        if (cutout) {
            cube([fw_exp[0]+0.1, fw_exp[1]+1.1, fw_exp[2]*2]);
        } else {
            translate([fw_exp[0]/2, fw_exp[1]+(cutout ? 0 : 6), fw_exp[2]/2 - _usb_c_h/2])
                rotate([0, 0, 90])
                usb_C(cutout=cutout);
        }
    }

    if (!cutout) {
        rotate([90, 0, 0])
            color("silver")
            cube(fw_exp);
    }
}

FW_M = 3;

module FrameworkGrid() {
    translate([BODY_T+CORE_T+FW_M, BODY_D-BODY_T-2, 1]) {
        Grid(
            grid = [2, 1],
            size = [fw_exp[0], fw_exp[2]],
            space = [BODY_W - 2*(BODY_T+CORE_T+FW_M), BODY_H]
        ) {
            children();
        }
    }
}
