// Teahaz Communicator v2
include <fw_expansion_card.scad>;

$fn = 10;
MODULE_MARGIN = 5;
MODULE_WIDTH = 90;

KB_ROWS = 5;
KB_COLS = 12;
KB_HEIGHT = 40;
KB_DEPTH = 10;
KB_LIP = 1;
KB_LIP_DEPTH = 2;
KB_GAP = 1.8;

function _key_dim(dim, count) = (
    (dim - 2*MODULE_MARGIN - (count-1)*KB_GAP) / count
);

KB_KEY_WIDTH = _key_dim(MODULE_WIDTH, KB_COLS);
KB_KEY_HEIGHT = _key_dim(KB_HEIGHT, KB_ROWS);
KB_KEY_DEPTH = 4;

KB_MK_WIDTH = 7;
KB_MK_HEIGHT = 5;
KB_MK_COUNT = 6;

KB_MACRO_HEIGHT = KB_MK_HEIGHT + MODULE_MARGIN;
KB_MACRO_GAP = (
    (MODULE_WIDTH - 2*MODULE_MARGIN - KB_MK_COUNT*KB_MK_WIDTH)
    / (KB_MK_COUNT-1)
);

KB_H_TOTAL = KB_HEIGHT + KB_MK_HEIGHT;

SCR_WIDTH = 67.25;
SCR_HEIGHT = 79.874;
SCR_DEPTH = 0.7;

SCR_BEZEL_L = SCR_WIDTH * 0.05;
SCR_BEZEL_R = SCR_WIDTH * 0.05;
SCR_BEZEL_X = SCR_BEZEL_L + SCR_BEZEL_R;
SCR_BEZEL_T = SCR_HEIGHT * 0.08;
SCR_BEZEL_B = SCR_HEIGHT * 0.12;
SCR_BEZEL_Y = SCR_BEZEL_T + SCR_BEZEL_B;

SCR_H_TOTAL = SCR_HEIGHT;

CASE_THICKNESS = 3;
CASE_RADIUS = 10;
CASE_HEIGHT = 2*MODULE_MARGIN+2*CASE_THICKNESS+KB_H_TOTAL+SCR_H_TOTAL;
CASE_WIDTH = 2*MODULE_MARGIN+MODULE_WIDTH+2*CASE_THICKNESS;
CASE_DEPTH = 23;
CASE_LIP = 1;
CASE_BEVEL = 2;

FP_DEPTH = 1.5;

INDICATOR_MARGIN = 0;
INDICATOR_WIDTH = MODULE_WIDTH - SCR_WIDTH - INDICATOR_MARGIN - SCR_BEZEL_X;
INDICATOR_HEIGHT = SCR_HEIGHT - SCR_BEZEL_Y;

module Keyboard(cutout=false) {
    color("black")
        translate([0, 0, 0])
        cube([MODULE_WIDTH, KB_DEPTH, KB_HEIGHT+KB_MK_HEIGHT]);
    
    color("gray")
        translate([MODULE_MARGIN, 0, MODULE_MARGIN])
        for (row = [0:KB_ROWS-1]) {
            for (col = [0:KB_COLS-1]) {
                translate([
                    col*(KB_GAP+KB_KEY_WIDTH),
                    -KB_KEY_DEPTH,
                    row*(KB_GAP+KB_KEY_HEIGHT),
                ])
                    if (row == 0 && 3 < col && col < 8) {
                        if (col == 4) cube([
                            (KB_KEY_WIDTH+KB_GAP) * 4 - KB_GAP,
                            KB_KEY_DEPTH,
                            KB_KEY_HEIGHT
                        ]);
                    } else {
                        cube([KB_KEY_WIDTH, KB_KEY_DEPTH, KB_KEY_HEIGHT]);
                    }
            }
        }
    
    color("gray")
        translate([MODULE_MARGIN, 0, MODULE_MARGIN])
        for (col = [0:KB_MK_COUNT-1]) {
            translate([
                col*(KB_MACRO_GAP+KB_MK_WIDTH),
                -KB_KEY_DEPTH,
                KB_HEIGHT-MODULE_MARGIN*1.5
            ])
                cube([KB_MK_WIDTH, KB_KEY_DEPTH, KB_MK_HEIGHT]);
        }
}

module Screen() {
    // https://aliexpress.com/item/1005007579014355.html
    // https://aliexpress.com/item/1005006168560918.html
    translate([0, 0, 0]) {
        difference() {
            color("gray") cube([SCR_WIDTH, SCR_DEPTH, SCR_HEIGHT]);
            translate([SCR_BEZEL_L, -1, SCR_BEZEL_B])
                cube([SCR_WIDTH-SCR_BEZEL_X, SCR_DEPTH+2, SCR_HEIGHT-SCR_BEZEL_Y]);
        }
        color("black")
            translate([SCR_BEZEL_L, 0, SCR_BEZEL_B])
            cube([SCR_WIDTH-SCR_BEZEL_X, SCR_DEPTH, SCR_HEIGHT-SCR_BEZEL_Y]);
    }
}

module Indicators() {
    count = 10;
    cols = 5;
    
