// Teahaz Communicator v0
$fn = 10;
MODULE_MARGIN = 5;
MODULE_WIDTH = 90;
CASE_THICKNESS = 2;

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
KB_KEY_DEPTH = 8;

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

CASE_HEIGHT = 2*CASE_THICKNESS+KB_H_TOTAL+SCR_H_TOTAL+MODULE_MARGIN;
CASE_WIDTH = MODULE_WIDTH+2*CASE_THICKNESS;
CASE_DEPTH = KB_DEPTH+CASE_THICKNESS;

FRONTPLATE_DEPTH = 1.5;

INDICATOR_MARGIN = 0;

module Keyboard(cutout=false) {
    lip_depth = cutout ? KB_LIP_DEPTH+1 : KB_LIP_DEPTH;
    color("red")
        translate([-KB_LIP, 1, -KB_LIP])
        cube([MODULE_WIDTH+KB_LIP*2, lip_depth, KB_H_TOTAL+KB_LIP*2]);
    
    color("blue")
        translate([0, cutout ? -1 : 0, 0])
        cube([MODULE_WIDTH, lip_depth, KB_HEIGHT+KB_MK_HEIGHT]);

    if (!cutout) {
        color("blue")
            translate([0, KB_LIP_DEPTH, 0])
            cube([MODULE_WIDTH, KB_DEPTH-KB_LIP_DEPTH, KB_HEIGHT+KB_MK_HEIGHT]);
        
        color("darkgray")
            translate([MODULE_MARGIN, KB_LIP_DEPTH, MODULE_MARGIN])
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
        
        color("darkgray")
            translate([MODULE_MARGIN, KB_LIP_DEPTH, MODULE_MARGIN])
            for (col = [0:KB_MK_COUNT-1]) {
                translate([
                    col*(KB_MACRO_GAP+KB_MK_WIDTH),
                    -KB_KEY_DEPTH,
                    KB_HEIGHT-MODULE_MARGIN*1.5
                ])
                    cube([KB_MK_WIDTH, KB_KEY_DEPTH, KB_MK_HEIGHT]);
            }
    }
    
}

module Screen(cutout=false) {
    // https://aliexpress.com/item/1005007579014355.html
    // https://aliexpress.com/item/1005006168560918.html
    translate([0, 1, 0]) {
        difference() {
            color("gray") cube([SCR_WIDTH, SCR_DEPTH, SCR_HEIGHT]);
            if (!cutout) {
                translate([SCR_BEZEL_L, -1, SCR_BEZEL_B])
                    color("black")
                    cube([SCR_WIDTH-SCR_BEZEL_X, 2, SCR_HEIGHT-SCR_BEZEL_Y]);
            }
        }
    }

    if (cutout) {
        translate([SCR_BEZEL_L, -1, SCR_BEZEL_B])
            cube([SCR_WIDTH-SCR_BEZEL_X, 3, SCR_HEIGHT-SCR_BEZEL_Y]);
    }
}

module Indicators() {
    width = MODULE_WIDTH - SCR_WIDTH - INDICATOR_MARGIN - SCR_BEZEL_L;
    height = SCR_HEIGHT - SCR_BEZEL_Y;
    echo(width, height);
    
    count = 10;
    cols = 5;
    y_gap = height / count;
    x_gap = width / cols;
    
    difference() {
        color("black") cube([width, 5, height]);
        *for (x = [0:cols-1]) {
            translate([x_gap/2 + x*x_gap, 2, y_gap/2]) {
                rotate([90, 0, 0]) {
                    for (y = [0:count-1]) {
                        translate([0, y_gap * y, 0]) cylinder(10, 1);
                    }
                }
            }
        }    
    }
}

module Case() {
    translate([0, FRONTPLATE_DEPTH, 0]) {
        cube([CASE_WIDTH, CASE_DEPTH-FRONTPLATE_DEPTH, CASE_HEIGHT]);
    }
    children();
}

module Frontplate() {
    difference() {
        cube([
            MODULE_WIDTH+2*CASE_THICKNESS,
            FRONTPLATE_DEPTH,
            2*CASE_THICKNESS+KB_H_TOTAL+SCR_H_TOTAL+MODULE_MARGIN
        ]);
        translate([CASE_THICKNESS, 0, CASE_THICKNESS]) {
            Keyboard(cutout=true);
            translate([0, 0, KB_H_TOTAL+KB_LIP]) {
                Screen(cutout=true);
                translate ([SCR_WIDTH + INDICATOR_MARGIN, -1, SCR_BEZEL_B])
                    Indicators();
            }
        }
    }
}

union() {
    *Case() translate([CASE_THICKNESS, 0, CASE_THICKNESS]) {
        Keyboard();
        translate([0, 0, KB_H_TOTAL+KB_LIP]) {
            Screen();
            translate ([SCR_WIDTH + INDICATOR_MARGIN, 0, SCR_BEZEL_B])
                Indicators();
        }
    }
    translate([0, 0.01, 0]) Frontplate();
}