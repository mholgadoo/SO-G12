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
                // Celda capturada (Cuerpo o Cabeza)
                int index_jugador = -cell_value;
                
                // Revisamos si esta celda (x, y) es exactamente donde está el jugador AHORA
                if (state->players[index_jugador].x == x && state->players[index_jugador].y == y) {
                    // Es la CABEZA del jugador (le ponemos corchetes o lo marcamos diferente)
                    printf("[%d]", index_jugador + 1);
                } else {
                    // Es el CUERPO / RASTRO (dejamos la J)
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

    fflush(stdout);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <width> <height>\n", argv[0]);
        return 1;
    }

    // la vista no recibe argumentos utiles por linea de comandos
    // todo lo que necesita lo va a buscar sola en shared memory usando los nombres fijos

    // primero abrimos /game_state, que es donde esta el estado compartido del juego
    int fd_state = shm_open("/game_state", O_RDONLY, 0);
    if (fd_state == -1) {
        perror("Error abriendo /game_state");
        return 1;
    }

    // como GameState termina con board[], no nos alcanza con sizeof(GameState)
    // entonces le preguntamos al sistema cuanto mide de verdad el objeto compartido
    struct stat state_info;
    if (fstat(fd_state, &state_info) == -1) {
        perror("Error obteniendo tamaño de /game_state");
        close(fd_state);
        return 1;
    }

    size_t state_size = (size_t)state_info.st_size;

    // recien ahora mapeamos esa memoria al proceso vista para obtener un puntero usable
    GameState *state = mmap(NULL, state_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (state == MAP_FAILED) {
        perror("Error mapeando /game_state");
        close(fd_state);
        return 1;
    }

    // el fd ya no hace falta despues del mmap, porque el acceso real lo hacemos con el puntero state
    close(fd_state);

    // ahora abrimos la otra shared memory, que tiene todos los semaforos y datos de sincronizacion
    int fd_sync = shm_open("/game_sync", O_RDWR, 0);
    if (fd_sync == -1) {
        perror("Error abriendo /game_sync");
        munmap(state, state_size);
        return 1;
    }

    // en Sync si nos alcanza con sizeof porque no tiene parte variable al final
    size_t sync_size = sizeof(Sync);

    Sync *sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) {
        perror("Error mapeando /game_sync");
        close(fd_sync);
        munmap(state, state_size);
        return 1;
    }

    close(fd_sync);

    // desde aca la vista queda "durmiendo" hasta que el master le avise que hay algo para mostrar
    while (true) {
        if (sem_wait(&sync->canPrint) == -1) {
            perror("Error en sem_wait de canPrint");
            munmap(sync, sync_size);
            munmap(state, state_size);
            return 1;
        }

        // cuando sem_wait destraba, significa que el master ya dejo un estado consistente para mirar
        print_state(state);

        // FIX race condition: guardamos finished en una variable local ANTES de hacer
        // sem_post(completedPrint). Una vez que hacemos post, el master se desbloquea y
        // podria modificar state. Si leyeramos state->finished despues del post estariamos
        // leyendo memoria que el master podria estar tocando al mismo tiempo.
        bool was_finished =state->finished;

        // con este sem_post le avisamos al master "listo, ya imprimi"
        if (sem_post(&sync->completedPrint) == -1) {
            perror("Error en sem_post de completedPrint");
            munmap(sync, sync_size);
            munmap(state, state_size);
            return 1;
        }

        // usamos la copia local, no state->finished directamente
        if (was_finished) {
            break;
        }
    }

    // cleanup basico: desmapear lo que mapeamos
    munmap(sync, sync_size);
    munmap(state, state_size);

    return 0;
}