    difference() {
        color("black") cube([INDICATOR_WIDTH, 5, INDICATOR_HEIGHT]);
    }
}

module Case() {
    r = CASE_RADIUS-CASE_THICKNESS;

    color("black") difference() {
        minkowski() {
            translate([CASE_RADIUS, -CASE_LIP, CASE_RADIUS])
            cube([
                CASE_WIDTH - 2*CASE_RADIUS,
                CASE_DEPTH+CASE_LIP,
                CASE_HEIGHT - 2*CASE_RADIUS
            ]);
            rotate([-90, 0, 0])
                cylinder(r=CASE_RADIUS, h=0.01, $fn=32);
        }
        hull() {
            translate([CASE_THICKNESS-CASE_BEVEL+r, -CASE_LIP-0.01, CASE_THICKNESS-CASE_BEVEL+r])
                minkowski() {
                    cube([
                        CASE_WIDTH - 2*(CASE_THICKNESS-CASE_BEVEL) - 2*r,
                        0.01,
                        CASE_HEIGHT - 2*(CASE_THICKNESS-CASE_BEVEL) - 2*r
                    ]);
                    rotate([-90, 0, 0])
                        cylinder(r=r, h=0.01, $fn=32);
                }
            translate([CASE_THICKNESS+r, 0, CASE_THICKNESS+r])
                minkowski() {
                    cube([
                        CASE_WIDTH - 2*CASE_THICKNESS - 2*r,
                        0.01,
                        CASE_HEIGHT - 2*CASE_THICKNESS - 2*r
                    ]);
                    rotate([-90, 0, 0])
                        cylinder(r=r, h=0.01, $fn=32);
                }
        }
        translate([CASE_THICKNESS+r, -CASE_LIP-1, CASE_THICKNESS+r])
            minkowski() {
                cube([
                    MODULE_WIDTH+2*MODULE_MARGIN - 2*r,
                    CASE_DEPTH+CASE_LIP,
                    KB_H_TOTAL+SCR_H_TOTAL+2*MODULE_MARGIN - 2*r
                ]);
                rotate([-90, 0, 0])
                    cylinder(r=r, h=0.01, $fn=32);
            }
    }
    children();
}

module Frontplate() {
    r = CASE_RADIUS-CASE_THICKNESS;
    cut_depth = FP_DEPTH+2;

    difference() {
        translate([r, 0, r])
        minkowski() {
            cube([
                MODULE_WIDTH+2*MODULE_MARGIN - 2*r,
                FP_DEPTH,
                KB_H_TOTAL+SCR_H_TOTAL+2*MODULE_MARGIN - 2*r
            ]);
            rotate([-90, 0, 0])
                cylinder(r=r, h=0.01, $fn=32);
        }

        x = MODULE_MARGIN;
        y = MODULE_MARGIN;

        translate([0, -1, 0]) {
            translate([x, 0, y])
                cube([MODULE_WIDTH, cut_depth, KB_H_TOTAL]);

            // Screen
            let(x = x+SCR_BEZEL_L)
            let(y = y+KB_H_TOTAL+SCR_BEZEL_B) {
                translate([x, 0, y])
                cube([SCR_WIDTH-SCR_BEZEL_X, cut_depth, SCR_HEIGHT-SCR_BEZEL_Y]);

            // Indicator
            let(x = x+SCR_WIDTH+INDICATOR_MARGIN) {
                translate([x, 0, y])
                cube([INDICATOR_WIDTH, cut_depth, INDICATOR_HEIGHT]);
            }}
        }
    }
}

module ExpansionBay(cutout=false) {
    count = 2;
    gap = 10;
    space = MODULE_WIDTH + 2*MODULE_MARGIN - count*base[0] - (count-1)*gap;

    difference() {
        if (!cutout) {
            translate([MODULE_MARGIN, -base[2]-3.5, -1])
                cube([MODULE_WIDTH, base[2]+6, base[1]+5]);
        }

        union() {
            for (i = [0:count-1]) {
                translate([space/2+(gap+base[0])*i, 0, -1])
                    if (cutout) {
                        rotate([90, 0, 0]) {
                            difference() {
                                cube([base[0], base[1], base[2]+1]);
                            }
                        }
                    } else {
                        expansion_card();
                    }
            }
        }
    }
}

union() {
    x = 0;
    y = 0;

    difference() {
        Case();
        translate([CASE_THICKNESS, CASE_DEPTH-CASE_THICKNESS, 0])
            ExpansionBay(cutout=true);
    }

    translate([CASE_THICKNESS+MODULE_MARGIN, FP_DEPTH, CASE_THICKNESS+MODULE_MARGIN]) {
        Keyboard();

        let(y = y+KB_H_TOTAL) {
            translate([x, 0, y]) Screen();

        let(x = x+SCR_BEZEL_L+SCR_WIDTH+INDICATOR_MARGIN)
        let(y = y+SCR_BEZEL_B) {
            translate ([x, 0, y]) Indicators();

        }}

    }

    translate([CASE_THICKNESS, 0, CASE_THICKNESS]) Frontplate();
}
