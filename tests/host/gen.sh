#!/bin/sh
# Turn source/main.c into a program that runs on a normal computer: the 3DS headers are swapped for the fakes in
# this folder, the SD card becomes ./sd, and main() becomes app_main() so each test can drive it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../build/host}
mkdir -p "$out/sd"
sed -e 's|#include <3ds.h>|#include "fake3ds.h"|' -e 's|#include <citro3d.h>|#include "fakecitro.h"|' -e '/#include <citro2d.h>/d' -e '/#include <malloc.h>/d' \
    -e 's|mkdir("sdmc:/3ds/syrup", 0777)|mkdir("sd", 0777)|g' -e 's|sdmc:/3ds/syrup/|sd/|g' -e 's|"sdmc:/3ds/syrup"|"sd"|g' \
    -e 's|^int main(void) {|int app_main(void) {|' \
    "$here/../../source/main.c" > "$out/syrup_host.c"
