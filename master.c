#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/mman.h>

#include "common.h" 

int main(int argc, char *argv[]) {
    // ### RECEPCION Y PROCESAMIENTO DE PARÁMETROS ### // 
    // 1. Variables de configuración con sus valores por defecto
    int width = 10;                 // Mínimo y default: 10
    int height = 10;                // Mínimo y default: 10
    int delay = 200;                // Default: 200 ms
    int timeout = 10;               // Default: 10 segundos
    int seed = time(NULL);          // Default: time(NULL)
    char *view_path = NULL;         // Default: Sin vista
    char *player_paths[9];          // Máximo 9 jugadores
    int num_players = 0;            // Contador de jugadores ingresados

    int opt;
    // 2. Bucle de getopt. Los dos puntos ':' indican que la bandera requiere un valor.
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:")) != -1) {
        switch (opt) {
            case 'w':
                width = atoi(optarg);
                if (width < 10) width = 10; // Validación de mínimo
                break;
            case 'h':
                height = atoi(optarg);
                if (height < 10) height = 10; // Validación de mínimo
                break;
            case 'd':
                delay = atoi(optarg);
                break;
            case 't':
                timeout = atoi(optarg);
                break;
            case 's':
                seed = atoi(optarg);
                break;
            case 'v':
                view_path = optarg;
                break;
            case 'p':
                // Primero guardamos el argumento que getopt ya capturó en optarg
                if (num_players < 9) {
                    player_paths[num_players++] = optarg;
                }
                // Revisamos manualmente los siguientes argumentos hasta encontrar otra bandera ('-')
                // Si no hacemos este while, solo se capturará el primer jugador y los demás serán ignorados por getopt
                while (optind < argc && argv[optind][0] != '-') {
                    if (num_players < 9) {
                        player_paths[num_players++] = argv[optind];
                    }
                    optind++; // Avanzamos el índice manualmente
                }
                break;
            default:
                fprintf(stderr, "Uso incorrecto de parámetros.\n");
                exit(EXIT_FAILURE);
        }
    }

    // 3. Validación de jugarores
    if (num_players == 0) {
        fprintf(stderr, "Error: Se requiere al menos un jugador.\n");
        exit(EXIT_FAILURE);
    }

    // Solo para comprobar que todo cargo bien 
    printf("Master iniciado con Tablero %dx%d, Delay: %d, Timeout: %d, Semilla: %d\n", 
           width, height, delay, timeout, seed);
    printf("Jugadores detectados: %d\n", num_players);

    // ### LÓGICA DE MEMORIA ### // 

    // 1. Limpieza preventiva de recursos compartidos anteriores
    shm_unlink("/game_state");
    shm_unlink("/game_sync");

    // 2. Cálculo de tamaños
    size_t board_bytes = width * height * sizeof(char);
    size_t state_size = sizeof(GameState) + board_bytes;  // Sumamos el tamaño estático del struct más el tamaño dinámico del tablero
    size_t sync_size = sizeof(Sync);

    // CREACIÓN DE MEMORIA COMPARTIDA: /game_state //
    int fd_state = shm_open("/game_state", O_CREAT | O_EXCL | O_RDWR, 0666); // Creamos la memoria compartida
    if (fd_state == -1) {
        perror("Error creando /game_state");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd_state, state_size) == -1) { // Ajustamos el tamaño de la memoria compartida
        perror("Error en ftruncate de /game_state");
        exit(EXIT_FAILURE);
    }

    GameState *state = mmap(NULL, state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_state, 0); // Mapeamos la memoria compartida a nuestro espacio de direcciones
    if (state == MAP_FAILED) {
        perror("Error en mmap de /game_state");
        exit(EXIT_FAILURE);
    }
    // Una vez mapeada la memoria, podemos cerrar el descriptor de archivo ya que no lo vamos a necesitar más 
    // El espacio de memoria seguirá existiendo y lo vamos a poder acceder a través del puntero state
    close(fd_state); 

    
    // CREACIÓN DE MEMORIA COMPARTIDA: /game_sync // 
    int fd_sync = shm_open("/game_sync", O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd_sync == -1) {
        perror("Error creando /game_sync");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd_sync, sync_size) == -1) {
        perror("Error en ftruncate de /game_sync");
        exit(EXIT_FAILURE);
    }

    Sync *sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) {
        perror("Error en mmap de /game_sync");
        exit(EXIT_FAILURE);
    }
    close(fd_sync);

    // ### INICIALIZACIÓN DE DATOS Y SINCRONIZACIÓN ### //
    
    // Inicializamos las variables estáticas del juego
    state->width = width;
    state->height = height;
    state->numPlayers = num_players;
    state->finished = false;
    // Limpiamos el tablero 
    memset(state->board, 0, board_bytes);

    // ### INICIALIZACIÓN DE SEMÁFOROS ### //

    // Master <-> Vista
    if (sem_init(&sync->canPrint, 1, 0) == -1) {
        perror("Error en sem_init de canPrint");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&sync->completedPrint, 1, 0) == -1) {
        perror("Error en sem_init de completedPrint");
        exit(EXIT_FAILURE);
    }

    // Master <-> Jugadores (Lectores-Escritores)
    if (sem_init(&sync->mutexWriter, 1, 1) == -1) {
        perror("Error en sem_init de mutexWriter");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&sync->mutexStatus, 1, 1) == -1) {
        perror("Error en sem_init de mutexStatus");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&sync->mutexReaders, 1, 1) == -1) {
        perror("Error en sem_init de mutexReaders");
        exit(EXIT_FAILURE);
    }
    sync->playersReading = 0;

    // Control de flujo individual por jugador
    for (int i = 0; i < num_players; i++) {
        if (sem_init(&sync->allowed_Mov[i], 1, 1) == -1) {
            perror("Error en sem_init de allowed_Mov");
            exit(EXIT_FAILURE);
        }
    }

    // ### CREACIÓN DE PIPES ### //

    int pipes[9][2]; // pipes[i][0] = lectura (master), pipes[i][1] = escritura (jugador)
    for (int i = 0; i < num_players; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("Error creando pipe");
            exit(EXIT_FAILURE);
        }
    }

    // ### CREACIÓN DE PROCESOS HIJOS ### // 


    // ### BUCLE PRINCIPAL DEL JUEGO ### // 

    return 0;
}
