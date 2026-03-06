include <NopSCADlib/core.scad>
include <NopSCADlib/vitamins/pcbs.scad>

KB_HEIGHT = 50;

module Keyboard(cutout=false) {
    macro_height = 10;

    width = 80;
    height = KB_HEIGHT-macro_height;
    depth = 10;

    gap = (81.674-width)/2;
    margin = 3;
    btn = button_4p5mm;

    key_w = btn[1];
    key_h = btn[1];
    rows = 5;
    cols = 12;

    inr_w = width-2*margin;
    inr_h = height-2*margin;
    inr_r = RADIUS-FP_MARGIN;

    translate([gap, -e, 0]) {
        if (cutout) {
            color("black")
                mirror([0, 1, 0]) {
                    translate([0, 0, KB_HEIGHT-inr_r])
                        cube([width, depth, inr_r]);
                    rounded_cube_xz([width, depth, KB_HEIGHT], inr_r);
                }

        } else {
            color("black")
                cube([width, depth, KB_HEIGHT]); 

            translate([margin, 0, margin]) {
                Grid([5, 1], [key_w, key_h], [inr_w, inr_h]) {
                    translate([key_w/2, 0, key_h/2])
                    rotate([90, 0, 0])
                    color("silver")
                    render()
                    square_button(btn);
                }
            }

            translate([margin, 0, margin+macro_height]) {
                Grid([cols, rows], [key_w, key_h], [inr_w, inr_h]) {
                    //cube([key_w, 5, key_h]);

                    translate([key_w/2, 0, key_h/2])
                    rotate([90, 0, 0])
                    color("silver")
                    render()
                    square_button(btn);
                }
            }
        }
    }
}
