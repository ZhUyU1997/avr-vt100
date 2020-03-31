#!/bin/bash
gcc main.c vt100/vt100.c -Ivt100 -Wall -std=c11 -pedantic `pkg-config sdl2 --libs --cflags` -lGL -lm -lSDL2_ttf -O0 -g 
