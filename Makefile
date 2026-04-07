CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lrt -lpthread

all: master vista jugador

master: master.c common.h
	$(CC) $(CFLAGS) -o master master.c $(LDFLAGS)

vista: vista.c common.h
	$(CC) $(CFLAGS) -o vista vista.c $(LDFLAGS)

jugador: jugador.c common.h
	$(CC) $(CFLAGS) -o jugador jugador.c $(LDFLAGS)

clean:
	rm -f master vista jugador

.PHONY: all clean
