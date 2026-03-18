module Pins(cutout=false, avail_width) {
    cols = 15;
    rows = 2;

    depth = 11;
    size = 2.5;
    ratio = 0.7;

    width = size*cols;
    height = size*rows;

    margin = 0.5;

    translate([(avail_width-width)/2, 0, 0]) {
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
