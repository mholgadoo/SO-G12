#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
 
static void print_state(const GameState *state)
{
    system("clear");
    
    printf("\n=== ESTADO DEL JUEGO ===\n");
    printf("Tablero: %hu x %hu\n", state->width, state->height);
    printf("Jugadores: %hhu\n", state->numPlayers);
    printf("Finalizado: %s\n", state->finished ? "si" : "no");

    // INFORMACION DE CADA JUGADOR //
    for (int i = 0; i < state->numPlayers; i++) {
        const Player *player = &state->players[i];
        printf("Jugador %d: %s | score=%u | valid=%u | invalid=%u | pos=(%hu,%hu) | blocked=%s | pid=%d\n",
               i+1, player->name, player->score, player->valid_mov, player->invalid_mov,
               player->x, player->y, player->blocked ? "si" : "no", (int)player->pid);
    }

    // DIBUJO DEL TABLERO //
    printf("\n--- TABLERO ---\n");
    
    for (int y = 0; y < state->height; y++) {
        for (int x = 0; x < state->width; x++) {
            
            signed char cell_value = state->board[y * state->width + x];
            
            if (cell_value >= 1 && cell_value <= 9) {
                // Recompensa
                printf(".%d ", cell_value); 
            } 
            else if (cell_value <= 0 && cell_value >= -8) {
                // Celda capturada (cuerpo o cabeza)
                int index_jugador = -cell_value;
                
                // Revisamos si esta celda (x, y) es exactamente donde esta el jugador ahora
                if (state->players[index_jugador].x == x && state->players[index_jugador].y == y) {
                    // Es la cabeza del jugador 
                    printf("[%d]", index_jugador + 1);
                } else {
                    // Es el cuerpo / rastro
                    printf("J%d ", index_jugador + 1);
                }
            } 
            else {
                printf(" ? ");
            }
        }
        printf("\n");
    }
    printf("------------\n");

}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <width> <height>\n", argv[0]);
        return 1;
    }

    // Primero abrimos /game_state, que es donde esta el estado compartido del juego
    int fd_state = shm_open("/game_state", O_RDONLY, 0);
    if (fd_state == -1) {
        perror("Error abriendo /game_state");
        return 1;
    }

    // Como GameState termina con board[], no nos alcanza con sizeof(GameState)
    // entonces le preguntamos al sistema cuanto mide de verdad el objeto compartido
    struct stat state_info;
    if (fstat(fd_state, &state_info) == -1) {
        perror("Error obteniendo tamaño de /game_state");
        close(fd_state);
        return 1;
    }

    size_t state_size = (size_t)state_info.st_size;

    // Recien ahora mapeamos esa memoria al proceso vista para obtener un puntero usable
    GameState *state = mmap(NULL, state_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (state == MAP_FAILED) {
        perror("Error mapeando /game_state");
        close(fd_state);
        return 1;
    }

    // El fd ya no hace falta despues del mmap, porque el acceso real lo hacemos con el puntero state
    close(fd_state);

    // Abrimos la otra shared memory, que tiene todos los semaforos y datos de sincronizacion
    int fd_sync = shm_open("/game_sync", O_RDWR, 0);
    if (fd_sync == -1) {
        perror("Error abriendo /game_sync");
        munmap(state, state_size);
        return 1;
    }

    // En Sync si nos alcanza con sizeof porque no tiene parte variable
    size_t sync_size = sizeof(Sync);

    Sync *sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) {
        perror("Error mapeando /game_sync");
        close(fd_sync);
        munmap(state, state_size);
        return 1;
    }

    close(fd_sync);

    // Desde aca la vista queda "durmiendo" hasta que el master le avise que hay algo para mostrar
    while (true) {
        if (sem_wait(&sync->canPrint) == -1) {
            perror("Error en sem_wait de canPrint");
            munmap(sync, sync_size);
            munmap(state, state_size);
            return 1;
        }

        // Cuando sem_wait destraba, significa que el master ya dejo un estado consistente para mirar
        print_state(state);

        bool was_finished =state->finished;

        if (sem_post(&sync->completedPrint) == -1) {
            perror("Error en sem_post de completedPrint");
            munmap(sync, sync_size);
            munmap(state, state_size);
            return 1;
        }

        if (was_finished) {
            break;
        }
    }

    // Cleanup
    munmap(sync, sync_size);
    munmap(state, state_size);

    return 0;
}
