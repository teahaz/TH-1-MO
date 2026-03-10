include <NopSCADlib/core.scad>
include <NopSCADlib/vitamins/pcbs.scad>
include <NopSCADlib/vitamins/rockers.scad>
include <NopSCADlib/vitamins/rocker.scad>

include <modules/screen.scad>
include <modules/keyboard.scad>
include <modules/framework.scad>
include <modules/pins.scad>

INNER_WIDTH = 81.674;
CUTOUT_HEIGHT = 121.05;

CASE = 1.5;
CORE = 1;

WIDTH = INNER_WIDTH + 2*(CASE+CORE);
BORDER = (WIDTH - 80) / 2;
HEIGHT = CUTOUT_HEIGHT + 2*BORDER;
DEPTH = 20;

RADIUS = 5;
CORE_MARGIN = 0.4;

FP_MARGIN = 0;
FP_DEPTH = 2;
FP_WIDTH = WIDTH - FP_MARGIN - CASE*2 - 2*CORE_MARGIN;
FP_HEIGHT = HEIGHT - FP_MARGIN - CASE*2 - 2*CORE_MARGIN;

FW_MARGIN = 8;

module FrameworkGrid() {
    translate([CASE + CORE + FW_MARGIN, DEPTH - 1, 1]) {
        Grid(
            grid = [2, 1],
            size = [fw_exp[0], fw_exp[2]],
            space = [WIDTH - 2*(CASE + CORE + FW_MARGIN), HEIGHT]
        ) {
            children();
        }
    }
}

module _assembly(cutout) {
    translate([0, 0, BORDER]) {
        Keyboard(cutout=cutout, avail_width=WIDTH);
        translate([0, 0, KB_HEIGHT])
            Screen(cutout=cutout, avail_width=WIDTH);
    }
}

module Clips(cutout=false) {
    margin = cutout ? 0.1 : 0;

    translate([WIDTH*0.2, 0, CASE]) {
        cube([3+margin, 2, 1]);
        translate([0, -1, 0]) cube([3+margin, 1, 2]);
    }

    translate([WIDTH*0.8-3, 0, CASE]) {
        cube([3+margin, 2, 1]);
        translate([0, -1, 0]) cube([3+margin, 1, 2]);
    }

    translate([WIDTH*0.8-3, 0, HEIGHT-CASE-1.84]) {
        cube([3+margin, 2, 1]);
        translate([0, -1, 0]) cube([3+margin, 1, 1.84]);
    }

    translate([WIDTH*0.2, 0, HEIGHT-CASE-1.84]) {
        cube([3+margin, 2, 1]);
        translate([0, -1, 0]) cube([3+margin, 1, 1.84]);
    }
}

module Frontplate(cutout=false) {
    color("silver") difference() {
        translate([CASE+CORE_MARGIN, 0, CASE+CORE_MARGIN])
            rounded_cube_xz([FP_WIDTH, FP_DEPTH+(cutout ? 10 : 0)-e, FP_HEIGHT], RADIUS-CASE);

        translate([0, FP_DEPTH+e, 0])
            _assembly(cutout=true);

        Clips(cutout=true);
    }
}

module Inserts(cutout=false) {
    depth = 18;
    b_cutout = cutout ? 1 : 0;

    jack_d = 6;
    jack_h = 6;
    usbc_w = 8.94;
    usbc_h = 3.26;

    core_top = HEIGHT - CASE - CORE_MARGIN - CORE;

    translate([0, DEPTH, HEIGHT*0.85])
        Pins(cutout=cutout, avail_width=WIDTH);

    translate([FW_MARGIN, (depth-jack_d)/2, core_top-9])
        mirror([0, 0, 1])
        rotate([0, 90, 90])
        jack(cutout=cutout);

    translate([FW_MARGIN + jack_d + 6, (depth-usbc_h)/2, HEIGHT-CASE-usbc_w/2-1])
        mirror([0, 0, 1])
        rotate([0, 90, 90])
        usb_C(cutout=cutout);

    translate([WIDTH-FW_MARGIN-8, depth/2, core_top+2.9])
        rotate([0, 0, 90])
        rocker(micro_rocker, colour="green");

