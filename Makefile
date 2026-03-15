CC = gcc
CFLAGS = -Wall

all: master vista jugador

master: master.c
	$(CC) $(CFLAGS) -o master master.c

vista: vista.c
	$(CC) $(CFLAGS) -o vista vista.c

jugador: jugador.c
	$(CC) $(CFLAGS) -o jugador jugador.c

clean:
	rm -f master vista jugador

.PHONY: all clean
