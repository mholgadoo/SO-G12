#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <indice_jugador> <pipe_fd>\n", argv[0]);
        return 1;
    }

    int player_index = atoi(argv[1]);
    int pipe_fd = atoi(argv[2]);
    //obtengo file descriptor de /game_state
    int fd_state = shm_open("/game_state", O_RDWR, 0);
    if (fd_state == -1) {
        perror("Error abriendo /game_state");
        close(pipe_fd);
        return 1;
    }
    //como GameState termina con board[], sizeof(GameState) es el tamano de la estructura GameState y me falta el tamano de board[]
    //por lo tanto, state_info.st_size es el tamano de la estructura GameState + el tamano de board[]
    struct stat state_info;
    if (fstat(fd_state, &state_info) == -1) {
        perror("Error obteniendo tamano de /game_state");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }

    if (state_info.st_size < (off_t)sizeof(GameState)) {
        fprintf(stderr, "Error: /game_state tiene un tamano invalido.\n");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }

    size_t state_size = (size_t)state_info.st_size;
    //guardo el puntero a la memoria compartida en state
    GameState *state = mmap(NULL, state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_state, 0);
    if (state == MAP_FAILED) {
        perror("Error mapeando /game_state");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }
    close(fd_state);

    //obtengo file descriptor de /game_sync
    int fd_sync = shm_open("/game_sync", O_RDWR, 0);
    if (fd_sync == -1) {
        perror("Error abriendo /game_sync");
        munmap(state, state_size);
        close(pipe_fd);
        return 1;
    }
    //como Sync termina con board[], sizeof(Sync) es el tamano de la estructura Sync y me falta el tamano de board[]
    //por lo tanto, sync_size es el tamano de la estructura Sync + el tamano de board[]
    size_t sync_size = sizeof(Sync);

    //guardo el puntero a la memoria compartida en sync
    Sync *sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) {
        perror("Error mapeando /game_sync");
        close(fd_sync);
        munmap(state, state_size);
        close(pipe_fd);
        return 1;
    }
    close(fd_sync);

    if (player_index < 0 || player_index >= state->numPlayers) {
        fprintf(stderr, "Error: indice de jugador fuera de rango.\n");
        munmap(sync, sync_size);
        munmap(state, state_size);
        close(pipe_fd);
        return 1;
    }

    // Por ahora solo comprobamos que el jugador ya puede ver el estado compartido.
    Player *self = &state->players[player_index];
    (void)self;
    (void)sync;

    munmap(sync, sync_size);
    munmap(state, state_size);
    close(pipe_fd);

    return 0;
}
