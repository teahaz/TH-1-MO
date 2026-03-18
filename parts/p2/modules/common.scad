e = $preview ? 0.01 : 0;

module Prism(l, w, h){
    polyhedron(
        points=[[0,0,0], [l,0,0], [l,w,0], [0,w,0], [0,w,h], [l,w,h]],
        faces=[[0,1,2,3],[5,4,3,2],[0,4,5,1],[0,3,4],[5,2,1]]
    );
}

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

module Rounded(size, r) {
    hull() {
       for (x = [r, size.x-r], y = [r, size.y-r], z = [r, size.z-r])
           translate([x, y, z]) sphere(r, $fn=32);
    }
}
