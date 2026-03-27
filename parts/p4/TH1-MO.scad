$fn = $preview ? 10 : 100;

include <NopSCADlib/core.scad>

include <modules/screen.scad>
include <modules/keyboard.scad>
include <modules/body.scad>

CORE_HOLLOW = true;

//incr = -20;
incr = 0;

module Lipo() {
    color("silver") cube([65, 9.5, 55]);
}

module CM5() {
    color("green") cube([55, 4.6, 40]);
}

union() {
    union() {
        *color("gray") translate([0, incr*0, 0])
            Body();

        color("white") translate([0, incr*1.4, 0])
            Core();

        color("silver") translate([0, incr*3, 0])
        Frontplate();
    }

    *translate([0, incr*2.5 + BODY_T+FP_D, 0]) {
        translate([0, 0, BODY_T+CORE_T+CORE_M])
            Keyboard(BODY_W);

        translate([0, 0, BODY_T+CORE_T+CORE_M+KB_H+1])
            Screen(BODY_W);
    }

    union() {
        _Inserts();

        translate([BODY_T+CORE_M, BODY_T+CORE_M, BODY_T+CORE_M]) difference() {
            translate([-BODY_T-CORE_M, -BODY_T-CORE_M, -BODY_T-CORE_M]) {
                FrameworkGrid() Framework(cutout=false); 
                FrameworkGrid() Framework(cutout=true); 
            }
        }

        *translate([(BODY_W-65)/2, FP_D+BODY_T+2.5, BODY_T+CORE_T+CORE_O])
            Lipo();

        *translate([30, 6, 85])
            CM5();
    }
}
