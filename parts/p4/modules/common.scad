e = $preview ? 0.01 : 0;

module Grid(grid, size, space) {
    cols = grid[0]-1;
    rows = grid[1]-1;

    width = size[0]; 
    height = size[1];

    total_width = space[0]; 
    total_height = space[1];

    incr_x = cols == 0 ? 0 : (total_width-width*(cols+1)) / cols + width;
    incr_y = rows == 0 ? 0 : (total_height-height*(rows+1)) / rows + height;

    for (row = [0:rows]) for (col = [0:cols]) {
        translate([incr_x*col, 0, incr_y*row])
            children();
    }
}

module USBC(space=0) {
    l = 7.35;
    w = 8.94;
    h = 3.26;
    r = h / 2 - 0.5;

    align = space == 0 ? 0 : (space - w) / 2;

    translate([align, 0, 0])
        linear_extrude(l)
        translate([0, h / 2])
        offset(r) offset(-r)
            square([w, h]);
}
