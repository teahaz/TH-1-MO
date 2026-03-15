KB_W = 80.85;
KB_D = 6.6;
KB_H = 67;

module _kb_stl() {
    mirror([0, 0, 0])
        rotate([90, 0, 0])
        translate([-56.4, 100.525, 0])
        color("green")
        import("modules/keyboard.stl", $fn=2);
}

module Keyboard(space=0, cutout=false) {
    align = space == 0 ? 0 : (space - KB_W) / 2;

    translate([align, 3.2, 0]) {
        if (cutout) cube([KB_W, KB_D, KB_H]);
        else _kb_stl();
    }
}
