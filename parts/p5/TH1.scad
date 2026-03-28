include <modules/common.scad>
include <modules/framework.scad>
include <modules/keyboard.scad>
include <modules/screen.scad>
include <modules/body.scad>

CORE_HOLLOW = false;
CORE_M = 0;

incr = 0;

color("silver") Frontplate();

translate([0, incr*4.5, 0])
    color("#5544FF") Core();

translate([0, incr*7.5, 0])
    color("white") Body();

*FrameworkBay(BODY_W);

translate([0, incr*2.5 + BODY_T+FP_D, 0]) {
    translate([0, 0, BODY_T+CORE_T+CORE_M])
        Keyboard(BODY_W);

    translate([0, 0, BODY_T+CORE_T+CORE_M+KB_H+1])
        Screen(BODY_W);
}

_Inserts();
