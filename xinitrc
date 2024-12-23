#!/bin/sh

xeyes && sleep 2 & # Optionally, initialize some programs. For now, this is unnecessary.
exec ./main # The output file created by the Makefile1