    *FrameworkGrid() {
        Framework();
    }
}

module SpeakerCutout(filled=false) {
    length = 4;
    width = 1;

    render() translate([-e, 4, HEIGHT*0.51]) rotate([0, 0, 90]) {
        if (filled) {
            mirror([0, 1, 0]) cube([DEPTH/2, 5, HEIGHT*0.3735]);
        } else {
            Grid([6, 10], [1, 1], [DEPTH/2, HEIGHT*0.35]) {
                mirror([0, 1, 0]) cube([width, 5, length]);
            }
        }
    }

    render() translate([WIDTH+e, DEPTH/2-6, HEIGHT*0.51]) rotate([0, 0, 90]) {
        if (filled) {
            cube([DEPTH/2, 5, HEIGHT*0.3735]);
        } else {
            Grid([6, 10], [1, 1], [DEPTH/2, HEIGHT*0.35]) {
                cube([width, 5, length]);
            }
        }
    }
}

module Core(cutout=false) {
    width = WIDTH-2*CASE;
    height = HEIGHT-2*CASE;
    depth = DEPTH-CASE;

    if (cutout) {
        translate([CASE, -RADIUS, CASE])
            Rounded([width, DEPTH+RADIUS-CASE, height], RADIUS-CASE);
    } else {
        let (width = width-CORE_MARGIN*2)
        let (height = height-CORE_MARGIN*2)
        union() {
            render() difference() {
                translate([CASE+CORE_MARGIN, FP_DEPTH+e, CASE+CORE_MARGIN]) union() {
                    Rounded([width, depth-FP_DEPTH, height], RADIUS-CASE-CORE_MARGIN);
                    rounded_cube_xz([width, RADIUS, height], RADIUS-CASE-CORE_MARGIN);
                }

                translate([CASE+CORE_MARGIN+CORE, 0, CASE+CORE_MARGIN+CORE]) {
                    Rounded([
                        width-2*CORE,
                        depth-CORE,
                        height-2*CORE
                    ], RADIUS-CASE-CORE_MARGIN-CORE);

                    rounded_cube_xz([width-2*CORE, RADIUS, height-2*CORE], RADIUS-CASE-CORE_MARGIN-CORE);
                }

                Inserts(cutout=true);
                SpeakerCutout(filled=true);
                FrameworkGrid() { Framework(cutout=true); }
            }

            render() difference() {
                intersection() {
                    translate([CASE+CORE_MARGIN, FP_DEPTH, CASE+CORE_MARGIN])
                        Rounded([width, depth-FP_DEPTH, height], RADIUS-CASE-CORE_MARGIN);

                    translate([0, DEPTH-2, 0])
                        mirror([0, 1, 0])
                        cube([WIDTH, fw_exp[2]+1, fw_exp[1]+2]);
                }

                FrameworkGrid() { Framework(cutout=true); }
            }

            Clips();
        }
    }
}

module Shell() {
    color("white") render() difference() {
        translate([0, -RADIUS, 0])
            Rounded([WIDTH, DEPTH+RADIUS, HEIGHT], RADIUS);

        Core(cutout=true);

        // Flatten front face
        translate([0, -10+e, 0])
            cube([WIDTH+e, 10, HEIGHT+e]);

        FrameworkGrid() { Framework(cutout=true); }

        Inserts(cutout=true);
        SpeakerCutout();
    }
}

module PCB() {
    color("green") translate([CASE+CORE+0.2, DEPTH-15-2, CASE+CORE+0.2+KB_HEIGHT]) {
        cube([WIDTH-2*CASE-2*CORE-0.2, 2, HEIGHT-2*CASE-2*CORE-0.2-KB_HEIGHT-5]);
    }
}

union() {
    *Shell();

    translate([0, 1, 0]) {
        *Core();
        Frontplate();

        *union() {
            Inserts();
            PCB();
            //SpeakerCutout(true);
            FrameworkGrid() { Framework(cutout=false); }

            translate([0, FP_DEPTH, 0])
                _assembly(cutout=false);
        }
    }
}
