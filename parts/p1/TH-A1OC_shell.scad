// TH-A1OC: Shell
include <parts.scad>

solid = is_undef(SOLID_SHELL) ? false : SOLID_SHELL;

render() difference() {
    Case(hollow=!solid);
    Externals(cutout=true);
    ExpansionBay(cutout=true);
}
