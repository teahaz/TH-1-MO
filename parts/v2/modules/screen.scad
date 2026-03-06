// https://aliexpress.com/item/1005006168560918.html
// -<> >#> @>^

include <common.scad>

module Screen(cutout=false) {
    width = 79.874;
    height = 67.25;
    depth = cutout ? 10 : 0.7;

    bezel = 0.9;
    chin = 3.8;

    difference() {
        if (!cutout) {
            translate([0, -e, 0])
            color("silver")
            cube([width+2*bezel, depth, height+bezel+chin]);
        }

        translate([bezel, 0, chin])
            color("black")
            mirror([0, 1, 0])
            cube([width, depth, height]);
    }
}
