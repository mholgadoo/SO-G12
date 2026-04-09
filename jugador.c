#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include "common.h"
#include <time.h>

// Comportamiento random 
static MoveDirection random_behavior(void) {
    return (MoveDirection)(rand() % 4);
}

// Comportamiento fijo 
static MoveDirection fixed_behavior(int turn) {
    MoveDirection secuencia[] = { MOVE_RIGHT, MOVE_RIGHT, MOVE_DOWN, MOVE_DOWN };
    return secuencia[turn % 4];
}

// Funcion util para el radar 
static bool is_reward(char c) {
    return (c == '&' || c == '@' || c == '%' || c == '#' || c == '$');
}

/*
logica base de begin_read, sin los if de validacion:

sem_wait(&sync->mutexWriter);
sem_wait(&sync->mutexReaders);
sync->playersReading++;

if (sync->playersReading == 1) {
    sem_wait(&sync->mutexStatus);
}

sem_post(&sync->mutexReaders);
sem_post(&sync->mutexWriter);
*/

static int begin_read(Sync *sync)
{
    // 1. primero pasamos por mutexWriter
    // esto hace de molinete: no deja que sigan entrando lectores para siempre
    // si el master quiere escribir, con esto evitamos que se muera de hambre
    if (sem_wait(&sync->mutexWriter) == -1) {
        perror("Error en sem_wait de mutexWriter");
        return -1;
    }

    // 2. ahora tomamos mutexReaders
    // este protege solamente playersReading
    // o sea: no protege el tablero, protege el contador de lectores
    if (sem_wait(&sync->mutexReaders) == -1) {
        perror("Error en sem_wait de mutexReaders");
        sem_post(&sync->mutexWriter);
        return -1;
    }

    // 3. nos anotamos como lector activo
    sync->playersReading++;

    // 4. si somos el primer lector, cerramos la puerta al escritor
    if (sync->playersReading == 1) {
        if (sem_wait(&sync->mutexStatus) == -1) {
            perror("Error en sem_wait de mutexStatus");
            sync->playersReading--;
            sem_post(&sync->mutexReaders);
            sem_post(&sync->mutexWriter);
            return -1;
        }
    }

    // 5. ya terminamos de tocar playersReading, soltamos mutexReaders
    if (sem_post(&sync->mutexReaders) == -1) {
        perror("Error en sem_post de mutexReaders");
        return -1;
    }

    // 6. soltamos mutexWriter
    if (sem_post(&sync->mutexWriter) == -1) {
        perror("Error en sem_post de mutexWriter");
        return -1;
    }

    // 7. desde aca ya podemos leer el estado compartido
    return 0;
}


