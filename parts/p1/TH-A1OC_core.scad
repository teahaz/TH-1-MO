// TH-A1OC: Core (inner shell)
include <parts.scad>

render() difference() {
    union() {
        Internals();
    }
    Externals(cutout=true);
    ExpansionBay(cutout=true);
}
