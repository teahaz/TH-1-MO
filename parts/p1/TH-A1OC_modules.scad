// TH-A1OC: Misc. modules
include <parts.scad>

x = 0;
y = 0;

translate([CASE_THICKNESS+MODULE_MARGIN, FPT_D, CASE_THICKNESS+MODULE_MARGIN]) {
    let(y = y+KB_H_TOTAL+KB_CHIN+SCR_MARGIN) {
        translate([x, 0, y]) Screen();

    let(x = x+SCR_H+SPK_MARGIN)
    let(y = y+SCR_BEZEL_R) {
        translate ([x, 0, y]) Speaker();

    }}
}
