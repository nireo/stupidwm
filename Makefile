all: build

build:
	gcc stupidwm.c -o main -Wall -Wextra -std=c17 -lX11 -lXrandr -lXft -I/usr/include/freetype2
