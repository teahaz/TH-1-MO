// Framework Expansion Card
// Simplified from Framework Computer LLC's Parametric Expansion Card
// Original © 2021 Nirav Patel at Framework Computer LLC
// Licensed under CC BY 4.0 — http://creativecommons.org/licenses/by/4.0/

fw_exp = [30.0, 32.0, 6.8];

// NopSCADlib usb_C connector height (h inside usb_C module)
_usb_c_h = 3.26;

margin = 0.2;

module Framework(cutout=false) {
    translate([0, -fw_exp[2], -e])
    mirror([0, 1, 0])
    rotate([90, 0, 0]) {
        if (cutout) {
            translate([-margin, -margin, 0])
                cube([fw_exp[0]+2*margin, fw_exp[1]+margin, fw_exp[2]*2]);
        }
        translate([fw_exp[0]/2, fw_exp[1]+(cutout ? 0 : 6), fw_exp[2]/2 - _usb_c_h/2])
        rotate([0, 0, 90])
        intersection() {
            cube([20, 10, 10], center=true);
            usb_C(cutout=true);
        }
    }

    if (!cutout) {
        rotate([90, 0, 0])
            color("silver")
            cube(fw_exp);
    }
}

FW_M = 3;

module FrameworkGrid(bay=false) {
    width = BODY_W - 2*(BODY_T+CORE_T+FW_M);
    height = fw_exp[2]+1;
    gap = width - 2*fw_exp[0];

    difference() {
        if (bay) translate([BODY_T+CORE_M+CORE_T, BODY_D, 2.8]) {
            mirror([0, 1, 0]) cube([width+1, height+1, fw_exp[1]+1]);
        }
    
        translate([BODY_T+CORE_T+FW_M-1, BODY_D-BODY_T, 1]) {
            Grid(
                grid = [2, 1],
                size = [fw_exp[0], fw_exp[2]],
                space = [width, height]
            ) {
                children();
            }
        }
    }
}
