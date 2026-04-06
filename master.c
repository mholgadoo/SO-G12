#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
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
    // Limpiamos el array de jugadores
    memset(state->players, 0, sizeof(state->players));

    // Inicializamos los jugadores
    for (int i = 0; i < num_players; i++) {
        Player *player = &state->players[i];
        // Obtenemos el nombre del jugador
        const char *base_name = strrchr(player_paths[i], '/');

        if (base_name != NULL) {
            // Si el nombre del jugador tiene una barra, lo ignoramos
            base_name++;
        } else {
            base_name = player_paths[i];
        }
        // Guardamos el nombre del jugador
        snprintf(player->name, sizeof(player->name), "%s", base_name);
        // Inicializamos las variables del jugador
        player->score = 0;
        player->invalid_mov = 0;
        player->valid_mov = 0;
        // Inicializamos las coordenadas del jugador
        /* ponemos en width y height porque el tablero va de 0 a width-1 y de 0 a height-1 entonces
        *  esto significa que el jugador no tiene una posicion asignada todavia
        */
        player->x = (unsigned short)width;
        player->y = (unsigned short)height;
        player->pid = 0;
        player->blocked = false;
    }

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
        // Arranca en 0 porque ahora el master reparte los turnos del round robin
        if (sem_init(&sync->allowed_Mov[i], 1, 0) == -1) {
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

    // ### CREACIÓN DEL PROCESO HIJO VISTA ### // 
    if (view_path != NULL) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("Error creando proceso vista");
            exit(EXIT_FAILURE);
        }
        // Proceso hijo: como la vista no es un jugador, no necesitamos los pipes -> los cerramos
        if (pid == 0) {
            for (int i = 0; i < num_players; i++) {
                close(pipes[i][0]);
                close(pipes[i][1]);
            }

            // En execv armamos manualmente el argv del nuevo programa.
            // char *args[] = { arg0, arg1, arg2, NULL };
            // execv(ruta, args);
            // "ruta" es el ejecutable a cargar.
            // args[0] pasa a ser argv[0] del nuevo programa.
            // El NULL final marca dónde termina el arreglo de argumentos.
            char *view_argv[] = { view_path, NULL };
            execv(view_path, view_argv);

            // Si llego aca, hubo un error en execv
            perror("Error ejecutando vista");
            _exit(EXIT_FAILURE);
        }
        // Proceso padre: sigue la ejecucion de master
    }

    // ### CREACIÓN DE PROCESOS HIJOS JUGADORES ### // 
    for (int i = 0; i < num_players; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("Error creando proceso jugador");
            exit(EXIT_FAILURE);
        }
        // Proceso hijo: ejecuta el jugador
        if (pid == 0) {
            char index_str[12];
            char fd_str[12];
            // Cerramos los pipes que no son del jugador actual
            for (int j = 0; j < num_players; j++) {
                // Cerramos todos los extremos de lectura porque un jugador no lee de ningun pipe
                close(pipes[j][0]);

                if (j != i) {
                    // cerramos el pipe de escritura de todos los otros jugadores
                    close(pipes[j][1]);
                }
            }
        // Hasta aca dejamos al jugador con un solo FD util: pipes[i][1] (escritura)
            
        
            // Esas dos lineas convierten enteros a texto, porque execv solo pasa strings en argv
            snprintf(index_str, sizeof(index_str), "%d", i);
            snprintf(fd_str, sizeof(fd_str), "%d", pipes[i][1]);

            char *player_argv[] = { player_paths[i], index_str, fd_str, NULL };
            execv(player_paths[i], player_argv);

            // Si llego aca, hubo un error en execv
            perror("Error ejecutando jugador");
            _exit(EXIT_FAILURE);
        }
        // Proceso padre: sigue la ejecucion de master
        state->players[i].pid = pid;
        close(pipes[i][1]); 
    }

    // si hay vista, hacemos una primera "foto" del estado inicial
    // la idea es: el master avisa que ya hay algo coherente para mostrar
    // y despues espera hasta que la vista confirme que termino de imprimir
    if (view_path != NULL) {
        if (sem_post(&sync->canPrint) == -1) {
            perror("Error avisando a la vista");
            exit(EXIT_FAILURE);
        }

        if (sem_wait(&sync->completedPrint) == -1) {
            perror("Error esperando a la vista");
            exit(EXIT_FAILURE);
        }
    }


    // ### BUCLE PRINCIPAL DEL JUEGO ### // 
    const int test_rounds = 3;
    bool stop_round_robin = false;

    // por ahora hacemos 3 rondas fijas, pero ya con el orden rotativo del round robin
    for (int round = 0; round < test_rounds && !stop_round_robin; round++) {
        for (int offset = 0; offset < num_players; offset++) {
            int player_index = (round + offset) % num_players;
            MoveRequest request;

            // con esta formula vamos rotando quien arranca primero en cada ronda
            // ejemplo con 3 jugadores:
            // ronda 0 -> 0, 1, 2
            // ronda 1 -> 1, 2, 0
            // ronda 2 -> 2, 0, 1
            if (sem_post(&sync->allowed_Mov[player_index]) == -1) {
                perror("Error habilitando movimiento del jugador");
                stop_round_robin = true;
                break;
            }

            // una vez habilitado ese jugador, esperamos especificamente su mensaje
            ssize_t bytes_read = read(pipes[player_index][0], &request, sizeof(request));

            if (bytes_read == -1) {
                perror("Error leyendo movimiento del jugador");
                stop_round_robin = true;
                break;
            } else if (bytes_read == 0) {
                fprintf(stderr, "El jugador %d cerro el pipe sin enviar movimiento.\n", player_index);
                stop_round_robin = true;
                break;
            } else if (bytes_read != (ssize_t)sizeof(request)) {
                fprintf(stderr, "Se recibio un movimiento incompleto del jugador %d.\n", player_index);
                stop_round_robin = true;
                break;
            } else {
                // por ahora solo mostramos quien mando y que direccion llego
                printf("Ronda %d, turno %d: jugador %d mando direccion %u\n",
                       round, offset, player_index, request.direction);
            }
        }
    }

    // primero marcamos finished para que el jugador, cuando se despierte, salga del loop
    state->finished = true;

    // hacemos una ultima señal a la vista para que vea el estado final con finished = true
    // de esa forma imprime una ultima vez y puede salir prolijamente de su loop
    if (view_path != NULL) {
        if (sem_post(&sync->canPrint) == -1) {
            perror("Error avisando a la vista");
            exit(EXIT_FAILURE);
        }

        if (sem_wait(&sync->completedPrint) == -1) {
            perror("Error esperando a la vista");
            exit(EXIT_FAILURE);
        }
    }

    // aca destrabamos a todos porque cada uno puede haber quedado esperando su proximo turno
    for (int i = 0; i < num_players; i++) {
        sem_post(&sync->allowed_Mov[i]);
    }

    return 0;
}
