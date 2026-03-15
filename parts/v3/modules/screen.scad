include <common.scad>

panel_w  = 79.874;
panel_h = 67.25;
panel_d = 0.5;

bezel = 0.9;
chin = 3.8;

SCR_W = panel_w + 2*bezel;
SCR_H = panel_h + bezel + chin;
SCR_D = 0.7;

echo (SCR_W, SCR_H);

module Screen(space=0) {
    align = space == 0 ? 0 : (space - SCR_W) / 2;

    panel = 0.5;

    difference() translate([align, 0, 0]) {
        color("red") cube([SCR_W, SCR_D, SCR_H]);
        color("black") translate([bezel, -e, chin])
            cube([panel_w, panel_d, panel_h]);
    }
}
