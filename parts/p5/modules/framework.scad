include <common.scad>

fw_port_m = 0;
fw_port_w = 30 + fw_port_m;
fw_port_h = 32 + fw_port_m;
fw_port_d = 6.8 + fw_port_m;
fw_g = 5;
fw_m = 3;
fw_t_m = 8;

FW_W = 2*(fw_port_w+fw_m)+fw_g;
FW_D = fw_port_d;
FW_H = fw_port_h+fw_m;

module FrameworkCard() {
    cube([fw_port_w, fw_port_d, fw_port_h]);
    translate([0, 0, fw_port_h-2]) USBC(fw_port_w);
}

module FrameworkBay(space=0, cutout=false) {
    align = space == 0 ? 0 : (space - FW_W) / 2;

    mirror([0, 1, 0])
        translate([0, -BODY_D-e, 0])
        translate([align, 0, 0]) difference() {
            union() {
                cube([FW_W, FW_D, FW_H]);
                translate([0, 0, FW_H])
                    cube([FW_W, FW_D, fw_t_m]);
            } 
            if (!cutout) translate([fw_m, -e, -e])
                Grid(
                    grid=[2, 1],
                    size=[fw_port_w, fw_port_h],
                    space=[FW_W-2*fw_m, FW_H-1*fw_m]
                ) FrameworkCard();
        }
}
