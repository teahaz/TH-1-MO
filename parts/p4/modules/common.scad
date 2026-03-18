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
