include <NopSCADlib/core.scad>
include <NopSCADlib/vitamins/pcbs.scad>
include <NopSCADlib/vitamins/rocker.scad>
include <NopSCADlib/vitamins/rockers.scad>

include <framework.scad>

CORE_HOLLOW = true;

FP_D = 2;
FP_W = 2;

BODY_W = 87;
BODY_D = 25;
BODY_H = SCR_H+KB_H+3*FP_W+0.7;
BODY_T = 1.5;
BODY_R = 2;
BODY_L = 0;

BODY_I_W = BODY_W - 2*BODY_T;
BODY_I_H = BODY_H - 2*BODY_T;
BODY_I_D = BODY_D - 2*BODY_T;

CORE_T = 1.5;
CORE_M = 0.3;

CORE_W = BODY_I_W - 2*CORE_M;
CORE_D = BODY_I_D - 2*CORE_M;
CORE_H = BODY_I_H - 2*CORE_M;
CORE_R = BODY_R-CORE_T-CORE_M;
CORE_O = BODY_T + CORE_M;

module _Inserts(cutout=false) {
    translate([15, BODY_D/2, CORE_H]) {
        translate([0, 2, -3.675])
            rotate([90, 90, 0])
            mirror([1, 0, 0]) usb_C(cutout=cutout);

        translate([15, 3.5, -8.5])
            rotate([90, 90, 0])
            mirror([1, 0, 0]) jack(cutout=cutout);
    }

    cols = 15;
    rows = 2;

    depth = 11;
    size = 2.5;
    ratio = 0.7;

    width = size*cols;
    height = size*rows;

    margin = 1;

    translate([BODY_W*0.8, BODY_D, BODY_H-16.5])
    rotate([0, 90, 0]) {
        if (cutout) {
            translate([-margin/2, -depth+e, -margin/2])
                cube([width+margin, depth+e, height+margin]);
        } else {
            mirror([0, 1, 0]) Grid([15, 2], [2.5, 2.5], [width, height]) {
                difference() {
                    color("black") cube([size, depth, size]);
                    color("silver")
                        translate([(size-size*ratio)/2, -e, (size-size*ratio)/2])
                        cube([size*ratio, depth*ratio, size*ratio]);
                }
            }
        }
    }
}

module Body() {
    render() {
        difference() {
            translate([BODY_R, BODY_R, BODY_R]) hull() {
                for (d = [-BODY_R,BODY_D-2*BODY_R]) {
                    translate([0, d, 0]) sphere(BODY_R);
                    translate([BODY_W-2*BODY_R, d, 0]) sphere(BODY_R);
                    translate([BODY_W-2*BODY_R, d, BODY_H-2*BODY_R]) sphere(BODY_R);
                    translate([0, d, BODY_H-2*BODY_R]) sphere(BODY_R);
                }
            }
            
            translate([BODY_T, -e, BODY_T]) {
                rounded_cube_xz(
                    [BODY_W-2*BODY_T, BODY_T*2, BODY_H-2*BODY_T],
                    BODY_R-BODY_T
                );
            }

            translate([BODY_T, -e, BODY_T]) intersection() {
                rounded_cube_xz(
                    [BODY_W-2*BODY_T, BODY_D-BODY_T+e, BODY_H-2*BODY_T],
                    BODY_R-BODY_T
                );
                rounded_cube_xy(
                    [BODY_W-2*BODY_T, BODY_D-BODY_T+e, BODY_H-2*BODY_T],
                    BODY_R-BODY_T
                );
                rounded_cube_yz(
                    [BODY_W-2*BODY_T, BODY_D-BODY_T+e, BODY_H-2*BODY_T],
                    BODY_R-BODY_T
                );
            }

            translate([-e, BODY_T-BODY_L, -e]) mirror([0, 1, 0])
                cube([BODY_W+2*e, BODY_D+2*e, BODY_H+2*e]);

            translate([0, -(BODY_H-BODY_D)/2, KB_H]) difference() {
                rotate([15, 0, 0]) {
                    cube([BODY_T*2, BODY_H, 45]);
                    translate([BODY_W-BODY_T, 0, 0])
                        cube([BODY_T*2, BODY_H, 45]);
                }
            }

            translate([3, BODY_D-2, 73.3]) let(h=37) {
                hull() {
                    rotate([0, 45, 0]) cube(4, center=true);
                    cube(3);
                }

                hull() {
                    cube(3);
                    translate([0, 0, h]) cube(3);
                }

                translate([0, 0, h+4]) rotate([0, 90, 0]) hull() {
                    cube(3);
                    rotate([0, 45, 0]) cube(4, center=true);
                }
            }

            translate([0, BODY_T+FP_D, 0]) {
                translate([BODY_W/2, 0, CORE_O]) {
                    _ScrewPost(true);
                }

                translate([BODY_W/2, 0, BODY_H-CORE_O-FP_D]) {
                    _ScrewPost(true);
                }
            }

            FrameworkBay(BODY_W, cutout=true);

            _Inserts(true);
        }
    }
}

