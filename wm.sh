#!/bin/sh

set -e

make build

XEPHYR=$(command -v Xephyr) # Absolute path of Xephyr's bin
xinit ./xinitrc -- \
    "$XEPHYR" +xinerama -screen 1920x1080 -screen 1920x1080 -screen 1920x1080 -ac :1 -host-cursor