/*
logica base de end_read, sin los if de validacion:

sem_wait(&sync->mutexReaders);
sync->playersReading--;

if (sync->playersReading == 0) {
    sem_post(&sync->mutexStatus);
}

sem_post(&sync->mutexReaders);
*/
static int end_read(Sync *sync)
{
    // 1. volvemos a tomar mutexReaders porque vamos a tocar playersReading
    if (sem_wait(&sync->mutexReaders) == -1) {
        perror("Error en sem_wait de mutexReaders");
        return -1;
    }

    // 2. dejamos de contarnos como lector activo
    sync->playersReading--;

    // 3. si eramos el ultimo lector, reabrimos la puerta al escritor
    if (sync->playersReading == 0) {
        if (sem_post(&sync->mutexStatus) == -1) {
            perror("Error en sem_post de mutexStatus");
            sem_post(&sync->mutexReaders);
            return -1;
        }
    }

    // 4. ya terminamos con el contador, soltamos mutexReaders
    if (sem_post(&sync->mutexReaders) == -1) {
        perror("Error en sem_post de mutexReaders");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <indice_jugador> <pipe_fd>\n", argv[0]);
        return 1;
    }

    int player_index = atoi(argv[1]);
    int pipe_fd = atoi(argv[2]);
    char *behavior = argv[3];

    int turn = 0; // Agregamos el contador de turnos
    srand(time(NULL) ^ getpid()); // Semilla única por jugador usando su PID

    // Obtengo file descriptor de /game_state
    int fd_state = shm_open("/game_state", O_RDWR, 0);
    if (fd_state == -1) {
        perror("Error abriendo /game_state");
        close(pipe_fd);
        return 1;
    }
    // Como GameState termina con board[], sizeof(GameState) es el tamaño de la estructura GameState y me falta el tamaño de board[]
    // por lo tanto, state_info.st_size es el tamaño de la estructura GameState + el tamaño de board[]
    struct stat state_info;
    if (fstat(fd_state, &state_info) == -1) {
        perror("Error obteniendo tamaño de /game_state");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }

    if (state_info.st_size < (off_t)sizeof(GameState)) {
        fprintf(stderr, "Error: /game_state tiene un tamaño invalido.\n");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }
    // Guardo el tamaño real de la /game_state
    size_t state_size = (size_t)state_info.st_size;
    // Guardo el puntero a la memoria compartida en state
    GameState *state = mmap(NULL, state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_state, 0);
    if (state == MAP_FAILED) {
        perror("Error mapeando /game_state");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }
    close(fd_state);

    // Obtengo file descriptor de /game_sync
    int fd_sync = shm_open("/game_sync", O_RDWR, 0);
    if (fd_sync == -1) {
        perror("Error abriendo /game_sync");
        munmap(state, state_size);
        close(pipe_fd);
        return 1;
    }
    // Como Sync termina con board[], sizeof(Sync) es el tamaño de la estructura Sync y me falta el tamaño de board[]
    // por lo tanto, sync_size es el tamaño de la estructura Sync + el tamaño de board[]
    size_t sync_size = sizeof(Sync);
 
    // Guardo el puntero a la memoria compartida en sync
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

    while (true) {
        MoveRequest request;
        bool finished;
        int radar = -1;

        // el jugador espera aca hasta que el master le diga "ya procese el anterior, manda otro"
        if (sem_wait(&sync->allowed_Mov[player_index]) == -1) {
            perror("Error en sem_wait de allowed_Mov");
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }

        // entramos como lector para mirar el estado sin pisarnos con el master
        if (begin_read(sync) == -1) {
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }

        finished = state->finished;

        // Si el juego sigue, prendemos el radar
        if (!finished) {
            unsigned short mi_x = state->players[player_index].x;
            unsigned short mi_y = state->players[player_index].y;
            unsigned short w = state->width;
            unsigned short h = state->height;

            // RADAR: Miramos las 4 celdas adyacentes.
            // OJO: Primero verificamos (mi_x + 1 < w) para no salirnos del tablero
            if (mi_x + 1 < w && is_reward(state->board[mi_y * w + (mi_x + 1)])) {
                radar = MOVE_RIGHT;
            } 
            else if (mi_x > 0 && is_reward(state->board[mi_y * w + (mi_x - 1)])) {
                radar = MOVE_LEFT;
            } 
            else if (mi_y + 1 < h && is_reward(state->board[(mi_y + 1) * w + mi_x])) {
                radar = MOVE_DOWN;
            } 
            else if (mi_y > 0 && is_reward(state->board[(mi_y - 1) * w + mi_x])) {
                radar = MOVE_UP;
            }
        }

        // salimos del protocolo lector cuando terminamos de mirar el estado
        if (end_read(sync) == -1) {
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }

        // si el master ya marco fin de juego, salimos sin mandar un movimiento extra
        if (finished) {
            break;
        }

        // por ahora mandamos siempre derecha solo para probar
        //request.direction = MOVE_RIGHT;
        
        // Analizamos si el radar detecto una recompensa 
        if (radar != -1) {
            request.direction = (MoveDirection)radar;
        } // Sino usamos el comportamiento que le toco al jugador
        else if (strcmp(behavior, "random") == 0) {
            request.direction = random_behavior();
        } else {
            // Asumimos que si no es random, es "fijo"
            request.direction = fixed_behavior(turn);
        }
        
        turn++; // Incrementamos el turno para el algoritmo fijo

        // write manda los bytes del struct por el pipe de este jugador hacia el master
        if (write(pipe_fd, &request, sizeof(request)) != (ssize_t)sizeof(request)) {
            perror("Error enviando movimiento");
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }
    }

    munmap(sync, sync_size);
    munmap(state, state_size);
    close(pipe_fd);

    return 0;
}
