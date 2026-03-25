CC = gcc
CFLAGS = -Wall
LDFLAGS = -lrt -lpthread
#-lrt enlaza la librería de realtime/POSIX donde están APIs como shm_open y shm_unlink. Es necesario para usar shared memory.
#-lpthread enlaza la librería de threads. Es necesario para usar threads.
all: master vista jugador

master: master.c
	$(CC) $(CFLAGS) -o master master.c $(LDFLAGS)

vista: vista.c
	$(CC) $(CFLAGS) -o vista vista.c $(LDFLAGS)

jugador: jugador.c
	$(CC) $(CFLAGS) -o jugador jugador.c $(LDFLAGS)

clean:
	rm -f master vista jugador

.PHONY: all clean
