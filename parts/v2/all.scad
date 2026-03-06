include <NopSCADlib/core.scad>
include <NopSCADlib/vitamins/pcbs.scad>
include <NopSCADlib/vitamins/rockers.scad>
include <NopSCADlib/vitamins/rocker.scad>

include <modules/screen.scad>
include <modules/keyboard.scad>
include <modules/framework.scad>
include <modules/pins.scad>

// TODO: Pass avail width to modules and let them center themselves

MARGIN = 2.5;
CASE = 2;
RADIUS = 5;

FP_MARGIN = 2.5;
FP_DEPTH = 1;

INNER_WIDTH = 81.674+2*MARGIN;
INNER_HEIGHT = 122+2*MARGIN;

FP_WIDTH = INNER_WIDTH;
FP_HEIGHT = INNER_HEIGHT;

WIDTH = FP_WIDTH;
HEIGHT = FP_HEIGHT;
DEPTH = 20;

FW_MARGIN = 8;

module _assembly(cutout) {
    translate([FP_MARGIN, 0, FP_MARGIN]) {
        Keyboard(cutout=cutout);
        translate([0, 0, KB_HEIGHT])
            Screen(cutout=cutout);
    }
}

module Frontplate(cutout=false) {
    color("silver") difference() {
        rounded_cube_xz([WIDTH, FP_DEPTH+(cutout ? 10 : 0), HEIGHT], RADIUS);
        translate([0, FP_DEPTH+e, 0]) _assembly(cutout=true);
    }
}

module Inserts(cutout=false) {
    depth = 18;
    b_cutout = cutout ? 1 : 0;

    translate([0, DEPTH, HEIGHT*0.85])
        Pins(cutout=cutout);

    if (cutout) {
        translate([FW_MARGIN+3, depth/2-3.5, HEIGHT-usb_c_w])
            mirror([0, 0, b_cutout])
            rotate([0, 90, 90])
            jack(cutout=cutout);

        translate([FW_MARGIN+15, depth/2-usb_c_h, HEIGHT-usb_c_w])
            mirror([0, 0, b_cutout])
            rotate([0, 90, 90])
            usb_C(cutout=cutout);
    }

    translate([WIDTH-FW_MARGIN-8, depth/2, HEIGHT])
        rotate([0, 0, 90])
        rocker(micro_rocker, colour="green");

    *translate([FP_MARGIN+FW_MARGIN, DEPTH-1, 1]) {
        Grid(
            grid=[2, 1],
            size=[fw_exp[0], fw_exp[2]],
            space=[INNER_WIDTH-2*(FP_MARGIN+FW_MARGIN), INNER_HEIGHT]
        ) {
            color("silver") FrameworkExpansion();
        }
    }
}

module SpeakerCutout() {
    length = 4;
    width = 1;

    render() translate([-e, 4, HEIGHT*0.51]) rotate([0, 0, 90]) {
        Grid([6, 10], [1, 1], [DEPTH/2, HEIGHT*0.35]) {
            mirror([0, 1, 0]) cube([width, 5, length]);
        }
    }

    render() translate([WIDTH+e, DEPTH/2-6, HEIGHT*0.51]) rotate([0, 0, 90]) {
        Grid([6, 10], [1, 1], [DEPTH/2, HEIGHT*0.35]) {
            cube([width, 5, length]);
        }
    }
}

module Case() {
    difference() {
        color("white")
            translate([0, -RADIUS, 0])
            Rounded([WIDTH, DEPTH+RADIUS, HEIGHT], RADIUS);

        translate([CASE/2, -RADIUS, CASE/2])
            Rounded([WIDTH-CASE, DEPTH+RADIUS-CASE, HEIGHT-CASE], RADIUS-CASE);

        translate([0, -10+e, 0])
            cube([FP_WIDTH+e, 10+FP_DEPTH, FP_HEIGHT+e]);

        translate([FP_MARGIN+FW_MARGIN, DEPTH-1, 1]) {
            Grid(
                grid=[2, 1],
                size=[fw_exp[0], fw_exp[2]],
                space=[INNER_WIDTH-2*(FP_MARGIN+FW_MARGIN), INNER_HEIGHT]
            ) {
                FrameworkVolume();
            }
        }

        Inserts(cutout=true);

        SpeakerCutout();
    }
}

union() {
    Case();

    Inserts();
    Frontplate();

    translate([0, FP_DEPTH, 0])
        _assembly(cutout=false);
}