module Core() {
    render() difference() {
        union() {
            translate([BODY_T+CORE_M, BODY_T+CORE_M, BODY_T+CORE_M]) difference() {
                translate([CORE_R, CORE_R, CORE_R]) {
                    hull() {
                        for (d = [-CORE_R,CORE_D-2*CORE_R]) {
                            translate([0, d, 0]) sphere(CORE_R);
                            translate([CORE_W-2*CORE_R, d, 0]) sphere(CORE_R);
                            translate([CORE_W-2*CORE_R, d, CORE_H-2*CORE_R]) sphere(CORE_R);
                            translate([0, d, CORE_H-2*CORE_R]) sphere(CORE_R);
                        }
                    }
                }

                if (CORE_HOLLOW) translate([CORE_T, -e, CORE_T])
                    rounded_cube_xz(
                        [CORE_W-2*CORE_T, CORE_D-CORE_T+e, CORE_H-2*CORE_T],
                        CORE_R-CORE_T
                    );

                translate([-e, FP_D+CORE_M+0.081+e, -e]) mirror([0, 1, 0])
                    cube([CORE_W+2*e, CORE_D+2*e, CORE_H+2*e]);

                translate([0, FP_D-CORE_M, 0]) {
                    translate([BODY_W/2-CORE_O, 0, 0]) {
                        _ScrewPost(true);
                    }

                    translate([BODY_W/2-CORE_O, 0, BODY_H-2*CORE_O-FP_D]) {
                        _ScrewPost(true);
                    }
                }
            }
        }

        FrameworkBay(BODY_W, cutout=true);

        _Inserts(true);
    } 
    FrameworkBay(BODY_W);
}
module _ScrewPost(cutout=false) {
    z = 8;
    d = cutout ? CORE_D*2 : FP_D+20;
    x = cutout ? 1.2 : 1;
    xh = x/2;

    ox = -FP_W*x/2;

     difference() {
        translate([ox, 0, 0]) linear_extrude(height=FP_D+e) {
            hull() {
                square(FP_W*x);
                translate([FP_W*x/2, z]) circle(FP_W/2*x);
            }
            
            translate([FP_W*x/2, z]) circle(FP_W/2);
        }

        translate([ox, 0, -d/2])
            linear_extrude(height=d)
            translate([FP_W*x/2, z]) circle(FP_W/4);
    }

    if (cutout)
        translate([ox, 0, -d/2])
        linear_extrude(height=d)
        translate([FP_W*x/2, z]) circle(FP_W/4);
}

module Frontplate() {
    translate([CORE_O, BODY_T, CORE_O]) {
        difference() {
            rounded_cube_xz([CORE_W, FP_D, CORE_H], BODY_R-BODY_T-CORE_M);
            translate([FP_W, -1, FP_W]) cube([CORE_W-2*FP_W, FP_D+2, CORE_H-2*FP_W]);
        }

        translate([0, 0, KB_H+2.2]) cube([CORE_W, FP_D, 3.8]);
    }

    translate([0, BODY_T+FP_D, 0]) {
        translate([BODY_W/2, 0, CORE_O]) _ScrewPost();
        translate([BODY_W/2, 0, BODY_H-CORE_O-FP_D]) _ScrewPost();
    }
}
