all: build

build:
	gcc stupidwm.c -o main -Wall -Wextra -std=c17 -lX11
