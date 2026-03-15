KB_W = 80;
KB_D = 9.98;
KB_H = 67;

module Keyboard(space=0) {
    align = space == 0 ? 0 : (space - KB_W) / 2;

    translate([align, 0, 0]) {
        cube([KB_W, KB_D, KB_H]);
    }
}
