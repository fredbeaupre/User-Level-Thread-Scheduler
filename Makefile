# .DEFAULT_GOAL=all
CC=gcc
CFLAGS=-pthread

sut.o: sut.c
	$(CC) -o sut.o -c sut.c


