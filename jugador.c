#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include "common.h"
#include <time.h>

// Desplazamientos (dx, dy) para cada MoveDirection (0..7)
static const int DIR_DX[8] = { 0, 1, 1, 1, 0,-1,-1,-1};
static const int DIR_DY[8] = {-1,-1, 0, 1, 1, 1, 0,-1};

/*
 * Comportamiento inteligente del jugador.
 *
 * Fase 1 – codiciosa: se mueve hacia la celda adyacente con mayor puntaje.
 * Fase 2 – sabotaje:  si no quedan recompensas cerca, se acerca al rival
 *                     con más puntos para bloquearle sus celdas.
 * Fase 3 – fallback:  cualquier celda válida (el master rechaza lo inválido).
 *
 * neighbor_val[d] = valor de la celda en la dirección d (-128 si fuera de tablero)
 * rival_x, rival_y = posición del jugador rival con mayor score (-1 si solo hay uno)
 */
static MoveDirection smart_behavior(signed char neighbor_val[8],
                                    int px, int py,
                                    int rival_x, int rival_y)
{
    // --- Fase 1: codiciosa ---
    int best_dir = -1;
    int best_val = 0; // solo valores estrictamente positivos nos interesan
    for (int d = 0; d < 8; d++) {
        if (neighbor_val[d] > best_val) {
            best_val = neighbor_val[d];
            best_dir = d;
        }
    }
    if (best_dir != -1)
        return (MoveDirection)best_dir;

    // --- Fase 2: sabotaje (acercarse al rival lider) ---
    if (rival_x >= 0) {
        int best_sabotage = -1;
        int best_dist = 0x7fffffff;
        for (int d = 0; d < 8; d++) {
            if (neighbor_val[d] < -100) continue; // fuera del tablero, saltar
            int nx = px + DIR_DX[d];
            int ny = py + DIR_DY[d];
            int dist = abs(nx - rival_x) + abs(ny - rival_y);
            if (dist < best_dist) {
                best_dist = dist;
                best_sabotage = d;
            }
        }
        if (best_sabotage != -1)
            return (MoveDirection)best_sabotage;
    }

    // --- Fase 3: fallback — cualquier celda dentro del tablero ---
    for (int d = 0; d < 8; d++) {
        if (neighbor_val[d] > -100)  // -128 indica fuera del tablero
            return (MoveDirection)d;
    }

    return (MoveDirection)(rand() % 8); // último recurso
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
    // 1. Primero pasamos por mutexWriter
    // esto hace de molinete: no deja que sigan entrando lectores para siempre
    // si el master quiere escribir, con esto evitamos que se muera de hambre
    if (sem_wait(&sync->mutexWriter) == -1) {
        perror("Error en sem_wait de mutexWriter");
        return -1;
    }

    // 2. Ahora tomamos mutexReaders
    // este protege solamente playersReading
    // o sea: no protege el tablero, protege el contador de lectores
    if (sem_wait(&sync->mutexReaders) == -1) {
        perror("Error en sem_wait de mutexReaders");
        sem_post(&sync->mutexWriter);
        return -1;
    }

    // 3. Nos anotamos como lector activo
    sync->playersReading++;

    // 4. Si somos el primer lector, cerramos la puerta al escritor
    if (sync->playersReading == 1) {
        if (sem_wait(&sync->mutexStatus) == -1) {
            perror("Error en sem_wait de mutexStatus");
            sync->playersReading--;
            sem_post(&sync->mutexReaders);
            sem_post(&sync->mutexWriter);
            return -1;
        }
    }

    // 5. Ya terminamos de tocar playersReading, soltamos mutexReaders
    if (sem_post(&sync->mutexReaders) == -1) {
        perror("Error en sem_post de mutexReaders");
        return -1;
    }

    // 6. Soltamos mutexWriter
    if (sem_post(&sync->mutexWriter) == -1) {
        perror("Error en sem_post de mutexWriter");
        return -1;
    }

    // 7. Desde aca ya podemos leer el estado compartido
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
    // 1. Volvemos a tomar mutexReaders porque vamos a tocar playersReading
    if (sem_wait(&sync->mutexReaders) == -1) {
        perror("Error en sem_wait de mutexReaders");
        return -1;
    }

    // 2. Dejamos de contarnos como lector activo
    sync->playersReading--;

    // 3. Si eramos el ultimo lector, reabrimos la puerta al escritor
    if (sync->playersReading == 0) {
        if (sem_post(&sync->mutexStatus) == -1) {
            perror("Error en sem_post de mutexStatus");
            sem_post(&sync->mutexReaders);
            return -1;
        }
    }

    // 4. Ya terminamos con el contador, soltamos mutexReaders
    if (sem_post(&sync->mutexReaders) == -1) {
        perror("Error en sem_post de mutexReaders");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <indice_jugador> <pipe_fd>\n", argv[0]);
        return 1;
    }

    // 2. El pipe siempre es el FD 1 (stdout) 
    int pipe_fd = 1;

    int turn = 0; // Contador de turnos
    srand(time(NULL) ^ getpid()); // Semilla unica por jugador usando su PID

    // Obtengo file descriptor de /game_state
    int fd_state = shm_open("/game_state", O_RDONLY, 0);
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
    
    // Tamaño real de la /game_state
    size_t state_size = (size_t)state_info.st_size;
    // Puntero a la memoria compartida en state
    GameState *state = mmap(NULL, state_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (state == MAP_FAILED) {
        perror("Error mapeando /game_state");
        close(fd_state);
        close(pipe_fd);
        return 1;
    }
    close(fd_state);

    // File descriptor de /game_sync
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
 
    // Puntero a la memoria compartida en sync
    Sync *sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) {
        perror("Error mapeando /game_sync");
        close(fd_sync);
        munmap(state, state_size);
        close(pipe_fd);
        return 1;
    }
    close(fd_sync);

    // Busca su propio indice usando mi PID
    pid_t mi_pid = getpid();
    int player_index = -1;
    
    if (begin_read(sync) == -1) return 1;

    for (int i = 0; i < state->numPlayers; i++) {
        if (state->players[i].pid == mi_pid) {
            player_index = i;
            break;
        }
    }

    end_read(sync);

    if (player_index == -1) {
        fprintf(stderr, "Error: No encontre mi PID (%d) en el GameState.\n", mi_pid);
        munmap(sync, sync_size);
        munmap(state, state_size);
        return 1;
    }

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

        // El jugador espera aca hasta que el master le diga "ya procese el anterior, manda otro"
        if (sem_wait(&sync->allowed_Mov[player_index]) == -1) {
            perror("Error en sem_wait de allowed_Mov");
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }

        // Entramos como lector para mirar el estado sin pisarnos con el master
        if (begin_read(sync) == -1) {
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }

        finished = state->finished;

        // --- LECTURA DE ESTADO PARA DECISION INTELIGENTE ---
        int px = state->players[player_index].x;
        int py = state->players[player_index].y;

        // Valor de cada celda vecina (-128 = fuera del tablero)
        signed char neighbor_val[8];
        bool atrapado = true;

        for (int d = 0; d < 8; d++) {
            int nx = px + DIR_DX[d];
            int ny = py + DIR_DY[d];
            if (nx >= 0 && nx < state->width && ny >= 0 && ny < state->height) {
                neighbor_val[d] = (signed char)state->board[ny * state->width + nx];
                if (neighbor_val[d] > 0)
                    atrapado = false;
            } else {
                neighbor_val[d] = -128; // fuera del tablero
            }
        }

        // Rival con mayor puntaje (para la fase de sabotaje)
        int rival_x = -1, rival_y = -1;
        unsigned int best_score = 0;
        for (int i = 0; i < state->numPlayers; i++) {
            if (i == player_index) continue;
            if (!state->players[i].blocked && state->players[i].score >= best_score) {
                best_score = state->players[i].score;
                rival_x = state->players[i].x;
                rival_y = state->players[i].y;
            }
        }

        // Salimos del protocolo lector cuando terminamos de mirar el estado
        if (end_read(sync) == -1) {
            munmap(sync, sync_size);
            munmap(state, state_size);
            close(pipe_fd);
            return 1;
        }

        // Si el master ya marco fin de juego, salimos sin mandar un movimiento extra
        if (finished) {
            break;
        }

        // Si estamos atrapados, nos rendimos cortando el ciclo
        if (atrapado) {
            break; 
        }
        
        request.direction = smart_behavior(neighbor_val, px, py, rival_x, rival_y);

        turn++; // Incrementamos el turno

        // Write manda los bytes del struct por el pipe de este jugador hacia el master
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
