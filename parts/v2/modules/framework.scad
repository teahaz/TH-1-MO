// Framework Expansion Card
// Simplified from Framework Computer LLC's Parametric Expansion Card
// Original © 2021 Nirav Patel at Framework Computer LLC
// Licensed under CC BY 4.0 — http://creativecommons.org/licenses/by/4.0/

fw_exp = [30.0, 32.0, 6.8];

// USB-C plug dimensions
_fw_usb_c_r = 1.315;
_fw_usb_c_w = 5.86 + _fw_usb_c_r * 2;
_fw_usb_c_h = 2.2;

// Internal: USB-C plug cutout shape (in card-local space)
module _fw_usb_c_cutout(open_top) {
    translate([-_fw_usb_c_w/2 + _fw_usb_c_r, 7-10+0.6, _fw_usb_c_r])
    rotate([-90, 0, 0]) union() {
        translate([0, _fw_usb_c_r, 0])
            cylinder(r = _fw_usb_c_r, h = 10, $fn = 64);
        translate([_fw_usb_c_w - _fw_usb_c_r*2, _fw_usb_c_r, 0])
            cylinder(r = _fw_usb_c_r, h = 10, $fn = 64);
        cube([_fw_usb_c_w - _fw_usb_c_r*2, _fw_usb_c_r*2, 10]);

        // Pin-side shell expansion
        translate([0, _fw_usb_c_r, 0])
            cylinder(r2 = _fw_usb_c_r, r1 = 3.84/2, h = 10-7.7, $fn = 64);
        translate([_fw_usb_c_w - _fw_usb_c_r*2, _fw_usb_c_r, 0])
            cylinder(r2 = _fw_usb_c_r, r1 = 3.84/2, h = 10-7.7, $fn = 64);
        translate([_fw_usb_c_w/2 - _fw_usb_c_r, _fw_usb_c_r, 0])
            scale([1.8, 1, 1]) rotate([0, 0, 45])
            cylinder(r2 = _fw_usb_c_r*sqrt(2), r1 = 3.84/2*sqrt(2), h = 10-7.7, $fn = 4);

        if (open_top) {
            translate([-_fw_usb_c_r, -10 + _fw_usb_c_r, 0])
                cube([_fw_usb_c_w, 10, 10]);
        }
    }
}

// Framework Expansion Card
//   cutout=false: a silver box of the card dimensions
//   cutout=true:  volume to subtract, including USB-C opening
//
// Oriented with the card sliding in along -Z (USB-C end at bottom),
// origin at the card's front-top-left corner — matching FrameworkGrid usage.
module Framework(cutout=false) {
    e = 0.01;

    if (cutout) {
        translate([0, -fw_exp[2], -e-1])
        mirror([0, 1, 0])
        rotate([90, 0, 0]) {
            cube([fw_exp[0]+0.1, fw_exp[1]+1.1, fw_exp[2]*2]);

            translate([fw_exp[0]/2, fw_exp[1], _fw_usb_c_r + _fw_usb_c_h])
                _fw_usb_c_cutout(false);
        }
    } else {
        rotate([90, 0, 0])
            color("silver")
            cube(fw_exp);
    }
}
