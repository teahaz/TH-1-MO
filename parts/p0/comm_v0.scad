key_depth = 8;
wall_thickness = 5;

module _kb_buttons(rows, cols, key_w, key_h, gap) {
    for (row = [0:rows-1]) {
        for (col = [0:cols-1]) {
            translate([col * (gap + key_w), -key_depth, row * (gap + key_h)])
            if (row == 0 && 3 < col && col < 8) {
                if (col == 4)
                    cube([(key_w+gap) * 4 - gap, key_depth, key_h]);
            } else
                cube([key_w, key_depth, key_h]);
        }
    }    
}

module Keyboard(cutout=false) {
    rows = 5;
    cols = 12;
    width = 90;
    height = 50;
    margin = 5;
    margin_h = margin / 2;
    lip = 4;
    
    gap = 1.8;
    key_w = (width - 2*margin - (cols-1) * gap) / cols;
    key_h = (height - 2*margin - (rows-1) * gap) / rows;
    mkey_w = 12;
    mkey_h = 5;
    mkey_gap = (width - 2*margin - 6*mkey_w) / 5;
    macro_h = mkey_h + margin;
    
    union() {
        color("blue") translate([-lip/2, -1, -lip/2]) cube([
            width+lip, 1, height+macro_h+lip
        ]);
        color("blue") translate([0, -5, 0]) cube([width, 6, height+macro_h]);
        translate([0, 1, 0]) {
            cube([width, 20, height]);
            if (!cutout) {
                translate([margin, 0, margin]) {
                    color("darkgray") _kb_buttons(rows, cols, key_w, key_h, gap);
                }
            }
            translate([0, 0, height]) {
                cube([width, 20, macro_h]);
                if (!cutout) {    
                    color("darkgray")
                    translate([margin, -key_depth, margin / 2])
                    for (i = [0:5]) {
                        translate([(mkey_w+mkey_gap)*i, 0, 0])
                            cube([mkey_w, key_depth, mkey_h]);
                    }
                }
            }
        }
    }
}

module Screen(cutout=false) {
    // https://it.aliexpress.com/item/1005007579014355.html
    scr_width = 87;
    scr_height = 56;
    left_bezel = 87 * 0.03;
    right_bezel = 87 * 0.1;
    hori_bezel = left_bezel + right_bezel;
    vert_bezel = scr_height * 0.09;
        
    if (!cutout) color("black") cube([scr_width, 5, scr_height]);
    color("white") translate([left_bezel, -1, vert_bezel])
        cube([
            scr_width - hori_bezel,
            cutout ? 10 : 2,
            scr_height - 2*vert_bezel
        ]);
}

module Frontplate() {
    difference() {
        cube([90 + 2*wall_thickness, 5, 116 + 2*wall_thickness]);
        translate([wall_thickness+3, 0, 67]) Screen(cutout=true);
        translate([wall_thickness, -1, wall_thickness]) Keyboard(cutout=true);
    }
}

*Frontplate();

translate([wall_thickness, wall_thickness, wall_thickness]) {
    Keyboard();
    translate([3, 0, 62]) Screen();
}